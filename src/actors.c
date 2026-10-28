/* The actor kernel (actors.h). */
#include "actors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRIM(name) static void name(Forth *f, Word *w __attribute__((unused)))
#define PUSH(x) forth_push(f, (Cell)(x))
#define POP() forth_pop(f)
#define K (f->actors)

#define ROUNDS_MAX 64           /* delivery rounds in one frame before the rest waits */
#define STATE_TYPES_MAX 128
#define BEHAVIOURS_MAX 256
#define MARK ((Cell)0x6B636142)   /* `behaviour` ... `end-behaviour`: what `on` left above it */
#define RECENT_MAX 256          /* the messages last delivered, kept for `.recent` (bug reports) */
#define FAILURES_KEPT 32        /* the latest failures, for the errors actor to read (a record's number names it) */
#define WORDS_OF(n) (((n) + 31) / 32)
#define NOBODY (-1)

typedef struct Kind {
    char name[FORTH_ACTOR_NAME_MAX + 1];
    int nargs;
} Kind;

typedef struct Behaviour {
    char name[FORTH_ACTOR_NAME_MAX + 1];
    struct Behaviour *parent;
    Word *on[FORTH_KINDS_MAX];
    uint8_t reported[FORTH_KINDS_MAX / 8];   /* unhandled kinds already reported */
    int unhandled;
} Behaviour;

typedef struct Field {
    char name[WORD_NAME_MAX + 1];
    int off, size;
} Field;

typedef struct StateType {
    char name[FORTH_ACTOR_NAME_MAX + 1];
    size_t size;
    Field *fields;          /* its fields, in order (for `.state`) */
    int nfields;
} StateType;

typedef struct Message {
    int kind, from, to, n;
    Cell a[FORTH_MSG_ARGS];
} Message;

typedef struct Actor {
    int used;
    char name[FORTH_ACTOR_NAME_MAX + 1];
    Behaviour *beh;
    int type;               /* its state's type (-1: none) */
    uint8_t *state;
    uint8_t *body;          /* the host's (body_bytes) */
    long handled;           /* messages it has handled */
    int dying;              /* its `killed` handler (its last words) is running */
    int doomed;             /* killed while a handler of its ran: gone when the handler returns */
    int running;            /* a handler of its is running */
    Task *task;             /* its handlers' stacks: one may wait in it */
    int busy;               /* a handler of its waits (in its task): its messages kept till it ends */
    int kind, from;         /* the message that handler handles (and who sent it) */
    int awaiting;           /* the kind its handler awaits (-1: none) */
    long await_until;       /* (await-within) the frame it stops waiting; -1: never */
    Message *mail;          /* kept for it while it was busy: oldest first, from mail_head */
    int mail_head, mail_n, mail_cap;
    uint32_t watchers[WORDS_OF(FORTH_ACTORS_MAX)];   /* those monitoring it: told `died` */
    uint32_t links[WORDS_OF(FORTH_ACTORS_MAX)];      /* those linked to it: killed with it */
} Actor;

/* a handler's error or a task's, kept for the errors actor (errors-to) */
typedef struct Failure {
    long number;            /* 0: none */
    int actor;              /* the actor whose handler failed; -1: a task */
    char who[FORTH_ACTOR_NAME_MAX + 6];   /* its name ("task <name>" for a task's) */
    char what[FORTH_ACTOR_NAME_MAX + 1];  /* the message it was handling ("" for a task's) */
    char error[256];
    char chain[1024];       /* the words running */
    long frame;
} Failure;

struct ForthActors {
    size_t body_bytes;
    Kind kinds[FORTH_KINDS_MAX];
    int nkinds;
    Behaviour *behaviours[BEHAVIOURS_MAX];
    int nbehaviours;
    StateType types[STATE_TYPES_MAX];
    int ntypes;
    Actor actors[FORTH_ACTORS_MAX];
    int next_id;            /* where the next spawn looks first: ids go round, so one just freed isn't reused at once
                             * (a stale id kept somewhere then names nobody, not a newcomer) */
    uint32_t subs[FORTH_KINDS_MAX][WORDS_OF(FORTH_ACTORS_MAX)];
    Message *queue;
    int queue_n, queue_cap;
    int self, sender;       /* whose message is being handled, and who sent it */
    int delivering, trace;
    long frame;
    long dead_letters;      /* messages to actors no longer there */
    struct { long frame; Message m; } recent[RECENT_MAX];
    long nrecent;
    uint32_t last_said[FORTH_KINDS_MAX][FORTH_ACTORS_MAX + 1];   /* per kind and sender (+1: the engine's): its last
                                                                   * arguments' hash - the same again isn't kept */
    Failure failures[FAILURES_KEPT];
    long failures_told;
    int errors_actor;
    Behaviour *defining;    /* behaviour ... end-behaviour */
};

static Actor *actor_at(Forth *f, int id) {
    return id >= 0 && id < FORTH_ACTORS_MAX && K->actors[id].used ? &K->actors[id] : NULL;
}

static int per_frame_kind(int kind) {   /* (the frame's own: missed by an actor busy, not kept for it) */
    return kind == FORTH_KIND_TICK || kind == FORTH_KIND_FRAME_END || kind == FORTH_KIND_HELD_TICK;
}

static void set_bit(uint32_t *bits, int id) { bits[id / 32] |= 1u << (id % 32); }
static void clear_bit(uint32_t *bits, int id) { bits[id / 32] &= ~(1u << (id % 32)); }
static int bit(const uint32_t *bits, int id) { return (bits[id / 32] >> (id % 32)) & 1; }

/* ---- kinds ---- */

static int kind_new(Forth *f, const char *name, size_t len, int nargs) {
    int k;

    for (k = 0; k < K->nkinds; k++) {
        if (strlen(K->kinds[k].name) == len && strncmp(K->kinds[k].name, name, len) == 0) {
            K->kinds[k].nargs = nargs;   /* (declared again: a script reloaded) */
            return k;
        }
    }
    if (K->nkinds == FORTH_KINDS_MAX) {
        return -1;
    }
    snprintf(K->kinds[K->nkinds].name, sizeof(K->kinds[0].name), "%.*s", (int)len, name);
    K->kinds[K->nkinds].nargs = nargs;
    return K->nkinds++;
}

static int kind_find(Forth *f, const char *name, size_t len) {
    int k;

    for (k = 0; k < K->nkinds; k++) {
        if (strlen(K->kinds[k].name) == len && strncmp(K->kinds[k].name, name, len) == 0) {
            return k;
        }
    }
    return -1;
}

/* the next name in the input, as a message kind (an error if it isn't one) */
static int parse_kind(Forth *f, const char *what) {
    size_t len;
    const char *name = forth_parse_name(f, &len);
    int k;

    if (len == 0) {
        forth_error(f, "%s: a message name expected", what);
    }
    k = kind_find(f, name, len);
    if (k < 0) {
        forth_error(f, "%s: no message %.*s (declare it with `message`)", what, (int)len, name);
    }
    return k;
}

static int kind_arg(Forth *f, Cell k, const char *what) {
    if (k < 0 || k >= K->nkinds) {
        forth_error(f, "%s: no message kind %ld", what, (long)k);
    }
    return (int)k;
}

/* ---- the queue ---- */

static void enqueue(Forth *f, int from, int to, int kind, const Cell *args, int n) {
    Message *m;

    if (K->queue_n == K->queue_cap) {
        int cap = K->queue_cap ? K->queue_cap * 2 : 256;
        Message *q = realloc(K->queue, (size_t)cap * sizeof(Message));

        if (q == NULL) {
            forth_error(f, "actors: out of memory for the queue");
        }
        K->queue = q;
        K->queue_cap = cap;
    }
    m = &K->queue[K->queue_n++];
    m->kind = kind;
    m->from = from;
    m->to = to;
    m->n = n;
    if (n > 0) {   /* (none: args may be NULL) */
        memcpy(m->a, args, (size_t)n * sizeof(Cell));
    }
}

void forth_actor_send(Forth *f, int from, int to, int kind, const Cell *args, int n) {
    enqueue(f, from, to, kind, args, n);
}

void forth_actor_broadcast(Forth *f, int from, int kind, const Cell *args, int n) {
    int id;

    for (id = 0; id < FORTH_ACTORS_MAX; id++) {
        if (K->actors[id].used && bit(K->subs[kind], id)) {
            enqueue(f, from, id, kind, args, n);
        }
    }
}

/* a message kept in an actor's mailbox (it is busy, or has older ones kept) */
static void mail_keep(Forth *f, Actor *a, const Message *m) {
    if (a->mail_head + a->mail_n == a->mail_cap) {
        if (a->mail_head > 0) {   /* (the room the taken ones left, first) */
            memmove(a->mail, a->mail + a->mail_head, (size_t)a->mail_n * sizeof(Message));
            a->mail_head = 0;
        }
        if (a->mail_n == a->mail_cap) {
            int cap = a->mail_cap ? a->mail_cap * 2 : 16;
            Message *mail = realloc(a->mail, (size_t)cap * sizeof(Message));

            if (mail == NULL) {
                forth_error(f, "actors: out of memory for %s's mail", a->name);
            }
            a->mail = mail;
            a->mail_cap = cap;
        }
    }
    a->mail[a->mail_head + a->mail_n++] = *m;
}

/* ---- failures: a handler's error or a task's, told to the errors actor (`errors-to`) ---- */

/* recorded, and the errors actor told (actor-failed); not its own: those are only said */
static void report_failure(Forth *f, int actor, const char *who, const char *what) {
    Failure *r;
    Cell number;

    if (actor_at(f, K->errors_actor) == NULL || actor == K->errors_actor) {
        return;
    }
    number = ++K->failures_told;
    r = &K->failures[number % FAILURES_KEPT];
    r->number = number;
    r->actor = actor;
    snprintf(r->who, sizeof(r->who), "%s", who);
    snprintf(r->what, sizeof(r->what), "%s", what);
    snprintf(r->error, sizeof(r->error), "%s", f->error);
    snprintf(r->chain, sizeof(r->chain), "%s", f->error_chain);
    r->frame = K->frame;
    enqueue(f, actor, K->errors_actor, FORTH_KIND_ACTOR_FAILED, &number, 1);
}

static void task_failed(Forth *f, Task *t) {
    char who[FORTH_ACTOR_NAME_MAX + 6];

    snprintf(who, sizeof(who), "task %.*s", FORTH_ACTOR_NAME_MAX, t->name);
    report_failure(f, -1, who, "");
}

static Failure *failure_arg(Forth *f) {   /* ( number -- ) the record (an empty one once it is gone) */
    static Failure gone = {.actor = -1};
    Cell number = POP();
    Failure *r = &K->failures[(number > 0 ? number : 0) % FAILURES_KEPT];

    return number > 0 && r->number == number ? r : &gone;
}

/* ---- delivery ---- */

static const char *actor_name(Forth *f, int id) {
    Actor *a = actor_at(f, id);

    return a != NULL ? a->name : id < 0 ? "engine" : "(gone)";
}

/* a cell as it reads best: a whole number, or a float (a field's cell holds either) */
static void print_cell(Forth *f, Cell c) {
    double d;

    memcpy(&d, &c, sizeof(d));
    if (c > -0x10000000000LL && c < 0x10000000000LL) {
        forth_printf(f, " %ld", (long)c);
    } else if (d == d && d > -1e12 && d < 1e12) {
        forth_printf(f, " %ge", d);
    } else {
        forth_printf(f, " $%lX", (unsigned long)c);
    }
}

static void trace(Forth *f, const Message *m) {
    int i;

    forth_printf(f, "%ld  %s -> %s  %s", K->frame, actor_name(f, m->from), actor_name(f, m->to), K->kinds[m->kind].name);
    for (i = 0; i < m->n; i++) {
        forth_printf(f, " %ld", (long)m->a[i]);
    }
    forth_printf(f, "\n");
}

/* kept for `.recent`: a message unlike the last of its kind from its sender (so the every-frame
 * broadcasts show their changes, once, and not each copy to each subscriber) */
static void remember(Forth *f, const Message *m) {
    uint32_t h = 2166136261u, *last;
    int i;

    for (i = 0; i < m->n; i++) {   /* (FNV-1a over the arguments) */
        const uint8_t *b = (const uint8_t *)&m->a[i];
        size_t k;

        for (k = 0; k < sizeof(Cell); k++) {
            h = (h ^ b[k]) * 16777619u;
        }
    }
    h |= 1;   /* (0: nothing said yet) */
    last = &K->last_said[m->kind][m->from >= 0 && m->from < FORTH_ACTORS_MAX ? m->from + 1 : 0];
    if (*last == h) {
        return;
    }
    *last = h;
    K->recent[K->nrecent % RECENT_MAX].frame = K->frame;
    K->recent[K->nrecent % RECENT_MAX].m = *m;
    K->nrecent++;
}

static void destroy(Forth *f, int id);

static Word *handler_of(const Actor *a, int kind) {
    const Behaviour *b;

    for (b = a->beh; b != NULL; b = b->parent) {
        if (b->on[kind] != NULL) {
            return b->on[kind];
        }
    }
    return NULL;
}

/* a cell pushed onto a task's data stack (its arguments, before it runs) */
static void task_push(Forth *f, Task *t, Cell x) {
    if (t->sp >= STACK_CELLS) {
        forth_error(f, "actors: a message's arguments overflow the stack");
    }
    t->ds[t->sp++] = x;
}

/* a turn of the actor's task: its handler going on (from its start, after a wait, or with what it awaited) */
static void handler_turn(Forth *f, int id) {
    Actor *a = &K->actors[id];
    int self = K->self, sender = K->sender, r;

    K->self = id;
    K->sender = a->from;
    a->running++;
    r = forth_task_turn(f, a->task);
    a->running--;
    K->self = self;
    K->sender = sender;
    if (!a->used) {
        return;
    }
    if (r == FORTH_TURN_WAITING) {
        a->busy = 1;
    } else {
        a->busy = 0;
        a->awaiting = -1;
        if (r == FORTH_TURN_FAILED) {
            if (!f->quiet) {   /* (a repeat: nor where it was) */
                forth_printf(f, "  (in %s's `on %s`, behaviour %s)\n", a->name, K->kinds[a->kind].name, a->beh->name);
            }
            report_failure(f, id, a->name, K->kinds[a->kind].name);
        } else if (a->task->sp != 0 || a->task->fp != 0) {
            forth_printf(f, "%s's `on %s` left %d cells and %d floats on the stacks\n", a->name, K->kinds[a->kind].name,
                         a->task->sp, a->task->fp);
        }
        a->handled++;
    }
    if (a->doomed && a->running == 0) {   /* (killed meanwhile: a handler waiting is let go) */
        destroy(f, id);
    }
}

/* a handler run now, inside whatever runs (an actor's last words, as it is killed): on its task's stacks, over
 * what they hold - it sees its arguments only, and can't wait */
static void handler_nested(Forth *f, int id, Word *h, const Message *m) {
    Actor *a = &K->actors[id];
    Task *t = a->task, *saved = f->t;
    int sp0 = t->sp, fp0 = t->fp, rp0 = t->rp, lp0 = t->lp, sfloor = t->sfloor, ffloor = t->ffloor, i;
    int self = K->self, sender = K->sender;

    t->sfloor = sp0;   /* (the handler sees its arguments, nothing of whoever delivered) */
    t->ffloor = fp0;
    f->t = t;
    for (i = 0; i < m->n; i++) {
        forth_push(f, m->a[i]);
    }
    K->self = m->to;
    K->sender = m->from;
    a->running++;
    i = forth_call(f, h);
    a->running--;
    if (i != 0) {
        if (!f->quiet) {
            forth_printf(f, "  (in %s's `on %s`, behaviour %s)\n", a->name, K->kinds[m->kind].name, a->beh->name);
        }
        report_failure(f, m->to, a->name, K->kinds[m->kind].name);
    } else if (t->sp != sp0 || t->fp != fp0) {
        forth_printf(f, "%s's `on %s` left %d cells and %d floats on the stacks\n", a->name, K->kinds[m->kind].name,
                     t->sp - sp0, t->fp - fp0);
    }
    t->sp = sp0;   /* (whatever happened, the stacks are as they were) */
    t->fp = fp0;
    t->rp = rp0;
    t->lp = lp0;
    t->sfloor = sfloor;
    t->ffloor = ffloor;
    f->t = saved;
    a->handled++;
    K->self = self;
    K->sender = sender;
}

/* a message delivered: to its actor's handler (begun in its task), or kept for it while it is busy */
static void dispatch(Forth *f, const Message *m, int from_mail) {
    Actor *a = actor_at(f, m->to);
    Word *h;
    int i;

    if (a == NULL) {
        K->dead_letters++;
        return;
    }
    if (a->busy || (!from_mail && a->mail_n > 0)) {   /* (busy, or older ones kept: they go first) */
        if (a->busy && a->awaiting == m->kind) {   /* what its handler awaits: it goes on with it */
            for (i = 0; i < m->n; i++) {
                task_push(f, a->task, m->a[i]);
            }
            if (a->await_until >= 0) {
                task_push(f, a->task, -1);
            }
            a->awaiting = -1;
            a->from = m->from;
            if (K->trace) {
                trace(f, m);
            }
            remember(f, m);
            handler_turn(f, m->to);
        } else if (!per_frame_kind(m->kind)) {
            mail_keep(f, a, m);
        }
        return;
    }
    h = handler_of(a, m->kind);
    if (!per_frame_kind(m->kind)) {
        remember(f, m);
    }
    if (K->trace) {
        trace(f, m);
    }
    if (h == NULL) {
        Behaviour *b = a->beh;

        if (b != NULL && m->kind >= FORTH_KERNEL_KINDS) {   /* (the kernel's own: ignored if unwanted) */
            b->unhandled++;
            if (!(b->reported[m->kind / 8] & (1 << (m->kind % 8)))) {
                b->reported[m->kind / 8] |= (uint8_t)(1 << (m->kind % 8));
                forth_printf(f, "%s (%s) doesn't handle %s\n", a->name, b->name, K->kinds[m->kind].name);
            }
        }
        return;
    }
    if (a->running > 0) {   /* (a handler of its runs, further out: this one inside it) */
        handler_nested(f, m->to, h, m);
        return;
    }
    forth_task_start(f, a->task, h);
    for (i = 0; i < m->n; i++) {
        task_push(f, a->task, m->a[i]);
    }
    a->kind = m->kind;
    a->from = m->from;
    handler_turn(f, m->to);
}

/* the messages kept for actors no longer busy: each one's, oldest first, until it is busy again; any run? */
static int deliver_mail(Forth *f) {
    int id, any = 0;

    for (id = 0; id < FORTH_ACTORS_MAX; id++) {
        Actor *a = &K->actors[id];

        while (a->used && !a->busy && a->mail_n > 0) {
            Message m = a->mail[a->mail_head++];

            a->mail_n--;
            if (a->mail_n == 0) {
                a->mail_head = 0;
            }
            dispatch(f, &m, 1);
            any = 1;
        }
    }
    return any;
}

/* rounds until the queue is empty (or ROUNDS_MAX) */
static void deliver(Forth *f) {
    int round, n, i;

    K->delivering = 1;
    for (round = 0; round < ROUNDS_MAX; round++) {
        int mail = deliver_mail(f);

        if (K->queue_n == 0) {
            if (!mail) {
                break;
            }
            continue;
        }
        n = K->queue_n;
        for (i = 0; i < n; i++) {
            Message m = K->queue[i];   /* (a copy: the queue may grow while it is handled) */

            dispatch(f, &m, 0);
        }
        memmove(K->queue, K->queue + n, (size_t)(K->queue_n - n) * sizeof(Message));
        K->queue_n -= n;
    }
    if (K->queue_n > 0) {
        forth_printf(f, "actors: %d messages still queued after %d rounds (they wait for the next frame)\n", K->queue_n,
                     ROUNDS_MAX);
    }
    K->delivering = 0;
}

/* the busy actors whose waits are over go on (their handlers' turns), in id order */
static void go_on(Forth *f) {
    int id;

    if (f->tasks_paused) {   /* (as the tasks: the world stands still) */
        return;
    }
    K->delivering = 1;
    for (id = 0; id < FORTH_ACTORS_MAX; id++) {
        Actor *a = &K->actors[id];

        if (!a->used || !a->busy) {
            continue;
        }
        if (a->awaiting >= 0) {
            if (a->await_until < 0 || f->frame < a->await_until) {
                continue;
            }
            task_push(f, a->task, 0);   /* (await-within: it didn't come) */
            a->awaiting = -1;
            handler_turn(f, id);
        } else if (forth_task_ready(f, a->task)) {
            handler_turn(f, id);
        }
    }
    K->delivering = 0;
}

void forth_actors_frame(Forth *f) {
    if (K->delivering) {
        return;
    }
    go_on(f);
    forth_actor_broadcast(f, -1, FORTH_KIND_TICK, NULL, 0);
    deliver(f);
    forth_actor_broadcast(f, -1, FORTH_KIND_FRAME_END, NULL, 0);
    deliver(f);
    K->frame++;
}

void forth_actors_held_frame(Forth *f) {
    if (K->delivering) {
        return;
    }
    go_on(f);
    forth_actor_broadcast(f, -1, FORTH_KIND_HELD_TICK, NULL, 0);
    deliver(f);
}

void *forth_actor_body(Forth *f, int id) {
    Actor *a = K != NULL ? actor_at(f, id) : NULL;

    return a != NULL ? a->body : NULL;
}

int forth_actor_self(Forth *f) { return K != NULL ? K->self : -1; }

int forth_actor_kind(Forth *f, const char *name) { return kind_find(f, name, strlen(name)); }

const char *forth_actor_name(Forth *f, int id) {
    Actor *a = K != NULL ? actor_at(f, id) : NULL;

    return a != NULL ? a->name : "";
}

static void forget(Forth *f, Actor *a) {   /* an actor's memory let go */
    free(a->state);
    free(a->body);
    free(a->mail);
    if (a->task != NULL) {
        forth_task_free(f, a->task);
    }
    memset(a, 0, sizeof(*a));
}

void forth_actors_reset(Forth *f) {
    int id;

    for (id = 0; id < FORTH_ACTORS_MAX; id++) {
        forget(f, &K->actors[id]);
    }
    memset(K->subs, 0, sizeof(K->subs));
    K->queue_n = 0;
    K->self = K->sender = -1;
    K->frame = 0;
    K->dead_letters = 0;
    K->next_id = 0;
    K->errors_actor = NOBODY;
}

/* ---- the words ---- */

static Actor *live(Forth *f, Cell id, const char *what) {
    Actor *a = id >= 0 && id < FORTH_ACTORS_MAX ? actor_at(f, (int)id) : NULL;

    if (a == NULL) {
        forth_error(f, "%s: no actor %ld", what, (long)id);
    }
    return a;
}

static Actor *me(Forth *f, const char *what) {
    if (actor_at(f, K->self) == NULL) {
        forth_error(f, "%s: no actor is being run", what);
    }
    return &K->actors[K->self];
}

static Behaviour *behaviour_arg(Forth *f, Cell x, const char *what) {
    int i;

    for (i = 0; i < K->nbehaviours; i++) {
        if ((Cell)K->behaviours[i] == x) {
            return K->behaviours[i];
        }
    }
    forth_error(f, "%s: that is not a behaviour", what);
}

/* message name ( a b -- ): the stack comment's inputs are its arguments */
PRIM(p_message) {
    size_t len, tl;
    const char *name = forth_parse_name(f, &len), *t;
    char cname[FORTH_ACTOR_NAME_MAX + 1];
    int nargs = 0, k;

    if (len == 0 || len > FORTH_ACTOR_NAME_MAX) {
        forth_error(f, "message: a name (up to %d characters) expected", FORTH_ACTOR_NAME_MAX);
    }
    t = forth_parse_name(f, &tl);
    if (tl != 1 || t[0] != '(') {
        forth_error(f, "message %.*s: its stack comment expected, ( args -- )", (int)len, name);
    }
    for (;;) {
        t = forth_parse_name(f, &tl);
        if (tl == 0) {
            forth_error(f, "message %.*s: the stack comment isn't closed", (int)len, name);
        }
        if (tl == 2 && t[0] == '-' && t[1] == '-') {
            break;
        }
        if (tl == 1 && t[0] == ')') {
            forth_error(f, "message %.*s: the stack comment needs --", (int)len, name);
        }
        nargs++;
    }
    do {
        t = forth_parse_name(f, &tl);
    } while (tl != 0 && !(tl == 1 && t[0] == ')'));
    if (nargs > FORTH_MSG_ARGS) {
        forth_error(f, "message %.*s: at most %d arguments", (int)len, name, FORTH_MSG_ARGS);
    }
    k = kind_new(f, name, len, nargs);
    if (k < 0) {
        forth_error(f, "message: too many message kinds");
    }
    snprintf(cname, sizeof(cname), "%.*s", (int)len, name);
    forth_constant(f, cname, k);   /* the name gives its kind */
}

/* send/broadcast: the arguments go now (a copy) */
static void send_now(Forth *f, int to, int kind) {
    int n = K->kinds[kind].nargs, i;
    Cell args[FORTH_MSG_ARGS];

    for (i = n - 1; i >= 0; i--) {
        args[i] = POP();
    }
    if (to < 0) {
        forth_actor_broadcast(f, K->self, kind, args, n);
    } else {
        live(f, to, "send");   /* (to one gone: an error now, where it is sent) */
        enqueue(f, K->self, to, kind, args, n);
    }
}
PRIM(p_send_rt) {   /* (send) ( args to kind -- ) */
    int kind = kind_arg(f, POP(), "send"), to = (int)POP();

    send_now(f, to, kind);
}
PRIM(p_broadcast_rt) {   /* (broadcast) ( args kind -- ) */
    send_now(f, -1, kind_arg(f, POP(), "broadcast"));
}
/* parse the message name; compiling: lay down its kind and `rt`, else run it now */
static void parsing(Forth *f, const char *what, const char *rt) {
    int k = parse_kind(f, what);
    Word *w = forth_find(f, rt, strlen(rt));

    if (f->compiling) {
        forth_compile_literal(f, k);
        forth_compile_word(f, w);
    } else {
        PUSH(k);
        forth_execute(f, w);
    }
}
PRIM(p_send) { parsing(f, "send", "actors:(send)"); }
PRIM(p_broadcast) { parsing(f, "broadcast", "actors:(broadcast)"); }

PRIM(p_subscribe_rt) {   /* (subscribe) ( id kind -- ) */
    int kind = kind_arg(f, POP(), "subscribe"), id = (int)POP();

    live(f, id, "subscribe");
    set_bit(K->subs[kind], id);
}
PRIM(p_unsubscribe_rt) {
    int kind = kind_arg(f, POP(), "unsubscribe"), id = (int)POP();

    live(f, id, "unsubscribe");
    clear_bit(K->subs[kind], id);
}
PRIM(p_subscribe) { parsing(f, "subscribe", "actors:(subscribe)"); }
PRIM(p_unsubscribe) { parsing(f, "unsubscribe", "actors:(unsubscribe)"); }

/* await kind ( -- args ): inside a handler, it waits until a message of that kind comes for its actor (others kept
 * for it meanwhile), then goes on with its arguments; `frames await-within kind ( -- args true | false )` gives up
 * after so many frames */
static void await_now(Forth *f, int kind, long until) {
    Actor *a = actor_at(f, K->self);

    if (a == NULL || f->t != a->task || f->depth != f->task_depth) {
        forth_error(f, "await: only in a handler, as its actor runs it (not its last words, nor from C)");
    }
    a->awaiting = kind;
    a->await_until = until;
    f->yielded = 1;
}
PRIM(p_await_rt) { await_now(f, kind_arg(f, POP(), "await"), -1); }   /* (await) ( kind -- ) */
PRIM(p_await_within_rt) {   /* (await-within) ( frames kind -- ) */
    int kind = kind_arg(f, POP(), "await-within");
    Cell frames = POP();

    await_now(f, kind, f->frame + (frames > 0 ? frames : 1));
}
PRIM(p_await) { parsing(f, "await", "actors:(await)"); }
PRIM(p_await_within) { parsing(f, "await-within", "actors:(await-within)"); }

/* the effect of a word laid down after a message's kind (send, await ...): as that message has it */
static ForthEffect literal_effect(Forth *f, Word *w, Cell kind) {
    ForthEffect e;
    int n = kind >= 0 && kind < K->nkinds ? K->kinds[kind].nargs : -1;

    memset(&e, 0, sizeof(e));
    e.kind = n < 0 ? EFFECT_UNKNOWN : EFFECT_FIXED;
    if (w->code == p_send_rt) {
        e.in = (int8_t)(n + 2);   /* (its arguments, whom to, the kind) */
    } else if (w->code == p_broadcast_rt) {
        e.in = (int8_t)(n + 1);
    } else if (w->code == p_await_rt) {
        e.in = 1;
        e.out = (int8_t)n;
    } else if (w->code == p_await_within_rt) {
        e.in = 2;
        e.out = (int8_t)(n + 1);
        e.out_false = 1;
        e.kind = n < 0 ? EFFECT_UNKNOWN : EFFECT_FLAGGED;
    } else {   /* (subscribe) (unsubscribe) */
        e.in = 2;
    }
    return e;
}

/* behaviour name ... end-behaviour */
PRIM(p_behaviour) {
    size_t len;
    const char *name = forth_parse_name(f, &len);
    char cname[FORTH_ACTOR_NAME_MAX + 1];
    Behaviour *b;
    int i;

    if (len == 0 || len > FORTH_ACTOR_NAME_MAX) {
        forth_error(f, "behaviour: a name (up to %d characters) expected", FORTH_ACTOR_NAME_MAX);
    }
    snprintf(cname, sizeof(cname), "%.*s", (int)len, name);
    b = NULL;
    for (i = 0; i < K->nbehaviours; i++) {   /* (defined again: a script reloaded - start it afresh) */
        if (strcmp(K->behaviours[i]->name, cname) == 0) {
            b = K->behaviours[i];
            memset(b, 0, sizeof(*b));
            snprintf(b->name, sizeof(b->name), "%s", cname);
        }
    }
    if (b == NULL) {
        if (K->nbehaviours == BEHAVIOURS_MAX) {
            forth_error(f, "behaviour: too many");
        }
        b = calloc(1, sizeof(*b));
        if (b == NULL) {
            forth_error(f, "behaviour: out of memory");
        }
        snprintf(b->name, sizeof(b->name), "%s", cname);
        K->behaviours[K->nbehaviours++] = b;
    }
    forth_constant(f, cname, (Cell)b);
    K->defining = b;
    PUSH(MARK);
}
PRIM(p_extends) {
    size_t len;
    const char *name = forth_parse_name(f, &len);
    Word *pw = forth_find(f, name, len);

    if (K->defining == NULL) {
        forth_error(f, "extends: only inside behaviour ... end-behaviour");
    }
    if (pw == NULL) {
        forth_error(f, "extends: no behaviour %.*s", (int)len, name);
    }
    forth_execute(f, pw);
    K->defining->parent = behaviour_arg(f, POP(), "extends");
}
/* on kind ( args -- ) ... ; - a handler (left on the stack with its kind for end-behaviour). Its effect is the
 * message's: its stack comment must say so, and its code is checked against it. */
PRIM(p_on) {
    char name[2 * FORTH_ACTOR_NAME_MAX + 8], effect[2 * FORTH_MSG_ARGS + 8];
    int k, i, n;

    if (K->defining == NULL) {
        forth_error(f, "on: only inside behaviour ... end-behaviour");
    }
    k = parse_kind(f, "on");
    PUSH(k);
    forth_execute(f, forth_find(f, ":noname", 7));
    /* its name, for the call chain (its length kept 0: no word is found by it) */
    snprintf(name, sizeof(name), "%s:on %s", K->defining->name, K->kinds[k].name);
    memcpy(f->latest->name, name, WORD_NAME_MAX);   /* (a long one cut short) */
    f->latest->name[WORD_NAME_MAX] = 0;
    n = snprintf(effect, sizeof(effect), "(");
    for (i = 0; i < K->kinds[k].nargs; i++) {
        n += snprintf(effect + n, sizeof(effect) - (size_t)n, " x");
    }
    snprintf(effect + n, sizeof(effect) - (size_t)n, " -- )");
    snprintf(name, sizeof(name), "its message carries %d", K->kinds[k].nargs);
    forth_declare_effect(f, effect, name);
}
PRIM(p_end_behaviour) {
    Cell x;

    if (K->defining == NULL) {
        forth_error(f, "end-behaviour without behaviour");
    }
    for (;;) {
        x = POP();
        if (x == MARK) {
            break;
        }
        K->defining->on[kind_arg(f, POP(), "end-behaviour")] = (Word *)x;
    }
    K->defining = NULL;
}
PRIM(p_become) { me(f, "become")->beh = behaviour_arg(f, POP(), "become"); }

/* state: name ... end-state; field ( type offset size "name" -- type offset' ) */
static void dofield(Forth *f, Word *w) {
    Cell x = w->body[0];
    int type = (int)(x / 0x10000), off = (int)(x % 0x10000);
    Actor *a;

    if (actor_at(f, K->self) == NULL) {
        forth_error(f, "%s: a field, but no actor is being run", w->name);
    }
    a = &K->actors[K->self];
    if (a->type != type) {
        forth_error(f, "%s: a field of %s, but %s's state is %s", w->name, K->types[type].name, a->name,
                    a->type >= 0 ? K->types[a->type].name : "nothing");
    }
    PUSH(a->state + off);
}
PRIM(p_state) {
    size_t len;
    const char *name = forth_parse_name(f, &len);
    char cname[FORTH_ACTOR_NAME_MAX + 1];
    int t;

    if (len == 0 || len > FORTH_ACTOR_NAME_MAX) {
        forth_error(f, "state: a name expected");
    }
    snprintf(cname, sizeof(cname), "%.*s", (int)len, name);
    for (t = 0; t < K->ntypes && strcmp(K->types[t].name, cname) != 0; t++) {
    }
    if (t == K->ntypes) {
        if (K->ntypes == STATE_TYPES_MAX) {
            forth_error(f, "state: too many types");
        }
        snprintf(K->types[K->ntypes++].name, sizeof(K->types[0].name), "%s", cname);
    }
    forth_constant(f, cname, t);
    PUSH(t);
    PUSH(0);
}
PRIM(p_field) {
    size_t len;
    Cell size = POP(), off = POP(), type = POP();
    const char *name = forth_parse_name(f, &len);
    char cname[WORD_NAME_MAX + 1];
    StateType *t;
    Word *fw;
    int i;

    if (type < 0 || type >= K->ntypes) {
        forth_error(f, "field: not inside state: ... end-state");
    }
    if (len == 0 || len > WORD_NAME_MAX) {
        forth_error(f, "field: a name expected");
    }
    off = (off + 7) & ~(Cell)7;   /* (every field 8-aligned: cells and floats) */
    snprintf(cname, sizeof(cname), "%.*s", (int)len, name);
    fw = forth_constant(f, cname, type * 0x10000 + off);
    fw->code = dofield;
    forth_set_effect(f, fw, "( -- addr )");
    t = &K->types[type];
    for (i = 0; i < t->nfields && strcmp(t->fields[i].name, cname) != 0; i++) {
    }
    if (i == t->nfields) {   /* (not one again: a script reloaded) */
        Field *fields = realloc(t->fields, (size_t)(t->nfields + 1) * sizeof(Field));

        if (fields == NULL) {
            forth_error(f, "field: out of memory");
        }
        t->fields = fields;
        t->nfields++;
    }
    snprintf(t->fields[i].name, sizeof(t->fields[i].name), "%s", cname);
    t->fields[i].off = (int)off;
    t->fields[i].size = (int)size;
    PUSH(type);
    PUSH(off + size);
}
PRIM(p_end_state) {
    Cell size = POP(), type = POP();

    if (type < 0 || type >= K->ntypes) {
        forth_error(f, "end-state without state:");
    }
    K->types[type].size = (size_t)size;
}

/* spawn ( behaviour type addr len -- id ): a new actor, its state zeroed */
PRIM(p_spawn) {
    Cell len = POP(), addr = POP(), type = POP();
    Behaviour *b = behaviour_arg(f, POP(), "spawn");
    Actor *a;
    int id, k;

    if (type < -1 || type >= K->ntypes) {
        forth_error(f, "spawn: no state type %ld", (long)type);
    }
    for (k = 0; k < FORTH_ACTORS_MAX && K->actors[(K->next_id + k) % FORTH_ACTORS_MAX].used; k++) {
    }
    if (k == FORTH_ACTORS_MAX) {
        forth_error(f, "spawn: too many actors");
    }
    id = (K->next_id + k) % FORTH_ACTORS_MAX;
    K->next_id = (id + 1) % FORTH_ACTORS_MAX;
    a = &K->actors[id];
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "%.*s", (int)(len > 0 ? len : 0), len > 0 ? (const char *)addr : "");
    a->beh = b;
    a->type = (int)type;
    a->awaiting = -1;
    a->state = calloc(1, type >= 0 && K->types[type].size > 0 ? K->types[type].size : 8);
    a->body = calloc(1, K->body_bytes > 0 ? K->body_bytes : 8);
    a->task = forth_task_new(f, a->name);
    if (a->state == NULL || a->body == NULL) {
        forget(f, a);
        forth_error(f, "spawn: out of memory");
    }
    a->used = 1;
    enqueue(f, K->self, id, FORTH_KIND_SPAWNED, NULL, 0);
    PUSH(id);
}

static void kill_actor(Forth *f, int id);

/* gone: those monitoring it told, those linked to it killed after it */
static void destroy(Forth *f, int id) {
    Actor *a = &K->actors[id];
    uint32_t links[WORDS_OF(FORTH_ACTORS_MAX)];
    Cell dead = id;
    int k;

    for (k = 0; k < FORTH_ACTORS_MAX; k++) {
        if (bit(a->watchers, k) && actor_at(f, k) != NULL && k != id) {
            enqueue(f, id, k, FORTH_KIND_DIED, &dead, 1);
        }
    }
    memcpy(links, a->links, sizeof(links));
    forget(f, a);
    for (k = 0; k < FORTH_KINDS_MAX; k++) {
        clear_bit(K->subs[k], id);
    }
    for (k = 0; k < K->queue_n; k++) {   /* (what was on its way to it: dead letters, not the next one's) */
        if (K->queue[k].to == id) {
            K->queue[k].to = -1;
        }
    }
    for (k = 0; k < FORTH_ACTORS_MAX; k++) {   /* (nor is it watching or linked to anyone now) */
        clear_bit(K->actors[k].watchers, id);
        clear_bit(K->actors[k].links, id);
    }
    if (K->errors_actor == id) {
        K->errors_actor = NOBODY;
    }
    for (k = 0; k < FORTH_ACTORS_MAX; k++) {
        if (bit(links, k) && actor_at(f, k) != NULL && !K->actors[k].dying) {
            kill_actor(f, k);
        }
    }
}

/* kill: the actor's `killed` handler first (its last words: it lets go of what it holds), then it's gone - after
 * its handler running, if one runs (it killed itself), or let go of one waiting */
static void kill_actor(Forth *f, int id) {
    Actor *a = &K->actors[id];
    Word *h;

    if (a->dying) {
        return;
    }
    a->dying = 1;
    if ((h = handler_of(a, FORTH_KIND_KILLED)) != NULL) {
        Message m = {FORTH_KIND_KILLED, K->self, id, 0, {0}};

        remember(f, &m);
        if (K->trace) {
            trace(f, &m);
        }
        handler_nested(f, id, h, &m);
    }
    if (a->running > 0) {
        a->doomed = 1;
        return;
    }
    destroy(f, id);
}
PRIM(p_kill) {
    Cell id = POP();

    live(f, id, "kill");
    kill_actor(f, (int)id);
}
PRIM(p_self) { PUSH(K->self); }
/* the console inside an actor: its fields, `self`, `become` and sends as that actor's (until
 * `leave-actor`); messages delivered meanwhile still run as their own actors */
PRIM(p_enter) {
    Cell id = POP();

    if (K->delivering) {
        forth_error(f, "enter: not from a handler");
    }
    live(f, id, "enter");
    K->self = (int)id;
    K->sender = -1;
}
PRIM(p_leave) { K->self = -1; K->sender = -1; }
PRIM(p_sender) { PUSH(K->sender); }
PRIM(p_alive) { Cell id = POP(); PUSH(id >= 0 && id < FORTH_ACTORS_MAX && actor_at(f, (int)id) != NULL ? -1 : 0); }
PRIM(p_actor_name) {   /* ( id -- addr len ) */
    Actor *a = live(f, POP(), "actor-name");

    PUSH(a->name);
    PUSH(strlen(a->name));
}
PRIM(p_actor_named) {   /* ( addr len -- id | -1 ) the first live actor with that name */
    Cell len = POP(), addr = POP();
    int id;

    for (id = 0; id < FORTH_ACTORS_MAX; id++) {
        if (K->actors[id].used && strlen(K->actors[id].name) == (size_t)len &&
            strncmp(K->actors[id].name, (const char *)addr, (size_t)len) == 0) {
            PUSH(id);
            return;
        }
    }
    PUSH(-1);
}
PRIM(p_behaviour_of) { PUSH(live(f, POP(), "behaviour-of")->beh); }
PRIM(p_busy) { PUSH(live(f, POP(), "actor-busy?")->busy ? -1 : 0); }   /* ( id -- flag ) a handler of its waits */
PRIM(p_mail) { PUSH(live(f, POP(), "actor-mail")->mail_n); }   /* ( id -- n ) messages kept for it till it's free */

/* monitor ( id -- ): this actor told `died ( id -- )` when that one is gone; link ( id -- ): the two killed together
 * (either gone, the other killed) */
PRIM(p_monitor) {
    Actor *who = me(f, "monitor"), *a = live(f, POP(), "monitor");

    set_bit(a->watchers, (int)(who - K->actors));
}
PRIM(p_unmonitor) {
    Actor *who = me(f, "unmonitor"), *a = live(f, POP(), "unmonitor");

    clear_bit(a->watchers, (int)(who - K->actors));
}
PRIM(p_link) {
    Actor *who = me(f, "link"), *a = live(f, POP(), "link");

    set_bit(a->links, (int)(who - K->actors));
    set_bit(who->links, (int)(a - K->actors));
}
PRIM(p_unlink) {
    Actor *who = me(f, "unlink"), *a = live(f, POP(), "unlink");

    clear_bit(a->links, (int)(who - K->actors));
    clear_bit(who->links, (int)(a - K->actors));
}

PRIM(p_frame) { forth_actors_frame(f); }
PRIM(p_deliver) {
    if (K->delivering) {
        forth_error(f, "deliver: already delivering (not from a handler)");
    }
    deliver(f);
}
PRIM(p_reset) {
    if (K->delivering) {
        forth_error(f, "actors-reset: not from a handler");
    }
    forth_actors_reset(f);
}
PRIM(p_trace_on) { K->trace = 1; }
PRIM(p_trace_off) { K->trace = 0; }
PRIM(p_queued) { PUSH(K->queue_n); }
PRIM(p_actor_frame) { PUSH(K->frame); }
PRIM(p_dot_actors) {
    int id, k, n;

    for (id = 0; id < FORTH_ACTORS_MAX; id++) {
        Actor *a = &K->actors[id];

        if (!a->used) {
            continue;
        }
        for (n = 0, k = 0; k < K->queue_n; k++) {
            n += K->queue[k].to == id;
        }
        forth_printf(f, "%3d %-20s %-16s %-16s %ld handled, %d queued", id, a->name, a->beh ? a->beh->name : "-",
                     a->type >= 0 ? K->types[a->type].name : "-", a->handled, n);
        if (a->busy) {
            forth_printf(f, ", busy in `on %s`%s%s, %d kept", K->kinds[a->kind].name, a->awaiting >= 0 ? " awaiting " : "",
                         a->awaiting >= 0 ? K->kinds[a->awaiting].name : "", a->mail_n);
        }
        forth_printf(f, "\n");
    }
    forth_printf(f, "%ld dead letters\n", K->dead_letters);
}
PRIM(p_dot_state) {   /* ( id -- ) every field of its state, by name */
    Actor *a = live(f, POP(), ".state");
    StateType *t;
    int i, k;

    if (a->type < 0) {
        forth_printf(f, "  (no state)\n");
        return;
    }
    t = &K->types[a->type];
    for (i = 0; i < t->nfields; i++) {
        const Field *fd = &t->fields[i];
        int cells = fd->size / (int)sizeof(Cell);

        forth_printf(f, "  %s", fd->name);
        if (fd->size % 4 == 0 && fd->size % (int)sizeof(Cell) != 0) {   /* (32-bit words: a vector's floats) */
            for (k = 0; k < fd->size / 4 && k < 8; k++) {
                int32_t v;
                float x;

                memcpy(&v, a->state + fd->off + k * 4, 4);
                memcpy(&x, &v, 4);
                if (v > -0x100000 && v < 0x100000) {
                    forth_printf(f, " %d", v);
                } else {
                    forth_printf(f, " %ge", x);
                }
            }
        } else if (fd->size % (int)sizeof(Cell) != 0 || cells == 0) {   /* (bytes: shown as such) */
            for (k = 0; k < fd->size && k < 32; k++) {
                forth_printf(f, " %02X", a->state[fd->off + k]);
            }
        } else {
            for (k = 0; k < cells && k < 8; k++) {
                Cell c;

                memcpy(&c, a->state + fd->off + k * (int)sizeof(Cell), sizeof(c));
                print_cell(f, c);
            }
        }
        if (fd->size > (fd->size % 4 == 0 ? 8 * (fd->size % (int)sizeof(Cell) == 0 ? (int)sizeof(Cell) : 4) : 32)) {
            forth_printf(f, " ...");
        }
        forth_printf(f, "\n");
    }
}
PRIM(p_dot_recent) {   /* ( n -- ) the last n messages delivered (up to 256: not the frames' own, nor
                         * one the same as its sender's last of that kind), oldest first */
    long n = (long)POP(), i;

    if (n > RECENT_MAX) {
        n = RECENT_MAX;
    }
    if (n > K->nrecent) {
        n = K->nrecent;
    }
    for (i = K->nrecent - n; i < K->nrecent; i++) {
        const Message *m = &K->recent[i % RECENT_MAX].m;
        int k;

        forth_printf(f, "%ld  %s -> %s  %s", K->recent[i % RECENT_MAX].frame, actor_name(f, m->from),
                     actor_name(f, m->to), K->kinds[m->kind].name);
        for (k = 0; k < m->n; k++) {
            print_cell(f, m->a[k]);
        }
        forth_printf(f, "\n");
    }
}
PRIM(p_dot_unhandled) {
    int i;

    for (i = 0; i < K->nbehaviours; i++) {
        if (K->behaviours[i]->unhandled) {
            forth_printf(f, "%-20s %d unhandled\n", K->behaviours[i]->name, K->behaviours[i]->unhandled);
        }
    }
}
PRIM(p_f_to_cell) {   /* f>cell ( F: x -- ) ( -- c ): a float to travel in a message */
    double d = forth_fpop(f);
    Cell c;

    memcpy(&c, &d, sizeof(c));
    PUSH(c);
}
PRIM(p_cell_to_f) {
    Cell c = POP();
    double d;

    memcpy(&d, &c, sizeof(d));
    forth_fpush(f, d);
}

/* a point (and a heading) to travel in a message and back: n floats to n cells in the same
 * order (x y z [yaw]), and back */
static void floats_to_cells(Forth *f, int n) {
    double d[4];
    Cell c;
    int i;

    for (i = n - 1; i >= 0; i--) {
        d[i] = forth_fpop(f);
    }
    for (i = 0; i < n; i++) {
        memcpy(&c, &d[i], sizeof(c));
        PUSH(c);
    }
}
static void cells_to_floats(Forth *f, int n) {
    Cell c[4];
    double d;
    int i;

    for (i = n - 1; i >= 0; i--) {
        c[i] = POP();
    }
    for (i = 0; i < n; i++) {
        memcpy(&d, &c[i], sizeof(d));
        forth_fpush(f, d);
    }
}
PRIM(p_xyz_to_cells) { floats_to_cells(f, 3); }      /* xyz>cells ( F: x y z -- ) ( -- x y z ) */
PRIM(p_cells_to_xyz) { cells_to_floats(f, 3); }      /* cells>xyz ( x y z -- ) ( F: -- x y z ) */
PRIM(p_xyzw_to_cells) { floats_to_cells(f, 4); }     /* xyzw>cells ( F: x y z w -- ) ( -- x y z w ) */
PRIM(p_cells_to_xyzw) { cells_to_floats(f, 4); }     /* cells>xyzw ( x y z w -- ) ( F: -- x y z w ) */

PRIM(p_errors_to) { K->errors_actor = (int)POP(); }   /* ( id -- ) failures told to it (nobody: none) */
PRIM(p_failure_actor) { PUSH(failure_arg(f)->actor); }   /* ( n -- id ) nobody: a task's */
PRIM(p_failure_frame) { PUSH(failure_arg(f)->frame); }
static void push_text(Forth *f, const char *s) { PUSH(s); PUSH(strlen(s)); }
PRIM(p_failure_who) { push_text(f, failure_arg(f)->who); }   /* ( n -- addr len ) */
PRIM(p_failure_what) { push_text(f, failure_arg(f)->what); }
PRIM(p_failure_error) { push_text(f, failure_arg(f)->error); }
PRIM(p_failure_chain) { push_text(f, failure_arg(f)->chain); }

void forth_actors(Forth *f, size_t body_bytes) {
    static const struct {
        const char *name;
        Code code;
        int immediate;
        const char *effect;   /* (NULL: not a fixed one) */
    } words[] = {
        {"message", p_message, 0, "( -- )"}, {"(send)", p_send_rt, 0, NULL}, {"(broadcast)", p_broadcast_rt, 0, NULL},
        {"send", p_send, 1, NULL}, {"broadcast", p_broadcast, 1, NULL},
        {"(subscribe)", p_subscribe_rt, 0, NULL}, {"(unsubscribe)", p_unsubscribe_rt, 0, NULL},
        {"subscribe", p_subscribe, 1, NULL}, {"unsubscribe", p_unsubscribe, 1, NULL},
        {"(await)", p_await_rt, 0, NULL}, {"(await-within)", p_await_within_rt, 0, NULL},
        {"await", p_await, 1, NULL}, {"await-within", p_await_within, 1, NULL},
        {"behaviour", p_behaviour, 0, NULL}, {"extends", p_extends, 0, NULL}, {"on", p_on, 0, NULL},
        {"end-behaviour", p_end_behaviour, 0, NULL}, {"become", p_become, 0, "( behaviour -- )"},
        {"state:", p_state, 0, NULL}, {"field", p_field, 0, NULL}, {"end-state", p_end_state, 0, NULL},
        {"spawn", p_spawn, 0, "( behaviour type addr len -- id )"}, {"kill", p_kill, 0, "( id -- )"},
        {"self", p_self, 0, "( -- id )"}, {"enter", p_enter, 0, "( id -- )"}, {"leave-actor", p_leave, 0, "( -- )"},
        {"sender", p_sender, 0, "( -- id )"}, {"alive?", p_alive, 0, "( id -- flag )"},
        {"actor-name", p_actor_name, 0, "( id -- addr len )"}, {"actor-named", p_actor_named, 0, "( addr len -- id )"},
        {"behaviour-of", p_behaviour_of, 0, "( id -- behaviour )"}, {"actor-busy?", p_busy, 0, "( id -- flag )"},
        {"actor-mail", p_mail, 0, "( id -- n )"}, {"monitor", p_monitor, 0, "( id -- )"},
        {"unmonitor", p_unmonitor, 0, "( id -- )"}, {"link", p_link, 0, "( id -- )"}, {"unlink", p_unlink, 0, "( id -- )"},
        {"actors-frame", p_frame, 0, "( -- )"}, {"deliver", p_deliver, 0, "( -- )"}, {"actors-reset", p_reset, 0, "( -- )"},
        {"trace-on", p_trace_on, 0, "( -- )"}, {"trace-off", p_trace_off, 0, "( -- )"}, {"queued", p_queued, 0, "( -- n )"},
        {"actors-frames", p_actor_frame, 0, "( -- n )"}, {".actors", p_dot_actors, 0, "( -- )"},
        {".unhandled", p_dot_unhandled, 0, "( -- )"}, {".state", p_dot_state, 0, "( id -- )"},
        {".recent", p_dot_recent, 0, "( n -- )"},
        {"f>cell", p_f_to_cell, 0, "( -- c ) ( F: x -- )"}, {"cell>f", p_cell_to_f, 0, "( c -- ) ( F: -- F: x )"},
        {"xyz>cells", p_xyz_to_cells, 0, "( -- x y z ) ( F: x y z -- )"},
        {"cells>xyz", p_cells_to_xyz, 0, "( x y z -- ) ( F: -- F: x y z )"},
        {"xyzw>cells", p_xyzw_to_cells, 0, "( -- x y z w ) ( F: x y z w -- )"},
        {"cells>xyzw", p_cells_to_xyzw, 0, "( x y z w -- ) ( F: -- F: x y z w )"},
        {"errors-to", p_errors_to, 0, "( id -- )"}, {"failure-actor", p_failure_actor, 0, "( n -- id )"},
        {"failure-frame", p_failure_frame, 0, "( n -- frame )"}, {"failure-who", p_failure_who, 0, "( n -- addr len )"},
        {"failure-what", p_failure_what, 0, "( n -- addr len )"}, {"failure-error", p_failure_error, 0, "( n -- addr len )"},
        {"failure-chain", p_failure_chain, 0, "( n -- addr len )"},
    };
    static const char *kernel_kinds[FORTH_KERNEL_KINDS] = {"tick", "frame-end", "spawned", "killed", "held-tick",
                                                           "actor-failed", "start-over", "died"};
    static const int kernel_nargs[FORTH_KERNEL_KINDS] = {0, 0, 0, 0, 0, 1, 0, 1};
    Vocab *saved = f->m.current, *v = forth_vocab(f, "actors");
    size_t i;

    if (K == NULL) {
        K = calloc(1, sizeof(*K));
        if (K == NULL) {
            forth_error(f, "actors: out of memory");
        }
        K->self = K->sender = K->errors_actor = NOBODY;
        for (i = 0; i < FORTH_KERNEL_KINDS; i++) {
            kind_new(f, kernel_kinds[i], strlen(kernel_kinds[i]), kernel_nargs[i]);
        }
    }
    K->body_bytes = body_bytes;
    forth_set_current(f, v);   /* scripts say USING: actors ; */
    v->state = VOCAB_LOADED;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        Word *wd = forth_prim(f, words[i].name, words[i].code);

        if (words[i].immediate) {
            wd->flags |= WORD_IMMEDIATE;
        }
        if (words[i].effect != NULL) {
            forth_set_effect(f, wd, words[i].effect);
        }
        if (words[i].code == p_send_rt || words[i].code == p_broadcast_rt || words[i].code == p_subscribe_rt ||
            words[i].code == p_unsubscribe_rt || words[i].code == p_await_rt || words[i].code == p_await_within_rt) {
            wd->flags |= WORD_EFFECT_OF_LITERAL;   /* (the message kind laid down before it says) */
        }
    }
    f->literal_effect = literal_effect;
    f->task_failed = task_failed;
    forth_constant(f, "nobody", NOBODY);
    forth_constant(f, "actors-max", FORTH_ACTORS_MAX);
    for (i = 0; i < FORTH_KERNEL_KINDS; i++) {
        forth_constant(f, kernel_kinds[i], (Cell)i);
    }
    forth_set_current(f, saved);
}

void forth_actors_free(Forth *f) {
    int i;

    if (K == NULL) {
        return;
    }
    forth_actors_reset(f);
    for (i = 0; i < K->nbehaviours; i++) {
        free(K->behaviours[i]);
    }
    for (i = 0; i < K->ntypes; i++) {
        free(K->types[i].fields);
    }
    free(K->queue);
    free(K);
    K = NULL;
}

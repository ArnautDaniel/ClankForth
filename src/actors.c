/* The actor kernel (actors.h). */
#include "actors.h"
#include "forth_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define K (f->actors)

#define ROUNDS_MAX 64           /* delivery rounds in one frame before the rest waits */
#define STATE_TYPES_MAX 128
#define BEHAVIOURS_MAX 256
#define HANDLER_TIMES_MAX 256   /* handlers .handler-times keeps (a behaviour and a kind each) */
#define MARK ((Cell)0x6B636142)   /* `behaviour` ... `end-behaviour`: what `on` left above it */
#define RECENT_MAX 256          /* the messages last delivered, kept for `.recent` (bug reports) */
#define FAILURES_KEPT 32        /* the latest failures, for the errors actor to read (a record's number names it) */
#define SAID_SLOTS_LOG2 10      /* 2^10 senders' last messages remembered at once, for `.recent` */
#define SAID_SLOTS (1 << SAID_SLOTS_LOG2)
#define FIELD_SPAN 0x10000      /* a field's cell: its state type's number times this, plus its offset - so a state is
                                 * under this many bytes */
#define GENERATIONS ((INT_MAX >> FORTH_ACTOR_SLOT_BITS) + 1)   /* times a slot is taken before its ids go round */
#define SLOT_MASK (FORTH_ACTOR_SLOTS_MAX - 1)
#define SPARE_KEPT 64           /* slots let go and kept before one is taken again (while the limit allows a new one):
                                 * each is taken again only after the 64 let go before it, so a stale id names nobody
                                 * for that much longer (at the limit one is taken at once) */
#define WORDS_OF(n) (((n) + 31) / 32)
#define OBJECT_SHOWN_BYTES 64   /* an object in a cell shown as what it is ("text", list(3)), at most this long */
#define NOBODY (-1)

typedef struct Kind {
    char name[FORTH_ACTOR_NAME_MAX + 1];
    int nargs;
    int quiet;   /* quiet-message: dropped without a word by an actor that doesn't handle it */
} Kind;

typedef struct Behaviour {
    char name[FORTH_ACTOR_NAME_MAX + 1];
    const Vocab *vocab;     /* where it was defined: the same name in another is another behaviour */
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
    const Vocab *vocab;     /* (as a behaviour's) */
    size_t size;
    Field *fields;          /* its fields, in order (for `.state`) */
    int nfields;
    Field *was;             /* (defined again: its fields as they were, till end-state moves its actors' states over) */
    int nwas;
    size_t was_size;
    int again;
} StateType;

typedef struct Message {
    int kind, to, n;
    Cell from;              /* its sender: an id, an actor's address on another node (nodes.c), -1 the engine */
    Cell a[FORTH_MSG_ARGS];
} Message;

/* actors - ids here, addresses on other nodes - an actor keeps: those watching it, those linked to it */
typedef struct Addresses {
    Cell *at;
    int n, cap;
} Addresses;

/* An actor's id is its place among the actors (its slot: under FORTH_ACTOR_SLOT_BITS bits) and, over that, how many
 * have had the place before it (its generation). So an id kept after its actor is gone names nobody - not whoever has
 * the place now - and the first to have a place has the slot for its id. The slots are made as they are needed, up to
 * the limit (forth_set_actors_max); one let go is taken again, the longest let go first. */
typedef struct Actor {
    int used;
    int id;
    Cell reason;            /* why it dies: the failure's record (its number), 0 killed */
    char name[FORTH_ACTOR_NAME_MAX + 1];
    Behaviour *beh;
    int type;               /* its state's type (-1: none) */
    uint8_t *state;
    uint8_t *body;          /* the host's (body_bytes) */
    long handled;           /* messages it has handled */
    int dying;              /* its `killed` handler (its last words) is running */
    int placing;            /* (spawn-anywhere's) asked another node to make an actor: the answer to come */
    long long handler_us;   /* its handler's time on the node so far, its turns together (.handler-times) */
    struct { Behaviour *beh; int type; char name[FORTH_ACTOR_NAME_MAX + 1]; } place;   /* ... made here if it can't */
    int doomed;             /* killed while a handler of its ran: gone when the handler returns */
    int running;            /* a handler of its is running */
    Task *task;             /* its handlers' stacks: one may wait in it */
    int region;             /* its objects' region (what its handlers rescue), round each handler's scratch region */
    int busy;               /* a handler of its waits (in its task): its messages kept till it ends */
    int kind;               /* the message that handler handles */
    Cell from;              /* (and who sent it) */
    int awaiting;           /* the kind its handler awaits (-1: none) */
    Cell await_until;       /* (await-within) the frame it stops waiting; -1: never */
    long long await_ms;     /* (await-within-ms) the clock's ms it stops waiting; 0: never */
    Message *mail;          /* kept for it while it was busy: oldest first, from mail_head */
    int mail_head, mail_n, mail_cap;
    Addresses watchers;     /* those monitoring it: told `died` */
    Addresses links;        /* those linked to it: killed with it */
    Addresses watching;     /* those on other nodes it monitors: told to stop as it goes (their lists don't grow) */
    uint32_t subs[WORDS_OF(FORTH_KINDS_MAX)];   /* the kinds it subscribes to */
    int generation;         /* its slot's: the actors that have had it and gone (kept as the slot is let go) */
} Actor;

/* a kind and its sender's last message (its arguments' hash): the same again isn't kept for `.recent` */
typedef struct Said {
    uint32_t who;           /* the kind and the sender as one number; 0: nobody's yet */
    uint32_t args;
} Said;

/* a handler's error or a task's, kept for the errors actor (errors-to) */
typedef struct Failure {
    long number;            /* 0: none */
    Cell actor;             /* the actor whose handler failed (an address: on another node); -1: a task */
    char who[FORTH_ACTOR_NAME_MAX + 6];   /* its name ("task <name>" for a task's) */
    char what[FORTH_ACTOR_NAME_MAX + 1];  /* the message it was handling ("" for a task's) */
    char error[256];
    char chain[1024];       /* the words running */
    Cell frame;
} Failure;

struct ForthActors {
    size_t body_bytes;
    Kind kinds[FORTH_KINDS_MAX];
    int nkinds;
    Behaviour *behaviours[BEHAVIOURS_MAX];
    int nbehaviours;
    StateType types[STATE_TYPES_MAX];
    int ntypes;
    Actor **actors;         /* by slot: each made as it is first needed, kept till the actors are reset */
    int nslots, slots_cap;
    int max;                /* the slots there may be (forth_set_actors_max) */
    int *spare;             /* the slots let go, the longest let go first (from spare_head); room for every slot */
    int spare_head, spare_n;
    Message *queue;
    int queue_n, queue_cap;
    int self;               /* whose message is being handled */
    Cell sender;            /* and who sent it (an address, from another node) */
    int delivering, trace;
    long slow_ms;           /* slow-handler-ms!: a handler's turn longer than this said (it held its node); 0 never */
    long long busy_us, load_at;   /* handlers' time since load_at (us), when the node's load was last worked out */
    struct HandlerTime { const Behaviour *beh; int kind; long runs; long long total_us, max_us; } times[HANDLER_TIMES_MAX];
    int ntimes;                   /* .handler-times: each behaviour's handler of a kind - its runs, time, longest */
    int load;                     /* the node's: thousandths of its time its handlers ran (smoothed) */
    int handlers;           /* handlers running now (an actor's last words too): no frame, delivery or reset inside */
    int *doomed;            /* those linked to one gone, to be killed after it (a worklist: no recursion) */
    int doomed_n, doomed_cap, killing;
    Cell frame;
    long dead_letters;      /* messages to actors no longer there */
    struct { Cell frame; Message m; } recent[RECENT_MAX];
    long nrecent;
    Said last_said[SAID_SLOTS];
    Failure failures[FAILURES_KEPT];
    long failures_told;
    int errors_actor;
    Behaviour *defining;    /* behaviour ... end-behaviour */
    int prompt_actor;       /* the prompt's (`attach`): each line it runs as this actor (NOBODY: none) */
};

static int slot_of(int id) { return id & SLOT_MASK; }   /* (of an id that is one: not negative) */

static Actor *actor_at(Forth *f, int id) {
    Actor *a = id >= 0 && slot_of(id) < K->nslots ? K->actors[slot_of(id)] : NULL;

    return a != NULL && a->used && a->id == id ? a : NULL;
}

static Actor *slot_actor(Forth *f, int id) { return K->actors[slot_of(id)]; }   /* (of an id whose slot is made) */

/* ---- addresses: who watches an actor, who is linked to it ---- */

static int remote(Cell address) { return address > INT_MAX; }   /* an actor's on another node */

/* an address as this node has it: one of its own actors' is that one's id */
static Cell here(Forth *f, Cell address) {
    return remote(address) && (address >> FORTH_NODE_SHIFT) == forth_node_of(f) ? address & FORTH_NODE_ID_MASK
                                                                                : address;
}

static int addresses_find(const Addresses *s, Cell x) {
    int i;

    for (i = 0; i < s->n && s->at[i] != x; i++) {
    }
    return i < s->n ? i : -1;
}

/* x among them (once); 0 if there is no memory for it. The actors here no longer alive are let go first, as the list
 * fills */
static int addresses_add(Forth *f, Addresses *s, Cell x) {
    int i, kept;

    if (addresses_find(s, x) >= 0) {
        return 1;
    }
    if (s->n == s->cap) {
        for (i = kept = 0; i < s->n; i++) {
            if (remote(s->at[i]) || actor_at(f, (int)s->at[i]) != NULL) {
                s->at[kept++] = s->at[i];
            }
        }
        s->n = kept;
    }
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 4;
        Cell *at = realloc(s->at, (size_t)cap * sizeof(Cell));

        if (at == NULL) {
            return 0;
        }
        s->at = at;
        s->cap = cap;
    }
    s->at[s->n++] = x;
    return 1;
}

static int addresses_remove(Addresses *s, Cell x) {   /* (was it there?) */
    int i = addresses_find(s, x);

    if (i < 0) {
        return 0;
    }
    s->at[i] = s->at[--s->n];
    return 1;
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
    K->kinds[K->nkinds].quiet = 0;
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

static void enqueue(Forth *f, Cell from, int to, int kind, const Cell *args, int n) {
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
        forth_objects_post(f, m->a, n);   /* (an object made in a scratch region: the message's now) */
    }
}

/* a message from C: a kind there is, and no more arguments than one carries (else said, and not sent) */
static int sendable(Forth *f, int kind, int n) {
    if (K != NULL && kind >= 0 && kind < K->nkinds && n >= 0 && n <= FORTH_MSG_ARGS) {
        return 1;
    }
    forth_printf(f, "actors: a message of kind %d with %d arguments, from C: not sent (no such kind, or too many)\n",
                 kind, n);
    return 0;
}

void forth_actor_send(Forth *f, int from, int to, int kind, const Cell *args, int n) {
    if (sendable(f, kind, n)) {
        enqueue(f, from, to, kind, args, n);
    }
}

static void broadcast(Forth *f, int from, int kind, const Cell *args, int n) {
    int slot;

    for (slot = 0; slot < K->nslots; slot++) {
        const Actor *a = K->actors[slot];

        if (a->used && bit(a->subs, kind)) {
            enqueue(f, from, a->id, kind, args, n);
        }
    }
}

void forth_actor_broadcast(Forth *f, int from, int kind, const Cell *args, int n) {
    if (sendable(f, kind, n)) {
        broadcast(f, from, kind, args, n);
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

/* recorded (its number given), and the errors actor told (actor-failed) - not of its own: those are only said */
static Cell report_failure(Forth *f, int actor, const char *who, const char *what) {
    Failure *r;
    Cell number = ++K->failures_told;

    r = &K->failures[number % FAILURES_KEPT];
    r->number = number;
    r->actor = actor;
    snprintf(r->who, sizeof(r->who), "%s", who);
    snprintf(r->what, sizeof(r->what), "%s", what);
    snprintf(r->error, sizeof(r->error), "%s", f->error);
    snprintf(r->chain, sizeof(r->chain), "%s", f->error_chain);
    r->frame = K->frame;
    if (actor_at(f, K->errors_actor) != NULL && actor != K->errors_actor) {
        enqueue(f, actor, K->errors_actor, FORTH_KIND_ACTOR_FAILED, &number, 1);
    }
    return number;
}

static void task_failed(Forth *f, Task *t) {
    char who[FORTH_ACTOR_NAME_MAX + 6];

    snprintf(who, sizeof(who), "task %.*s", FORTH_ACTOR_NAME_MAX, t->name);
    (void)report_failure(f, -1, who, "");
}

static Failure *failure_arg(Forth *f) {   /* ( number -- ) the record (an empty one once it is gone) */
    static _Thread_local Failure gone = {.actor = -1};
    Cell number = POP();
    Failure *r = &K->failures[(number > 0 ? number : 0) % FAILURES_KEPT];

    return number > 0 && r->number == number ? r : &gone;
}

/* ---- delivery ---- */

static const char *actor_name(Forth *f, Cell id) {
    static _Thread_local char elsewhere[48];
    Actor *a;

    if (id > INT_MAX) {   /* (an actor on another node: its address) */
        snprintf(elsewhere, sizeof(elsewhere), "node %ld:%ld", (long)(id >> FORTH_NODE_SHIFT),
                 (long)(id & FORTH_NODE_ID_MASK));
        return elsewhere;
    }
    a = actor_at(f, (int)id);
    return a != NULL ? a->name : id < 0 ? "engine" : "(gone)";
}

/* a cell as it reads best: a whole number, or a float (a field's cell holds either) */
static void print_cell(Forth *f, Cell c) {
    char object[OBJECT_SHOWN_BYTES];
    double d;

    memcpy(&d, &c, sizeof(d));
    if (forth_object_describe(f, c, object, sizeof(object))) {   /* (an object: what it is - "text", list(3) ...) */
        forth_printf(f, " %s", object);
    } else if (c > -0x10000000000LL && c < 0x10000000000LL) {
        forth_printf(f, " %ld", (long)c);
    } else if (d == d && d > -1e12 && d < 1e12) {
        forth_printf(f, " %ge", d);
    } else {
        forth_printf(f, " $%lX", (unsigned long)c);
    }
}

static void trace(Forth *f, const Message *m) {
    int i;

    forth_printf(f, "%lld  %s -> %s  %s", (long long)K->frame, actor_name(f, m->from), actor_name(f, m->to), K->kinds[m->kind].name);
    for (i = 0; i < m->n; i++) {
        forth_printf(f, " %ld", (long)m->a[i]);
    }
    forth_printf(f, "\n");
}

/* kept for `.recent`: a message unlike the last of its kind from its sender (so the every-frame
 * broadcasts show their changes, once, and not each copy to each subscriber) */
static void remember(Forth *f, const Message *m) {
    uint32_t h = 2166136261u, who;
    Said *last;
    int i;

    for (i = 0; i < m->n; i++) {   /* (FNV-1a over the arguments) */
        const uint8_t *b = (const uint8_t *)&m->a[i];
        size_t k;

        for (k = 0; k < sizeof(Cell); k++) {
            h = (h ^ b[k]) * 16777619u;
        }
    }
    /* (who: the kind and the sender - the engine 0 - as one number, never 0. Two that share a slot take it from each
     * other: at worst a message the same as the last is kept again) */
    who = (uint32_t)m->kind * (FORTH_ACTOR_SLOTS_MAX + 1u) + 1;
    if (m->from >= 0) {
        who += (uint32_t)(m->from & SLOT_MASK) + 1;
    }
    who += who == 0;
    last = &K->last_said[(who * 2654435761u) >> (32 - SAID_SLOTS_LOG2)];
    if (last->who == who && last->args == h) {
        return;
    }
    last->who = who;
    last->args = h;
    K->recent[K->nrecent % RECENT_MAX].frame = K->frame;
    K->recent[K->nrecent % RECENT_MAX].m = *m;
    K->nrecent++;
}

static void destroy(Forth *f, int id);
static void kill_actor(Forth *f, int id);

/* a handler of the actor's failed (the error said): recorded, and the errors actor told - it decides (`start-over`,
 * `kill`). With no errors actor nobody would put it right: the actor dies of it (its last words, then those linked
 * to it, those monitoring it told why). The errors actor's own failure is only said. */
static void handler_failed(Forth *f, int id, int kind) {
    Actor *a = slot_actor(f, id);
    Cell number = report_failure(f, id, a->name, K->kinds[kind].name);

    if (actor_at(f, K->errors_actor) != NULL || a->dying) {
        return;
    }
    forth_printf(f, "  (%s dies of it: no errors actor to tell - errors-to)\n", a->name);
    a->reason = number;
    if (a->running == 0) {   /* (the failed handler's stacks are nobody's now: its last words have them empty) */
        a->task->sp = a->task->rp = a->task->fp = a->task->lp = 0;
    }
    kill_actor(f, id);
}

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
#define SLOW_MS_DEFAULT 200   /* a handler's turn this long holds up every actor on its node: said */
#define MS_PER_S 1000
#define US_PER_MS 1000
#define NS_PER_US 1000
#define LOAD_WINDOW_US 250000   /* the node's load worked out this often (each the mean of it and the last) */
#define PER_MILLE 1000
static long long wall_us(void) {   /* (monotonic: a clock set meanwhile doesn't make a turn look long) */
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * MS_PER_S * US_PER_MS + ts.tv_nsec / NS_PER_US;
}
static long long wall_ms(void) { return wall_us() / US_PER_MS; }
/* a turn that held its node past slow_ms: said - every other actor on the node waited it out (an await ends a turn:
 * the wait itself isn't counted) */
static void slow_turn(Forth *f, const Actor *a, long long ms) {
    if (actor_at(f, K->errors_actor) != NULL && a->id != K->errors_actor) {   /* (told it: the app's log, beside its
                                                                              *  failures - the supervisor's a line) */
        Cell args[3] = {a->id, a->kind, (Cell)ms};

        enqueue(f, a->id, K->errors_actor, FORTH_KIND_ACTOR_SLOW, args, 3);
        return;
    }
    forth_printf(f, "slow: %s's `on %s` held its node %lld ms (the node's other actors waited: split it, or do it on "
                 "a node of its own - slow-handler-ms! says when this is said)\n",
                 a->name, K->kinds[a->kind].name, ms);
}
/* a handler done: its time on the node (its turns together) to its behaviour's and kind's line */
static void handler_timed(Forth *f, Actor *a) {
    struct HandlerTime *h = NULL;
    int i;

    for (i = 0; i < K->ntimes && h == NULL; i++) {
        if (K->times[i].beh == a->beh && K->times[i].kind == a->kind) {
            h = &K->times[i];
        }
    }
    if (h == NULL && K->ntimes < HANDLER_TIMES_MAX) {
        h = &K->times[K->ntimes++];
        *h = (struct HandlerTime){a->beh, a->kind, 0, 0, 0};
    }
    if (h != NULL) {
        h->runs++;
        h->total_us += a->handler_us;
        if (a->handler_us > h->max_us) {
            h->max_us = a->handler_us;
        }
    }
    a->handler_us = 0;
}
static void handler_turn(Forth *f, int id) {
    Actor *a = slot_actor(f, id);
    int self = K->self, r;
    Cell sender = K->sender;
    long long t0 = wall_us(), took;

    K->self = id;
    K->sender = a->from;
    a->running++;
    K->handlers++;
    f->next_slice = f->slice;   /* (sliced: it gives way when its steps are up, and goes on in a later turn) */
    r = forth_task_turn(f, a->task);
    f->next_slice = 0;          /* (a turn that didn't take it - its task done already - leaves it for none) */
    took = wall_us() - t0;
    K->busy_us += took;   /* (the node's load: its handlers' share of its time) */
    if (a->used && a->id == id) {
        a->handler_us += took;
    }
    if (K->slow_ms > 0 && a->used && a->id == id && took / US_PER_MS >= K->slow_ms) {
        slow_turn(f, a, took / US_PER_MS);
    }
    K->handlers--;
    a->running--;
    K->self = self;
    K->sender = sender;
    if (!a->used || a->id != id) {
        return;
    }
    if (r == FORTH_TURN_WAITING) {
        a->busy = 1;
    } else {
        a->placing = 0;   /* (its handler over: no answer from another node is its any more) */
        handler_timed(f, a);
        forth_regions_unwind(f, a->task, 0);   /* (what the handler made, and didn't rescue: freed) */
        a->busy = 0;
        a->awaiting = -1;
        a->handled++;
        if (r == FORTH_TURN_FAILED) {
            if (!f->quiet) {   /* (a repeat: nor where it was) */
                forth_printf(f, "  (in %s's `on %s`, behaviour %s)\n", a->name, K->kinds[a->kind].name, a->beh->name);
            }
            handler_failed(f, id, a->kind);
            if (!a->used || a->id != id) {   /* (it died of it) */
                return;
            }
        } else if (a->task->sp != 0 || a->task->fp != 0) {
            forth_printf(f, "%s's `on %s` left %d cells and %d floats on the stacks\n", a->name, K->kinds[a->kind].name,
                         a->task->sp, a->task->fp);
        }
    }
    if (a->doomed && a->running == 0) {   /* (killed meanwhile: a handler waiting is let go) */
        destroy(f, id);
    }
}

/* what a waiting handler goes on with, onto its task's stack: the message it awaited (n cells), and for an
 * await-within the flag over them. 0 if its stack hasn't the room: the handler is stopped there, a failure as any
 * other of its (said, and told to the errors actor) - not an error of whoever was delivering. */
static int give_awaited(Forth *f, int id, const Cell *args, int n, int flagged, Cell flag) {
    Actor *a = slot_actor(f, id);
    Task *t = a->task;
    int i;

    if (t->sp + n + (flagged ? 1 : 0) > STACK_CELLS) {
        snprintf(f->error, sizeof(f->error), "no room on its stack for what it awaited (%d cells there)", t->sp);
        f->error_chain[0] = 0;
        f->errors++;
        forth_printf(f, "\nerror: %s\n  (in %s's `on %s`, behaviour %s)\n", f->error, a->name, K->kinds[a->kind].name,
                     a->beh->name);
        t->state = TASK_DONE;
        forth_regions_unwind(f, t, 0);
        forth_objects_drop(f, args, n);
        a->busy = 0;
        a->awaiting = -1;
        a->handled++;
        handler_failed(f, id, a->kind);
        if (a->used && a->id == id && a->doomed && a->running == 0) {
            destroy(f, id);
        }
        return 0;
    }
    for (i = 0; i < n; i++) {
        t->ds[t->sp++] = args[i];
    }
    if (flagged) {
        t->ds[t->sp++] = flag;
    }
    forth_objects_take(f, t, args, n);   /* (into the region it waits in) */
    a->awaiting = -1;
    return 1;
}

/* a handler run now, inside whatever runs (an actor's last words, as it is killed): on its task's stacks, over
 * what they hold - it sees its arguments only, and can't wait */
static void handler_nested(Forth *f, int id, Word *h, const Message *m) {
    Actor *a = slot_actor(f, id);
    Task *t = a->task, *saved = f->t;
    int sp0 = t->sp, fp0 = t->fp, rp0 = t->rp, lp0 = t->lp, sfloor = t->sfloor, ffloor = t->ffloor, i, failed;
    int self = K->self, region = t->region;
    Cell sender = K->sender;

    t->sfloor = sp0;   /* (the handler sees its arguments, nothing of whoever delivered) */
    t->ffloor = fp0;
    f->t = t;
    t->region = a->region;   /* (a scratch region of its own, in its actor's, as it first makes an object) */
    forth_objects_take(f, t, m->a, m->n);
    for (i = 0; i < m->n; i++) {
        forth_push(f, m->a[i]);
    }
    K->self = m->to;
    K->sender = m->from;
    a->running++;
    K->handlers++;
    failed = forth_call(f, h) != 0;
    K->handlers--;
    a->running--;
    if (failed) {
        if (!f->quiet) {
            forth_printf(f, "  (in %s's `on %s`, behaviour %s)\n", a->name, K->kinds[m->kind].name, a->beh->name);
        }
    } else if (t->sp != sp0 || t->fp != fp0) {
        forth_printf(f, "%s's `on %s` left %d cells and %d floats on the stacks\n", a->name, K->kinds[m->kind].name,
                     t->sp - sp0, t->fp - fp0);
    }
    forth_regions_unwind(f, t, a->region);
    t->region = region;
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
    if (failed) {   /* (last: it may die of it, its task gone with it) */
        handler_failed(f, id, m->kind);
    }
}

/* a message delivered: to its actor's handler (begun in its task), or kept for it while it is busy */
static void dispatch(Forth *f, const Message *m, int from_mail) {
    Actor *a = actor_at(f, m->to);
    Word *h;
    int i;

    if (a == NULL) {
        K->dead_letters++;
        forth_objects_drop(f, m->a, m->n);
        return;
    }
    if (a->busy || (!from_mail && a->mail_n > 0)) {   /* (busy, or older ones kept: they go first) */
        if (a->busy && a->awaiting == m->kind) {   /* what its handler awaits: it goes on with it */
            if (K->trace) {
                trace(f, m);
            }
            remember(f, m);
            if (give_awaited(f, m->to, m->a, m->n, a->await_until >= 0 || a->await_ms != 0, -1)) {
                a->from = m->from;
                handler_turn(f, m->to);
            }
        } else if (!per_frame_kind(m->kind)) {
            mail_keep(f, a, m);
        } else {
            forth_objects_drop(f, m->a, m->n);
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

        if (b != NULL && m->kind >= FORTH_KERNEL_KINDS && !K->kinds[m->kind].quiet) {   /* (the kernel's own, a quiet one: ignored if unwanted) */
            b->unhandled++;
            if (!(b->reported[m->kind / 8] & (1 << (m->kind % 8)))) {
                b->reported[m->kind / 8] |= (uint8_t)(1 << (m->kind % 8));
                forth_printf(f, "%s (%s) doesn't handle %s\n", a->name, b->name, K->kinds[m->kind].name);
            }
        }
        forth_objects_drop(f, m->a, m->n);
        return;
    }
    if (a->running > 0) {   /* (a handler of its runs, further out: this one inside it) */
        handler_nested(f, m->to, h, m);
        return;
    }
    forth_task_start(f, a->task, h);
    a->task->region = a->region;   /* (its handler's objects: in a scratch region of their own, in the actor's) */
    forth_objects_take(f, a->task, m->a, m->n);
    for (i = 0; i < m->n; i++) {
        task_push(f, a->task, m->a[i]);
    }
    a->kind = m->kind;
    a->from = m->from;
    handler_turn(f, m->to);
}

/* the messages kept for actors no longer busy: each one's, oldest first, until it is busy again; any run? */
static int deliver_mail(Forth *f) {
    int slot, any = 0;

    for (slot = 0; slot < K->nslots; slot++) {
        Actor *a = K->actors[slot];

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
    forth_node_receive(f);   /* (what other nodes sent: queued, as sent here) */
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

/* the busy actors whose waits are over go on (their handlers' turns), in slot order */
int forth_actor_handler_task(Forth *f) {
    const Actor *a = K != NULL ? actor_at(f, K->self) : NULL;

    return a != NULL && f->t == a->task;
}
/* the busy actors whose waits are over go on - at a frame all of them; between frames (between: true) only those that
 * gave way, whose job is done or sleep over (one that yielded goes on at the next frame, as ever) */
static void go_on_now(Forth *f, int between) {
    int slot;

    if (f->tasks_paused) {   /* (as the tasks: the world stands still) */
        return;
    }
    K->delivering = 1;
    for (slot = 0; slot < K->nslots; slot++) {
        Actor *a = K->actors[slot];
        int id = a->id;

        if (!a->used || !a->busy) {
            continue;
        }
        if (a->awaiting >= 0) {
            if ((a->await_until < 0 || f->frame < a->await_until) && (a->await_ms == 0 || wall_ms() < a->await_ms)) {
                continue;   /* (neither its frames nor its ms up yet) */
            }
            if (give_awaited(f, id, NULL, 0, 1, 0)) {   /* (await-within: it didn't come) */
                handler_turn(f, id);
            }
        } else if (between ? forth_task_urgent(f, a->task) : forth_task_ready(f, a->task)) {
            handler_turn(f, id);
        }
    }
    K->delivering = 0;
}
static void go_on(Forth *f) { go_on_now(f, 0); }

void forth_actors_frame(Forth *f) {
    if (K->delivering || K->handlers > 0) {
        return;
    }
    go_on(f);
    broadcast(f, -1, FORTH_KIND_TICK, NULL, 0);
    deliver(f);
    broadcast(f, -1, FORTH_KIND_FRAME_END, NULL, 0);
    deliver(f);
    K->frame++;
}

void forth_actors_go_on(Forth *f) {
    if (K == NULL || K->delivering || K->handlers > 0) {
        return;
    }
    go_on_now(f, 1);
    deliver(f);
}
/* a loop's step (serve's, a node's): a frame - the tasks' turns and the actors' - once frame_ms has gone by since the
 * last (*next: when the next is due); between, what came delivered and handlers that gave way going on. How long the
 * loop may wait for what comes: none while a handler is ready to go on, else till the next frame */
/* the node's load: its handlers' share of the time since it was last worked out, the mean of that and the last -
 * told to the nodes' table, where least-busy-node reads it */
static void work_out_load(Forth *f) {
    long long now = wall_us(), span = now - K->load_at;

    if (K->load_at == 0) {
        K->load_at = now;
        K->busy_us = 0;
        return;
    }
    if (span < LOAD_WINDOW_US) {
        return;
    }
    K->load = (int)((K->load + (K->busy_us > span ? span : K->busy_us) * PER_MILLE / span) / 2);
    K->busy_us = 0;
    K->load_at = now;
    forth_node_set_load(f, K->load);
}
long forth_loop_step(Forth *f, long long *next, long frame_ms) {
    long long now = wall_ms(), wake;
    int slot;

    if (K != NULL) {
        work_out_load(f);
    }

    if (now >= *next) {
        forth_run_tasks(f);
        forth_actors_frame(f);
        *next = now + frame_ms;
    } else {
        forth_actors_go_on(f);
    }
    if (forth_actors_runnable(f)) {
        return 0;
    }
    now = wall_ms();
    wake = *next;
    for (slot = 0; K != NULL && slot < K->nslots; slot++) {   /* (a handler's sleep ending sooner: woken for it) */
        const Actor *a = K->actors[slot];
        long long w = !a->used || !a->busy ? 0 : a->awaiting >= 0 ? a->await_ms : forth_task_wake_ms(a->task);

        if (w != 0 && w < wake) {
            wake = w;
        }
    }
    return wake > now ? (long)(wake - now) : 0;
}
int forth_actors_runnable(Forth *f) {
    int slot;

    if (K == NULL || f->tasks_paused) {
        return 0;
    }
    if (K->queue_n > 0) {
        return 1;
    }
    for (slot = 0; slot < K->nslots; slot++) {
        Actor *a = K->actors[slot];

        if (a->used && a->busy && a->awaiting < 0 && forth_task_urgent(f, a->task)) {
            return 1;
        }
    }
    return 0;
}

void forth_actors_held_frame(Forth *f) {
    if (K->delivering || K->handlers > 0) {
        return;
    }
    go_on(f);
    broadcast(f, -1, FORTH_KIND_HELD_TICK, NULL, 0);
    deliver(f);
}

void *forth_actor_body(Forth *f, int id) {
    Actor *a = K != NULL ? actor_at(f, id) : NULL;

    return a != NULL ? a->body : NULL;
}

int forth_actor_self(Forth *f) { return K != NULL ? K->self : -1; }

int forth_actor_kind(Forth *f, const char *name) { return K != NULL ? kind_find(f, name, strlen(name)) : -1; }

int forth_actors_declare(Forth *f, const char *name, int nargs) {
    int k;

    if (K == NULL) {
        forth_error(f, "%s: the actor kernel isn't on (forth_actors)", name);
    }
    if ((k = kind_new(f, name, strlen(name), nargs)) < 0) {
        forth_error(f, "%s: too many message kinds", name);
    }
    return k;
}

int forth_actor_slot(int id) { return id >= 0 ? slot_of(id) : -1; }

int forth_actors_max(Forth *f) { return K != NULL ? K->max : 0; }

void forth_set_actors_max(Forth *f, int n) {
    if (K != NULL) {
        K->max = n < K->nslots ? K->nslots : n > FORTH_ACTOR_SLOTS_MAX ? FORTH_ACTOR_SLOTS_MAX : n;
    }
}

const char *forth_actor_name(Forth *f, int id) {
    Actor *a = K != NULL ? actor_at(f, id) : NULL;

    return a != NULL ? a->name : "";
}

static void forget(Forth *f, Actor *a) {   /* an actor's memory let go (its slot's generation kept) */
    int generation = a->generation, i;

    for (i = 0; i < a->mail_n; i++) {   /* (what its mail carried: nobody's now) */
        const Message *m = &a->mail[a->mail_head + i];

        forth_objects_drop(f, m->a, m->n);
    }
    free(a->state);
    free(a->body);
    free(a->mail);
    free(a->watchers.at);
    free(a->links.at);
    free(a->watching.at);
    if (a->task != NULL) {
        forth_task_free(f, a->task);   /* (its handler's region ended: then its own) */
    }
    forth_region_close(f, a->region);
    memset(a, 0, sizeof(*a));
    a->generation = generation;
}

/* those on other nodes watching an actor here, or linked to it, told it is gone (as it is forgotten without dying: the
 * actors reset, its node ending) */
static void tell_other_nodes(Forth *f, const Actor *a, Cell reason) {
    Cell me = 0;   /* (its address: asked only if one on another node is told - a Forth of no node joins none) */
    int i;

    for (i = 0; i < a->watchers.n; i++) {
        if (remote(a->watchers.at[i])) {
            me = me ? me : forth_node_address(f, a->id);
            forth_node_control(f, FORTH_NODE_DIED, me, a->watchers.at[i], reason, NULL, 0);
        }
    }
    for (i = 0; i < a->links.n; i++) {
        if (remote(a->links.at[i])) {
            me = me ? me : forth_node_address(f, a->id);
            forth_node_control(f, FORTH_NODE_EXIT, me, a->links.at[i], reason, NULL, 0);
        }
    }
    for (i = 0; i < a->watching.n; i++) {
        if (remote(a->watching.at[i])) {
            me = me ? me : forth_node_address(f, a->id);
            forth_node_control(f, FORTH_NODE_UNMONITOR, me, a->watching.at[i], 0, NULL, 0);
        }
    }
}

void forth_actors_reset(Forth *f) {
    int slot, keep = f->node != 0;   /* (a node's: other nodes may hold its actors' addresses - not given again) */

    if (K->delivering || K->handlers > 0) {
        forth_printf(f, "actors: not reset from inside a handler or a delivery\n");
        return;
    }
    K->spare_head = K->spare_n = 0;
    for (slot = 0; slot < K->nslots; slot++) {
        Actor *a = K->actors[slot];

        if (a->used) {
            tell_other_nodes(f, a, FORTH_NO_ACTOR);
        }
        forget(f, a);
        if (keep) {
            a->generation = (a->generation + 1) % GENERATIONS;
            K->spare[K->spare_n++] = slot;
        } else {
            free(a);   /* (the world anew: its ids from the first again) */
        }
    }
    if (!keep) {
        K->nslots = 0;
    }
    K->doomed_n = K->killing = 0;
    K->nrecent = 0;   /* (what they said, and their failures: of actors gone - their ids another's now) */
    memset(K->last_said, 0, sizeof(K->last_said));
    memset(K->failures, 0, sizeof(K->failures));
    for (slot = 0; slot < K->queue_n; slot++) {   /* (what the queue carried: nobody's now) */
        forth_objects_drop(f, K->queue[slot].a, K->queue[slot].n);
    }
    K->queue_n = 0;
    K->self = K->sender = -1;
    K->frame = 0;
    K->dead_letters = 0;
    K->errors_actor = NOBODY;
    K->prompt_actor = NOBODY;
}

/* ---- the words ---- */

static Actor *live(Forth *f, Cell id, const char *what) {
    Actor *a = id >= 0 && id <= INT_MAX ? actor_at(f, (int)id) : NULL;

    if (a == NULL) {
        forth_error(f, "%s: no actor %ld", what, (long)id);
    }
    return a;
}

static Actor *me(Forth *f, const char *what) {
    Actor *a = actor_at(f, K->self);

    if (a == NULL) {
        forth_error(f, "%s: no actor is being run", what);
    }
    return a;
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
/* quiet-message ( "name" -- ) the message kind dropped without a word by an actor that doesn't handle it - an answer
 * that may come after its asker stopped waiting for it */
PRIM(p_quiet_message) { K->kinds[parse_kind(f, "quiet-message")].quiet = 1; }
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
static int pop_args(Forth *f, int kind, Cell *args) {
    int n = K->kinds[kind].nargs, i;

    for (i = n - 1; i >= 0; i--) {
        args[i] = POP();
    }
    return n;
}
PRIM(p_send_rt) {   /* (send) ( args to kind -- ) */
    int kind = kind_arg(f, POP(), "send"), n;
    Cell to = POP(), args[FORTH_MSG_ARGS];

    n = pop_args(f, kind, args);
    if (to < 0) {   /* to nobody (the engine as `sender`, a name `actor-named` didn't find): a dead letter */
        K->dead_letters++;
        return;
    }
    to = here(f, to);
    if (remote(to)) {   /* an actor on another node: copied there (its node ended, a dead letter) */
        if (!forth_node_send(f, forth_node_address(f, K->self), to, K->kinds[kind].name, args, n)) {
            K->dead_letters++;
        }
        return;
    }
    live(f, to, "send");   /* (to one gone: an error now, where it is sent) */
    enqueue(f, K->self, (int)to, kind, args, n);
}
PRIM(p_broadcast_rt) {   /* (broadcast) ( args kind -- ) */
    int kind = kind_arg(f, POP(), "broadcast"), n;
    Cell args[FORTH_MSG_ARGS];

    n = pop_args(f, kind, args);
    broadcast(f, K->self, kind, args, n);
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
    int kind = kind_arg(f, POP(), "subscribe");

    set_bit(live(f, POP(), "subscribe")->subs, kind);
}
PRIM(p_unsubscribe_rt) {
    int kind = kind_arg(f, POP(), "unsubscribe");

    clear_bit(live(f, POP(), "unsubscribe")->subs, kind);
}
PRIM(p_subscribe) { parsing(f, "subscribe", "actors:(subscribe)"); }
PRIM(p_unsubscribe) { parsing(f, "unsubscribe", "actors:(unsubscribe)"); }

/* the first message of that kind its mail kept (it came while the handler ran, before this await): taken out, its
 * arguments given as a message awaited is - 0 if none is there */
static int await_kept(Forth *f, Actor *a, int kind, int flagged) {
    Task *t = a->task;
    Message m;
    int i, k;

    for (i = 0; i < a->mail_n && a->mail[a->mail_head + i].kind != kind; i++) {
    }
    if (i == a->mail_n) {
        return 0;
    }
    m = a->mail[a->mail_head + i];
    if (t->sp + m.n + (flagged ? 1 : 0) > STACK_CELLS) {
        forth_error(f, "await: no room on the stack for what it awaited (%d cells there)", t->sp);
    }
    memmove(&a->mail[a->mail_head + i], &a->mail[a->mail_head + i + 1], (size_t)(a->mail_n - i - 1) * sizeof(Message));
    a->mail_n--;
    remember(f, &m);
    for (k = 0; k < m.n; k++) {
        t->ds[t->sp++] = m.a[k];
    }
    if (flagged) {
        t->ds[t->sp++] = -1;
    }
    forth_objects_take(f, t, m.a, m.n);   /* (into the region it runs in) */
    a->from = m.from;
    return 1;
}

/* await kind ( -- args ): inside a handler, it waits until a message of that kind comes for its actor (others kept
 * for it meanwhile), then goes on with its arguments; `frames await-within kind ( -- args true | false )` gives up
 * after so many frames */
static void await_now(Forth *f, int kind, Cell until, long long until_ms) {
    Actor *a = actor_at(f, K->self);

    if (a == NULL || f->t != a->task || f->depth != f->task_depth) {
        forth_error(f, "await: only in a handler, as its actor runs it (not its last words, nor from C)");
    }
    if (await_kept(f, a, kind, until >= 0 || until_ms != 0)) {   /* (one came while it ran, and was kept: no wait) */
        return;
    }
    a->awaiting = kind;
    a->await_until = until;
    a->await_ms = until_ms;
    f->yielded = 1;
    forth_native_suspend(f);   /* (a coroutine: its frames kept till the message comes) */
}
PRIM(p_await_rt) { await_now(f, kind_arg(f, POP(), "await"), -1, 0); }   /* (await) ( kind -- ) */
PRIM(p_await_within_rt) {   /* (await-within) ( frames kind -- ) */
    int kind = kind_arg(f, POP(), "await-within");
    Cell frames = POP();

    await_now(f, kind, forth_frame_after(f, frames), 0);
}
PRIM(p_await_within_ms_rt) {   /* (await-within-ms) ( ms kind -- ) the clock's, not frames: a server's timeout */
    int kind = kind_arg(f, POP(), "await-within-ms");
    Cell ms = POP();

    await_now(f, kind, -1, wall_ms() + (ms > 0 ? ms : 0) + 1);   /* (+1: never 0, which is none) */
}
PRIM(p_await) { parsing(f, "await", "actors:(await)"); }
PRIM(p_await_within) { parsing(f, "await-within", "actors:(await-within)"); }
PRIM(p_await_within_ms) { parsing(f, "await-within-ms", "actors:(await-within-ms)"); }

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
    } else if (w->code == p_await_within_rt || w->code == p_await_within_ms_rt) {
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
    for (i = 0; i < K->nbehaviours; i++) {   /* (defined again in its vocabulary: a script reloaded - start it afresh) */
        if (strcmp(K->behaviours[i]->name, cname) == 0 && K->behaviours[i]->vocab == forth_current(f)) {
            b = K->behaviours[i];
            memset(b, 0, sizeof(*b));
            snprintf(b->name, sizeof(b->name), "%s", cname);
            b->vocab = forth_current(f);
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
        b->vocab = forth_current(f);
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
    Behaviour *parent, *b;

    if (K->defining == NULL) {
        forth_error(f, "extends: only inside behaviour ... end-behaviour");
    }
    if (pw == NULL) {
        forth_error(f, "extends: no behaviour %.*s", (int)len, name);
    }
    forth_execute(f, pw);
    parent = behaviour_arg(f, POP(), "extends");
    for (b = parent; b != NULL; b = b->parent) {   /* (handler_of walks the chain: it must end) */
        if (b == K->defining) {
            forth_error(f, "extends: %s would extend itself (%s extends it already)", K->defining->name,
                        parent->name);
        }
    }
    K->defining->parent = parent;
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
        K->defining->on[kind_arg(f, POP(), "end-behaviour")] = forth_xt_arg(f, x, "end-behaviour");
    }
    K->defining = NULL;
}
PRIM(p_become) { me(f, "become")->beh = behaviour_arg(f, POP(), "become"); }

/* state: name ... end-state; field ( type offset size "name" -- type offset' ) */
static void dofield(Forth *f, Word *w) {
    Cell x = w->body[0];   /* (its type, offset and size: (type * FIELD_SPAN + offset) * FIELD_SPAN + size) */
    int size = (int)(x % FIELD_SPAN), off = (int)(x / FIELD_SPAN % FIELD_SPAN), type = (int)(x / FIELD_SPAN / FIELD_SPAN);
    Actor *a = actor_at(f, K->self);

    if (a == NULL) {
        forth_error(f, "%s: a field, but no actor is being run", w->name);
    }
    if (a->type != type) {
        forth_error(f, "%s: a field of %s, but %s's state is %s", w->name, K->types[type].name, a->name,
                    a->type >= 0 ? K->types[a->type].name : "nothing");
    }
    if ((size_t)(off + size) > K->types[type].size) {   /* (compiled before its state was defined again without it) */
        forth_error(f, "%s: a field of %s past its state's %zu bytes (the state defined again since)", w->name,
                    K->types[type].name, K->types[type].size);
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
    for (t = 0; t < K->ntypes && (strcmp(K->types[t].name, cname) != 0 || K->types[t].vocab != forth_current(f)); t++) {
    }
    if (t == K->ntypes) {   /* (a new one; or another vocabulary's of that name: its own) */
        if (K->ntypes == STATE_TYPES_MAX) {
            forth_error(f, "state: too many types");
        }
        K->types[K->ntypes].vocab = forth_current(f);
        snprintf(K->types[K->ntypes++].name, sizeof(K->types[0].name), "%s", cname);
    } else {   /* (defined again - a reload: its fields anew, the old kept for moving its actors' states over) */
        StateType *st = &K->types[t];

        free(st->was);
        st->was = st->fields;
        st->nwas = st->nfields;
        st->was_size = st->size;
        st->fields = NULL;
        st->nfields = 0;
        st->again = 1;
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
    if (off < 0 || off >= FIELD_SPAN || size < 0 || size >= FIELD_SPAN) {
        forth_error(f, "field %.*s: its size and its state's are under %d bytes", (int)len, name, FIELD_SPAN);
    }
    off = (off + 7) & ~(Cell)7;   /* (every field 8-aligned: cells and floats) */
    if (off + size > FIELD_SPAN) {
        forth_error(f, "field %.*s: a state is at most %d bytes", (int)len, name, FIELD_SPAN);
    }
    snprintf(cname, sizeof(cname), "%.*s", (int)len, name);
    fw = forth_constant(f, cname, (type * FIELD_SPAN + off) * FIELD_SPAN + size);
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
static int same_fields(const StateType *t) {   /* defined again as it was? */
    int i;

    if (t->nfields != t->nwas || t->size != t->was_size) {
        return 0;
    }
    for (i = 0; i < t->nfields; i++) {
        if (strcmp(t->fields[i].name, t->was[i].name) != 0 || t->fields[i].off != t->was[i].off ||
            t->fields[i].size != t->was[i].size) {
            return 0;
        }
    }
    return 1;
}

/* a state type defined again: each live actor's state made anew, each field's bytes moved to where the field is now
 * (by its name: as many as both have room for), a field new to it zeroed, one gone let go */
static void move_states(Forth *f, int type) {
    StateType *t = &K->types[type];
    int slot, i, k;

    for (slot = 0; slot < K->nslots; slot++) {
        Actor *a = K->actors[slot];
        uint8_t *state;

        if (!a->used || a->type != type) {
            continue;
        }
        if ((state = calloc(1, t->size > 0 ? t->size : 8)) == NULL) {
            forth_error(f, "end-state: out of memory for %s's state", a->name);
        }
        for (i = 0; i < t->nfields; i++) {
            for (k = 0; k < t->nwas && strcmp(t->was[k].name, t->fields[i].name) != 0; k++) {
            }
            if (k < t->nwas) {
                int n = t->fields[i].size < t->was[k].size ? t->fields[i].size : t->was[k].size;

                memcpy(state + t->fields[i].off, a->state + t->was[k].off, (size_t)n);
            }
        }
        free(a->state);
        a->state = state;
    }
}

PRIM(p_end_state) {
    Cell size = POP(), type = POP();
    StateType *t;

    if (type < 0 || type >= K->ntypes || size < 0 || size > FIELD_SPAN) {
        forth_error(f, "end-state without state:");
    }
    t = &K->types[type];
    t->size = (size_t)size;
    if (t->again) {   /* (defined again: its live actors' states moved over, if it changed) */
        if (!same_fields(t)) {
            move_states(f, (int)type);
        }
        free(t->was);
        t->was = NULL;
        t->nwas = 0;
        t->again = 0;
    }
}

/* a slot for a new actor (its Actor cleared, but for its generation): one let go - the longest let go - once
 * SPARE_KEPT are kept or no more may be made; else a new one. -1 if there is none (why: what to say) */
static int slot_new(Forth *f, const char **why) {
    int slot;

    if (K->spare_n > 0 && (K->spare_n >= SPARE_KEPT || K->nslots >= K->max)) {
        slot = K->spare[K->spare_head++];
        if (--K->spare_n == 0) {
            K->spare_head = 0;
        }
        return slot;
    }
    if (K->nslots >= K->max) {
        *why = "too many actors alive (the limit: actors-max)";
        return -1;
    }
    if (K->nslots == K->slots_cap) {   /* (the table, and room to keep every slot let go: none lost in destroy) */
        int cap = K->slots_cap ? K->slots_cap * 2 : 64;
        Actor **actors = realloc(K->actors, (size_t)cap * sizeof(Actor *));
        int *spare;

        if (actors == NULL) {
            *why = "out of memory";
            return -1;
        }
        K->actors = actors;
        if ((spare = realloc(K->spare, (size_t)cap * sizeof(int))) == NULL) {
            *why = "out of memory";
            return -1;
        }
        K->spare = spare;
        K->slots_cap = cap;
    }
    if ((K->actors[K->nslots] = calloc(1, sizeof(Actor))) == NULL) {
        *why = "out of memory";
        return -1;
    }
    return K->nslots++;
}

static void slot_let_go(Forth *f, int slot) {   /* (to the end of the spare: room for it, slots_cap of them) */
    if (K->spare_head + K->spare_n == K->slots_cap) {
        memmove(K->spare, K->spare + K->spare_head, (size_t)K->spare_n * sizeof(int));
        K->spare_head = 0;
    }
    K->spare[K->spare_head + K->spare_n++] = slot;
}

/* a new actor, its state zeroed: its id - or -1, why said in *why (an error here would leave from C's loop) */
static int spawn_actor(Forth *f, Behaviour *b, int type, const char *name, size_t name_len, const char **why) {
    Actor *a;
    Task *task;
    int slot;

    if (type < -1 || type >= K->ntypes) {
        *why = "no such state type";
        return -1;
    }
    if ((task = forth_task_new(f, "actor")) == NULL) {
        *why = "out of memory";
        return -1;
    }
    if ((slot = slot_new(f, why)) < 0) {
        forth_task_free(f, task);
        return -1;
    }
    a = K->actors[slot];
    a->task = task;
    snprintf(task->name, sizeof(task->name), "%.*s", (int)(name_len < sizeof(task->name) ? name_len
                                                                                        : sizeof(task->name) - 1), name);
    a->id = a->generation << FORTH_ACTOR_SLOT_BITS | slot;
    snprintf(a->name, sizeof(a->name), "%.*s", (int)(name_len < sizeof(a->name) ? name_len : sizeof(a->name) - 1),
             name);
    a->beh = b;
    a->type = type;
    a->awaiting = -1;
    a->placing = 0;
    a->handler_us = 0;   /* (a slot one killed mid-handler held: none of its time is this one's) */
    a->region = forth_region_lasting(f);
    a->state = calloc(1, type >= 0 && K->types[type].size > 0 ? K->types[type].size : 8);
    a->body = calloc(1, K->body_bytes > 0 ? K->body_bytes : 8);
    if (a->state == NULL || a->body == NULL) {
        forget(f, a);
        slot_let_go(f, slot);
        *why = "out of memory";
        return -1;
    }
    a->used = 1;
    enqueue(f, K->self, a->id, FORTH_KIND_SPAWNED, NULL, 0);
    return a->id;
}

/* spawn ( behaviour type addr len -- id ): a new actor, its state zeroed */
PRIM(p_spawn) {
    Cell len = POP(), addr = POP(), type = POP();
    Behaviour *b = behaviour_arg(f, POP(), "spawn");
    size_t name_len;
    const char *name = forth_string_arg(f, addr, len, "spawn", &name_len), *why = "";
    int id;

    if (type < -1 || type >= K->ntypes) {
        forth_error(f, "spawn: no state type %ld", (long)type);
    }
    if ((id = spawn_actor(f, b, (int)type, name, name_len, &why)) < 0) {
        forth_error(f, "spawn: %s", why);
    }
    PUSH(id);
}

/* ---- an actor made on another node (spawn-on, spawn-anywhere): its behaviour and state type there by name - the
 * same vocabulary loaded there, its words its own ---- */
static Behaviour *behaviour_named(Forth *f, const char *vocab, const char *name) {
    int i;

    for (i = 0; i < K->nbehaviours; i++) {
        const Behaviour *b = K->behaviours[i];

        if (strcmp(b->name, name) == 0 && b->vocab != NULL && strcmp(b->vocab->name, vocab) == 0) {
            return K->behaviours[i];
        }
    }
    return NULL;
}
static int type_named(Forth *f, const char *vocab, const char *name) {   /* (-1: none - "" its name - or -2 no such) */
    int i;

    if (name[0] == 0) {
        return -1;
    }
    for (i = 0; i < K->ntypes; i++) {
        if (strcmp(K->types[i].name, name) == 0 && K->types[i].vocab != NULL &&
            strcmp(K->types[i].vocab->name, vocab) == 0) {
            return i;
        }
    }
    return -2;
}
#define PLACE_TEXT_MAX (4 * (VOCAB_NAME_MAX + 1) + FORTH_ACTOR_NAME_MAX + 1)
/* what another node needs to make it: the behaviour's vocabulary and name, the state type's, the actor's name -
 * each ended by a 0 */
static size_t place_text(Forth *f, char *out, const Behaviour *b, int type, const char *name, size_t name_len) {
    const char *tv = type >= 0 ? K->types[type].vocab->name : "", *tn = type >= 0 ? K->types[type].name : "";
    int n = snprintf(out, PLACE_TEXT_MAX, "%s%c%s%c%s%c%s%c%.*s", b->vocab->name, 0, b->name, 0, tv, 0, tn, 0,
                     (int)(name_len < FORTH_ACTOR_NAME_MAX ? name_len : FORTH_ACTOR_NAME_MAX), name);

    return n > 0 && n < PLACE_TEXT_MAX ? (size_t)n + 1 : 0;
}
/* (FORTH_NODE_SPAWN, on the node asked) made here - or not, its behaviour or type not here - and the asker told */
static void spawn_asked(Forth *f, Cell from, const void *extra, size_t n) {
    const char *t[5], *at = extra, *end = at + n, *why = "";
    Cell placed = NOBODY;
    int i, type, id;
    Behaviour *b;

    for (i = 0; i < 5; i++) {
        const char *z = at < end ? memchr(at, 0, (size_t)(end - at)) : NULL;

        if (z == NULL) {
            return;   /* (not one of ours) */
        }
        t[i] = at;
        at = z + 1;
    }
    b = behaviour_named(f, t[0], t[1]);
    type = type_named(f, t[2], t[3]);
    if (b != NULL && type >= -1 && (id = spawn_actor(f, b, type, t[4], strlen(t[4]), &why)) >= 0) {
        placed = forth_node_address(f, id);
    }
    forth_node_send(f, forth_node_address(f, 0), from, "placed", &placed, 1);
}

/* (the answer on the stack) spawn-anywhere's, nobody - the other node hadn't its behaviour: made here instead */
static void placed(Forth *f, Actor *a) {
    int anywhere = a->placing == 2;
    const char *why = "";
    int id;

    a->placing = 0;
    if (anywhere && f->t->sp > f->t->sfloor && f->t->ds[f->t->sp - 1] == NOBODY) {
        (void)POP();
        if ((id = spawn_actor(f, a->place.beh, a->place.type, a->place.name, strlen(a->place.name), &why)) < 0) {
            forth_error(f, "spawn-anywhere: %s", why);
        }
        PUSH(forth_node_address(f, id));
    }
}
#define PLACE_MARGIN 100   /* spawn-anywhere: another node only if it is a tenth of its time less busy than this one */
/* the asker's side: another node asked to make it, the answer awaited (in a handler); the address - or, made there
 * failing and anywhere set, made here. Threaded, the primitive runs again with the answer (a->placing). */
static void place(Forth *f, const Word *w, Behaviour *b, int type, const char *name, size_t name_len, int node,
                  int anywhere) {
    Actor *a = actor_at(f, K->self);
    char text[PLACE_TEXT_MAX];
    size_t n;
    const char *why = "";
    int id;

    if (node == 0 || node == forth_node_of(f)) {   /* (here: at once) */
        if ((id = spawn_actor(f, b, type, name, name_len, &why)) < 0) {
            forth_error(f, "spawn: %s", why);
        }
        PUSH(forth_node_address(f, id));
        return;
    }
    if (a == NULL || f->t != a->task || f->depth != f->task_depth) {
        forth_error(f, "spawn-on: only in a handler (it awaits the other node's answer)");
    }
    if (!forth_native_in_coroutine(f) && (f->t->ip == NULL || (const Word *)f->t->ip[-1] != w)) {
        forth_error(f, "spawn-on: another node's called directly, not through execute (threaded, it goes on there)");
    }
    if ((n = place_text(f, text, b, type, name, name_len)) == 0) {
        forth_error(f, "spawn-on: a name too long");
    }
    a->placing = anywhere ? 2 : 1;
    a->place.beh = b;
    a->place.type = type;
    snprintf(a->place.name, sizeof(a->place.name), "%.*s", (int)(name_len < sizeof(a->place.name) ? name_len
                                                                 : sizeof(a->place.name) - 1), name);
    if (!forth_node_control(f, FORTH_NODE_SPAWN, forth_node_address(f, K->self), (Cell)((UCell)node << FORTH_NODE_SHIFT),
                            0, text, n)) {
        PUSH(NOBODY);   /* (no such node, or ended) */
    } else {
        await_now(f, FORTH_KIND_PLACED, -1, 0);
        if (f->yielded) {   /* (threaded: this primitive again, the answer on the stack) */
            f->t->ip--;
            return;
        }
    }
    placed(f, a);
}
/* ( behaviour type addr len -- ) the arguments a spawn takes, checked */
static Behaviour *spawn_args(Forth *f, int *type, const char **name, size_t *name_len, const char *word) {
    Cell len = POP(), addr = POP(), t = POP();
    Behaviour *b = behaviour_arg(f, POP(), word);

    if (t < -1 || t >= K->ntypes) {
        forth_error(f, "%s: no state type %ld", word, (long)t);
    }
    *type = (int)t;
    *name = forth_string_arg(f, addr, len, word, name_len);
    return b;
}
/* the running actor's answer from the other node, come as its primitive runs again (threaded); 1 if it was that */
static int placing_answered(Forth *f) {
    Actor *a = actor_at(f, K->self);

    if (a == NULL || f->t != a->task || a->placing == 0 || f->depth != f->task_depth) {
        return 0;
    }
    placed(f, a);
    return 1;
}
/* spawn-on ( behaviour type addr len node -- addr ) a new actor on that node (its vocabulary loaded there): its
 * address, or nobody (the behaviour or state type not there, the node gone). Another node's: in a handler. */
PRIM(p_spawn_on) {
    Cell node;
    int type;
    const char *name;
    size_t name_len;
    Behaviour *b;

    if (placing_answered(f)) {
        return;
    }
    node = POP();
    b = spawn_args(f, &type, &name, &name_len, "spawn-on");
    place(f, w, b, type, name, name_len, (int)node, 0);
}
/* spawn-anywhere ( behaviour type addr len -- addr ) a new actor on the least busy node (this one, unless another is
 * clearly less) - here, if it can't be made there */
PRIM(p_spawn_anywhere) {
    int type;
    const char *name;
    size_t name_len;
    Behaviour *b;

    if (placing_answered(f)) {
        return;
    }
    b = spawn_args(f, &type, &name, &name_len, "spawn-anywhere");
    place(f, w, b, type, name, name_len, forth_node_least_busy(f, PLACE_MARGIN), 1);
}
/* a failure's record as another node has it: the record carried to it (FORTH_NODE_DIED, FORTH_NODE_EXIT) */
static void carry_reason(Forth *f, Cell reason, const void **extra, size_t *n) {
    const Failure *r = &K->failures[(reason > 0 ? reason : 0) % FAILURES_KEPT];

    *extra = reason > 0 && r->number == reason ? r : NULL;
    *n = *extra != NULL ? sizeof(Failure) : 0;
}

/* one linked to an actor gone: killed after it (destroy's worklist; with no room for it, at once) */
static void doom(Forth *f, int id) {
    if (K->doomed_n == K->doomed_cap) {
        int cap = K->doomed_cap ? K->doomed_cap * 2 : 16;
        int *doomed = realloc(K->doomed, (size_t)cap * sizeof(int));

        if (doomed == NULL) {
            kill_actor(f, id);
            return;
        }
        K->doomed = doomed;
        K->doomed_cap = cap;
    }
    K->doomed[K->doomed_n++] = id;
}

/* gone: those monitoring it told, those linked to it killed after it - here, or on their nodes */
static void destroy(Forth *f, int id) {
    int slot = slot_of(id), k;
    Actor *a = slot_actor(f, id);
    Addresses watchers = a->watchers, links = a->links;
    Cell reason = a->reason, died[2] = {id, a->reason};   /* died ( id reason -- ) */
    const void *record;
    size_t record_n;

    carry_reason(f, reason, &record, &record_n);
    a->watchers = a->links = (Addresses){NULL, 0, 0};   /* (its lists, these now: let go at the end) */
    for (k = 0; k < watchers.n; k++) {
        Cell w = watchers.at[k];

        if (remote(w)) {
            forth_node_control(f, FORTH_NODE_DIED, forth_node_address(f, id), w, reason, record, record_n);
        } else if (w != id && actor_at(f, (int)w) != NULL) {
            enqueue(f, id, (int)w, FORTH_KIND_DIED, died, 2);
            addresses_remove(&actor_at(f, (int)w)->watching, id);
        }
    }
    for (k = 0; k < a->watching.n; k++) {   /* (what it watched: its name off their lists - an id comes round again) */
        Cell t = a->watching.at[k];
        Actor *b;

        if (remote(t)) {
            forth_node_control(f, FORTH_NODE_UNMONITOR, forth_node_address(f, id), t, 0, NULL, 0);
        } else if ((b = actor_at(f, (int)t)) != NULL) {
            addresses_remove(&b->watchers, id);
        }
    }
    forget(f, a);
    /* (the next to have the slot: another id - so what is still on its way to this one is a dead letter) */
    a->generation = (a->generation + 1) % GENERATIONS;
    slot_let_go(f, slot);
    if (K->errors_actor == id) {
        K->errors_actor = NOBODY;
    }
    for (k = 0; k < links.n; k++) {
        Cell l = links.at[k];
        Actor *b;

        if (remote(l)) {
            forth_node_control(f, FORTH_NODE_EXIT, forth_node_address(f, id), l, reason, record, record_n);
        } else if ((b = actor_at(f, (int)l)) != NULL) {
            addresses_remove(&b->links, id);
            if (!b->dying) {
                b->reason = reason;   /* (those linked to it die of what it died of) */
                doom(f, b->id);
            }
        }
    }
    free(watchers.at);
    free(links.at);
    if (!K->killing) {   /* (those doomed killed in turn, here: a long chain of links is a loop, not recursion) */
        K->killing = 1;
        while (K->doomed_n > 0) {
            int next = K->doomed[--K->doomed_n];

            if (actor_at(f, next) != NULL) {
                kill_actor(f, next);
            }
        }
        K->killing = 0;
    }
}

/* kill: the actor's `killed` handler first (its last words: it lets go of what it holds), then it's gone - after
 * its handler running, if one runs (it killed itself), or let go of one waiting */
static void kill_actor(Forth *f, int id) {
    Actor *a = slot_actor(f, id);
    Word *h;

    if (a->dying) {
        return;
    }
    a->dying = 1;
    if ((h = handler_of(a, FORTH_KIND_KILLED)) != NULL) {
        Message m = {.kind = FORTH_KIND_KILLED, .from = K->self, .to = id, .n = 0};

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
/* what an actor asks of one on another node (to: its address): sent there - an error if that node has ended, as
 * one gone here is */
static void ask_other_node(Forth *f, int what, Cell to, const char *word) {
    Cell from = K->self >= 0 ? forth_node_address(f, K->self) : -1;

    if (!forth_node_control(f, what, from, to, 0, NULL, 0)) {
        forth_error(f, "%s: no actor %ld:%ld (its node has ended)", word, (long)(to >> FORTH_NODE_SHIFT),
                    (long)(to & FORTH_NODE_ID_MASK));
    }
}

PRIM(p_kill) {
    Cell id = here(f, POP());

    if (remote(id)) {
        ask_other_node(f, FORTH_NODE_KILL, id, "kill");
        return;
    }
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

/* ---- the prompt's actor, and looking at a running system: one fact a line, as `key: value` ---- */

/* attach ( id -- ) the prompt's lines (forth_eval's, a repl session's) run as this actor from here - its fields, self,
 * a send from it - till `detach`; enter for one line, leave-actor for the rest of it */
PRIM(p_attach) {
    Cell id = POP();

    if (K->delivering || K->handlers > 0) {
        forth_error(f, "attach: not from a handler");
    }
    live(f, id, "attach");
    K->self = K->prompt_actor = (int)id;
    K->sender = -1;
}
PRIM(p_detach) { K->self = K->sender = K->prompt_actor = NOBODY; }   /* detach ( -- ) */
PRIM(p_attached) { PUSH(K->prompt_actor); }   /* attached ( -- id ) the prompt's actor (-1: none) */

int forth_actors_prompt_enter(Forth *f) {
    int saved;

    if (K == NULL || K->delivering || K->handlers > 0) {   /* (forth_eval inside a handler: as it is) */
        return K != NULL ? K->self : NOBODY;
    }
    saved = K->self;
    if (K->prompt_actor != NOBODY && actor_at(f, K->prompt_actor) == NULL) {
        forth_printf(f, "(the actor attached, %d, is gone: detached)\n", K->prompt_actor);
        K->prompt_actor = NOBODY;
    }
    if (K->prompt_actor != NOBODY) {
        K->self = K->prompt_actor;
        K->sender = -1;
    }
    return saved;
}
void forth_actors_prompt_leave(Forth *f, int saved) {
    if (K == NULL || K->delivering || K->handlers > 0) {
        return;
    }
    K->prompt_actor = K->self;   /* (the actor the line ended in - entered, attached, left - the next line's) */
    K->self = saved;
    K->sender = -1;
}
int forth_actor_attached(Forth *f) { return K != NULL ? K->prompt_actor : NOBODY; }
int forth_actors_attach(Forth *f, int id) {
    int was = K != NULL ? K->prompt_actor : NOBODY;

    if (K != NULL) {
        K->prompt_actor = id >= 0 && actor_at(f, id) != NULL ? id : NOBODY;
    }
    return was;
}

static void print_addresses(Forth *f, const char *key, const Addresses *s) {
    int i;

    forth_printf(f, "%s:", key);
    for (i = 0; i < s->n; i++) {
        forth_printf(f, " %ld (%s)", (long)s->at[i], actor_name(f, s->at[i]));
    }
    forth_printf(f, "\n");
}

static void print_message(Forth *f, const Message *m) {   /* kind args <- sender */
    int k;

    forth_printf(f, "%s", K->kinds[m->kind].name);
    for (k = 0; k < m->n; k++) {
        print_cell(f, m->a[k]);
    }
    forth_printf(f, "  <- %s\n", actor_name(f, m->from));
}

static const Failure *last_failure(Forth *f, int id) {   /* its latest failure kept, NULL none */
    const Failure *best = NULL;
    int i;

    for (i = 0; i < FAILURES_KEPT; i++) {
        const Failure *r = &K->failures[i];

        if (r->number > 0 && r->actor == id && (best == NULL || r->number > best->number)) {
            best = r;
        }
    }
    return best;
}

/* .actor ( id -- ) all of one actor: who, its behaviour and what it handles, its state, how busy, its mail, objects,
 * watchers, links, subscriptions, its last failure */
PRIM(p_dot_actor) {
    Actor *a = live(f, POP(), ".actor");
    const Behaviour *b;
    const Failure *r = last_failure(f, a->id);
    int k, queued = 0;

    for (k = 0; k < K->queue_n; k++) {
        queued += K->queue[k].to == a->id;
    }
    forth_printf(f, "actor: %d\nname: %s\n", a->id, a->name);
    forth_printf(f, "behaviour: %s", a->beh != NULL ? a->beh->name : "-");
    for (b = a->beh != NULL ? a->beh->parent : NULL; b != NULL; b = b->parent) {
        forth_printf(f, " < %s", b->name);
    }
    forth_printf(f, "\nhandles:");
    for (k = 0; k < K->nkinds; k++) {
        if (handler_of(a, k) != NULL) {
            forth_printf(f, " %s", K->kinds[k].name);
        }
    }
    forth_printf(f, "\nstate: %s\n", a->type >= 0 ? K->types[a->type].name : "-");
    forth_printf(f, "handled: %ld\n", a->handled);
    if (a->busy) {
        forth_printf(f, "busy: in `on %s`%s%s\n", K->kinds[a->kind].name, a->awaiting >= 0 ? ", awaiting " : "",
                     a->awaiting >= 0 ? K->kinds[a->awaiting].name : "");
    } else {
        forth_printf(f, "busy: no\n");
    }
    forth_printf(f, "mail: %d kept, %d queued\n", a->mail_n, queued);
    forth_printf(f, "objects: %ld, %lu bytes\n", forth_region_objects(f, a->region),
                 (unsigned long)forth_region_bytes(f, a->region));
    print_addresses(f, "watchers", &a->watchers);
    print_addresses(f, "links", &a->links);
    forth_printf(f, "subscribes:");
    for (k = 0; k < K->nkinds; k++) {
        if (bit(a->subs, k)) {
            forth_printf(f, " %s", K->kinds[k].name);
        }
    }
    forth_printf(f, "\nlast failure:");
    if (r != NULL) {
        forth_printf(f, " %ld, frame %ld, in `on %s`: %s\n  in %s\n", r->number, (long)r->frame, r->what, r->error,
                     r->chain);
    } else {
        forth_printf(f, " none\n");
    }
}

PRIM(p_dot_mail) {   /* .mail ( id -- ) the messages kept for it (busy), oldest first, then those queued */
    Actor *a = live(f, POP(), ".mail");
    int i;

    for (i = 0; i < a->mail_n; i++) {
        forth_printf(f, "kept: ");
        print_message(f, &a->mail[a->mail_head + i]);
    }
    for (i = 0; i < K->queue_n; i++) {
        if (K->queue[i].to == a->id) {
            forth_printf(f, "queued: ");
            print_message(f, &K->queue[i]);
        }
    }
}

PRIM(p_dot_failures) {   /* .failures ( -- ) the failures kept (the latest 32), oldest first */
    long n;

    for (n = K->failures_told - FAILURES_KEPT + 1; n <= K->failures_told; n++) {
        const Failure *r = &K->failures[(n > 0 ? n : 0) % FAILURES_KEPT];

        if (n <= 0 || r->number != n) {
            continue;
        }
        forth_printf(f, "failure %ld: frame %ld, %s", r->number, (long)r->frame, r->who);
        if (r->actor >= 0) {
            forth_printf(f, " (%ld%s)", (long)r->actor, actor_at(f, (int)r->actor) != NULL ? "" : ", gone");
        }
        if (r->what[0]) {
            forth_printf(f, " in `on %s`", r->what);
        }
        forth_printf(f, ": %s\n  in %s\n", r->error, r->chain);
    }
}

PRIM(p_dot_behaviour) {   /* .behaviour ( behaviour -- ) what it handles, and where each handler is from */
    const Behaviour *beh = behaviour_arg(f, POP(), ".behaviour"), *b;
    int k;

    forth_printf(f, "behaviour: %s", beh->name);
    for (b = beh->parent; b != NULL; b = b->parent) {
        forth_printf(f, " < %s", b->name);
    }
    forth_printf(f, "\n");
    for (k = 0; k < K->nkinds; k++) {
        for (b = beh; b != NULL && b->on[k] == NULL; b = b->parent) {
        }
        if (b != NULL) {
            forth_printf(f, "on %s ( %d )%s%s\n", K->kinds[k].name, K->kinds[k].nargs, b != beh ? "  from " : "",
                         b != beh ? b->name : "");
        }
    }
}

/* see-handler ( behaviour kind -- ) its handler of that message (`lookable poke see-handler`) decompiled, as `see`
 * shows a word (one it inherits: its parent's) */
PRIM(p_see_handler) {
    int kind = kind_arg(f, POP(), "see-handler");
    const Behaviour *beh = behaviour_arg(f, POP(), "see-handler"), *b;

    for (b = beh; b != NULL && b->on[kind] == NULL; b = b->parent) {
    }
    if (b == NULL) {
        forth_printf(f, "%s doesn't handle %s\n", beh->name, K->kinds[kind].name);
        return;
    }
    forth_see(f, b->on[kind]);
}
PRIM(p_sender) { PUSH(K->sender); }
PRIM(p_alive) { Cell id = POP(); PUSH(id >= 0 && id <= INT_MAX && actor_at(f, (int)id) != NULL ? -1 : 0); }
/* actor-slot ( id -- n ) its place among the actors, 0 .. actors-max - 1 (a table with a cell for each actor is by
 * this, not by the id); -1 for nobody. Whether the actor is still there is `alive?`'s to say. */
PRIM(p_actor_slot) { Cell id = POP(); PUSH(id >= 0 && id <= INT_MAX ? slot_of((int)id) : -1); }
PRIM(p_actors_max) { PUSH(K->max); }   /* ( -- n ) actors alive at once, at most (the host's limit) */
PRIM(p_actors_alive) {   /* ( -- n ) actors alive now */
    int slot, n = 0;

    for (slot = 0; slot < K->nslots; slot++) {
        n += K->actors[slot]->used;
    }
    PUSH(n);
}
PRIM(p_actor_name) {   /* ( id -- addr len ) */
    Actor *a = live(f, POP(), "actor-name");

    PUSH(a->name);
    PUSH(strlen(a->name));
}
/* kind-name ( kind -- addr len ) a message kind's name (actor-slow's kind, a record's) */
PRIM(p_kind_name) {
    Cell k = POP();

    if (k < 0 || k >= K->nkinds) {
        forth_error(f, "kind-name: no message kind %ld", (long)k);
    }
    PUSH(K->kinds[k].name);
    PUSH(strlen(K->kinds[k].name));
}
/* actor-type ( id -- type ) the state type it was spawned with (-1 none; nobody's, or one gone: -2) - what its fields
 * are, before a word reads one of a type's */
PRIM(p_actor_type) {
    Actor *a = actor_at(f, POP());

    PUSH(a != NULL ? a->type : -2);
}
PRIM(p_actor_named) {   /* ( addr len -- id | -1 ) the first live actor with that name */
    Cell n = POP(), addr = POP();
    size_t len;
    const char *name = forth_string_arg(f, addr, n, "actor-named", &len);
    int slot;

    for (slot = 0; slot < K->nslots; slot++) {
        const Actor *a = K->actors[slot];

        if (a->used && strlen(a->name) == len && strncmp(a->name, name, len) == 0) {
            PUSH(a->id);
            return;
        }
    }
    PUSH(-1);
}
PRIM(p_behaviour_of) { PUSH(live(f, POP(), "behaviour-of")->beh); }
PRIM(p_busy) { PUSH(live(f, POP(), "actor-busy?")->busy ? -1 : 0); }   /* ( id -- flag ) a handler of its waits */
PRIM(p_mail) { PUSH(live(f, POP(), "actor-mail")->mail_n); }   /* ( id -- n ) messages kept for it till it's free */
/* ( obj -- obj ) kept by the running actor, with what it holds: till it dies (its last words see it), or `dispose`.
 * Outside an actor's handler: kept till `dispose`, as `rescue-all` keeps one */
PRIM(p_rescue_actor) {
    Cell x = POP();
    Actor *a = K->self >= 0 ? actor_at(f, K->self) : NULL;

    forth_object_keep(f, x, a != NULL ? a->region : 0, "rescue-actor");
    PUSH(x);
}
/* ( id -- n ) the objects it holds: those its handlers rescued, and those of the handler running or waiting */
PRIM(p_actor_objects) { PUSH(forth_region_objects(f, live(f, POP(), "actor-objects")->region)); }
/* actor-bytes ( id -- n ) the bytes its objects take (what it rescued, its handler's): its memory, near enough */
PRIM(p_actor_bytes) { PUSH((Cell)forth_region_bytes(f, live(f, POP(), "actor-bytes")->region)); }
/* errors-actor ( -- id ) the errors actor errors-to named (nobody: none - an actor that fails dies of it) */
PRIM(p_errors_actor) { PUSH(actor_at(f, K->errors_actor) != NULL ? K->errors_actor : NOBODY); }
PRIM(p_dot_actor_objects) { forth_region_print(f, live(f, POP(), ".actor-objects")->region); }   /* ( id -- ) by kind */

static void out_of_memory_unless(Forth *f, int added, const char *word) {
    if (!added) {
        forth_error(f, "%s: out of memory", word);
    }
}

/* monitor ( id -- ): this actor told `died ( id reason -- )` when that one is gone; link ( id -- ): the two killed
 * together (either gone, the other killed). Either may be on another node (its address): asked there - a `died` (or
 * the link's kill) comes back if it is gone already, or when its node ends. */
PRIM(p_monitor) {
    Actor *who = me(f, "monitor");
    Cell id = here(f, POP());

    if (remote(id)) {
        out_of_memory_unless(f, addresses_add(f, &who->watching, id), "monitor");
        ask_other_node(f, FORTH_NODE_MONITOR, id, "monitor");
        return;
    }
    out_of_memory_unless(f, addresses_add(f, &live(f, id, "monitor")->watchers, who->id), "monitor");
    out_of_memory_unless(f, addresses_add(f, &who->watching, id), "monitor");   /* (so its death takes it off) */
}
PRIM(p_unmonitor) {
    Actor *who = me(f, "unmonitor");
    Cell id = here(f, POP());

    if (remote(id)) {
        addresses_remove(&who->watching, id);
        (void)forth_node_control(f, FORTH_NODE_UNMONITOR, forth_node_address(f, who->id), id, 0, NULL, 0);
        return;
    }
    addresses_remove(&live(f, id, "unmonitor")->watchers, who->id);
    addresses_remove(&who->watching, id);
}
PRIM(p_link) {
    Actor *who = me(f, "link"), *a;
    Cell id = here(f, POP());

    if (remote(id)) {
        out_of_memory_unless(f, addresses_add(f, &who->links, id), "link");
        if (!forth_node_control(f, FORTH_NODE_LINK, forth_node_address(f, who->id), id, 0, NULL, 0)) {
            addresses_remove(&who->links, id);
            forth_error(f, "link: no actor %ld:%ld (its node has ended)", (long)(id >> FORTH_NODE_SHIFT),
                        (long)(id & FORTH_NODE_ID_MASK));
        }
        return;
    }
    a = live(f, id, "link");
    out_of_memory_unless(f, addresses_add(f, &a->links, who->id), "link");
    if (!addresses_add(f, &who->links, a->id)) {
        addresses_remove(&a->links, who->id);
        out_of_memory_unless(f, 0, "link");
    }
}
PRIM(p_unlink) {
    Actor *who = me(f, "unlink");
    Cell id = here(f, POP());

    if (remote(id)) {
        addresses_remove(&who->links, id);
        (void)forth_node_control(f, FORTH_NODE_UNLINK, forth_node_address(f, who->id), id, 0, NULL, 0);
        return;
    }
    addresses_remove(&live(f, id, "unlink")->links, who->id);
    addresses_remove(&who->links, id);
}

PRIM(p_slice_steps) { Cell n = POP(); forth_set_slice(f, n > 0 ? (long)n : 0); }   /* slice-steps! ( n -- ) 0: none */
PRIM(p_slice_steps_q) { PUSH(f->slice); }                                          /* slice-steps ( -- n ) */
/* actors-runnable? ( -- flag ) a handler can go on now (it gave way, its job is done, a message waits): a loop of the
 * program's own - deliver, then wait for what comes - delivers again at once rather than waiting */
/* .handler-times ( -- ) each handler that has run: its behaviour and message, how often, its time on the node all told
 * and at most (a handler's turns together: waits not counted) - the most time first. handler-times-reset forgets them */
PRIM(p_dot_handler_times) {
    char done[HANDLER_TIMES_MAX] = {0};
    int n, i;

    forth_printf(f, "%-24s %-20s %8s %10s %9s %9s\n", "behaviour", "message", "runs", "total ms", "avg ms", "max ms");
    for (n = 0; n < K->ntimes; n++) {
        int best = -1;

        for (i = 0; i < K->ntimes; i++) {
            if (!done[i] && (best < 0 || K->times[i].total_us > K->times[best].total_us)) {
                best = i;
            }
        }
        done[best] = 1;
        forth_printf(f, "%-24s %-20s %8ld %10.1f %9.3f %9.1f\n", K->times[best].beh->name,
                     K->kinds[K->times[best].kind].name, K->times[best].runs, K->times[best].total_us / 1e3,
                     K->times[best].total_us / 1e3 / (double)K->times[best].runs, K->times[best].max_us / 1e3);
    }
}
PRIM(p_handler_times_reset) { K->ntimes = 0; }
PRIM(p_actors_runnable) { PUSH(forth_actors_runnable(f) ? -1 : 0); }
PRIM(p_slow_ms) { Cell ms = POP(); K->slow_ms = ms > 0 ? (long)ms : 0; }   /* slow-handler-ms! ( ms -- ) 0: never */
PRIM(p_frame) { forth_actors_frame(f); }
PRIM(p_deliver) {
    if (K->delivering || K->handlers > 0) {
        forth_error(f, "deliver: already delivering (not from a handler)");
    }
    deliver(f);
    go_on_now(f, 1);   /* (and the handlers that gave way, their slice over: a slice more each - a loop of net-poll and
                 *  deliver keeps them going, and its sockets too, as serve's does) */
    deliver(f);
}
PRIM(p_reset) {
    if (K->delivering || K->handlers > 0) {
        forth_error(f, "actors-reset: not from a handler (nor an actor's last words)");
    }
    forth_actors_reset(f);
}
PRIM(p_trace_on) { K->trace = 1; }
PRIM(p_trace_off) { K->trace = 0; }
PRIM(p_queued) { PUSH(K->queue_n); }
PRIM(p_actor_frame) { PUSH(K->frame); }
PRIM(p_dot_actors) {
    int slot, k, n;

    for (slot = 0; slot < K->nslots; slot++) {
        Actor *a = K->actors[slot];
        int id = a->id;

        if (!a->used) {
            continue;
        }
        for (n = 0, k = 0; k < K->queue_n; k++) {
            n += K->queue[k].to == id;
        }
        forth_printf(f, "%3d %-20s %-16s %-16s %ld handled, %d queued, %ld objects (%lu KB)", id, a->name,
                     a->beh ? a->beh->name : "-", a->type >= 0 ? K->types[a->type].name : "-", a->handled, n,
                     forth_region_objects(f, a->region), (unsigned long)((forth_region_bytes(f, a->region) + 1023) / 1024));
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

        forth_printf(f, "%lld  %s -> %s  %s", (long long)K->recent[i % RECENT_MAX].frame, actor_name(f, m->from),
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

PRIM(p_errors_to) {   /* ( id -- ) failures told to it (nobody: none) - an actor here */
    Cell id = here(f, POP());

    if (id < NOBODY || id > INT_MAX) {
        forth_error(f, "errors-to: an actor of this node's, or nobody (not %ld)", (long)id);
    }
    K->errors_actor = (int)id;
}
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
        {"message", p_message, 0, "( -- )"}, {"quiet-message", p_quiet_message, 0, "( -- )"}, {"(send)", p_send_rt, 0, NULL}, {"(broadcast)", p_broadcast_rt, 0, NULL},
        {"send", p_send, 1, NULL}, {"broadcast", p_broadcast, 1, NULL},
        {"(subscribe)", p_subscribe_rt, 0, NULL}, {"(unsubscribe)", p_unsubscribe_rt, 0, NULL},
        {"subscribe", p_subscribe, 1, NULL}, {"unsubscribe", p_unsubscribe, 1, NULL},
        {"(await)", p_await_rt, 0, NULL}, {"(await-within)", p_await_within_rt, 0, NULL},
        {"(await-within-ms)", p_await_within_ms_rt, 0, NULL},
        {"await", p_await, 1, NULL}, {"await-within", p_await_within, 1, NULL},
        {"await-within-ms", p_await_within_ms, 1, NULL},
        {"behaviour", p_behaviour, 0, NULL}, {"extends", p_extends, 0, NULL}, {"on", p_on, 0, NULL},
        {"end-behaviour", p_end_behaviour, 0, NULL}, {"become", p_become, 0, "( behaviour -- )"},
        {"state:", p_state, 0, NULL}, {"field", p_field, 0, NULL}, {"end-state", p_end_state, 0, NULL},
        {"spawn", p_spawn, 0, "( behaviour type addr len -- id )"},
        {"spawn-on", p_spawn_on, 0, "( behaviour type addr len node -- addr )"},
        {"spawn-anywhere", p_spawn_anywhere, 0, "( behaviour type addr len -- addr )"}, {"kill", p_kill, 0, "( id -- )"},
        {"self", p_self, 0, "( -- id )"}, {"enter", p_enter, 0, "( id -- )"}, {"leave-actor", p_leave, 0, "( -- )"},
        {"attach", p_attach, 0, "( id -- )"}, {"detach", p_detach, 0, "( -- )"}, {"attached", p_attached, 0, "( -- id )"},
        {".actor", p_dot_actor, 0, "( id -- )"}, {".mail", p_dot_mail, 0, "( id -- )"},
        {".failures", p_dot_failures, 0, "( -- )"}, {".behaviour", p_dot_behaviour, 0, "( behaviour -- )"},
        {"see-handler", p_see_handler, 0, "( behaviour kind -- )"},
        {"sender", p_sender, 0, "( -- id )"}, {"alive?", p_alive, 0, "( id -- flag )"},
        {"actor-slot", p_actor_slot, 0, "( id -- n )"}, {"actors-max", p_actors_max, 0, "( -- n )"},
        {"actors-alive", p_actors_alive, 0, "( -- n )"},
        {"actor-name", p_actor_name, 0, "( id -- addr len )"}, {"actor-named", p_actor_named, 0, "( addr len -- id )"},
        {"actor-type", p_actor_type, 0, "( id -- type )"}, {"kind-name", p_kind_name, 0, "( kind -- addr len )"},
        {"behaviour-of", p_behaviour_of, 0, "( id -- behaviour )"}, {"actor-busy?", p_busy, 0, "( id -- flag )"},
        {"actor-mail", p_mail, 0, "( id -- n )"}, {"actor-objects", p_actor_objects, 0, "( id -- n )"},
        {"actor-bytes", p_actor_bytes, 0, "( id -- n )"}, {"errors-actor", p_errors_actor, 0, "( -- id )"},
        {"rescue-actor", p_rescue_actor, 0, "( obj -- obj )"},
        {".actor-objects", p_dot_actor_objects, 0, "( id -- )"}, {"monitor", p_monitor, 0, "( id -- )"},
        {"unmonitor", p_unmonitor, 0, "( id -- )"}, {"link", p_link, 0, "( id -- )"}, {"unlink", p_unlink, 0, "( id -- )"},
        {"slow-handler-ms!", p_slow_ms, 0, "( ms -- )"},
        {"slice-steps!", p_slice_steps, 0, "( n -- )"}, {"slice-steps", p_slice_steps_q, 0, "( -- n )"},
        {"actors-runnable?", p_actors_runnable, 0, "( -- flag )"},
        {".handler-times", p_dot_handler_times, 0, "( -- )"}, {"handler-times-reset", p_handler_times_reset, 0, "( -- )"},
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
                                                           "actor-failed", "start-over", "died", "placed",
                                                           "actor-slow"};
    static const int kernel_nargs[FORTH_KERNEL_KINDS] = {0, 0, 0, 0, 0, 1, 0, 2, 1, 3};
    Vocab *saved = f->m.current, *v = forth_vocab(f, "actors");
    size_t i;

    if (K == NULL) {
        K = calloc(1, sizeof(*K));
        if (K == NULL) {
            forth_error(f, "actors: out of memory");
        }
        K->self = K->sender = K->errors_actor = K->prompt_actor = NOBODY;
        K->max = FORTH_ACTORS_MAX;
        K->slow_ms = SLOW_MS_DEFAULT;
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
    forth_constant(f, "no-actor", FORTH_NO_ACTOR);
    forth_nodes_words(f, v);
    for (i = 0; i < FORTH_KERNEL_KINDS; i++) {
        forth_constant(f, kernel_kinds[i], (Cell)i);
    }
    forth_set_current(f, saved);
}

/* ---- for nodes.c: a message from another node ---- */

size_t forth_actors_body_bytes(Forth *f) { return K != NULL ? K->body_bytes : 0; }

/* its kind here (by name, as many arguments), if actor `to` is here to take it; -1 if not (a dead letter) */
int forth_actors_accepts(Forth *f, int to, const char *kind, int n) {
    int k = K != NULL ? kind_find(f, kind, strlen(kind)) : -1;

    return k >= 0 && K->kinds[k].nargs == n && actor_at(f, to) != NULL ? k : -1;
}

void forth_actors_arrive(Forth *f, Cell from, int to, int kind, const Cell *args, int n) {
    enqueue(f, from, to, kind, args, n);
}

void forth_actors_dead_letter(Forth *f) {
    if (K != NULL) {
        K->dead_letters++;
    }
}

/* a failure on another node, kept here as a record of this node's (its number the reason here): the errors actor
 * isn't told - it is the other node's to put right */
static Cell keep_failure(Forth *f, Cell from, const void *extra, size_t n) {
    Failure *r;
    Cell number;

    if (extra == NULL || n != sizeof(Failure)) {
        return FORTH_NO_ACTOR;
    }
    number = ++K->failures_told;
    r = &K->failures[number % FAILURES_KEPT];
    memcpy(r, extra, sizeof(Failure));
    r->number = number;
    r->actor = from;
    r->frame = K->frame;
    return number;
}

/* what an actor on another node asks of one here, or tells it (from: its address; to: the id here) */
void forth_actors_control(Forth *f, int what, Cell from, int to, Cell reason, const void *extra, size_t n) {
    Actor *a;
    Cell me, died[2];

    if (K == NULL) {
        return;
    }
    a = actor_at(f, to);
    me = forth_node_address(f, to);
    switch (what) {
    case FORTH_NODE_MONITOR:   /* (gone already, or no room to note it: told so at once) */
        if (a == NULL || !addresses_add(f, &a->watchers, from)) {
            forth_node_control(f, FORTH_NODE_DIED, me, from, FORTH_NO_ACTOR, NULL, 0);
        }
        break;
    case FORTH_NODE_UNMONITOR:
        if (a != NULL) {
            addresses_remove(&a->watchers, from);
        }
        break;
    case FORTH_NODE_LINK:
        if (a == NULL || !addresses_add(f, &a->links, from)) {
            forth_node_control(f, FORTH_NODE_EXIT, me, from, FORTH_NO_ACTOR, NULL, 0);
        }
        break;
    case FORTH_NODE_UNLINK:
        if (a != NULL) {
            addresses_remove(&a->links, from);
        }
        break;
    case FORTH_NODE_KILL:
        if (a != NULL && !a->dying) {
            a->reason = 0;
            kill_actor(f, to);
        }
        break;
    case FORTH_NODE_EXIT:   /* (only while it is still linked to the one gone: an unlink meanwhile, nothing) */
        if (a != NULL && addresses_remove(&a->links, from) && !a->dying) {
            a->reason = reason > 0 ? keep_failure(f, from, extra, n) : reason;
            kill_actor(f, to);
        }
        break;
    case FORTH_NODE_SPAWN:
        spawn_asked(f, from, extra, n);
        break;
    case FORTH_NODE_DIED:
        if (a != NULL) {
            addresses_remove(&a->watching, from);
            died[0] = from;
            died[1] = reason > 0 ? keep_failure(f, from, extra, n) : reason;
            enqueue(f, from, to, FORTH_KIND_DIED, died, 2);
        }
        break;
    default:
        break;
    }
}

void forth_actors_free(Forth *f) {
    int i;

    if (K == NULL) {
        return;
    }
    forth_actors_reset(f);
    for (i = 0; i < K->nslots; i++) {   /* (a node's reset keeps its slots: let go now) */
        free(K->actors[i]);
    }
    free(K->actors);
    free(K->spare);
    free(K->doomed);
    for (i = 0; i < K->nbehaviours; i++) {
        free(K->behaviours[i]);
    }
    for (i = 0; i < K->ntypes; i++) {
        free(K->types[i].fields);
        free(K->types[i].was);
    }
    free(K->queue);
    free(K);
    K = NULL;
}

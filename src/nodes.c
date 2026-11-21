/* Nodes: many Forths in one process, each on a thread of its own - one to a core - their actors sending each other
 * messages as they send their own. Nothing is shared between them but this: a table of the nodes, each with an
 * inbox, and a table of names. A message to another node is copied there - its kind by name, its arguments, the
 * objects among them made again on the other side (forth_object_pack) - and the receiver takes what has come as its
 * actors deliver.
 *
 * An actor on another node is known by its address: the node's number over FORTH_NODE_SHIFT bits, the actor's id
 * under them. A plain id is an actor here, as ever. `sender` of a message from another node is its sender's address,
 * so a reply goes back with `sender send ...`. See docs/actors.md, "Nodes". */
#include "forth_internal.h"
#include "actors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CLANKFORTH_NODES)

#include <errno.h>
#include <pthread.h>
#include <time.h>

#define NODES_MAX 64            /* nodes alive at once */
#define NODE_NAMES_MAX 256      /* names registered at once (register-name) */
#define NODE_FRAME_MS 16        /* a node started by start-node: a frame at least this often (its waits count them) */
#define NODE_GENERATIONS (INT32_MAX / NODES_MAX)   /* times a place in the table is taken before its numbers go round */
#define ID_MASK ((Cell)0xFFFFFFFF)
#define NS_PER_MS 1000000L
#define NS_PER_S 1000000000L

typedef struct Packet {
    struct Packet *next;
    Cell from;                  /* its sender's address (-1: no actor's) */
    int to;                     /* the actor's id, on the receiving node */
    char kind[FORTH_ACTOR_NAME_MAX + 1];
    int n;
    Cell a[FORTH_MSG_ARGS];
    uint32_t objects;           /* bit i: argument i an object, packed in bytes (in order) */
    size_t nbytes;
    uint8_t bytes[];
} Packet;

/* A node's number is its place in the table and, over that, how many have had the place before - as an actor's id is
 * (actors.c) - plus 1, so a node's address bits are never 0: an address kept after its node ended names no one. */
typedef struct Node {
    Forth *f;                   /* NULL: the place is free */
    int number;
    int generation;
    int ready;                  /* its lock and condition made */
    pthread_mutex_t lock;
    pthread_cond_t arrived;
    Packet *first, *last;       /* its inbox */
    long arrivals, seen;        /* packets come, and those its waits have seen */
    int stopping;               /* stop-node: its loop to end */
} Node;

static Node sNodes[NODES_MAX];
static struct { char name[FORTH_ACTOR_NAME_MAX + 1]; Cell address; } sNames[NODE_NAMES_MAX];
/* (the tables: taken, then a node's own lock - never the other way round) */
static pthread_mutex_t sTable = PTHREAD_MUTEX_INITIALIZER;

static int place_of(int number) { return (number - 1) % NODES_MAX; }

/* the node of that number, its lock taken (the table's let go); NULL if it has ended */
static Node *node_locked(int number) {
    Node *n = NULL;

    if (number <= 0) {
        return NULL;
    }
    pthread_mutex_lock(&sTable);
    if (sNodes[place_of(number)].f != NULL && sNodes[place_of(number)].number == number) {
        n = &sNodes[place_of(number)];
        pthread_mutex_lock(&n->lock);
    }
    pthread_mutex_unlock(&sTable);
    return n;
}

int forth_node_of(Forth *f) {
    int i;

    if (f->node != 0) {
        return f->node;
    }
    pthread_mutex_lock(&sTable);
    for (i = 0; i < NODES_MAX && sNodes[i].f != NULL; i++) {
    }
    if (i < NODES_MAX) {
        Node *n = &sNodes[i];

        if (!n->ready) {
            pthread_mutex_init(&n->lock, NULL);
            pthread_cond_init(&n->arrived, NULL);
            n->ready = 1;
        }
        n->generation = (n->generation + 1) % NODE_GENERATIONS;
        n->number = n->generation * NODES_MAX + i + 1;
        n->f = f;
        n->first = n->last = NULL;
        n->arrivals = n->seen = 0;
        n->stopping = 0;
        f->node = n->number;
    }
    pthread_mutex_unlock(&sTable);
    if (f->node == 0) {
        forth_error(f, "nodes: %d already, no room for another", NODES_MAX);
    }
    return f->node;
}

int forth_node(Forth *f) { return forth_node_of(f); }
void forth_set_node_setup(Forth *f, void (*setup)(Forth *f)) { f->node_setup = setup; }

Cell forth_node_address(Forth *f, int id) {
    return id < 0 ? id : (Cell)((UCell)forth_node_of(f) << FORTH_NODE_SHIFT | (UCell)id);
}

int forth_node_send(Forth *f, Cell from, Cell to, const char *kind, const Cell *args, int n) {
    ForthBytes packed = {NULL, 0, 0};
    Packet *p;
    Node *node;
    uint32_t objects = 0;
    Cell a[FORTH_MSG_ARGS];
    int i;

    for (i = 0; i < n; i++) {   /* (here, where an error can be said: an object too deep, out of memory) */
        a[i] = args[i];
        if (forth_object_pack(f, args[i], &packed)) {
            objects |= 1u << i;
            a[i] = 0;
        }
    }
    p = malloc(sizeof(Packet) + packed.n);
    if (p == NULL) {
        free(packed.bytes);
        forth_error(f, "send: out of memory for a message to another node");
    }
    memset(p, 0, sizeof(Packet));
    p->from = from;
    p->to = (int)(to & ID_MASK);
    snprintf(p->kind, sizeof(p->kind), "%s", kind);
    p->n = n;
    memcpy(p->a, a, (size_t)n * sizeof(Cell));
    p->objects = objects;
    p->nbytes = packed.n;
    if (packed.n > 0) {
        memcpy(p->bytes, packed.bytes, packed.n);
    }
    free(packed.bytes);
    if ((node = node_locked((int)(to >> FORTH_NODE_SHIFT))) == NULL) {
        free(p);
        return 0;   /* (its node has ended: a dead letter) */
    }
    if (node->last != NULL) {
        node->last->next = p;
    } else {
        node->first = p;
    }
    node->last = p;
    node->arrivals++;
    pthread_cond_signal(&node->arrived);
    pthread_mutex_unlock(&node->lock);
    return 1;
}

/* what has come for this node, handed to the actor kernel - each message's objects made here first */
void forth_node_receive(Forth *f) {
    Node *node;
    Packet *p, *next;

    if (f->node == 0 || (node = node_locked(f->node)) == NULL) {
        return;
    }
    p = node->first;
    node->first = node->last = NULL;
    pthread_mutex_unlock(&node->lock);
    for (; p != NULL; p = next) {
        const uint8_t *at = p->bytes, *end = p->bytes + p->nbytes;
        int kind = forth_actors_accepts(f, p->to, p->kind, p->n), i, whole = kind >= 0;

        next = p->next;
        for (i = 0; whole && i < p->n; i++) {
            if ((p->objects >> i) & 1u) {
                whole = forth_object_unpack(f, &at, end, &p->a[i]);
            }
        }
        if (whole) {
            forth_actors_arrive(f, p->from, p->to, kind, p->a, p->n);
        } else {
            forth_actors_dead_letter(f);   /* (no such actor here, kind, or class: dropped and counted) */
        }
        free(p);
    }
}

int forth_node_wait(Forth *f, long ms) {
    Node *node;
    struct timespec until;
    int came;

    if (f->node == 0 || (node = node_locked(f->node)) == NULL) {
        return 0;
    }
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (ms % 1000) * NS_PER_MS;
    if (until.tv_nsec >= NS_PER_S) {
        until.tv_sec++;
        until.tv_nsec -= NS_PER_S;
    }
    while (node->arrivals == node->seen && !node->stopping) {
        if (pthread_cond_timedwait(&node->arrived, &node->lock, &until) == ETIMEDOUT) {
            break;
        }
    }
    came = node->arrivals != node->seen;
    node->seen = node->arrivals;
    pthread_mutex_unlock(&node->lock);
    return came;
}

void forth_node_leave(Forth *f) {
    Node *node;
    Packet *p, *next;
    int i;

    if (f->node == 0) {
        return;
    }
    pthread_mutex_lock(&sTable);
    node = &sNodes[place_of(f->node)];
    for (i = 0; i < NODE_NAMES_MAX; i++) {
        if (sNames[i].name[0] && (int)(sNames[i].address >> FORTH_NODE_SHIFT) == f->node) {
            sNames[i].name[0] = 0;
        }
    }
    pthread_mutex_lock(&node->lock);
    node->f = NULL;
    p = node->first;
    node->first = node->last = NULL;
    pthread_mutex_unlock(&node->lock);
    pthread_mutex_unlock(&sTable);
    for (; p != NULL; p = next) {
        next = p->next;
        free(p);
    }
    f->node = 0;
}

/* ---- start-node: a vocabulary loaded in a new Forth, on a thread of its own ---- */

typedef struct Starting {
    Forth *f;
    char vocab[VOCAB_NAME_MAX + 1];
} Starting;

/* its loop: tasks and actors, a frame whenever messages come or NODE_FRAME_MS has gone by, till `bye` or stop-node */
static void *node_main(void *arg) {
    Starting *s = arg;
    Forth *f = s->f;
    char line[VOCAB_NAME_MAX + 16];
    Node *node;
    int stopping = 0;

    snprintf(line, sizeof(line), "USING: %s ;", s->vocab);
    free(s);
    forth_report_crashes(f);   /* (this thread's Forth: a crash here names its words) */
    forth_eval(f, line, strlen(line), "start-node");
    while (!forth_bye(f) && !stopping) {
        forth_run_tasks(f);
        forth_actors_frame(f);
        forth_node_wait(f, NODE_FRAME_MS);
        if ((node = node_locked(f->node)) != NULL) {
            stopping = node->stopping;
            pthread_mutex_unlock(&node->lock);
        }
    }
    forth_free(f);
    return NULL;
}

static void p_start_node(Forth *f, Word *w) {   /* start-node ( addr len -- node ) */
    Cell n = forth_pop(f), a = forth_pop(f);
    size_t len;
    const char *vocab = forth_string_arg(f, a, n, "start-node", &len);
    Starting *s;
    Forth *g;
    pthread_t thread;
    int i;

    if (len == 0 || len > VOCAB_NAME_MAX) {
        forth_error(f, "start-node: a vocabulary's name, not \"%.*s\"", (int)len, vocab);
    }
    if ((g = forth_new((size_t)(f->end - f->mem))) == NULL || (s = calloc(1, sizeof(Starting))) == NULL) {
        if (g != NULL) {
            forth_free(g);
        }
        forth_error(f, "start-node: out of memory");
    }
    g->node_setup = f->node_setup;
    if (g->node_setup != NULL) {
        g->node_setup(g);
    }
    if (g->actors == NULL) {   /* (the host's setup may have turned them on: the tool's does) */
        forth_actors(g, forth_actors_body_bytes(f));
    }
    if (forth_prelude(g) != 0) {
        forth_free(g);
        free(s);
        forth_error(f, "start-node: the new node's prelude failed");
    }
    for (i = 0; i < f->nroots; i++) {
        forth_add_root(g, f->roots[i]);
    }
    forth_node_of(g);
    s->f = g;
    snprintf(s->vocab, sizeof(s->vocab), "%.*s", (int)len, vocab);
    if (pthread_create(&thread, NULL, node_main, s) != 0) {
        forth_free(g);
        free(s);
        forth_error(f, "start-node: no thread for it");
    }
    pthread_detach(thread);
    forth_push(f, g->node);
}

static void p_stop_node(Forth *f, Word *w) {   /* stop-node ( node -- ) its loop ends after its frame */
    Node *node = node_locked((int)forth_pop(f));

    if (node != NULL) {
        node->stopping = 1;
        pthread_cond_signal(&node->arrived);
        pthread_mutex_unlock(&node->lock);
    }
}

static void p_node(Forth *f, Word *w) { forth_push(f, forth_node_of(f)); }   /* node ( -- n ) this one's */

static void p_address(Forth *f, Word *w) {   /* address ( id -- addr ) an actor's here, as other nodes reach it */
    Cell id = forth_pop(f);

    forth_push(f, id >= 0 && id <= INT32_MAX ? forth_node_address(f, (int)id) : id);
}

static void p_node_of(Forth *f, Word *w) {   /* node-of ( addr -- n ) the node an address is on (this one's: a plain id) */
    Cell a = forth_pop(f);

    forth_push(f, a < 0 ? 0 : a >> FORTH_NODE_SHIFT ? a >> FORTH_NODE_SHIFT : forth_node_of(f));
}

static void p_register_name(Forth *f, Word *w) {   /* register-name ( addr len id -- ) any node's whereis finds it */
    Cell id = forth_pop(f), n = forth_pop(f), a = forth_pop(f);
    size_t len;
    const char *name = forth_string_arg(f, a, n, "register-name", &len);
    Cell address = id >= 0 && id <= INT32_MAX ? forth_node_address(f, (int)id) : id;
    int i, free_at = -1;

    if (len == 0 || len > FORTH_ACTOR_NAME_MAX) {
        forth_error(f, "register-name: a name of 1 to %d characters", FORTH_ACTOR_NAME_MAX);
    }
    pthread_mutex_lock(&sTable);
    for (i = 0; i < NODE_NAMES_MAX; i++) {
        if (sNames[i].name[0] == 0) {
            free_at = free_at < 0 ? i : free_at;
        } else if (strlen(sNames[i].name) == len && memcmp(sNames[i].name, name, len) == 0) {
            free_at = i;   /* (registered again: the newer) */
            break;
        }
    }
    if (free_at >= 0) {
        snprintf(sNames[free_at].name, sizeof(sNames[free_at].name), "%.*s", (int)len, name);
        sNames[free_at].address = address;
    }
    pthread_mutex_unlock(&sTable);
    if (free_at < 0) {
        forth_error(f, "register-name: %d names already", NODE_NAMES_MAX);
    }
}

static void p_whereis(Forth *f, Word *w) {   /* whereis ( addr len -- addr | nobody ) */
    Cell n = forth_pop(f), a = forth_pop(f), found = -1;
    size_t len;
    const char *name = forth_string_arg(f, a, n, "whereis", &len);
    int i;

    pthread_mutex_lock(&sTable);
    for (i = 0; i < NODE_NAMES_MAX; i++) {
        if (sNames[i].name[0] && strlen(sNames[i].name) == len && memcmp(sNames[i].name, name, len) == 0) {
            found = sNames[i].address;
        }
    }
    pthread_mutex_unlock(&sTable);
    forth_push(f, found);
}

static void p_node_wait(Forth *f, Word *w) {   /* node-wait ( ms -- flag ) wait that long at most for a message */
    Cell ms = forth_pop(f);

    forth_node_of(f);
    forth_push(f, forth_node_wait(f, ms > 0 ? (long)ms : 0) ? -1 : 0);
}

void forth_nodes_words(Forth *f, Vocab *actors) {
    static const struct { const char *name; Code code; const char *effect; } words[] = {
        {"start-node", p_start_node, "( addr len -- node )"}, {"stop-node", p_stop_node, "( node -- )"},
        {"node", p_node, "( -- n )"}, {"address", p_address, "( id -- addr )"}, {"node-of", p_node_of, "( addr -- n )"},
        {"register-name", p_register_name, "( addr len id -- )"}, {"whereis", p_whereis, "( addr len -- addr )"},
        {"node-wait", p_node_wait, "( ms -- flag )"},
    };
    Vocab *was = forth_current(f);
    size_t i;

    forth_set_current(f, actors);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        forth_set_effect(f, forth_prim(f, words[i].name, words[i].code), words[i].effect);
    }
    forth_set_current(f, was);
}

#else   /* (no threads: one node, this one) */

int forth_node_of(Forth *f) { return 0; }
int forth_node(Forth *f) { return 0; }
void forth_set_node_setup(Forth *f, void (*setup)(Forth *f)) { f->node_setup = setup; }
Cell forth_node_address(Forth *f, int id) { return id; }
int forth_node_send(Forth *f, Cell from, Cell to, const char *kind, const Cell *args, int n) { return 0; }
void forth_node_receive(Forth *f) { (void)f; }
int forth_node_wait(Forth *f, long ms) { return 0; }
void forth_node_leave(Forth *f) { (void)f; }
void forth_nodes_words(Forth *f, Vocab *actors) { (void)f; (void)actors; }

#endif

/* A program's nodes as processes, and the supervisor that keeps them.
 *
 * `n as-nodes` near a program's top: the process that says it becomes the supervisor - it starts the program again n
 * times, each a node (a process of its own: its own Forth, actors, memory, files), and never comes back from the
 * word. In each of those the word does nothing but join the pool: the program goes on, as node `node-index` of
 * `node-count`. Every node is the same program; what one of them alone should do, it asks its index for.
 *
 * The supervisor does little, so that little can go wrong in it: it starts the nodes, sees one end (its process),
 * starts it again - a new life, `node-life` - and tells the others; told to stop (SIGTERM, Ctrl-C) it tells the
 * nodes, waits for them, and ends. A node that ends with status 0 is done, not started again; one that ends as it
 * starts, again and again, stops the pool (its program is wrong: said). The supervisor gone, its nodes end too.
 *
 * Each node has a line to the supervisor (a socket on this machine, a secret said first): the supervisor's word to
 * stop, and who the other nodes are, come down it; its closing is the supervisor's end. */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
#include "forth_internal.h"
#include "actors.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POOL_ENV "CLANKFORTH_NODE"   /* a node's: "index/count/life/port/secret" - set by its supervisor */
#define SECRET_BYTES 16              /* the secret a node says first on its line: random, the pool's own */
#define SECRET_HEX (2 * SECRET_BYTES)
#define POOL_LINE_MAX 256            /* a line of the supervisor's, or a node's hello */

static int sArgc;        /* the command line this process was started by: a node is that again (forth_set_program) */
static char **sArgv;

void forth_set_program(Forth *f, int argc, char **argv) {
    (void)f;
    sArgc = argc;
    sArgv = argv;
}

#if defined(CLANKFORTH_NODES)

#include <pthread.h>
#ifdef _WIN32
typedef SOCKET Sock;
typedef HANDLE Proc;
#define NO_SOCK INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int Sock;
typedef pid_t Proc;
#define NO_SOCK (-1)
#define sock_close close
extern char **environ;
#endif

#define TICK_MS 100              /* the supervisor looks at its nodes this often (and as their lines say anything) */
#define QUICK_MS 2000            /* a life shorter than this ended as it started */
#define QUICK_DEATHS_MAX 5       /* so many of those in a row: the program is wrong - the pool stops */
#define BACKOFF_FIRST_MS 100     /* a node that ended as it started waits this before its next life, then twice ... */
#define BACKOFF_MAX_MS 5000      /* ... this at most */
#define STOP_MARGIN_MS 5000      /* told to stop, the nodes have their grace (shutdown-grace!) and this: then killed */
#define EXIT_GAVE_UP 1

/* ---- this process, as a node of a pool (or the only node there is) ---- */

static struct {
    int known;                   /* the environment looked at */
    int supervised;              /* started by a supervisor */
    int index, count, life;      /* 1 1 1 for a program no supervisor started */
    int port;                    /* the supervisor's line */
    char secret[SECRET_HEX + 1];
    int joined;                  /* its line to the supervisor is up */
    Sock control;
} sMe;

static void me(void) {
    const char *env;

    if (sMe.known) {
        return;
    }
    sMe.known = 1;
    sMe.index = sMe.count = sMe.life = 1;
    sMe.control = NO_SOCK;
    env = getenv(POOL_ENV);
    if (env != NULL) {
        int index, count, life, port;
        char secret[SECRET_HEX + 1];

        if (sscanf(env, "%d/%d/%d/%d/%32[0-9a-f]", &index, &count, &life, &port, secret) == 5 && index >= 1 &&
            index <= count && count <= FORTH_NODES_MAX && life >= 1 && strlen(secret) == SECRET_HEX) {
            sMe.supervised = 1;
            sMe.index = index;
            sMe.count = count;
            sMe.life = life;
            sMe.port = port;
            memcpy(sMe.secret, secret, sizeof(secret));
        }
    }
}

int forth_pool_supervised(void) {
    me();
    return sMe.supervised;
}
int forth_pool_index(void) {
    me();
    return sMe.index;
}
int forth_pool_life(void) {
    me();
    return sMe.life;
}

/* ---- sockets on this machine, time, processes: what differs between systems ---- */

static long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int sockets_up(void) {
#ifdef _WIN32
    static int up;
    WSADATA data;

    if (!up && WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return 0;
    }
    up = 1;
#endif
    return 1;
}

static void loopback(struct sockaddr_in *sa, int port) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa->sin_port = htons((unsigned short)port);
}

/* a listener on this machine only, on a port the system picks (*port) */
static Sock listen_local(int *port) {
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    Sock s = socket(AF_INET, SOCK_STREAM, 0);

    if (s == NO_SOCK) {
        return NO_SOCK;
    }
    loopback(&sa, 0);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(s, FORTH_NODES_MAX) != 0 ||
        getsockname(s, (struct sockaddr *)&sa, &len) != 0) {
        sock_close(s);
        return NO_SOCK;
    }
    *port = ntohs(sa.sin_port);
    return s;
}

static Sock connect_local(int port) {
    struct sockaddr_in sa;
    Sock s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;

    if (s == NO_SOCK) {
        return NO_SOCK;
    }
    loopback(&sa, port);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        sock_close(s);
        return NO_SOCK;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return s;
}

static int send_all(Sock s, const char *p, size_t n) {
    while (n > 0) {
#ifdef MSG_NOSIGNAL
        long sent = (long)send(s, p, n, MSG_NOSIGNAL);
#else
        long sent = (long)send(s, p, (int)n, 0);
#endif

        if (sent <= 0) {
            return 0;
        }
        p += sent;
        n -= (size_t)sent;
    }
    return 1;
}

static int send_line(Sock s, const char *line) { return s != NO_SOCK && send_all(s, line, strlen(line)); }

/* ---- a node's side: its line to the supervisor, its links to the other nodes ----
 *
 * A node has a listener of its own (a socket on this machine, its port told to the supervisor, who tells the
 * others) and one thread that only reads: the supervisor's line - stop, who the other nodes are, which has ended -
 * and what the other nodes send down the connections they make here: packets for this node's actors (put in its
 * inbox, as another thread's node would put them), and the names they register. That thread never sends and never
 * waits for anyone, so two nodes with much to say to each other can't each wait for the other to read.
 *
 * What a node sends, its own thread sends (the one that runs its Forth): down a connection it makes to the other
 * node's listener the first time, saying the pool's secret and who it is. A node that can't be sent to in
 * LINK_SEND_S - stuck, or gone and not yet seen to be - loses the message, as a send to a node that has ended does. */

#define FRAME_HELLO 1            /* a connection's first: the secret, who (index, life) */
#define FRAME_PACKET 2           /* a node's number, then a packet for it (nodes.c's, as it is) */
#define FRAME_NAME 3             /* an address, then the name registered for it */
#define FRAME_HEAD 5             /* a frame: its payload's length (4 bytes), its kind (1), the payload */
#define FRAME_BYTES_MAX ((size_t)1 << 30)
#define LINK_SEND_S 5            /* a send to a node that takes nothing, given up after this */
#define LINKS_IN_MAX (2 * FORTH_NODES_MAX)   /* connections from other nodes at once (a node started again: a new one) */

typedef struct Peer {            /* another node, as the supervisor last said */
    int known;
    int life, port;
} Peer;
typedef struct Out {             /* this node's connection to another (its own thread's alone) */
    Sock s;
    int life;
} Out;
typedef struct In {              /* a connection from another node (the reading thread's alone) */
    Sock s;
    int said;                    /* its hello taken */
    int place;                   /* ... from which node */
    uint8_t *buf;
    size_t n, cap;
} In;
typedef struct Named {
    char name[FORTH_ACTOR_NAME_MAX + 1];
    Cell address;
} Named;

static struct {
    pthread_mutex_t lock;        /* the peers and the names: never held while anything is sent or read */
    Sock listener;
    int port;
    Peer peer[FORTH_NODES_MAX];
    Out out[FORTH_NODES_MAX];
    In in[LINKS_IN_MAX];
    Named *theirs;               /* names the other nodes registered */
    int ntheirs, theirs_cap;
    Named *mine;                 /* names this node registered: said again to a node that joins */
    int nmine, mine_cap;
} sLink = {.lock = PTHREAD_MUTEX_INITIALIZER};

static int place_of_number(int number) { return (number - 1) % FORTH_NODES_MAX; }
static int life_of_number(int number) { return (number - 1) / FORTH_NODES_MAX; }

/* this process's node number, as its actors' addresses have it: its place its index, its generation its life - so an
 * address names one life of a node (0: no supervisor started this process) */
int forth_pool_number(void) {
    me();
    return sMe.supervised ? sMe.life * FORTH_NODES_MAX + sMe.index : 0;
}

static int frame_send(Sock s, int kind, const void *a, size_t an, const void *b, size_t bn) {
    uint8_t head[FRAME_HEAD];
    uint32_t n = (uint32_t)(an + bn);

    memcpy(head, &n, sizeof(n));
    head[sizeof(n)] = (uint8_t)kind;
    return send_all(s, (const char *)head, sizeof(head)) && (an == 0 || send_all(s, a, an)) &&
           (bn == 0 || send_all(s, b, bn));
}

/* this node's connection to the node at that place, in the life the supervisor last said (made if there is none,
 * its hello said); NO_SOCK if there is no such node now. The node's own thread. */
static Sock link_out(int place, int *life) {
    Out *o = &sLink.out[place];
    Peer p;

    pthread_mutex_lock(&sLink.lock);
    p = sLink.peer[place];
    pthread_mutex_unlock(&sLink.lock);
    if (o->s != NO_SOCK && (!p.known || o->life != p.life)) {   /* (to a life that is over) */
        sock_close(o->s);
        o->s = NO_SOCK;
    }
    if (!p.known) {
        return NO_SOCK;
    }
    *life = p.life;
    if (o->s == NO_SOCK) {
        struct { char secret[SECRET_HEX]; int32_t index, life; } hello;
#ifdef _WIN32
        DWORD wait = LINK_SEND_S * 1000;
#else
        struct timeval wait = {LINK_SEND_S, 0};
#endif

        if ((o->s = connect_local(p.port)) == NO_SOCK) {
            return NO_SOCK;
        }
        setsockopt(o->s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&wait, sizeof(wait));
        memcpy(hello.secret, sMe.secret, SECRET_HEX);
        hello.index = sMe.index;
        hello.life = sMe.life;
        o->life = p.life;
        if (!frame_send(o->s, FRAME_HELLO, &hello, sizeof(hello), NULL, 0)) {
            sock_close(o->s);
            o->s = NO_SOCK;
        }
    }
    return o->s;
}

static int link_send(int place, int life, int kind, const void *a, size_t an, const void *b, size_t bn) {
    int now = 0;
    Sock s = link_out(place, &now);

    if (s == NO_SOCK || (life != 0 && life != now)) {   /* (for a life that is over: no one's) */
        return 0;
    }
    if (!frame_send(s, kind, a, an, b, bn)) {
        sock_close(s);
        sLink.out[place].s = NO_SOCK;
        return 0;
    }
    return 1;
}

/* a packet to a node of another process (nodes.c's post): 1 sent, 0 no such node now */
int forth_pool_send(int number, const void *packet, size_t n) {
    int32_t to = number;

    if (!sMe.joined || number <= 0 || n > FRAME_BYTES_MAX - sizeof(to)) {
        return 0;
    }
    return link_send(place_of_number(number), life_of_number(number), FRAME_PACKET, &to, sizeof(to), packet, n);
}

static int named_put(Named **list, int *n, int *cap, const char *name, size_t len, Cell address, int place) {
    int i;

    for (i = 0; i < *n; i++) {   /* (the name again from that node: the newer) */
        if (strlen((*list)[i].name) == len && memcmp((*list)[i].name, name, len) == 0 &&
            (place < 0 || place_of_number((int)((*list)[i].address >> FORTH_NODE_SHIFT)) == place)) {
            (*list)[i].address = address;
            return 1;
        }
    }
    if (*n == *cap) {
        int more = *cap ? *cap * 2 : 16;
        Named *grown = realloc(*list, (size_t)more * sizeof(Named));

        if (grown == NULL) {
            return 0;
        }
        *list = grown;
        *cap = more;
    }
    memset(&(*list)[*n], 0, sizeof(Named));
    memcpy((*list)[*n].name, name, len);
    (*list)[*n].address = address;
    (*n)++;
    return 1;
}

static void name_to(int place, const Named *x) {
    link_send(place, 0, FRAME_NAME, &x->address, sizeof(x->address), x->name, strlen(x->name));
}

/* a name this node registered: kept, and said to the other nodes (the node's own thread) */
void forth_pool_named(const char *name, size_t len, Cell address) {
    Named x;
    int place;

    if (!sMe.joined || len == 0 || len > FORTH_ACTOR_NAME_MAX) {
        return;
    }
    memset(&x, 0, sizeof(x));
    memcpy(x.name, name, len);
    x.address = address;
    pthread_mutex_lock(&sLink.lock);
    named_put(&sLink.mine, &sLink.nmine, &sLink.mine_cap, name, len, address, -1);
    pthread_mutex_unlock(&sLink.lock);
    for (place = 0; place < FORTH_NODES_MAX; place++) {
        if (place != sMe.index - 1) {
            name_to(place, &x);
        }
    }
}

/* a node has joined: this node's names said to it (the node's own thread, told by the reading one) */
void forth_pool_greet(int number) {
    Named *copy = NULL;
    int n, i;

    pthread_mutex_lock(&sLink.lock);
    n = sLink.nmine;
    if (n > 0 && (copy = malloc((size_t)n * sizeof(Named))) != NULL) {
        memcpy(copy, sLink.mine, (size_t)n * sizeof(Named));
    }
    pthread_mutex_unlock(&sLink.lock);
    for (i = 0; copy != NULL && i < n; i++) {
        name_to(place_of_number(number), &copy[i]);
    }
    free(copy);
}

/* who another node registered by that name: the lowest node's, if several did (-1: no one) */
Cell forth_pool_whereis(const char *name, size_t len) {
    Cell found = -1;
    int i;

    if (!sMe.joined) {
        return -1;
    }
    pthread_mutex_lock(&sLink.lock);
    for (i = 0; i < sLink.ntheirs; i++) {
        const Named *x = &sLink.theirs[i];

        if (strlen(x->name) == len && memcmp(x->name, name, len) == 0 &&
            (found < 0 || place_of_number((int)(x->address >> FORTH_NODE_SHIFT)) <
                              place_of_number((int)(found >> FORTH_NODE_SHIFT)))) {
            found = x->address;
        }
    }
    pthread_mutex_unlock(&sLink.lock);
    return found;
}

/* (the reading thread) the supervisor says a node is there - its life, where it listens - or has ended */
static void peer_said(int index, int life, int port) {
    int place = index - 1, number = life * FORTH_NODES_MAX + index, i, kept = 0, gone = 0;

    if (index < 1 || index > FORTH_NODES_MAX || index == sMe.index) {
        return;
    }
    pthread_mutex_lock(&sLink.lock);
    if (port > 0) {
        sLink.peer[place].known = 1;
        sLink.peer[place].life = life;
        sLink.peer[place].port = port;
    } else if (sLink.peer[place].known && sLink.peer[place].life == life) {
        sLink.peer[place].known = 0;
        gone = 1;
        for (i = 0; i < sLink.ntheirs; i++) {   /* (its names: no one's now) */
            if ((int)(sLink.theirs[i].address >> FORTH_NODE_SHIFT) != number) {
                sLink.theirs[kept++] = sLink.theirs[i];
            }
        }
        sLink.ntheirs = kept;
    }
    pthread_mutex_unlock(&sLink.lock);
    if (port > 0) {
        forth_node_joined(number);   /* (this node's own thread says its names to it) */
    } else if (gone) {
        forth_node_gone(number);     /* (... and tells its actors that watched that node's) */
    }
}

static void control_line(const char *line) {
    int index, life, port;

    if (strcmp(line, "stop") == 0) {
        forth_shutdown_ask();
    } else if (sscanf(line, "peer %d %d %d", &index, &life, &port) == 3 && port > 0) {
        peer_said(index, life, port);
    } else if (sscanf(line, "gone %d %d", &index, &life) == 2) {
        peer_said(index, life, 0);
    }
}

/* a frame from another node's connection: 0 if it isn't one this pool's node would send (the connection closed) */
static int frame_taken(In *in, int kind, const uint8_t *p, size_t n) {
    if (kind == FRAME_HELLO) {
        struct { char secret[SECRET_HEX]; int32_t index, life; } hello;

        if (in->said || n != sizeof(hello)) {
            return 0;
        }
        memcpy(&hello, p, sizeof(hello));
        if (memcmp(hello.secret, sMe.secret, SECRET_HEX) != 0 || hello.index < 1 || hello.index > FORTH_NODES_MAX) {
            return 0;
        }
        in->said = 1;
        in->place = hello.index - 1;
        return 1;
    }
    if (!in->said) {
        return 0;
    }
    if (kind == FRAME_PACKET) {
        int32_t to;

        if (n < sizeof(to)) {
            return 0;
        }
        memcpy(&to, p, sizeof(to));
        forth_node_from_link(to, p + sizeof(to), n - sizeof(to));
        return 1;
    }
    if (kind == FRAME_NAME) {
        Cell address;
        size_t len = n - sizeof(address);

        if (n <= sizeof(address) || len > FORTH_ACTOR_NAME_MAX) {
            return 0;
        }
        memcpy(&address, p, sizeof(address));
        if (place_of_number((int)(address >> FORTH_NODE_SHIFT)) != in->place) {   /* (only its own actors') */
            return 0;
        }
        pthread_mutex_lock(&sLink.lock);
        named_put(&sLink.theirs, &sLink.ntheirs, &sLink.theirs_cap, (const char *)p + sizeof(address), len, address,
                  in->place);
        pthread_mutex_unlock(&sLink.lock);
        return 1;
    }
    return 0;
}

static void in_close(In *in) {
    sock_close(in->s);
    free(in->buf);
    memset(in, 0, sizeof(*in));
    in->s = NO_SOCK;
}

/* what has come down a connection: its whole frames taken */
static void in_read(In *in) {
    size_t at = 0;
    long got;

    if (in->cap - in->n < 65536) {
        size_t cap = in->cap ? in->cap * 2 : 65536;
        uint8_t *buf = realloc(in->buf, cap);

        if (buf == NULL) {
            in_close(in);
            return;
        }
        in->buf = buf;
        in->cap = cap;
    }
    got = (long)recv(in->s, (char *)in->buf + in->n, (int)(in->cap - in->n > INT32_MAX ? INT32_MAX : in->cap - in->n), 0);
    if (got <= 0) {
        in_close(in);
        return;
    }
    in->n += (size_t)got;
    while (in->n - at >= FRAME_HEAD) {
        uint32_t n;

        memcpy(&n, in->buf + at, sizeof(n));
        if (n > FRAME_BYTES_MAX) {
            in_close(in);
            return;
        }
        if (in->n - at - FRAME_HEAD < n) {
            break;
        }
        if (!frame_taken(in, in->buf[at + sizeof(n)], in->buf + at + FRAME_HEAD, n)) {
            in_close(in);
            return;
        }
        at += FRAME_HEAD + n;
    }
    in->n -= at;
    memmove(in->buf, in->buf + at, in->n);
}

/* the reading thread: the supervisor's line, the listener, the other nodes' connections. The supervisor's line
 * closed - the supervisor gone - is a word to stop too. */
static void *link_main(void *arg) {
    char line[POOL_LINE_MAX];
    size_t n = 0;
    int i;

    (void)arg;
    for (;;) {
        fd_set in;
        Sock top = sMe.control > sLink.listener ? sMe.control : sLink.listener;

        FD_ZERO(&in);
        FD_SET(sMe.control, &in);
        FD_SET(sLink.listener, &in);
        for (i = 0; i < LINKS_IN_MAX; i++) {
            if (sLink.in[i].s != NO_SOCK) {
                FD_SET(sLink.in[i].s, &in);
                top = sLink.in[i].s > top ? sLink.in[i].s : top;
            }
        }
        if (select((int)top + 1, &in, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (i = 0; i < LINKS_IN_MAX; i++) {
            if (sLink.in[i].s != NO_SOCK && FD_ISSET(sLink.in[i].s, &in)) {
                in_read(&sLink.in[i]);
            }
        }
        if (FD_ISSET(sLink.listener, &in)) {
            Sock s = accept(sLink.listener, NULL, NULL);

            for (i = 0; s != NO_SOCK && i < LINKS_IN_MAX && sLink.in[i].s != NO_SOCK; i++) {
            }
            if (s != NO_SOCK && i < LINKS_IN_MAX) {
                sLink.in[i].s = s;
            } else if (s != NO_SOCK) {
                sock_close(s);
            }
        }
        if (FD_ISSET(sMe.control, &in)) {
            long got = (long)recv(sMe.control, line + n, (int)(sizeof(line) - 1 - n), 0);
            char *eol;

            if (got <= 0) {
                break;
            }
            n += (size_t)got;
            while ((eol = memchr(line, '\n', n)) != NULL) {
                *eol = 0;
                control_line(line);
                n -= (size_t)(eol + 1 - line);
                memmove(line, eol + 1, n);
            }
            if (n == sizeof(line) - 1) {   /* (a line too long to be the supervisor's: let go) */
                n = 0;
            }
        }
    }
    forth_shutdown_ask();
    return NULL;
}

/* this node joins its pool: its listener made, its line to the supervisor, itself said */
static void join_pool(Forth *f) {
    char line[POOL_LINE_MAX];
    pthread_t thread;
    int i;

    if (sMe.joined) {
        return;
    }
    for (i = 0; i < LINKS_IN_MAX; i++) {
        sLink.in[i].s = NO_SOCK;
    }
    for (i = 0; i < FORTH_NODES_MAX; i++) {
        sLink.out[i].s = NO_SOCK;
    }
    if (!sockets_up() || (sLink.listener = listen_local(&sLink.port)) == NO_SOCK ||
        (sMe.control = connect_local(sMe.port)) == NO_SOCK) {
        forth_error(f, "as-nodes: no line to this node's supervisor (it has gone?)");
    }
    (void)forth_node_of(f);   /* (its place among the nodes taken: its index, its life - before anything comes for it) */
#ifdef _WIN32
    snprintf(line, sizeof(line), "hello %s %d %d %d %lu\n", sMe.secret, sMe.index, sMe.life, sLink.port,
             (unsigned long)GetCurrentProcessId());
#else
    snprintf(line, sizeof(line), "hello %s %d %d %d %ld\n", sMe.secret, sMe.index, sMe.life, sLink.port, (long)getpid());
#endif
    sMe.joined = 1;
    if (!send_line(sMe.control, line) || pthread_create(&thread, NULL, link_main, NULL) != 0) {
        sMe.joined = 0;
        sock_close(sMe.control);
        sMe.control = NO_SOCK;
        forth_error(f, "as-nodes: this node couldn't join its pool");
    }
    pthread_detach(thread);
}

/* ---- the supervisor ---- */

typedef struct Child {
    Proc proc;
    int running;                 /* its process is there */
    int done;                    /* it ended with status 0: not started again */
    int life;                    /* its lives so far (the running one's number) */
    int quick;                   /* lives in a row that ended as they started */
    long long born_ms;           /* when this life began */
    long long again_ms;          /* when its next is to begin; 0: none waiting */
    Sock control;                /* its line; NO_SOCK till it says hello */
    int link_port;               /* where the other nodes reach it (its hello's); 0 till then */
    long pid;
} Child;

/* a line not yet a node's: it has said nothing, or not all of its hello */
typedef struct Caller {
    Sock s;
    char buf[POOL_LINE_MAX];
    size_t n;
} Caller;

typedef struct Pool {
    int n;
    Child child[FORTH_NODES_MAX];
    Caller caller[FORTH_NODES_MAX];
    Sock listener;
    int port;
    char secret[SECRET_HEX + 1];
    int stopping;                /* told to stop, or giving up: no node is started again */
    long long kill_ms;           /* ... and those still there then are killed */
    int status;                  /* what the supervisor ends with */
} Pool;

static volatile sig_atomic_t sStop;   /* the supervisor told to stop (a signal's) */

#ifdef _WIN32
static BOOL WINAPI stop_event(DWORD event) {
    (void)event;
    sStop = 1;
    return TRUE;
}
static HANDLE sJob;   /* the nodes' job: the supervisor gone, however, the system ends them */
#else
static void stop_signal(int sig) {
    (void)sig;
    sStop = 1;
}
#endif

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;

    fputs("clankforth: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* node i's next life begun: its process, the program's command line again with who it is in its environment */
static int child_start(Pool *p, int i) {
    Child *c = &p->child[i];
    char value[POOL_LINE_MAX];

    snprintf(value, sizeof(value), "%d/%d/%d/%d/%s", i + 1, p->n, c->life + 1, p->port, p->secret);
#ifdef _WIN32
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char *command = _strdup(GetCommandLineA());
        BOOL made;

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (command == NULL) {
            return 0;
        }
        SetEnvironmentVariableA(POOL_ENV, value);
        made = CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
        SetEnvironmentVariableA(POOL_ENV, NULL);
        free(command);
        if (!made) {
            return 0;
        }
        if (sJob != NULL) {
            AssignProcessToJobObject(sJob, pi.hProcess);
        }
        CloseHandle(pi.hThread);
        c->proc = pi.hProcess;
        c->pid = (long)pi.dwProcessId;
    }
#else
    {
        char entry[sizeof(POOL_ENV) + 1 + POOL_LINE_MAX], exe[4096], **env;
        const char *path = sArgv[0];
        size_t n = 0, k = 0, i2;
        long got = (long)readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        pid_t pid;
        int failed;

        if (got > 0) {   /* (the program itself, wherever it was found; elsewhere: by its name, as it was) */
            exe[got] = 0;
            path = exe;
        }
        while (environ[n] != NULL) {
            n++;
        }
        if ((env = calloc(n + 2, sizeof(char *))) == NULL) {
            return 0;
        }
        for (i2 = 0; i2 < n; i2++) {   /* (the supervisor's environment, without a node's own of another pool) */
            if (strncmp(environ[i2], POOL_ENV "=", sizeof(POOL_ENV)) != 0) {
                env[k++] = environ[i2];
            }
        }
        snprintf(entry, sizeof(entry), "%s=%s", POOL_ENV, value);
        env[k++] = entry;
        failed = posix_spawnp(&pid, path, NULL, NULL, sArgv, env);
        free(env);
        if (failed != 0) {
            return 0;
        }
        c->proc = pid;
        c->pid = (long)pid;
    }
#endif
    c->life++;
    c->running = 1;
    c->born_ms = now_ms();
    c->again_ms = 0;
    c->control = NO_SOCK;
    c->link_port = 0;
    return 1;
}

/* has its process ended? *status: what it ended with (a signal's number, negative) */
static int child_ended(Child *c, int *status) {
#ifdef _WIN32
    DWORD code = 0;

    if (WaitForSingleObject(c->proc, 0) != WAIT_OBJECT_0) {
        return 0;
    }
    GetExitCodeProcess(c->proc, &code);
    CloseHandle(c->proc);
    *status = (int)code;
#else
    int st = 0;

    if (waitpid(c->proc, &st, WNOHANG) != c->proc) {
        return 0;
    }
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? -WTERMSIG(st) : -1;
#endif
    return 1;
}

static void child_kill(Child *c) {
#ifdef _WIN32
    TerminateProcess(c->proc, 1);
#else
    kill(c->proc, SIGKILL);
#endif
}

static void pool_stop(Pool *p, int status) {   /* the nodes told to stop (once); those still there after, killed */
    int i;

    if (p->stopping) {
        return;
    }
    p->stopping = 1;
    p->status = status;
    p->kill_ms = now_ms() + forth_shutdown_grace() + STOP_MARGIN_MS;
    for (i = 0; i < p->n; i++) {
        send_line(p->child[i].control, "stop\n");
    }
}

/* a node's hello: "hello secret index life port pid" - its line is its own from then */
static void pool_hello(Pool *p, Caller *who) {
    char secret[SECRET_HEX + 2];
    int index, life, port, i;
    long pid;
    Child *c;

    if (sscanf(who->buf, "hello %33s %d %d %d %ld", secret, &index, &life, &port, &pid) != 5 ||
        strcmp(secret, p->secret) != 0 || index < 1 || index > p->n || p->child[index - 1].life != life ||
        !p->child[index - 1].running || p->child[index - 1].control != NO_SOCK) {
        sock_close(who->s);   /* (not a node of this pool's, or not the one running now) */
        who->s = NO_SOCK;
        return;
    }
    c = &p->child[index - 1];
    c->control = who->s;
    c->link_port = port;
    who->s = NO_SOCK;
    if (p->stopping) {   /* (it came as the pool was stopping: told at once) */
        send_line(c->control, "stop\n");
    }
    for (i = 0; i < p->n; i++) {   /* (the nodes there told of it, and it of them) */
        Child *other = &p->child[i];
        char line[POOL_LINE_MAX];

        if (other == c || other->control == NO_SOCK || other->link_port == 0) {
            continue;
        }
        snprintf(line, sizeof(line), "peer %d %d %d\n", index, life, port);
        send_line(other->control, line);
        snprintf(line, sizeof(line), "peer %d %d %d\n", i + 1, other->life, other->link_port);
        send_line(c->control, line);
    }
}

/* what the nodes' lines have: new ones (to say hello), and those closed */
static void pool_lines(Pool *p) {
    fd_set in;
    struct timeval tick = {TICK_MS / 1000, (TICK_MS % 1000) * 1000};
    Sock top = p->listener;
    int i;

    FD_ZERO(&in);
    FD_SET(p->listener, &in);
    for (i = 0; i < FORTH_NODES_MAX; i++) {
        Sock s = p->caller[i].s;

        if (s != NO_SOCK) {
            FD_SET(s, &in);
            top = s > top ? s : top;
        }
        s = i < p->n ? p->child[i].control : NO_SOCK;
        if (s != NO_SOCK) {
            FD_SET(s, &in);
            top = s > top ? s : top;
        }
    }
    if (select((int)top + 1, &in, NULL, NULL, &tick) <= 0) {
        return;
    }
    if (FD_ISSET(p->listener, &in)) {
        Sock s = accept(p->listener, NULL, NULL);

        for (i = 0; s != NO_SOCK && i < FORTH_NODES_MAX && p->caller[i].s != NO_SOCK; i++) {
        }
        if (s != NO_SOCK && i < FORTH_NODES_MAX) {
            p->caller[i].s = s;
            p->caller[i].n = 0;
        } else if (s != NO_SOCK) {
            sock_close(s);
        }
    }
    for (i = 0; i < FORTH_NODES_MAX; i++) {
        Caller *who = &p->caller[i];

        if (who->s != NO_SOCK && FD_ISSET(who->s, &in)) {
            long got = (long)recv(who->s, who->buf + who->n, (int)(sizeof(who->buf) - 1 - who->n), 0);

            FD_CLR(who->s, &in);   /* (read: as a node's line, below, it has nothing more now - not read again) */
            if (got <= 0 || who->n + (size_t)got == sizeof(who->buf) - 1) {
                sock_close(who->s);
                who->s = NO_SOCK;
                continue;
            }
            who->n += (size_t)got;
            who->buf[who->n] = 0;
            if (memchr(who->buf, '\n', who->n) != NULL) {
                pool_hello(p, who);
            }
        }
    }
    for (i = 0; i < p->n; i++) {   /* (a node says nothing more on its line: read, it has closed it - or ended) */
        Child *c = &p->child[i];
        char scrap[POOL_LINE_MAX];

        if (c->control != NO_SOCK && FD_ISSET(c->control, &in) && recv(c->control, scrap, sizeof(scrap), 0) <= 0) {
            sock_close(c->control);
            c->control = NO_SOCK;
        }
    }
}

/* the nodes looked at: one ended is done (status 0), or started again - at once after a life of any length, later
 * and later after lives that ended as they started, and not at all after too many of those */
static void pool_children(Pool *p) {
    long long now = now_ms();
    int i, status;

    for (i = 0; i < p->n; i++) {
        Child *c = &p->child[i];

        if (c->running && child_ended(c, &status)) {
            char line[POOL_LINE_MAX];
            int j;

            c->running = 0;
            if (c->control != NO_SOCK) {
                sock_close(c->control);
                c->control = NO_SOCK;
            }
            snprintf(line, sizeof(line), "gone %d %d\n", i + 1, c->life);
            for (j = 0; j < p->n; j++) {   /* (the others told: what their actors watched of it is gone) */
                send_line(p->child[j].control, line);
            }
            c->link_port = 0;
            if (status == 0 || p->stopping) {
                c->done = 1;
                continue;
            }
            c->quick = now - c->born_ms < QUICK_MS ? c->quick + 1 : 0;
            if (c->quick >= QUICK_DEATHS_MAX) {
                say("node %d ended as it started, %d times running (status %d): its program is wrong - the pool "
                    "stops", i + 1, c->quick, status);
                c->done = 1;
                pool_stop(p, EXIT_GAVE_UP);
                continue;
            }
            c->again_ms = now + (c->quick == 0 ? 0 : c->quick > 6 ? BACKOFF_MAX_MS :
                                 BACKOFF_FIRST_MS << (c->quick - 1) > BACKOFF_MAX_MS ? BACKOFF_MAX_MS :
                                 BACKOFF_FIRST_MS << (c->quick - 1));
            if (status < 0) {
                say("node %d ended (signal %d): started again, its life %d", i + 1, -status, c->life + 1);
            } else {
                say("node %d ended (status %d): started again, its life %d", i + 1, status, c->life + 1);
            }
        }
        if (!c->running && !c->done && !p->stopping && c->again_ms <= now) {
            if (!child_start(p, i)) {
                say("node %d couldn't be started (%s)", i + 1, strerror(errno));
                c->again_ms = now + BACKOFF_MAX_MS;
            }
        }
    }
}

static int pool_over(const Pool *p) {   /* every node done (none running, none to start again)? */
    int i;

    for (i = 0; i < p->n; i++) {
        if (p->child[i].running || (!p->child[i].done && !p->stopping)) {
            return 0;
        }
    }
    return 1;
}

/* this process the supervisor of n nodes, till they are done or it is told to stop: it ends with the status */
static _Noreturn void supervise(Forth *f, int n) {
    static Pool pool;   /* (one: this process is nothing else from here) */
    Pool *p = &pool;
    uint8_t secret[SECRET_BYTES];
    int i;

    if (sArgv == NULL || sArgc < 1) {
        forth_error(f, "as-nodes: how this program is started isn't known here (a host says: forth_set_program)");
    }
    forth_secure_random(f, secret, sizeof(secret));
    for (i = 0; i < SECRET_BYTES; i++) {
        snprintf(p->secret + 2 * i, 3, "%02x", secret[i]);
    }
    if (!sockets_up() || (p->listener = listen_local(&p->port)) == NO_SOCK) {
        forth_error(f, "as-nodes: no socket on this machine for the nodes' lines (%s)", strerror(errno));
    }
    p->n = n;
    for (i = 0; i < FORTH_NODES_MAX; i++) {
        p->caller[i].s = NO_SOCK;
        p->child[i].control = NO_SOCK;
    }
    fflush(NULL);
#ifdef _WIN32
    SetConsoleCtrlHandler(stop_event, TRUE);
    if ((sJob = CreateJobObjectA(NULL, NULL)) != NULL) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;

        memset(&limits, 0, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(sJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
#else
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGPIPE, SIG_IGN);
#endif
    for (;;) {
        if (sStop) {
            pool_stop(p, 0);
        }
        pool_children(p);
        if (p->stopping && now_ms() > p->kill_ms) {
            for (i = 0; i < p->n; i++) {
                if (p->child[i].running) {
                    say("node %d didn't end when told to: ended", i + 1);
                    child_kill(&p->child[i]);
                }
            }
            p->kill_ms += STOP_MARGIN_MS;   /* (and seen gone, in a moment) */
        }
        if (pool_over(p)) {
            break;
        }
        pool_lines(p);
    }
    fflush(NULL);
    exit(p->status);
}

/* ---- the words ---- */

/* as-nodes ( n -- ) this program as n nodes, each a process: here the supervisor of them (it never comes back); in a
 * node, nothing - the program goes on */
static void p_as_nodes(Forth *f, Word *w) {
    Cell n = forth_pop(f);

    (void)w;
    me();
    if (n < 1 || n > FORTH_NODES_MAX) {
        forth_error(f, "as-nodes: %ld nodes - 1 to %d", (long)n, FORTH_NODES_MAX);
    }
    if (sMe.supervised) {
        join_pool(f);
        return;
    }
    supervise(f, (int)n);
}

#else   /* (no threads: one node, this process) */

int forth_pool_supervised(void) { return 0; }
int forth_pool_index(void) { return 1; }
int forth_pool_life(void) { return 1; }
int forth_pool_number(void) { return 0; }
int forth_pool_send(int number, const void *packet, size_t n) { (void)number, (void)packet, (void)n; return 0; }
void forth_pool_named(const char *name, size_t len, Cell address) { (void)name, (void)len, (void)address; }
void forth_pool_greet(int number) { (void)number; }
Cell forth_pool_whereis(const char *name, size_t len) { (void)name, (void)len; return -1; }
static void p_as_nodes(Forth *f, Word *w) {
    (void)w;
    (void)forth_pop(f);
    forth_error(f, "as-nodes: a system built without nodes (no threads) is one node, this process");
}

#endif

static void p_node_index(Forth *f, Word *w) { (void)w; forth_push(f, forth_pool_index()); }   /* node-index ( -- i ) */
static void p_node_life(Forth *f, Word *w) { (void)w; forth_push(f, forth_pool_life()); }     /* node-life ( -- k ) */
static void p_node_count(Forth *f, Word *w) {   /* node-count ( -- n ) the nodes of this program's pool: 1 alone */
    (void)w;
#if defined(CLANKFORTH_NODES)
    me();
    forth_push(f, sMe.count);
#else
    forth_push(f, 1);
#endif
}

/* (system.c's: with the system vocabulary's words) */
void forth_pool_words(Forth *f) {
    static const struct { const char *name; Code code; const char *effect; } words[] = {
        {"as-nodes", p_as_nodes, "( n -- )"}, {"node-index", p_node_index, "( -- i )"},
        {"node-count", p_node_count, "( -- n )"}, {"node-life", p_node_life, "( -- k )"},
    };
    size_t i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        forth_set_effect(f, forth_prim(f, words[i].name, words[i].code), words[i].effect);
    }
}

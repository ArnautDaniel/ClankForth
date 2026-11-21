/* The net vocabulary: TCP sockets and timers, on the actor kernel. A socket is an actor's: what happens on it comes
 * to that actor as a message, as everything else does - a connection accepted (net-accepted), bytes come
 * (net-received), the other end gone (net-closed), a connection made (net-connected); and a timer's time come
 * (timer-fired). An event loop waits for all of it - and for other nodes' messages - and the frames run between
 * (`serve`, or a host's own loop calling forth_net_poll). A host gives its scripts these words with forth_net; the
 * tool does. Scripts say USING: net ; Docs: docs/net.md. */
#include "forth_internal.h"
#include "actors.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   /* (Vista's: WSAPoll, inet_ntop) */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
typedef WSAPOLLFD PollFd;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define poll_sockets(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))
static int sock_error(void) { return WSAGetLastError(); }
static int would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR; }
static int set_nonblocking(Sock s) {
    u_long on = 1;

    return ioctlsocket(s, FIONBIO, &on);
}
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
typedef struct pollfd PollFd;
#define BAD_SOCK (-1)
#define sock_close close
#define poll_sockets(fds, n, ms) poll((fds), (nfds_t)(n), (ms))
static int sock_error(void) { return errno; }
static int would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EINTR; }
static int set_nonblocking(Sock s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL   /* (the other end gone: an error to say, not SIGPIPE killing the program) */
#else
#define SEND_FLAGS 0
#endif
#endif

#define CONN_SLOT_BITS 20               /* a connection's handle: its slot under these bits, its generation over */
#define CONNS_MAX (1 << CONN_SLOT_BITS)
#define CONN_GENERATIONS ((INT32_MAX >> CONN_SLOT_BITS) + 1)
#define READ_CHUNK 65536                /* bytes read at once from a connection */
#define LISTEN_BACKLOG 128
#define FRAME_MS 16                     /* `serve`: a frame at least this often (the tasks' waits count them) */
#define NS_PER_MS 1000000LL
#define MS_PER_S 1000LL
#define PEER_BYTES 64                   /* "address:port" */

enum { CONN_FREE, CONN_LISTENER, CONN_CONNECTING, CONN_OPEN };

typedef struct Conn {
    int state;          /* CONN_* */
    int generation;
    Sock sock;
    int owner;          /* the actor it is: told what happens on it (gone: the connection closed) */
    uint8_t *in;        /* what has come and not been taken (net-consume) */
    size_t in_n, in_cap;
    uint8_t *out;       /* what is still to go, from out_at */
    size_t out_at, out_n, out_cap;
    int closing;        /* net-close: closed once out has gone */
    int ended;          /* the other end gone (net-closed told): freed at the next poll */
    char peer[PEER_BYTES];
} Conn;

typedef struct Timer {
    Cell id;            /* 0: free */
    int owner;
    long long due;      /* ms on the monotonic clock */
} Timer;

struct ForthNet {
    Conn *conns;
    int nconns, conns_cap;
    Timer *timers;
    int ntimers, timers_cap;
    Cell next_timer;
    Sock wake[2];       /* (a pair: written by another node's thread as a message comes, read here to wake) */
    int k_accepted, k_received, k_closed, k_connected, k_timer;
    PollFd *fds;
    int *fd_conn;       /* each polled socket's connection (-1: the wake pair) */
    int fds_cap;
};

#define N (f->net)

static long long now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * MS_PER_S + ts.tv_nsec / NS_PER_MS;
}

/* ---- the wake pair: a socket pair (Windows: two loopback sockets) another thread writes a byte to ---- */

#ifdef _WIN32
static int wake_pair(Sock s[2]) {
    struct sockaddr_in a;
    int len = sizeof(a);
    Sock l = socket(AF_INET, SOCK_STREAM, 0);

    s[0] = s[1] = BAD_SOCK;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l == BAD_SOCK || bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &len) != 0 || (s[1] = socket(AF_INET, SOCK_STREAM, 0)) == BAD_SOCK ||
        connect(s[1], (struct sockaddr *)&a, sizeof(a)) != 0 || (s[0] = accept(l, NULL, NULL)) == BAD_SOCK) {
        if (l != BAD_SOCK) {
            sock_close(l);
        }
        return -1;
    }
    sock_close(l);
    set_nonblocking(s[0]);
    set_nonblocking(s[1]);
    return 0;
}
#else
static int wake_pair(Sock s[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0) {
        return -1;
    }
    set_nonblocking(s[0]);
    set_nonblocking(s[1]);
    return 0;
}
#endif

static void wake_up(void *ctx) {   /* (another thread's: a byte - one is enough, a full pair already wakes it) */
    struct ForthNet *net = ctx;
    char b = 1;

    (void)send(net->wake[1], &b, 1, SEND_FLAGS);
}

static void wake_drain(struct ForthNet *net) {
    char buf[64];

    while (recv(net->wake[0], buf, sizeof(buf), 0) > 0) {
    }
}

/* ---- connections: a handle is the slot and, over it, its generation (a closed one's names nobody) ---- */

static Cell conn_handle(const struct ForthNet *net, int slot) {
    return (Cell)net->conns[slot].generation << CONN_SLOT_BITS | slot;
}

static Conn *conn_at(Forth *f, Cell h) {   /* NULL: none of that handle (closed, or never) */
    int slot = (int)(h & (CONNS_MAX - 1));

    if (h < 0 || h > INT32_MAX || slot >= N->nconns || N->conns[slot].state == CONN_FREE ||
        N->conns[slot].generation != (int)(h >> CONN_SLOT_BITS)) {
        return NULL;
    }
    return &N->conns[slot];
}

static Conn *conn_arg(Forth *f, Cell h, const char *word) {
    Conn *c = conn_at(f, h);

    if (c == NULL) {
        forth_error(f, "%s: no connection %ld (closed?)", word, (long)h);
    }
    return c;
}

/* a free slot (the table grown if it must be): its number */
static int conn_new(Forth *f, Sock s, int state, int owner) {
    int slot;
    Conn *c;

    for (slot = 0; slot < N->nconns && N->conns[slot].state != CONN_FREE; slot++) {
    }
    if (slot == N->nconns) {
        if (N->nconns == N->conns_cap) {
            int cap = N->conns_cap ? N->conns_cap * 2 : 16;
            Conn *more = cap <= CONNS_MAX ? realloc(N->conns, (size_t)cap * sizeof(Conn)) : NULL;

            if (more == NULL) {
                sock_close(s);
                forth_error(f, "net: out of memory for connections (%d open)", N->nconns);
            }
            memset(more + N->conns_cap, 0, (size_t)(cap - N->conns_cap) * sizeof(Conn));
            N->conns = more;
            N->conns_cap = cap;
        }
        N->nconns++;
    }
    c = &N->conns[slot];
    c->state = state;
    c->sock = s;
    c->owner = owner;
    c->in_n = c->out_n = c->out_at = 0;
    c->closing = c->ended = 0;
    c->peer[0] = 0;
    return slot;
}

static void conn_free(Forth *f, Conn *c) {
    (void)f;
    if (c->sock != BAD_SOCK) {
        sock_close(c->sock);
    }
    free(c->in);
    free(c->out);
    c->in = c->out = NULL;
    c->in_cap = c->out_cap = 0;
    c->sock = BAD_SOCK;
    c->state = CONN_FREE;
    c->generation = (c->generation + 1) % CONN_GENERATIONS;
}

static int self_actor(Forth *f, const char *word) {
    int me = forth_actor_self(f);

    if (me < 0) {
        forth_error(f, "%s: only an actor's (inside a handler, or `enter`ed): what happens is told to it", word);
    }
    return me;
}

static void tell(Forth *f, int to, int kind, Cell a, Cell b, int n) {
    Cell args[2] = {a, b};

    forth_actor_send(f, -1, to, kind, args, n);
}

static void describe_peer(Conn *c, const struct sockaddr_storage *sa) {
    char host[INET6_ADDRSTRLEN] = "?";
    int port = 0;

    if (sa->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;

        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        port = ntohs(in->sin_port);
    } else if (sa->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;

        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
    }
    snprintf(c->peer, sizeof(c->peer), "%s:%d", host, port);
}

static void no_delay(Sock s) {   /* (a reply goes now, not held back to join the next) */
    int on = 1;

    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
}

/* ---- the words ---- */

/* listen ( port -- listener ) TCP on every address of this machine (port 0: any free one - listener-port says);
 * net-accepted ( listener conn -- ) comes to this actor for each connection, the connection its */
static void p_listen(Forth *f, Word *w) {
    Cell port = forth_pop(f);
    int me = self_actor(f, "listen"), on = 1;
    struct sockaddr_in a;
    Sock s;

    (void)w;
    if (port < 0 || port > 65535) {
        forth_error(f, "listen: no port %ld", (long)port);
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BAD_SOCK) {
        forth_error(f, "listen: no socket (%s)", strerror(sock_error()));
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
#ifdef SO_REUSEPORT   /* (each node may listen on the port: the system spreads the connections over them) */
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof(on));
#endif
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, LISTEN_BACKLOG) != 0 || set_nonblocking(s) != 0) {
        int e = sock_error();

        sock_close(s);
        forth_error(f, "listen: port %ld: %s", (long)port, strerror(e));
    }
    forth_push(f, conn_handle(N, conn_new(f, s, CONN_LISTENER, me)));
}

static void p_listener_port(Forth *f, Word *w) {   /* listener-port ( listener -- port ) */
    Conn *c = conn_arg(f, forth_pop(f), "listener-port");
    struct sockaddr_in a;
    socklen_t len = sizeof(a);

    (void)w;
    if (getsockname(c->sock, (struct sockaddr *)&a, &len) != 0) {
        forth_error(f, "listener-port: %s", strerror(sock_error()));
    }
    forth_push(f, ntohs(a.sin_port));
}

/* connect ( host n port -- conn ) to host (a name or an address) - the name looked up now, the connection made as the
 * loop runs: net-connected ( conn -- ) to this actor, or net-closed ( conn -- ) if it couldn't be */
static void p_connect(Forth *f, Word *w) {
    Cell port = forth_pop(f), n = forth_pop(f), a = forth_pop(f);
    int me = self_actor(f, "connect"), slot;
    size_t len;
    const char *h = forth_string_arg(f, a, n, "connect", &len);
    char host[256], service[16];
    struct addrinfo hints, *res = NULL;
    Sock s;
    int r;

    (void)w;
    if (len >= sizeof(host) || port < 1 || port > 65535) {
        forth_error(f, "connect: no host \"%.*s\", port %ld", (int)len, h, (long)port);
    }
    memcpy(host, h, len);
    host[len] = 0;
    snprintf(service, sizeof(service), "%ld", (long)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((r = getaddrinfo(host, service, &hints, &res)) != 0 || res == NULL) {
        forth_error(f, "connect: %s: %s", host, gai_strerror(r));
    }
    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == BAD_SOCK || set_nonblocking(s) != 0) {
        int e = sock_error();

        if (s != BAD_SOCK) {
            sock_close(s);
        }
        freeaddrinfo(res);
        forth_error(f, "connect: no socket (%s)", strerror(e));
    }
    r = connect(s, res->ai_addr, (socklen_t)res->ai_addrlen);
    if (r != 0 && !would_block(sock_error())) {
        int e = sock_error();

        sock_close(s);
        freeaddrinfo(res);
        forth_error(f, "connect: %s:%ld: %s", host, (long)port, strerror(e));
    }
    no_delay(s);
    slot = conn_new(f, s, CONN_CONNECTING, me);
    snprintf(N->conns[slot].peer, sizeof(N->conns[slot].peer), "%.40s:%ld", host, (long)port);
    freeaddrinfo(res);
    forth_push(f, conn_handle(N, slot));
}

/* net-write ( a n conn -- ) the bytes to go (kept till they can: the loop sends them) */
static void p_net_write(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-write");
    Cell n = forth_pop(f), a = forth_pop(f);
    size_t len;
    const char *s = forth_string_arg(f, a, n, "net-write", &len);

    (void)w;
    if (c->state == CONN_LISTENER || c->ended || c->closing) {
        return;   /* (nowhere for them to go: dropped, as a message to an actor gone is) */
    }
    if (c->out_at > 0 && c->out_at == c->out_n) {
        c->out_at = c->out_n = 0;
    }
    if (c->out_n + len > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : READ_CHUNK;
        uint8_t *more;

        while (cap < c->out_n + len) {
            cap *= 2;
        }
        if ((more = realloc(c->out, cap)) == NULL) {
            forth_error(f, "net-write: out of memory for %zu bytes to go", c->out_n + len);
        }
        c->out = more;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_n, s, len);
    c->out_n += len;
}

/* net-input ( conn -- a n ) what has come and not been taken: its address good till the next poll, or net-consume */
static void p_net_input(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-input");

    (void)w;
    forth_push(f, (Cell)(c->in != NULL ? (const char *)c->in : ""));
    forth_push(f, (Cell)c->in_n);
}

static void p_net_consume(Forth *f, Word *w) {   /* net-consume ( n conn -- ) the first n bytes of its input taken */
    Conn *c = conn_arg(f, forth_pop(f), "net-consume");
    Cell n = forth_pop(f);

    (void)w;
    if (n < 0 || (size_t)n > c->in_n) {
        forth_error(f, "net-consume: %ld bytes of %zu", (long)n, c->in_n);
    }
    memmove(c->in, c->in + n, c->in_n - (size_t)n);
    c->in_n -= (size_t)n;
}

/* net-close ( conn -- ) closed once what is to go has gone (a listener: at once). No net-closed for it. */
static void p_net_close(Forth *f, Word *w) {
    Conn *c = conn_at(f, forth_pop(f));

    (void)w;
    if (c == NULL) {
        return;   /* (closed already) */
    }
    if (c->state != CONN_OPEN || c->out_at == c->out_n || c->ended) {
        conn_free(f, c);
    } else {
        c->closing = 1;
    }
}

static void p_net_owner(Forth *f, Word *w) {   /* net-owner! ( actor conn -- ) told what happens on it from now */
    Conn *c = conn_arg(f, forth_pop(f), "net-owner!");
    Cell id = forth_pop(f);

    (void)w;
    if (forth_actor_body(f, (int)id) == NULL || id < 0 || id > INT32_MAX) {
        forth_error(f, "net-owner!: no actor %ld", (long)id);
    }
    c->owner = (int)id;
}

static void p_conn_peer(Forth *f, Word *w) {   /* conn-peer ( conn -- s len ) "address:port" of its other end */
    Conn *c = conn_arg(f, forth_pop(f), "conn-peer");
    size_t len = strlen(c->peer);

    (void)w;
    forth_push(f, (Cell)forth_new_string(f, c->peer, len));
    forth_push(f, (Cell)len);
}

/* after ( ms -- timer ) timer-fired ( timer -- ) to this actor once ms have gone by (the clock's, not frames) */
static void p_after(Forth *f, Word *w) {
    Cell ms = forth_pop(f);
    int me = self_actor(f, "after"), i;

    (void)w;
    for (i = 0; i < N->ntimers && N->timers[i].id != 0; i++) {
    }
    if (i == N->ntimers) {
        if (N->ntimers == N->timers_cap) {
            int cap = N->timers_cap ? N->timers_cap * 2 : 16;
            Timer *more = realloc(N->timers, (size_t)cap * sizeof(Timer));

            if (more == NULL) {
                forth_error(f, "after: out of memory for timers");
            }
            N->timers = more;
            N->timers_cap = cap;
        }
        N->ntimers++;
    }
    N->timers[i].id = ++N->next_timer;
    N->timers[i].owner = me;
    N->timers[i].due = now_ms() + (ms > 0 ? (long long)ms : 0);
    forth_push(f, N->timers[i].id);
}

static void p_cancel_timer(Forth *f, Word *w) {   /* cancel-timer ( timer -- ) (fired or cancelled already: nothing) */
    Cell id = forth_pop(f);
    int i;

    (void)w;
    for (i = 0; i < N->ntimers; i++) {
        if (N->timers[i].id == id) {
            N->timers[i].id = 0;
        }
    }
}

/* ---- the loop ---- */

static int fds_room(Forth *f, int n) {
    if (n > N->fds_cap) {
        PollFd *fds = realloc(N->fds, (size_t)n * sizeof(PollFd));
        int *which;

        if (fds == NULL) {
            return 0;
        }
        N->fds = fds;
        if ((which = realloc(N->fd_conn, (size_t)n * sizeof(int))) == NULL) {
            return 0;
        }
        N->fd_conn = which;
        N->fds_cap = n;
    }
    return 1;
}

static void accept_all(Forth *f, int slot) {
    for (;;) {
        struct sockaddr_storage sa;
        socklen_t len = sizeof(sa);
        Sock s = accept(N->conns[slot].sock, (struct sockaddr *)&sa, &len);
        int c;

        if (s == BAD_SOCK) {
            return;   /* (none more now; an error - out of descriptors - tried again next time) */
        }
        set_nonblocking(s);
        no_delay(s);
        c = conn_new(f, s, CONN_OPEN, N->conns[slot].owner);   /* (the table may move: by slot from here) */
        describe_peer(&N->conns[c], &sa);
        tell(f, N->conns[c].owner, N->k_accepted, conn_handle(N, slot), conn_handle(N, c), 2);
    }
}

static void ended(Forth *f, Conn *c, int slot) {   /* the other end gone, or an error: net-closed, freed next poll */
    if (!c->ended) {
        c->ended = 1;
        tell(f, c->owner, N->k_closed, conn_handle(N, slot), 0, 1);
    }
}

static void read_all(Forth *f, int slot) {
    Conn *c = &N->conns[slot];
    int got_any = 0;

    for (;;) {
        long got;

        if (c->in_cap - c->in_n < READ_CHUNK) {
            uint8_t *more = realloc(c->in, c->in_cap + READ_CHUNK);

            if (more == NULL) {
                break;   /* (read again next time) */
            }
            c->in = more;
            c->in_cap += READ_CHUNK;
        }
        got = (long)recv(c->sock, (char *)c->in + c->in_n, (int)(c->in_cap - c->in_n), 0);
        if (got > 0) {
            c->in_n += (size_t)got;
            got_any = 1;
            continue;
        }
        if (got == 0 || !would_block(sock_error())) {
            if (got_any) {
                tell(f, c->owner, N->k_received, conn_handle(N, slot), 0, 1);
            }
            ended(f, c, slot);
            return;
        }
        break;
    }
    if (got_any) {
        tell(f, c->owner, N->k_received, conn_handle(N, slot), 0, 1);
    }
}

static void write_some(Forth *f, int slot) {
    Conn *c = &N->conns[slot];

    while (c->out_at < c->out_n) {
        long sent = (long)send(c->sock, (const char *)c->out + c->out_at, (int)(c->out_n - c->out_at), SEND_FLAGS);

        if (sent > 0) {
            c->out_at += (size_t)sent;
        } else {
            if (!would_block(sock_error())) {
                ended(f, c, slot);
            }
            return;
        }
    }
    c->out_at = c->out_n = 0;
    if (c->closing) {
        conn_free(f, c);
    }
}

static void connect_done(Forth *f, int slot) {
    Conn *c = &N->conns[slot];
    int err = 0;
    socklen_t len = sizeof(err);

    getsockopt(c->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
    if (err != 0) {
        ended(f, c, slot);
        return;
    }
    c->state = CONN_OPEN;
    tell(f, c->owner, N->k_connected, conn_handle(N, slot), 0, 1);
}

/* one round: waits up to ms (less if a timer is due sooner) for the sockets or a message from another node, then
 * what happened told to the actors (queued: the next delivery runs their handlers); 1 if anything did */
int forth_net_poll(Forth *f, long ms) {
    long long now = now_ms();
    int i, n = 0, any = 0, ready;

    if (N == NULL) {
        return forth_node_wait(f, ms);
    }
    for (i = 0; i < N->nconns; i++) {   /* (the ended - told last round - and those whose actor is gone: closed;
                                          * one closing still sends what it has, its actor gone or not) */
        Conn *c = &N->conns[i];

        if (c->state != CONN_FREE && (c->ended || (forth_actor_body(f, c->owner) == NULL && !c->closing))) {
            conn_free(f, c);
        }
    }
    for (i = 0; i < N->ntimers; i++) {
        if (N->timers[i].id != 0 && N->timers[i].due - now < ms) {
            ms = N->timers[i].due - now > 0 ? (long)(N->timers[i].due - now) : 0;
        }
    }
    if (!fds_room(f, N->nconns + 1)) {
        forth_error(f, "net: out of memory for the poll");
    }
    N->fds[n].fd = N->wake[0];
    N->fds[n].events = POLLIN;
    N->fds[n].revents = 0;
    N->fd_conn[n++] = -1;
    for (i = 0; i < N->nconns; i++) {
        Conn *c = &N->conns[i];
        short ev = c->state == CONN_CONNECTING ? POLLOUT : c->state == CONN_LISTENER ? POLLIN
                   : (short)(POLLIN | (c->out_at < c->out_n ? POLLOUT : 0));

        if (c->state == CONN_FREE) {
            continue;
        }
        N->fds[n].fd = c->sock;
        N->fds[n].events = ev;
        N->fds[n].revents = 0;
        N->fd_conn[n++] = i;
    }
    ready = poll_sockets(N->fds, n, (int)(ms > INT32_MAX ? INT32_MAX : ms));
    for (i = 0; ready > 0 && i < n; i++) {
        int slot = N->fd_conn[i];
        short ev = N->fds[i].revents;

        if (ev == 0) {
            continue;
        }
        any = 1;
        if (slot < 0) {
            wake_drain(N);
            continue;
        }
        if (N->conns[slot].state == CONN_FREE) {
            continue;
        }
        switch (N->conns[slot].state) {
        case CONN_LISTENER:
            accept_all(f, slot);
            break;
        case CONN_CONNECTING:
            connect_done(f, slot);
            break;
        default:
            if (ev & (POLLIN | POLLHUP | POLLERR)) {
                read_all(f, slot);
            }
            if ((ev & POLLOUT) && N->conns[slot].state == CONN_OPEN && !N->conns[slot].ended) {
                write_some(f, slot);
            }
        }
    }
    now = now_ms();
    for (i = 0; i < N->ntimers; i++) {
        if (N->timers[i].id != 0 && N->timers[i].due <= now) {
            Cell id = N->timers[i].id;

            N->timers[i].id = 0;
            tell(f, N->timers[i].owner, N->k_timer, id, 0, 1);
            any = 1;
        }
    }
    for (i = 0; i < N->nconns; i++) {   /* (what was written this round: sent now, not a round later) */
        Conn *c = &N->conns[i];

        if (c->state == CONN_OPEN && !c->ended && c->out_at < c->out_n) {
            write_some(f, i);
        }
    }
    return any;
}

static int event_wait(Forth *f, long ms) { return forth_net_poll(f, ms); }

static void p_net_poll(Forth *f, Word *w) {   /* net-poll ( ms -- flag ) one round of the loop (no delivery) */
    Cell ms = forth_pop(f);

    (void)w;
    forth_push(f, forth_net_poll(f, ms > 0 ? (long)ms : 0) ? -1 : 0);
}

/* serve ( -- ) the loop, until `bye`: the tasks' turns, the actors' frame, then a wait for what comes (a frame at
 * least every 16 ms). From the interpreter (a program's last line). */
static void p_serve(Forth *f, Word *w) {
    (void)w;
    if (f->t != f->main) {
        forth_error(f, "serve: only from the interpreter (it runs the tasks and the actors itself)");
    }
    while (!forth_bye(f)) {
        forth_run_tasks(f);
        forth_actors_frame(f);
        forth_net_poll(f, FRAME_MS);
    }
}

void forth_net_free(Forth *f) {
    int i;

    if (N == NULL) {
        return;
    }
    if (f->node != 0) {
        forth_node_set_wake(f, NULL, NULL);   /* (no thread to write to these after they're gone) */
    }
    for (i = 0; i < N->nconns; i++) {
        if (N->conns[i].state != CONN_FREE) {
            conn_free(f, &N->conns[i]);
        }
    }
    sock_close(N->wake[0]);
    sock_close(N->wake[1]);
    free(N->conns);
    free(N->timers);
    free(N->fds);
    free(N->fd_conn);
    free(N);
    N = NULL;
}

void forth_net(Forth *f) {
    static const ForthPrim prims[] = {
        {"listen", p_listen, "( port -- listener )"}, {"listener-port", p_listener_port, "( listener -- port )"},
        {"connect", p_connect, "( host n port -- conn )"}, {"net-write", p_net_write, "( a n conn -- )"},
        {"net-input", p_net_input, "( conn -- a n )"}, {"net-consume", p_net_consume, "( n conn -- )"},
        {"net-close", p_net_close, "( conn -- )"}, {"net-owner!", p_net_owner, "( actor conn -- )"},
        {"conn-peer", p_conn_peer, "( conn -- s len )"}, {"after", p_after, "( ms -- timer )"},
        {"cancel-timer", p_cancel_timer, "( timer -- )"}, {"net-poll", p_net_poll, "( ms -- flag )"},
        {"serve", p_serve, "( -- )"},
    };
    Vocab *was = forth_current(f), *v;

    if (N != NULL) {
        return;
    }
    if (f->actors == NULL) {
        forth_error(f, "forth_net: the actor kernel first (forth_actors): sockets are actors'");
    }
#ifdef _WIN32
    {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            forth_error(f, "forth_net: no Winsock");
        }
    }
#endif
    if ((N = calloc(1, sizeof(struct ForthNet))) == NULL) {
        forth_error(f, "forth_net: out of memory");
    }
    if (wake_pair(N->wake) != 0) {
        free(N);
        N = NULL;
        forth_error(f, "forth_net: no socket pair to wake the loop");
    }
    N->k_accepted = forth_actors_declare(f, "net-accepted", 2);
    N->k_received = forth_actors_declare(f, "net-received", 1);
    N->k_closed = forth_actors_declare(f, "net-closed", 1);
    N->k_connected = forth_actors_declare(f, "net-connected", 1);
    N->k_timer = forth_actors_declare(f, "timer-fired", 1);
    f->event_wait = event_wait;
#if defined(CLANKFORTH_NODES)
    forth_node_set_wake(f, wake_up, N);   /* (another node's message wakes the loop as a socket does) */
#else
    (void)wake_up;
#endif
    v = forth_vocab(f, "net");
    forth_set_current(f, v);   /* scripts say USING: net ; */
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_set_current(f, was);
}

/* The net vocabulary: TCP sockets and timers, on the actor kernel. A socket is an actor's: what happens on it comes
 * to that actor as a message, as everything else does - a connection accepted (net-accepted), bytes come
 * (net-received), the other end gone (net-closed), a connection made (net-connected); and a timer's time come
 * (timer-fired). An event loop waits for all of it - and for other nodes' messages - and the frames run between
 * (`serve`, or a host's own loop calling forth_net_poll). A host gives its scripts these words with forth_net; the
 * tool does. Scripts say USING: net ; Docs: docs/net.md.
 *
 * Built with OpenSSL (CLANKFORTH_TLS), a connection may be TLS's: tls-listen's accepted ones, tls-connect's. The
 * handshake goes on in the loop as the bytes come; the words and messages are the same as for plain TCP.
 *
 * A repl (`repl-listen`, on this machine only): each connection a session no actor owns, whose lines are run as the
 * prompt's are (forth_eval) - on a task of the session's own (its stack), as the actor it has attached - in the loop
 * (forth_net_poll from the interpreter: never inside a handler), each answered with what it printed and a last line,
 * `ok` (`ok name#id` attached), `compiling` or `error`. A line that leaves a definition open is let go and kept -
 * `compiling` - and run again with the next: the dictionary is everyone's, so no definition stays open between lines
 * (another session's, the host's own prompt, would land in the middle of it). */
#include "forth_internal.h"
#include "actors.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef CLANKFORTH_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

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
#define SHUT_WR SD_SEND
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

#ifdef CLANKFORTH_EPOLL
#include <sys/epoll.h>
#endif

#define CONN_SLOT_BITS 20               /* a connection's handle: its slot under these bits, its generation over */
#define CONNS_MAX (1 << CONN_SLOT_BITS)
#define CONN_GENERATIONS ((INT32_MAX >> CONN_SLOT_BITS) + 1)
#define READ_CHUNK 65536                /* bytes read at once from a file or a program's output */
#define BUF_START 4096                  /* a connection's buffers begin this big, doubling as they need - and are let go
                                         * once empty, so an idle connection holds none */
/* (one port, several nodes: below) */
#if defined(CLANKFORTH_NODES) && !defined(SO_REUSEPORT)
#define SHARE_PORTS 1
#include <pthread.h>
static void share_forget(int entry);
#endif
/* connections the system holds for a listener till accepted (a burst's): the most it allows - on Windows SOMAXCONN
 * asks only for a "reasonable" backlog (some 200, so a node busy with 200 clients refused more); a hint asks for so
 * many (Windows 8 on: ws2def.h's SOMAXCONN_HINT, which MinGW's headers lack) */
#ifdef _WIN32
#ifndef SOMAXCONN_HINT
#define SOMAXCONN_HINT(b) (-(b))
#endif
#define WINDOWS_BACKLOG 65535
#define LISTEN_BACKLOG SOMAXCONN_HINT(WINDOWS_BACKLOG)
#else
#define LISTEN_BACKLOG SOMAXCONN
#endif
#define DRAIN_POLL_MS 16                /* `serve` stopping: the nodes' last words waited for in waits of this long */
#define NS_PER_MS 1000000LL
#define MS_PER_S 1000LL
#define PEER_BYTES 64                   /* "address:port" */
#define HOST_BYTES 256                  /* a host's name, at most */
#define V4_IN_V6_AT 12                  /* an IPv4-mapped IPv6 address: its IPv4 address from this byte */
#define TLS_ERROR_BYTES 256             /* OpenSSL's reason, said in an error */
#define IN_MAX_DEFAULT ((size_t)64 << 20)    /* what has come and not been taken, at most: past it, not read on */
#define OUT_MAX_DEFAULT ((size_t)64 << 20)   /* what is still to go, at most: past it, the connection ended */
#define LINGER_MS 2000                  /* a connection closed with bytes still coming: read and let go so long, at most */
#define LINGER_READ 4096                /* ... in reads of this many bytes */
#define REPL_LINE_MAX 65536             /* a repl session's line, at most (a longer one is refused) */

enum { SHUT_OUT_ASKED = 1, SHUT_OUT_DONE };          /* a connection's shut_out (net-shut) */
enum { REPL_NONE, REPL_LISTENER, REPL_SESSION };   /* a connection's part in a repl */

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
    char why[TLS_ERROR_BYTES];   /* why it ended, if an error ended it ("" the other end closed it, or not yet) */
    int shared;         /* a listener on a port nodes share (where the system can't spread it): its entry, else -1 */
    size_t in_max;      /* net-input-max!: what has come and not been taken, at most - past it, not read till taken */
    size_t out_max;     /* net-output-max!: what is still to go, at most - past it, the connection ended */
    int held;           /* its reading stopped at in_max: read again once there is room (TLS's bytes the poll can't see) */
    int shut;           /* the other end shut its sending (a clean end of input): ended, but still sent to - its
                         * answer to what it asked goes out before the connection is let go */
    int shut_out;       /* net-shut: this end's sending to be shut once out has gone (1), shut (2) */
    int lingering;      /* closed by the program, its sending shut: what still comes read and let go till the other end
                         * closes (or linger_until) - a close with bytes unread would be a reset, the answer lost */
    long long linger_until;
    int repl;           /* REPL_*: a repl's listener or session, no actor's (owner NOBODY) */
    Task *task;         /* a session's: its lines run on it (their stack its own) */
    int attached;       /* a session's actor `attach`ed (-1: none) */
    char *pending;      /* a session's lines of a definition not finished yet (run again with the next) */
    size_t pending_n;
#ifdef CLANKFORTH_IOCP
    struct Watch *watch;    /* Windows: its read asked of the completion port (NULL: none yet) */
    long edged;         /* ... the events its socket wakes the loop for, as one of the few polled (FD_*; 0 none) */
    int unported;       /* ... the port wouldn't take it: polled for reading too */
#endif
    int tended;         /* on the list of those a round looks at (N->tend) */
    int fd_at;          /* where its socket is among those polled (N->fds), -1 not there (epoll: 0 once it is asked
                         * about, and `asked` what was asked) */
    short asked;
    int next_free;      /* a free slot's: the next free one, -1 none */
    int own_next, own_prev;   /* its owner's other connections (slots, -1 none): found as the owner dies */
    struct addrinfo *addrs;   /* connect's: the host's addresses (freed once connected) ... */
    struct addrinfo *next_addr;   /* ... and the next to try if this one is refused (an IPv6 one with no one there) */
#ifdef CLANKFORTH_TLS
    SSL *ssl;           /* a TLS connection's (NULL: plain TCP) */
    SSL_CTX *ctx;       /* a TLS listener's: its certificate, for the connections it accepts */
    int tls_ready;      /* the handshake done */
    int tls_want_write; /* OpenSSL waits to write (the poll asks for room to) */
    int tls_client;     /* tls-connect's: net-connected told once the handshake is done */
    size_t record_max;  /* tls-record-max!: a TLS record's bytes at most (0: OpenSSL's own, 16 KB) */
#endif
} Conn;

static void ended(Forth *f, Conn *c, int slot);   /* (the other end gone, or an error: net-closed told) */

/* shut with bytes still to go: not freed yet - closing, so they go first (then it lingers, and is let go) */
static int shut_with_output(const Conn *c) { return c->shut && c->out_at < c->out_n; }
static void close_after_output(Conn *c) {
    c->ended = 0;
    c->closing = 1;
}

#define TIMER_SLOT_BITS 24              /* a timer's id: its slot under these bits, a count over them (no two the same) */
#define TIMERS_MAX (1 << TIMER_SLOT_BITS)

typedef struct Timer {
    Cell id;            /* 0: free */
    int owner;
    long long due;      /* ms on the monotonic clock */
    int at;             /* where it is in the heap; a free one's: the next free slot, -1 none */
} Timer;

struct ForthNet {
    Conn *conns;
    int nconns, conns_cap;
    int free_conn;      /* the first free slot of those used before (the next: its next_free), -1 none */
    int *tend;          /* the connections a round looks at - those something happened to, or that wait for something
                         * (to be freed, to send, to read on): most of a server's thousands are not among them */
    int ntend;
    Timer *timers;
    int ntimers, timers_cap;
    int free_timer;     /* the first free slot (the next: its `at`), -1 none */
    int *heap;          /* the set timers' slots, the soonest due first (a heap: each before its two below) */
    int nheap;
    UCell next_timer;
    Sock wake[2];       /* (a pair: written by another node's thread as a message comes, read here to wake) */
    int k_accepted, k_received, k_closed, k_connected, k_timer;
    PollFd *fds;        /* the sockets polled: kept from round to round, a connection's changed as it is tended */
    int *fd_conn;       /* each polled socket's connection (-1: the wake pair, the first) */
    int nfds;
#ifdef CLANKFORTH_EPOLL
    int epoll;          /* Linux: the sockets are the system's to watch (epoll) - a wait tells those with something
                         * on them, whatever the number of the quiet ones; the array above is not used */
#endif
#ifdef CLANKFORTH_IOCP
    HANDLE iocp;        /* Windows: the wait is a completion port's - a connection's read is asked of it, and it
                         * tells those something came for, whatever the number of the quiet ones. The array above is
                         * the few it can't tell of: the listeners, those connecting, those with bytes to go */
    HANDLE edge;        /* ... an event something on one of those few sets, */
    HANDLE edge_wait;   /* ... and the wait on it that then wakes the port */
    int watches_gone;   /* reads still out for connections closed since (each freed as it comes back) */
#endif
#ifdef CLANKFORTH_TLS
    SSL_CTX *client_ctx;   /* tls-connect's (made as first wanted; tls-ca! makes it again) */
    char ca_file[HOST_BYTES * 4];   /* tls-ca!'s: the certificates trusted ("" the system's) */
#endif
};

#define N (f->net)
#define NOBODY_HERE (-1)   /* no actor: a repl's listener and sessions have none */

static long long now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * MS_PER_S + ts.tv_nsec / NS_PER_MS;
}

/* ---- the wake pair: a socket pair (Windows: two loopback sockets) another thread writes a byte to ---- */

#ifdef CLANKFORTH_IOCP
/* a completion port tells what came by packets: a connection's read by its Watch (key 0), and these with none */
#define IOCP_WAKE ((ULONG_PTR)1)   /* another thread's wake: a message from another node, a job done */
#define IOCP_EDGE ((ULONG_PTR)2)   /* something on one of the few sockets polled */

static int wake_pair(Sock s[2]) {   /* (none: the port is woken by a packet) */
    s[0] = s[1] = BAD_SOCK;
    return 0;
}
static void wake_up(void *ctx) {
    struct ForthNet *net = ctx;

    PostQueuedCompletionStatus(net->iocp, 0, IOCP_WAKE, NULL);
}
#else
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
#endif

/* ---- connections: a handle is the slot and, over it, its generation (a closed one's names nobody) ---- */

static void tend(struct ForthNet *net, int slot);

static Cell conn_handle(const struct ForthNet *net, int slot) {
    return (Cell)net->conns[slot].generation << CONN_SLOT_BITS | slot;
}

static Conn *conn_at(Forth *f, Cell h) {   /* NULL: none of that handle (closed, or never) */
    int slot = (int)(h & (CONNS_MAX - 1));

    if (h < 0 || h > INT32_MAX || slot >= N->nconns || N->conns[slot].state == CONN_FREE ||
        N->conns[slot].generation != (int)(h >> CONN_SLOT_BITS) || N->conns[slot].lingering) {
        return NULL;   /* (one lingering: closed, as the program sees it) */
    }
    tend(N, slot);   /* (a word names it: what the round does for it may change) */
    return &N->conns[slot];
}

static Conn *conn_arg(Forth *f, Cell h, const char *word) {
    Conn *c = conn_at(f, h);

    if (c == NULL) {
        forth_error(f, "%s: no connection %ld (closed?)", word, (long)h);
    }
    return c;
}

/* A round looks at the connections on the tend list, not at every one: a connection is put there by whatever may
 * change what the round does for it - a word naming it (conn_at), something on its socket, its owner's death - and
 * stays while it waits for something (tend_more). A server's thousands of quiet connections cost a round nothing. */
static void tend(struct ForthNet *net, int slot) {
    Conn *c = &net->conns[slot];

    if (!c->tended) {   /* (room for every connection there is: made with the table) */
        c->tended = 1;
        net->tend[net->ntend++] = slot;
    }
}

/* the table of connections, and what is sized by it (the tend list, the polled sockets), made bigger: 0 if it can't */
static int conns_grow(struct ForthNet *net) {
    int cap = net->conns_cap ? net->conns_cap * 2 : 16;
    Conn *conns;
    int *tends, *which;
    PollFd *fds;

    if (cap > CONNS_MAX) {
        return 0;
    }
    if ((tends = realloc(net->tend, (size_t)cap * sizeof(int))) == NULL) {
        return 0;
    }
    net->tend = tends;
    if ((fds = realloc(net->fds, ((size_t)cap + 1) * sizeof(PollFd))) == NULL) {   /* (and one: the wake pair's) */
        return 0;
    }
    net->fds = fds;
    if ((which = realloc(net->fd_conn, ((size_t)cap + 1) * sizeof(int))) == NULL) {
        return 0;
    }
    net->fd_conn = which;
    if ((conns = realloc(net->conns, (size_t)cap * sizeof(Conn))) == NULL) {
        return 0;
    }
    memset(conns + net->conns_cap, 0, (size_t)(cap - net->conns_cap) * sizeof(Conn));
    net->conns = conns;
    net->conns_cap = cap;
    return 1;
}

/* a connection's owner: the actor told what happens on it. An actor's connections are a list (its first kept with
 * the actor: forth_actor_conns), so those of one that dies are found - and closed - without a look at every one. */
static void own(Forth *f, int slot, int owner) {
    Conn *c = &N->conns[slot];
    int *first = owner >= 0 ? forth_actor_conns(f, owner) : NULL;

    c->owner = owner;
    c->own_prev = c->own_next = -1;
    if (first == NULL) {   /* (nobody's - a repl's - or of one gone already: the round closes that) */
        return;
    }
    c->own_next = *first - 1;
    if (c->own_next >= 0) {
        N->conns[c->own_next].own_prev = slot;
    }
    *first = slot + 1;
}
static void disown(Forth *f, int slot) {
    Conn *c = &N->conns[slot];
    int *first;

    if (c->own_prev >= 0) {
        N->conns[c->own_prev].own_next = c->own_next;
    } else if (c->owner >= 0 && (first = forth_actor_conns(f, c->owner)) != NULL && *first == slot + 1) {
        *first = c->own_next + 1;
    }
    if (c->own_next >= 0) {
        N->conns[c->own_next].own_prev = c->own_prev;
    }
    c->own_prev = c->own_next = -1;
}
/* (actors.c's, as an actor is forgotten) its connections - the list from `first` - looked at by the next round */
void forth_net_orphaned(Forth *f, int first) {
    int slot;

    for (slot = N != NULL ? first - 1 : -1; slot >= 0 && slot < N->nconns; slot = N->conns[slot].own_next) {
        tend(N, slot);
    }
}

/* a free slot (the table grown if it must be): its number */
static int conn_new(Forth *f, Sock s, int state, int owner) {
    int slot = N->free_conn;
    Conn *c;

    if (slot >= 0) {
        N->free_conn = N->conns[slot].next_free;
    } else {
        if (N->nconns == N->conns_cap && !conns_grow(N)) {
            sock_close(s);
            forth_error(f, "net: out of memory for connections (%d open)", N->nconns);
        }
        slot = N->nconns++;
        N->conns[slot].tended = 0;
    }
    c = &N->conns[slot];
    c->state = state;
    c->sock = s;
    c->fd_at = -1;
#ifdef CLANKFORTH_IOCP
    c->watch = NULL;
    c->edged = 0;
    c->unported = 0;
#endif
    own(f, slot, owner);
    tend(N, slot);
    c->in_n = c->out_n = c->out_at = 0;
    c->closing = c->ended = 0;
    c->peer[0] = 0;
    c->why[0] = 0;
    c->shared = -1;
    c->in_max = IN_MAX_DEFAULT;
    c->held = 0;
    c->lingering = 0;
    c->shut = 0;
    c->shut_out = 0;
    c->out_max = OUT_MAX_DEFAULT;
    c->repl = REPL_NONE;
    c->task = NULL;
    c->attached = -1;
    c->pending = NULL;
    c->pending_n = 0;
    c->addrs = c->next_addr = NULL;
#ifdef CLANKFORTH_TLS
    c->ssl = NULL;
    c->ctx = NULL;
    c->tls_ready = c->tls_want_write = c->tls_client = 0;
    c->record_max = 0;
#endif
    return slot;
}

static void unpolled(struct ForthNet *net, Conn *c);
static int polled(struct ForthNet *net, int slot);

static void conn_free(Forth *f, Conn *c) {
    int slot = (int)(c - N->conns);

    disown(f, slot);
    unpolled(N, c);
#ifdef SHARE_PORTS
    if (c->shared >= 0) {
        share_forget(c->shared);
        c->shared = -1;
    }
#endif
    if (c->task != NULL) {   /* (a repl session's) */
        forth_task_free(f, c->task);
        c->task = NULL;
    }
    free(c->pending);
    c->pending = NULL;
    c->pending_n = 0;
    c->lingering = c->held = 0;
#ifdef CLANKFORTH_TLS
    if (c->ssl != NULL) {
        if (c->tls_ready && !c->ended) {
            SSL_shutdown(c->ssl);   /* (its close_notify, as the socket takes it: not waited for) */
        }
        SSL_free(c->ssl);
        ERR_clear_error();
        c->ssl = NULL;
    }
    if (c->ctx != NULL) {
        SSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
#endif
    if (c->sock != BAD_SOCK) {
        sock_close(c->sock);
    }
    if (c->addrs != NULL) {
        freeaddrinfo(c->addrs);
        c->addrs = c->next_addr = NULL;
    }
    free(c->in);
    free(c->out);
    c->in = c->out = NULL;
    c->in_cap = c->out_cap = 0;
    c->sock = BAD_SOCK;
    c->state = CONN_FREE;
    c->generation = (c->generation + 1) % CONN_GENERATIONS;
    c->next_free = N->free_conn;
    N->free_conn = slot;
}

static int conn_queue(Conn *c, const void *p, size_t n) {   /* bytes to go (0: no memory for them) */
    if (c->out_at > 0 && c->out_n + n > c->out_cap) {   /* (no room after what waits: what has gone, at its front,
                                                         *  is room - a client slower than what was written for it
                                                         *  never took it all, and the buffer grew to all it was
                                                         *  ever sent) */
        memmove(c->out, c->out + c->out_at, c->out_n - c->out_at);
        c->out_n -= c->out_at;
        c->out_at = 0;
    }
    if (c->out_n + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : BUF_START;
        uint8_t *more;

        while (cap < c->out_n + n) {
            cap *= 2;
        }
        if ((more = realloc(c->out, cap)) == NULL) {
            return 0;
        }
        c->out = more;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_n, p, n);
    c->out_n += n;
    return 1;
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

        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {   /* (an IPv4 client of a listener on both: said as IPv4) */
            inet_ntop(AF_INET, &in6->sin6_addr.s6_addr[V4_IN_V6_AT], host, sizeof(host));
        } else {
            inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        }
        port = ntohs(in6->sin6_port);
    }
    snprintf(c->peer, sizeof(c->peer), "%s:%d", host, port);
}

static void no_delay(Sock s) {   /* (a reply goes now, not held back to join the next) */
    int on = 1;

    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
}

/* ---- the words ---- */

/* ---- one port, several nodes, where the system can't spread its connections over them (no SO_REUSEPORT: Windows) -
 * the node that binds it accepts each connection and hands it round to the nodes listening on it, itself among them
 * (in turn); each takes it as its own listener's. ---- */
#ifdef SHARE_PORTS
#define SHARED_MAX 256          /* listeners on shared ports, in the process */
#define HANDED_MAX 512          /* connections handed to a listener and not yet taken */
typedef struct Shared {
    int used, bound;            /* bound: the one with the socket, accepting for all */
    long port;
    struct ForthNet *net;       /* its node's (its wake pair, its connections) */
    Cell listener;              /* its listener there */
    Sock *handed;               /* connections handed to it, waiting for its node to take them ... */
    struct sockaddr_storage *peers;   /* ... and who each is from */
    int nhanded;
} Shared;
static Shared sShared[SHARED_MAX];
static pthread_mutex_t sSharedLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned sTurn;          /* whose turn the next connection is */

/* this node's listener on the port, registered: its entry (-1: no room) */
static int share_register(Forth *f, long port, int slot, int bound) {
    int i;

    pthread_mutex_lock(&sSharedLock);
    for (i = 0; i < SHARED_MAX && sShared[i].used; i++) {
    }
    if (i == SHARED_MAX || (sShared[i].handed = malloc(HANDED_MAX * sizeof(Sock))) == NULL ||
        (sShared[i].peers = malloc(HANDED_MAX * sizeof(struct sockaddr_storage))) == NULL) {
        if (i < SHARED_MAX) {
            free(sShared[i].handed);
        }
        pthread_mutex_unlock(&sSharedLock);
        return -1;
    }
    sShared[i].used = 1;
    sShared[i].bound = bound;
    sShared[i].port = port;
    sShared[i].net = N;
    sShared[i].listener = conn_handle(N, slot);
    sShared[i].nhanded = 0;
    pthread_mutex_unlock(&sSharedLock);
    return i;
}
static int share_bound(long port) {   /* is the port bound by a node of this process? */
    int i, found = 0;

    pthread_mutex_lock(&sSharedLock);
    for (i = 0; i < SHARED_MAX; i++) {
        found = found || (sShared[i].used && sShared[i].bound && sShared[i].port == port);
    }
    pthread_mutex_unlock(&sSharedLock);
    return found;
}
static void share_forget(int entry) {   /* (its listener freed) - what was handed to it and not taken closed */
    int i;

    if (!sShared[entry].bound && forth_pool_supervised()) {   /* (the supervisor hands this node no more of them) */
        forth_pool_unlisten(sShared[entry].port);
    }
    pthread_mutex_lock(&sSharedLock);
    for (i = 0; i < sShared[entry].nhanded; i++) {
        sock_close(sShared[entry].handed[i]);
    }
    free(sShared[entry].handed);
    free(sShared[entry].peers);
    memset(&sShared[entry], 0, sizeof(sShared[entry]));
    pthread_mutex_unlock(&sSharedLock);
}
/* a connection the bound listener accepted: handed to the node whose turn it is - 1, or 0 if that is this node (its
 * own to take now) */
static int share_hand(Forth *f, int entry, Sock s, const struct sockaddr_storage *sa) {
    int i, n = 0, pick, handed = 0;

    pthread_mutex_lock(&sSharedLock);
    for (i = 0; i < SHARED_MAX; i++) {
        n += sShared[i].used && sShared[i].port == sShared[entry].port;
    }
    pick = n > 0 ? (int)(sTurn++ % (unsigned)n) : 0;
    for (i = 0; i < SHARED_MAX; i++) {
        if (!sShared[i].used || sShared[i].port != sShared[entry].port || pick-- != 0) {
            continue;
        }
        if (sShared[i].net != N && sShared[i].nhanded < HANDED_MAX) {
            sShared[i].handed[sShared[i].nhanded] = s;
            sShared[i].peers[sShared[i].nhanded++] = *sa;
            wake_up(sShared[i].net);   /* (inside the lock: its node can't be gone meanwhile) */
            handed = 1;
        }
        break;
    }
    pthread_mutex_unlock(&sSharedLock);
    return handed;
}
/* a connection the supervisor accepted on the port and handed to this process (supervisor.c's reading thread): to
 * this node's listener on it, as one another node handed - or closed, if it has none now */
void forth_net_handed(long port, uintptr_t handed) {
    Sock s = (Sock)handed;
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    int i, taken = 0;

    memset(&sa, 0, sizeof(sa));
    getpeername(s, (struct sockaddr *)&sa, &len);
    pthread_mutex_lock(&sSharedLock);
    for (i = 0; i < SHARED_MAX && !taken; i++) {
        if (sShared[i].used && sShared[i].port == port && sShared[i].nhanded < HANDED_MAX) {
            sShared[i].handed[sShared[i].nhanded] = s;
            sShared[i].peers[sShared[i].nhanded++] = sa;
            wake_up(sShared[i].net);
            taken = 1;
        }
    }
    pthread_mutex_unlock(&sSharedLock);
    if (!taken) {
        sock_close(s);
    }
}
static void accepted(Forth *f, int slot, Sock s, const struct sockaddr_storage *sa);
/* the connections handed to this node's listeners: taken as theirs */
static void share_take(Forth *f) {
    int i, j;

    for (i = 0; i < SHARED_MAX; i++) {
        Sock got[HANDED_MAX];
        struct sockaddr_storage peers[64];
        int n = 0;
        Cell listener = 0;

        pthread_mutex_lock(&sSharedLock);
        if (sShared[i].used && sShared[i].net == N && sShared[i].nhanded > 0) {
            n = sShared[i].nhanded < 64 ? sShared[i].nhanded : 64;
            for (j = 0; j < n; j++) {
                got[j] = sShared[i].handed[j];
                peers[j] = sShared[i].peers[j];
            }
            memmove(sShared[i].handed, sShared[i].handed + n, (size_t)(sShared[i].nhanded - n) * sizeof(Sock));
            memmove(sShared[i].peers, sShared[i].peers + n,
                    (size_t)(sShared[i].nhanded - n) * sizeof(struct sockaddr_storage));
            sShared[i].nhanded -= n;
            listener = sShared[i].listener;
            if (sShared[i].nhanded > 0) {
                wake_up(N);   /* (more than one round's: the next poll takes the rest) */
            }
        }
        pthread_mutex_unlock(&sSharedLock);
        for (j = 0; j < n; j++) {
            Conn *l = conn_at(f, listener);

            if (l == NULL) {
                sock_close(got[j]);
                continue;
            }
            accepted(f, (int)(l - N->conns), got[j], &peers[j]);
        }
    }
}
#endif

#ifndef SHARE_PORTS
void forth_net_handed(long port, uintptr_t handed) { (void)port, (void)handed; }   /* (the system spreads a port) */
#endif

/* listen ( port -- listener ) TCP on every address of this machine (port 0: any free one - listener-port says);
 * net-accepted ( listener conn -- ) comes to this actor for each connection, the connection its */
/* a listener on the port, this actor's: its slot, or minus the error's number (no socket, the port taken) */
/* a listener on the port, its connections told to the actor `me` (NOBODY: a repl's); local: on this machine only
 * (127.0.0.1), else every address */
static int listen_bind(Forth *f, Cell port, int me, int local);
#ifdef SHARE_PORTS
static pthread_mutex_t sListenLock = PTHREAD_MUTEX_INITIALIZER;   /* (two nodes listening at once: one binds) */
#endif
static int listen_try(Forth *f, Cell port, int me, int local) {
#ifdef SHARE_PORTS
    int slot, entry;

    if (me == NOBODY_HERE || port == 0) {   /* (a repl's, or any free port: this node's alone) */
        return listen_bind(f, port, me, local);
    }
    pthread_mutex_lock(&sListenLock);
    if (forth_pool_supervised()) {   /* (a node of a pool of processes: the supervisor has the port for them all, and
                                      *  hands each connection to a node - asked for it here) */
        int refused;

        slot = conn_new(f, BAD_SOCK, CONN_LISTENER, me);
        entry = share_register(f, (long)port, slot, 0);   /* (before the supervisor is asked: a connection it hands
                                                           *  as it answers finds its listener here) */
        if (entry >= 0) {
            N->conns[slot].shared = entry;
        }
        refused = entry < 0 ? ENOMEM : forth_pool_listen((long)port, local);
        pthread_mutex_unlock(&sListenLock);
        if (refused != 0) {
            conn_free(f, &N->conns[slot]);   /* (its entry forgotten with it) */
            return -refused;
        }
        return slot;
    } else if (share_bound((long)port)) {   /* (another node of this process has it: connections handed from there) */
        slot = conn_new(f, BAD_SOCK, CONN_LISTENER, me);
    } else if ((slot = listen_bind(f, port, me, local)) < 0) {
        pthread_mutex_unlock(&sListenLock);
        return slot;
    }
    entry = share_register(f, (long)port, slot, N->conns[slot].sock != BAD_SOCK);
    pthread_mutex_unlock(&sListenLock);
    if (entry < 0) {
        conn_free(f, &N->conns[slot]);
        return -ENOMEM;
    }
    N->conns[slot].shared = entry;
    return slot;
#else
    return listen_bind(f, port, me, local);
#endif
}
static int listen_bind(Forth *f, Cell port, int me, int local) {
    int on = 1, off = 0;
    struct sockaddr_storage sa;
    socklen_t sa_len;
    Sock s;

    memset(&sa, 0, sizeof(sa));
    s = local ? BAD_SOCK : socket(AF_INET6, SOCK_STREAM, 0);   /* IPv6 and IPv4 both, on one socket where ... */
    if (s != BAD_SOCK && setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof(off)) == 0) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&sa;

        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons((unsigned short)port);
        sa_len = sizeof(*a6);
    } else {   /* ... IPv4 alone where it hasn't */
        struct sockaddr_in *a4 = (struct sockaddr_in *)&sa;

        if (s != BAD_SOCK) {
            sock_close(s);
        }
        if ((s = socket(AF_INET, SOCK_STREAM, 0)) == BAD_SOCK) {
            return -sock_error();
        }
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(local ? INADDR_LOOPBACK : INADDR_ANY);
        a4->sin_port = htons((unsigned short)port);
        sa_len = sizeof(*a4);
    }
#ifdef _WIN32   /* (Windows' SO_REUSEADDR lets another process take the port too: this one's alone) */
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
#endif
#ifdef SO_REUSEPORT   /* (each node may listen on the port: the system spreads the connections over them) */
    if (me != NOBODY_HERE) {   /* (not a repl's: one each node would answer at random) */
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof(on));
    }
#endif
    if (bind(s, (struct sockaddr *)&sa, sa_len) != 0 || listen(s, LISTEN_BACKLOG) != 0 || set_nonblocking(s) != 0) {
        int e = sock_error();

        sock_close(s);
        return -e;
    }
    return conn_new(f, s, CONN_LISTENER, me);
}
static int listen_on(Forth *f, Cell port, const char *word, int local) {   /* as listen_try, its failure an error */
    int slot, me;

    if (port < 0 || port > 65535) {
        forth_error(f, "%s: no port %ld", word, (long)port);
    }
    me = self_actor(f, word);
    if ((slot = listen_try(f, port, me, local)) < 0) {
        forth_error(f, "%s: port %ld: %s", word, (long)port, strerror(-slot));
    }
    return slot;
}
static void p_listen(Forth *f, Word *w) {   /* listen ( port -- listener ) */
    Cell port = forth_pop(f);

    (void)w;
    forth_push(f, conn_handle(N, listen_on(f, port, "listen", 0)));
}
static void p_listen_local(Forth *f, Word *w) {   /* listen-local ( port -- listener ) on this machine only */
    Cell port = forth_pop(f);

    (void)w;
    forth_push(f, conn_handle(N, listen_on(f, port, "listen-local", 1)));
}

static void p_listener_port(Forth *f, Word *w) {   /* listener-port ( listener -- port ) */
    Conn *c = conn_arg(f, forth_pop(f), "listener-port");
    struct sockaddr_storage a;
    socklen_t len = sizeof(a);

    (void)w;
#ifdef SHARE_PORTS
    if (c->sock == BAD_SOCK && c->shared >= 0) {   /* (a node's share of a port another node bound) */
        forth_push(f, (Cell)sShared[c->shared].port);
        return;
    }
#endif
    if (getsockname(c->sock, (struct sockaddr *)&a, &len) != 0) {
        forth_error(f, "listener-port: %s", strerror(sock_error()));
    }
    forth_push(f, a.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&a)->sin6_port)
                                          : ntohs(((struct sockaddr_in *)&a)->sin_port));
}

/* connect ( host n port -- conn ) to host (a name or an address) - the name looked up now, the connection made as the
 * loop runs: net-connected ( conn -- ) to this actor, or net-closed ( conn -- ) if it couldn't be */
/* a connection to the address begun (non-blocking): its socket, or BAD_SOCK (*e the error) */
static Sock connect_start(const struct addrinfo *ai, int *e) {
    Sock s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

    if (s == BAD_SOCK || set_nonblocking(s) != 0 ||
        (connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0 && !would_block(sock_error()))) {
        *e = sock_error();
        if (s != BAD_SOCK) {
            sock_close(s);
        }
        return BAD_SOCK;
    }
    no_delay(s);
    return s;
}

static int connect_to(Forth *f, Cell a, Cell n, Cell port, const char *word, char host[HOST_BYTES]) {
    int me = self_actor(f, word), slot, e = 0;
    size_t len;
    const char *h = forth_string_arg(f, a, n, word, &len);
    char service[16];
    struct addrinfo hints, *res = NULL, *ai;
    Sock s = BAD_SOCK;
    int r;

    if (len >= HOST_BYTES || port < 1 || port > 65535) {
        forth_error(f, "%s: no host \"%.*s\", port %ld", word, (int)len, h, (long)port);
    }
    memcpy(host, h, len);
    host[len] = 0;
    snprintf(service, sizeof(service), "%ld", (long)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((r = getaddrinfo(host, service, &hints, &res)) != 0 || res == NULL) {
        forth_error(f, "%s: %s: %s", word, host, gai_strerror(r));
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {   /* (the first that starts; one refused later: the next tried) */
        if ((s = connect_start(ai, &e)) != BAD_SOCK) {
            break;
        }
    }
    if (ai == NULL) {
        freeaddrinfo(res);
        forth_error(f, "%s: %s:%ld: %s", word, host, (long)port, strerror(e));
    }
    slot = conn_new(f, s, CONN_CONNECTING, me);
    snprintf(N->conns[slot].peer, sizeof(N->conns[slot].peer), "%.40s:%ld", host, (long)port);
    N->conns[slot].addrs = res;
    N->conns[slot].next_addr = ai->ai_next;
    return slot;
}
static void p_connect(Forth *f, Word *w) {
    Cell port = forth_pop(f), n = forth_pop(f), a = forth_pop(f);
    char host[HOST_BYTES];

    (void)w;
    forth_push(f, conn_handle(N, connect_to(f, a, n, port, "connect", host)));
}

/* net-write ( a n conn -- ) the bytes to go (kept till they can: the loop sends them) */
static void p_net_write(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-write");
    Cell n = forth_pop(f), a = forth_pop(f);
    size_t len;
    const char *s = forth_string_arg(f, a, n, "net-write", &len);

    (void)w;
    if (c->state == CONN_LISTENER || (c->ended && !c->shut) || c->closing || c->shut_out || len == 0) {
        return;   /* (nowhere for them to go: dropped, as a message to an actor gone is; or none) */
    }
    if (c->out_n - c->out_at + len > c->out_max) {   /* (the other end not reading: ended, not kept growing) */
        snprintf(c->why, sizeof(c->why), "more than %zu bytes waiting to go", c->out_max);
        ended(f, c, (int)(c - N->conns));
        c->closing = 1;
        return;
    }
    if (!conn_queue(c, s, len)) {
        forth_error(f, "net-write: out of memory for %zu bytes to go", c->out_n + len);
    }
}

/* net-input-max! ( n conn -- ) what has come and not been taken, at most (64 MiB at first): past it the socket isn't
 * read till the program takes some - a sender can't fill the memory faster than it is read */
static void p_net_input_max(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-input-max!");
    Cell n = forth_pop(f);

    (void)w;
    c->in_max = n > 0 ? (size_t)n : 1;
}
/* net-output-max! ( n conn -- ) what is still to go, at most (64 MiB at first): a write past it ends the connection (a
 * client that doesn't read what it asked for) */
static void p_net_output_max(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-output-max!");
    Cell n = forth_pop(f);

    (void)w;
    c->out_max = n > 0 ? (size_t)n : 1;
}

/* net-input ( conn -- a n ) what has come and not been taken: its address good till the next poll, or net-consume */
static void p_net_input(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-input");

    (void)w;
    forth_push(f, (Cell)(c->in != NULL ? (const char *)c->in : ""));
    forth_push(f, (Cell)c->in_n);
}

/* net-ended? ( conn -- flag ) the other end gone, an error ended it, or the handle names nothing now: what a handler
 * that is busy (streaming, waiting) asks, its net-closed kept till it is done */
static void p_net_ended(Forth *f, Word *w) {
    Conn *c = conn_at(f, forth_pop(f));

    (void)w;
    forth_push(f, c == NULL || c->ended || c->closing ? -1 : 0);
}

/* net-gone? ( conn -- flag ) nothing more can be sent it: the handle names nothing, an error ended it, or it is
 * closing. Not so for one whose other end only shut its sending (a clean end: net-ended? is true of it) - its
 * answer still goes. What a handler sending a long answer asks. */
static void p_net_gone(Forth *f, Word *w) {
    Conn *c = conn_at(f, forth_pop(f));

    (void)w;
    forth_push(f, c == NULL || (c->ended && !c->shut) || c->closing ? -1 : 0);
}

/* net-pending ( conn -- n ) bytes written and not yet sent (0 for one gone): a writer that keeps up waits for it */
static void p_net_pending(Forth *f, Word *w) {
    Conn *c = conn_at(f, forth_pop(f));

    (void)w;
    forth_push(f, c == NULL ? 0 : (Cell)(c->out_n - c->out_at));
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

/* a connection closed with all sent: its sending shut (TLS: its close_notify first), what still comes read and let go
 * till the other end closes too - closed at once with bytes unread, the system would send a reset, and the other end
 * could lose the answer before it (a 413 sent before the body, a 503 before the request) */
static void conn_linger(Forth *f, Conn *c) {
    if (c->state != CONN_OPEN || c->ended || c->lingering || c->sock == BAD_SOCK || c->repl != REPL_NONE) {
        conn_free(f, c);
        return;
    }
#ifdef CLANKFORTH_TLS
    if (c->ssl != NULL && c->tls_ready) {
        SSL_shutdown(c->ssl);
        c->tls_ready = 0;   /* (said: not again when it is freed) */
    }
#endif
    shutdown(c->sock, SHUT_WR);
    c->lingering = c->closing = 1;
    c->held = 0;
    c->linger_until = now_ms() + LINGER_MS;
}
/* a lingering connection's bytes read and let go: freed once the other end has closed (or failed) */
static void linger_read(Forth *f, Conn *c) {
    char buf[LINGER_READ];

    for (;;) {
        long got = (long)recv(c->sock, buf, (int)sizeof(buf), 0);

        if (got > 0) {
            continue;
        }
        if (got < 0 && would_block(sock_error())) {
            return;
        }
        conn_free(f, c);
        return;
    }
}

/* net-close ( conn -- ) closed once what is to go has gone (a listener: at once). No net-closed for it. */
static void p_net_close(Forth *f, Word *w) {
    Conn *c = conn_at(f, forth_pop(f));

    (void)w;
    if (c == NULL || c->lingering) {
        return;   /* (closed already) */
    }
    if (shut_with_output(c)) {   /* (its answer still to go to an end that shut its sending) */
        close_after_output(c);
        return;
    }
    if (c->state != CONN_OPEN || c->ended) {
        conn_free(f, c);
    } else if (c->out_at == c->out_n) {
        conn_linger(f, c);
    } else {
        c->closing = 1;
    }
}

/* net-shut ( conn -- ) this end has no more to send: what was written goes, then its sending is shut - the other
 * end sees a clean end of what it reads, and what it sends still comes (a request whole, its answer awaited). Not
 * a TLS connection's: that one's end is its close. */
static void p_net_shut(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-shut");

    (void)w;
#ifdef CLANKFORTH_TLS
    if (c->ssl != NULL) {
        forth_error(f, "net-shut: not a TLS connection's (its sending ends as it closes: net-close)");
    }
#endif
    if (c->state == CONN_LISTENER || c->ended || c->closing || c->shut_out) {
        return;
    }
    c->shut_out = SHUT_OUT_ASKED;
    if (c->state == CONN_OPEN && c->out_at == c->out_n) {   /* (nothing waits to go: now; else as the last of it
                                                             *  goes) */
        shutdown(c->sock, SHUT_WR);
        c->shut_out = SHUT_OUT_DONE;
    }
}

static void p_net_owner(Forth *f, Word *w) {   /* net-owner! ( actor conn -- ) told what happens on it from now */
    Conn *c = conn_arg(f, forth_pop(f), "net-owner!");
    Cell id = forth_pop(f);

    (void)w;
    if (forth_actor_body(f, (int)id) == NULL || id < 0 || id > INT32_MAX) {
        forth_error(f, "net-owner!: no actor %ld", (long)id);
    }
    disown(f, (int)(c - N->conns));
    own(f, (int)(c - N->conns), (int)id);
}

static void p_conn_peer(Forth *f, Word *w) {   /* conn-peer ( conn -- s len ) "address:port" of its other end */
    Conn *c = conn_arg(f, forth_pop(f), "conn-peer");
    size_t len = strlen(c->peer);

    (void)w;
    forth_push(f, (Cell)forth_new_string(f, c->peer, len));
    forth_push(f, (Cell)len);
}

/* net-waits ( -- s len ) how this build waits for its sockets: "epoll" (Linux), "iocp" (Windows: a completion
 * port), or "poll" - which looks at every socket held at each wake. What a host that builds the sources itself
 * asks, to know it defined what its system has (CLANKFORTH_EPOLL, CLANKFORTH_IOCP). A new string. */
static void p_net_waits(Forth *f, Word *w) {
#if defined(CLANKFORTH_EPOLL)
    static const char how[] = "epoll";
#elif defined(CLANKFORTH_IOCP)
    static const char how[] = "iocp";
#else
    static const char how[] = "poll";
#endif

    (void)w;
    forth_push(f, (Cell)forth_new_string(f, how, sizeof(how) - 1));
    forth_push(f, (Cell)(sizeof(how) - 1));
}

/* net-why ( conn -- s len ) why an error ended it - a connection refused, a TLS handshake failed and why ("the
 * certificate: ...") - or "" (the other end closed it, or it hasn't ended): what a net-closed handler asks */
static void p_net_why(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "net-why");
    size_t len = strlen(c->why);

    (void)w;
    forth_push(f, (Cell)forth_new_string(f, c->why, len));
    forth_push(f, (Cell)len);
}

/* ---- timers: a heap of them, the soonest due first - set, cancelled and found due without a look at each ---- */

/* a before b: due sooner - or, due together, set sooner */
static int timer_before(const struct ForthNet *net, int a, int b) {
    const Timer *x = &net->timers[a], *y = &net->timers[b];

    return x->due != y->due ? x->due < y->due : (UCell)x->id < (UCell)y->id;
}
static void heap_put(struct ForthNet *net, int at, int slot) {
    net->heap[at] = slot;
    net->timers[slot].at = at;
}
static void heap_up(struct ForthNet *net, int at) {
    int slot = net->heap[at];

    while (at > 0 && timer_before(net, slot, net->heap[(at - 1) / 2])) {
        heap_put(net, at, net->heap[(at - 1) / 2]);
        at = (at - 1) / 2;
    }
    heap_put(net, at, slot);
}
static void heap_down(struct ForthNet *net, int at) {
    int slot = net->heap[at];

    for (;;) {
        int below = at * 2 + 1;

        if (below + 1 < net->nheap && timer_before(net, net->heap[below + 1], net->heap[below])) {
            below++;
        }
        if (below >= net->nheap || !timer_before(net, net->heap[below], slot)) {
            break;
        }
        heap_put(net, at, net->heap[below]);
        at = below;
    }
    heap_put(net, at, slot);
}
/* a set timer out of the heap, its slot free */
static void timer_drop(struct ForthNet *net, int slot) {
    int at = net->timers[slot].at, last = net->heap[--net->nheap];

    if (at < net->nheap) {   /* (the last in its place: up or down to where it belongs) */
        heap_put(net, at, last);
        heap_up(net, at);
        heap_down(net, net->timers[last].at);
    }
    net->timers[slot].id = 0;
    net->timers[slot].at = net->free_timer;
    net->free_timer = slot;
}

/* a timer of the running actor's, due then: its id */
static Cell timer_set(Forth *f, long long due, const char *word) {
    int me = self_actor(f, word), slot = N->free_timer;
    Timer *t;

    if (slot >= 0) {
        N->free_timer = N->timers[slot].at;
    } else {
        if (N->ntimers == N->timers_cap) {
            int cap = N->timers_cap ? N->timers_cap * 2 : 16;
            Timer *more = cap <= TIMERS_MAX ? realloc(N->timers, (size_t)cap * sizeof(Timer)) : NULL;
            int *heap = more != NULL ? realloc(N->heap, (size_t)cap * sizeof(int)) : NULL;

            if (more != NULL) {
                N->timers = more;
            }
            if (heap == NULL) {
                forth_error(f, "%s: out of memory for timers (%d set)", word, N->nheap);
            }
            N->heap = heap;
            N->timers_cap = cap;
        }
        slot = N->ntimers++;
    }
    t = &N->timers[slot];
    t->id = (Cell)(++N->next_timer << TIMER_SLOT_BITS | (UCell)slot);
    t->owner = me;
    t->due = due;
    N->heap[N->nheap] = slot;
    heap_up(N, N->nheap++);
    return t->id;
}

/* after ( ms -- timer ) timer-fired ( timer -- ) to this actor once ms have gone by (the clock's, not frames) */
static void p_after(Forth *f, Word *w) {
    Cell ms = forth_pop(f);

    (void)w;
    forth_push(f, timer_set(f, now_ms() + (ms > 0 ? (long long)ms : 0), "after"));
}

/* after-together ( ms step -- timer ) as after, at the clock's next multiple of `step` ms once ms have gone by: timers
 * so set fire together - a thousand actors each doing a little every so often wake the node once for all of them,
 * not a thousand times (a websocket's ping) */
static void p_after_together(Forth *f, Word *w) {
    Cell step = forth_pop(f), ms = forth_pop(f);
    long long due = now_ms() + (ms > 0 ? (long long)ms : 0);

    (void)w;
    if (step > 1) {
        due = (due / step + 1) * step;
    }
    forth_push(f, timer_set(f, due, "after-together"));
}

static void p_cancel_timer(Forth *f, Word *w) {   /* cancel-timer ( timer -- ) (fired or cancelled already: nothing) */
    Cell id = forth_pop(f);
    int slot = (int)((UCell)id & (TIMERS_MAX - 1));

    (void)w;
    if (id != 0 && slot < N->ntimers && N->timers[slot].id == id) {
        timer_drop(N, slot);
    }
}

/* ---- the loop ---- */

/* what the poll is asked about a connection: to read (not one full, nor one whose input is over), to write (one
 * with bytes to go) */
static short conn_events(const Conn *c) {
    int wants_out = c->out_at < c->out_n;

#ifdef CLANKFORTH_TLS
    wants_out = wants_out || (c->ssl != NULL && c->tls_want_write);
#endif
    return c->state == CONN_CONNECTING ? POLLOUT : c->state == CONN_LISTENER ? POLLIN
           : c->lingering ? POLLIN
           : (short)((c->in_n >= c->in_max || c->shut ? 0 : POLLIN) | (wants_out ? POLLOUT : 0));
}
#ifdef CLANKFORTH_EPOLL
#define EPOLL_WAKE UINT64_MAX   /* an event's data: the wake pair (a connection's: its generation over its slot) */
#define EPOLL_GENERATION_SHIFT 32   /* ... the slot under these bits */
#define EPOLL_READY_MAX 256     /* events taken at a wait (more: the next round's) */

static uint32_t epoll_asks(short ev) { return (ev & POLLIN ? EPOLLIN : 0u) | (ev & POLLOUT ? EPOLLOUT : 0u); }
static short epoll_says(uint32_t ev) {
    return (short)((ev & EPOLLIN ? POLLIN : 0) | (ev & EPOLLOUT ? POLLOUT : 0) | (ev & EPOLLERR ? POLLERR : 0) |
                   (ev & EPOLLHUP ? POLLHUP : 0));
}
/* its socket watched for what it wants now (a shadow listener has none of its own: not watched); 0 if the system
 * couldn't take it (asked again the next round) */
static int polled(struct ForthNet *net, int slot) {
    Conn *c = &net->conns[slot];
    struct epoll_event e;
    short ev;

    if (c->state == CONN_FREE || c->sock == BAD_SOCK) {
        unpolled(net, c);
        return 1;
    }
    ev = conn_events(c);
    if (c->fd_at >= 0 && c->asked == ev) {
        return 1;
    }
    memset(&e, 0, sizeof(e));
    e.events = epoll_asks(ev);
    e.data.u64 = (uint64_t)(uint32_t)c->generation << EPOLL_GENERATION_SHIFT | (uint32_t)slot;
    if (epoll_ctl(net->epoll, c->fd_at < 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, c->sock, &e) != 0) {
        return 0;
    }
    c->fd_at = 0;
    c->asked = ev;
    return 1;
}
/* ... and no more (before its socket is closed: the number may be another's next) */
static void unpolled(struct ForthNet *net, Conn *c) {
    if (c->fd_at >= 0) {
        struct epoll_event e;   /* (not read: kernels before 2.6.9 wanted one) */

        memset(&e, 0, sizeof(e));
        (void)epoll_ctl(net->epoll, EPOLL_CTL_DEL, c->sock, &e);
        c->fd_at = -1;
    }
}
#else
/* its socket among those polled, asked for these events */
static void fds_put(struct ForthNet *net, int slot, short events) {
    Conn *c = &net->conns[slot];

    if (c->fd_at < 0) {   /* (room for every connection's: made with the table) */
        c->fd_at = net->nfds++;
        net->fd_conn[c->fd_at] = slot;
        net->fds[c->fd_at].revents = 0;
    }
    net->fds[c->fd_at].fd = c->sock;
    net->fds[c->fd_at].events = events;
}
/* ... and out of them: the last in its place */
static void fds_drop(struct ForthNet *net, Conn *c) {
    int at = c->fd_at, last = net->nfds - 1;

    if (at < 0) {
        return;
    }
    if (at != last) {
        net->fds[at] = net->fds[last];
        net->fd_conn[at] = net->fd_conn[last];
        net->conns[net->fd_conn[at]].fd_at = at;
    }
    net->nfds--;
    c->fd_at = -1;
}
#ifdef CLANKFORTH_IOCP
/* Windows. WSAPoll looks at every socket it is given at each wake: thousands of quiet connections cost each wake a
 * walk of them all. A completion port doesn't: a connection's read is asked of it - a read of no bytes, which the
 * system ends when there is something to read (or the other end has gone) - and the port tells those that ended, a
 * packet each, whatever the number still waiting. What the port can't tell of this way - a listener's connections
 * to accept, a connect made, room to send again - is the few sockets still polled (N->fds): an event their
 * sockets set (WSAEventSelect) wakes the port, and each round polls them, not waiting, to see which.
 *
 * A read out belongs to the system till it comes back, so what it is asked with (a Watch: its OVERLAPPED) is not
 * the connection's own memory, which moves and is let go: a connection closed with a read out leaves its Watch to
 * be freed as the read comes back, ended by the close. */
typedef struct Watch {
    OVERLAPPED ov;      /* (first: a packet's OVERLAPPED is its Watch) */
    int slot;           /* its connection's */
    int pending;        /* a read asked for, not yet told of */
    int gone;           /* its connection closed since: freed when told */
} Watch;
#define IOCP_ENTRIES 64         /* packets taken at a wait (more: the next round's) */
#define IOCP_RECHECK_MS 250     /* while a socket waits to connect, or to send: looked at this often at least,
                                 * whatever wakes the port or doesn't */
#define IOCP_DRAIN_TRIES 20     /* the system ending: reads still out waited for in so many waits of ... */
#define IOCP_DRAIN_MS 50

/* a read of no bytes asked of the port for the connection, unless one is out: 0 if the port won't have its socket
 * (or no memory): polled then, as the few are */
static int watch_read(struct ForthNet *net, int slot) {
    Conn *c = &net->conns[slot];
    Watch *w = c->watch;
    WSABUF none = {0, NULL};
    DWORD got = 0, flags = 0;

    if (w == NULL) {
        if ((w = calloc(1, sizeof(Watch))) == NULL) {
            return 0;
        }
        if (CreateIoCompletionPort((HANDLE)c->sock, net->iocp, 0, 0) == NULL) {
            free(w);
            return 0;
        }
        w->slot = slot;
        c->watch = w;
    }
    if (w->pending) {
        return 1;
    }
    memset(&w->ov, 0, sizeof(w->ov));
    w->pending = 1;
    if (WSARecv(c->sock, &none, 1, &got, &flags, &w->ov, NULL) != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        PostQueuedCompletionStatus(net->iocp, 0, 0, &w->ov);   /* (an error at once - the connection reset: told as
                                                                 * something to read, and the read finds it) */
    }
    return 1;   /* (done at once or later: the port tells either way) */
}

/* a connection as the wait is to see it now: its read asked of the port; one the port can't tell of - a listener,
 * one connecting, one with bytes to go - among the few polled, its socket's events waking the port */
static int polled(struct ForthNet *net, int slot) {
    Conn *c = &net->conns[slot];
    short ev;
    long wake;

    if (c->state == CONN_FREE || c->sock == BAD_SOCK) {   /* (a shadow listener has no socket of its own) */
        unpolled(net, c);
        return 1;
    }
    ev = conn_events(c);
    if (c->state == CONN_OPEN && !c->unported && (ev & POLLIN) && !watch_read(net, slot)) {
        c->unported = 1;
    }
    if (c->state == CONN_OPEN && !c->unported) {
        ev &= (short)~POLLIN;   /* (its reading is the port's) */
    }
    if (ev == 0) {
        fds_drop(net, c);
        return 1;
    }
    fds_put(net, slot, ev);
    wake = c->state == CONN_LISTENER ? FD_ACCEPT : FD_CONNECT | FD_WRITE | (c->unported ? FD_READ | FD_CLOSE : 0);
    if (c->edged != wake && WSAEventSelect(c->sock, net->edge, wake) == 0) {
        c->edged = wake;
    }
    return 1;
}
/* ... and no more (before its socket is closed): out of the few polled; a read out is ended by the close, its Watch
 * freed as it comes back */
static void unpolled(struct ForthNet *net, Conn *c) {
    Watch *w = c->watch;

    fds_drop(net, c);
    c->watch = NULL;
    c->edged = 0;
    c->unported = 0;
    if (w == NULL) {
        return;
    }
    if (w->pending) {
        w->gone = 1;
        net->watches_gone++;
        CancelIoEx((HANDLE)c->sock, &w->ov);
    } else {
        free(w);
    }
}
/* (the wait on the event, a thread of the system's pool) something on one of the few polled: the port woken */
static VOID CALLBACK edge_seen(PVOID ctx, BOOLEAN timed_out) {
    struct ForthNet *net = ctx;

    (void)timed_out;
    PostQueuedCompletionStatus(net->iocp, 0, IOCP_EDGE, NULL);
}
#else
/* its socket among those polled, asked what it wants now (a shadow listener has none of its own: not there) */
static int polled(struct ForthNet *net, int slot) {
    Conn *c = &net->conns[slot];

    if (c->state == CONN_FREE || c->sock == BAD_SOCK) {
        unpolled(net, c);
        return 1;
    }
    fds_put(net, slot, conn_events(c));
    return 1;
}
static void unpolled(struct ForthNet *net, Conn *c) { fds_drop(net, c); }
#endif
#endif

/* ---- bytes in and out, plain or TLS ---- */

#define IO_WAIT (-1)    /* conn_recv / conn_send: nothing now (the poll says when) */
#define IO_FAILED (-2)  /* an error: the connection ended */

#ifdef CLANKFORTH_TLS
/* the handshake taken on: 1 done, 0 not yet (OpenSSL waits to read, or to write), -1 failed (why kept: net-why) */
static int tls_handshake(Conn *c) {
    int r = SSL_do_handshake(c->ssl), e;
    long verified;
    unsigned long err;

    if (r == 1) {
        c->tls_ready = 1;
        c->tls_want_write = 0;
        return 1;
    }
    switch (e = SSL_get_error(c->ssl, r)) {
    case SSL_ERROR_WANT_READ: c->tls_want_write = 0; return 0;
    case SSL_ERROR_WANT_WRITE: c->tls_want_write = 1; return 0;
    default:
        verified = SSL_get_verify_result(c->ssl);
        err = ERR_peek_last_error();
        if (verified != X509_V_OK) {
            snprintf(c->why, sizeof(c->why), "TLS: the certificate: %s", X509_verify_cert_error_string(verified));
        } else if (err != 0) {
            snprintf(c->why, sizeof(c->why), "TLS: ");
            ERR_error_string_n(err, c->why + strlen(c->why), sizeof(c->why) - strlen(c->why));
        } else {
            snprintf(c->why, sizeof(c->why), "TLS: the other end closed during the handshake (error %d)", e);
        }
        ERR_clear_error();
        return -1;
    }
}
/* a TLS connection ready for its bytes: its handshake done (tls-connect's actor told net-connected as it is) - 0 if
 * not yet, or it failed (net-closed told) */
static int tls_go(Forth *f, int slot) {
    Conn *c = &N->conns[slot];
    int r;

    if (c->ssl == NULL || c->tls_ready) {
        return 1;
    }
    if ((r = tls_handshake(c)) < 0) {
        ended(f, c, slot);
        return 0;
    }
    if (r > 0 && c->tls_client) {
        tell(f, c->owner, N->k_connected, conn_handle(N, slot), 0, 1);
    }
    return r > 0;
}
static long tls_result(Conn *c, int r) {   /* what SSL_read / SSL_write's r means */
    if (r > 0) {
        return r;
    }
    switch (SSL_get_error(c->ssl, r)) {
    case SSL_ERROR_WANT_READ: return IO_WAIT;
    case SSL_ERROR_WANT_WRITE: c->tls_want_write = 1; return IO_WAIT;
    case SSL_ERROR_ZERO_RETURN: return 0;
    default: ERR_clear_error(); return IO_FAILED;
    }
}
#endif

/* bytes read: how many, 0 the other end closed, IO_WAIT, IO_FAILED */
static long conn_recv(Conn *c, uint8_t *buf, size_t n) {
    long got;

#ifdef CLANKFORTH_TLS
    if (c->ssl != NULL) {
        c->tls_want_write = 0;
        return tls_result(c, SSL_read(c->ssl, buf, n > INT32_MAX ? INT32_MAX : (int)n));
    }
#endif
    got = (long)recv(c->sock, (char *)buf, (int)(n > INT32_MAX ? INT32_MAX : n), 0);
    if (got >= 0) {
        return got;
    }
    return would_block(sock_error()) ? IO_WAIT : IO_FAILED;
}
/* bytes sent: how many, IO_WAIT, IO_FAILED */
static long conn_send(Conn *c, const uint8_t *buf, size_t n) {
    long sent;

#ifdef CLANKFORTH_TLS
    if (c->ssl != NULL) {
        long r;

        c->tls_want_write = 0;
        if (c->record_max > 0 && n > c->record_max) {   /* (a record each, of so many bytes: a protocol's packets) */
            n = c->record_max;
        }
        r = tls_result(c, SSL_write(c->ssl, buf, n > INT32_MAX ? INT32_MAX : (int)n));
        return r == 0 ? IO_FAILED : r;
    }
#endif
    sent = (long)send(c->sock, (const char *)buf, (int)(n > INT32_MAX ? INT32_MAX : n), SEND_FLAGS);
    if (sent > 0) {
        return sent;
    }
    return sent < 0 && would_block(sock_error()) ? IO_WAIT : IO_FAILED;
}

/* ---- the repl: sessions whose lines run as the prompt's ---- */

typedef struct ReplOut {   /* what a session's line prints: kept, sent once it is done (not if it is let go) */
    char *p;
    size_t n, cap;
} ReplOut;

static void repl_out(void *ctx, const char *s, size_t n) {
    ReplOut *ro = ctx;

    if (ro->n + n > ro->cap) {
        size_t cap = ro->cap ? ro->cap : READ_CHUNK;
        char *more;

        while (cap < ro->n + n) {
            cap *= 2;
        }
        if ((more = realloc(ro->p, cap)) == NULL) {
            return;   /* (what doesn't fit isn't said) */
        }
        ro->p = more;
        ro->cap = cap;
    }
    memcpy(ro->p + ro->n, s, n);
    ro->n += n;
}

static void repl_say(Conn *c, const char *s) { (void)conn_queue(c, s, strlen(s)); }

static void repl_session(Forth *f, int slot) {   /* a connection accepted: greeted, its task made */
    Conn *c = &N->conns[slot];
    char hello[256];

    c->repl = REPL_SESSION;
    disown(f, slot);
    c->owner = NOBODY_HERE;
    c->task = forth_task_new(f, "repl");
    snprintf(hello, sizeof(hello),
             "clankforth %s repl: each line runs as at the prompt; `id attach` runs them as that actor, `detach`; "
             "`bye` ends this session\nok\n", forth_version());
    repl_say(c, hello);
}

/* a session's line - after its lines of a definition not finished, if any - run as the prompt's: on its task, as its
 * actor; what it prints, and how it went, said. One that leaves a definition open: let go, kept for the next line */
static void repl_run(Forth *f, int slot, const char *line, size_t n) {
    ReplOut ro = {NULL, 0, 0};
    OutputFn out;
    void *ctx;
    Task *was = f->t, *t = N->conns[slot].task, before;
    Cell handle = conn_handle(N, slot);
    int task_depth = f->task_depth, bye = f->bye, prompt, attached, r, open;
    Conn *c = &N->conns[slot];
    char last[64], *text = malloc(c->pending_n + n + 2);
    size_t len = 0;

    if (text == NULL) {
        repl_say(c, "error: out of memory for the line\nerror\n");
        return;
    }
    if (c->pending_n > 0) {
        memcpy(text, c->pending, c->pending_n);
        len = c->pending_n;
        text[len++] = '\n';
    }
    memcpy(text + len, line, n);
    len += n;
    text[len] = 0;
    before = *t;   /* (its stacks as they were: put back if the line is let go) */
    forth_get_output(f, &out, &ctx);
    forth_set_output(f, repl_out, &ro);
    prompt = forth_actors_attach(f, c->attached);
    f->t = t;
    f->task_depth = -1;   /* (no task's turn: a wait is an error, as at the prompt) */
    r = forth_eval(f, text, len, "repl");
    f->t = was;
    f->task_depth = task_depth;
    attached = forth_actors_attach(f, prompt);
    forth_set_output(f, out, ctx);
    open = r == 0 && forth_compiling(f);
    if (open) {   /* (let go: the dictionary and the stacks as before it) */
        forth_drop_definition(f);
        memcpy(t->ds, before.ds, sizeof(t->ds));
        memcpy(t->fs, before.fs, sizeof(t->fs));
        t->sp = before.sp;
        t->fp = before.fp;
    }
    if ((c = conn_at(f, handle)) == NULL) {   /* (closed by its own line) */
        free(text);
        free(ro.p);
        return;
    }
    free(c->pending);
    c->pending = open ? text : NULL;
    c->pending_n = open ? len : 0;
    if (!open) {
        free(text);
        c->attached = attached;
        if (ro.n > 0) {
            (void)conn_queue(c, ro.p, ro.n);
            if (ro.p[ro.n - 1] != '\n') {
                repl_say(c, "\n");
            }
        }
    }
    free(ro.p);
    if (f->bye && !bye) {   /* (`bye` ends the session, not the system) */
        f->bye = 0;
        repl_say(c, "bye\n");
        c->closing = 1;
        return;
    }
    if (r != 0) {
        snprintf(last, sizeof(last), "error\n");
    } else if (open) {
        snprintf(last, sizeof(last), "compiling\n");
    } else if (attached >= 0) {
        snprintf(last, sizeof(last), "ok %.31s#%d\n", forth_actor_name(f, attached), attached);
    } else {
        snprintf(last, sizeof(last), "ok\n");
    }
    repl_say(c, last);
}

/* every session's whole lines run, in turn; any? */
/* a line of an HTTP request - its first (`POST / HTTP/1.1`) or its Host header: a web page the machine's browser
 * has open sending to the repl's port (a text/plain POST needs no preflight - its body would run, line by line, as
 * Forth). Such a session is ended before the line runs, as Redis does. */
static int http_line(const char *line, size_t n) {
    static const char host[] = "host:";
    static const char version[] = " HTTP/1.";
    size_t hn = sizeof(host) - 1, vn = sizeof(version) - 1, i;

    if (n >= hn) {
        for (i = 0; i < hn && tolower((unsigned char)line[i]) == host[i]; i++) {
        }
        if (i == hn) {
            return 1;
        }
    }
    return n >= vn + 1 && memcmp(line + n - vn - 1, version, vn) == 0;   /* (... HTTP/1.x) */
}
static int repl_lines(Forth *f) {
    int i, any = 0;

    for (i = 0; i < N->ntend; i++) {   /* (a session is tended as long as it is one; what a line runs may move the
                                         * table, and the list: each read anew) */
        for (;;) {
            int slot = N->tend[i];
            Conn *c = &N->conns[slot];
            uint8_t *nl;
            size_t n;
            char *line;

            if (c->state != CONN_OPEN || c->repl != REPL_SESSION || c->ended || c->closing) {
                break;
            }
            nl = c->in_n > 0 ? memchr(c->in, '\n', c->in_n) : NULL;
            if (nl == NULL) {
                if (c->in_n > REPL_LINE_MAX) {
                    c->in_n = 0;
                    repl_say(c, "error: a line longer than the repl takes\nerror\n");
                }
                break;
            }
            n = (size_t)(nl - c->in);
            if ((line = malloc(n + 1)) == NULL) {
                break;
            }
            memcpy(line, c->in, n);
            line[n] = 0;
            c->in_n -= n + 1;
            memmove(c->in, nl + 1, c->in_n);
            if (n > 0 && line[n - 1] == '\r') {   /* (a telnet's line) */
                n--;
            }
            if (http_line(line, n)) {
                forth_printf(f, "repl: a session sent HTTP (a web page reaching the repl's port?): ended, nothing run\n");
                c->in_n = 0;
                c->closing = 1;
                free(line);
                break;
            }
            repl_run(f, slot, line, n);
            free(line);
            any = 1;
        }
    }
    return any;
}

/* repl-listen ( port -- listener ) a repl on this machine's port (127.0.0.1 only: what connects runs any code) */
static void p_repl_listen(Forth *f, Word *w) {
    Cell port = forth_pop(f);
    int slot;

    (void)w;
    if (port < 0 || port > 65535) {
        forth_error(f, "repl-listen: no port %ld", (long)port);
    }
    if ((slot = listen_try(f, port, NOBODY_HERE, 1)) < 0) {
        forth_error(f, "repl-listen: port %ld: %s", (long)port, strerror(-slot));
    }
    N->conns[slot].repl = REPL_LISTENER;
    forth_push(f, conn_handle(N, slot));
}

/* a connection the listener at slot accepted (or was handed: a shared port's): its own, its actor told */
static void accepted(Forth *f, int slot, Sock s, const struct sockaddr_storage *sa) {
    int c;

    set_nonblocking(s);
    no_delay(s);
    c = conn_new(f, s, CONN_OPEN, N->conns[slot].owner);   /* (the table may move: by slot from here) */
    describe_peer(&N->conns[c], sa);
    if (N->conns[slot].repl == REPL_LISTENER) {   /* (a repl session: no actor told, a task of its own) */
        repl_session(f, c);
        return;
    }
#ifdef CLANKFORTH_TLS
    if (N->conns[slot].ctx != NULL) {   /* (a TLS listener's: the handshake as the client's bytes come) */
        SSL *ssl = SSL_new(N->conns[slot].ctx);

        if (ssl == NULL || SSL_set_fd(ssl, (int)s) != 1) {
            SSL_free(ssl);
            ERR_clear_error();
            conn_free(f, &N->conns[c]);
            return;
        }
        SSL_set_accept_state(ssl);
        N->conns[c].ssl = ssl;
    }
#endif
    tell(f, N->conns[c].owner, N->k_accepted, conn_handle(N, slot), conn_handle(N, c), 2);
}
static void accept_all(Forth *f, int slot) {
    for (;;) {
        struct sockaddr_storage sa;
        socklen_t len = sizeof(sa);
        Sock s = accept(N->conns[slot].sock, (struct sockaddr *)&sa, &len);

        if (s == BAD_SOCK) {
            return;   /* (none more now; an error - out of descriptors - tried again next time) */
        }
#ifdef CLANKFORTH_IOCP
        WSAEventSelect(s, NULL, 0);   /* (it has its listener's events, as accepted: none of its own yet) */
#endif
#ifdef SHARE_PORTS
        if (N->conns[slot].shared >= 0 && share_hand(f, N->conns[slot].shared, s, &sa)) {
            continue;   /* (another node's turn: handed to it) */
        }
#endif
        accepted(f, slot, s, &sa);
    }
}

static void ended(Forth *f, Conn *c, int slot) {   /* the other end gone, or an error: net-closed, freed next poll */
    tend(N, slot);
    if (!c->ended) {
        c->ended = 1;
        if (c->repl == REPL_NONE) {
            tell(f, c->owner, N->k_closed, conn_handle(N, slot), 0, 1);
        }
    }
}

static void read_all(Forth *f, int slot) {
    Conn *c = &N->conns[slot];
    int got_any = 0;

    if (c->repl == REPL_NONE && forth_actor_stopped_mid_work(f, c->owner)) {
        /* (its owner's handler stands where its slice ended, in the middle of this input perhaps - a body's chunks,
         *  a frame unmasked: loops of Forth's words over net-input's address, longer than a slice. More read now
         *  would move the bytes from under it: a body of 340,000 one-byte chunks sent in pieces had the handler
         *  reading where they had been, freed - anyone's request, and the server gone. Read when it has gone on:
         *  held, so the round comes back to it) */
        c->held = 1;
        return;
    }
#ifdef CLANKFORTH_TLS
    if (!tls_go(f, slot)) {
        return;
    }
#endif
    for (;;) {
        long got;
        size_t want;

        if (c->in_n >= c->in_max) {   /* (enough not yet taken: read on once some is - its poll asks no more till then) */
            c->held = 1;
            break;
        }
        if (c->in_cap - c->in_n < BUF_START) {   /* (room for a piece more: doubled - a request's few kB, a body's more) */
            size_t cap = c->in_cap ? c->in_cap * 2 : BUF_START;
            uint8_t *more = realloc(c->in, cap);

            if (more == NULL) {
                break;   /* (read again next time) */
            }
            c->in = more;
            c->in_cap = cap;
        }
        want = c->in_cap - c->in_n;
        if (want > c->in_max - c->in_n) {   /* (no more than its limit lets in) */
            want = c->in_max - c->in_n;
        }
        got = conn_recv(c, c->in + c->in_n, want);
        if (got > 0) {
            c->in_n += (size_t)got;
            got_any = 1;
            continue;
        }
        if (got != IO_WAIT) {
            if (got_any && c->repl == REPL_NONE) {
                tell(f, c->owner, N->k_received, conn_handle(N, slot), 0, 1);
            }
            c->shut = got == 0 && c->repl == REPL_NONE;   /* (a clean end: what it asked may still be answered) */
            ended(f, c, slot);
            return;
        }
        break;
    }
    if (got_any && c->repl == REPL_NONE) {   /* (a repl session's lines: run at the poll's end) */
        tell(f, c->owner, N->k_received, conn_handle(N, slot), 0, 1);
    }
}

static void write_some(Forth *f, int slot) {
    Conn *c = &N->conns[slot];

#ifdef CLANKFORTH_TLS
    if (!tls_go(f, slot)) {
        return;
    }
#endif
    while (c->out_at < c->out_n) {
        long sent = conn_send(c, c->out + c->out_at, c->out_n - c->out_at);

        if (sent > 0) {
            c->out_at += (size_t)sent;
        } else {
            if (sent != IO_WAIT) {   /* (nothing more can be sent it: what waited is dropped, and no clean end of
                                      *  its own has it tried again - it was, each round, for ever) */
                c->out_at = c->out_n = 0;
                c->shut = 0;
                ended(f, c, slot);
            }
            return;
        }
    }
    c->out_at = c->out_n = 0;
    free(c->out);   /* (all sent: an idle connection keeps no buffer) */
    c->out = NULL;
    c->out_cap = 0;
    if (c->closing) {
        conn_linger(f, c);
    } else if (c->shut_out == SHUT_OUT_ASKED) {
        shutdown(c->sock, SHUT_WR);
        c->shut_out = SHUT_OUT_DONE;
    }
}

static void connect_done(Forth *f, int slot) {
    Conn *c = &N->conns[slot];
    int err = 0;
    socklen_t len = sizeof(err);

    getsockopt(c->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
    while (err != 0 && c->next_addr != NULL) {   /* refused there: the host's next address */
        const struct addrinfo *ai = c->next_addr;
        Sock s;

        c->next_addr = ai->ai_next;
        if ((s = connect_start(ai, &err)) != BAD_SOCK) {
            unpolled(N, c);   /* (another socket: asked about anew) */
            sock_close(c->sock);
            c->sock = s;
#ifdef CLANKFORTH_TLS
            if (c->ssl != NULL) {
                SSL_set_fd(c->ssl, (int)s);
            }
#endif
            return;   /* (the poll says how this one goes) */
        }
    }
    if (err != 0) {
        snprintf(c->why, sizeof(c->why), "%s", strerror(err));
        ended(f, c, slot);
        return;
    }
    freeaddrinfo(c->addrs);
    c->addrs = c->next_addr = NULL;
    c->state = CONN_OPEN;
#ifdef CLANKFORTH_TLS
    if (c->ssl != NULL) {   /* (tls-connect's: told once its handshake is done) */
        tls_go(f, slot);
        return;
    }
#endif
    tell(f, c->owner, N->k_connected, conn_handle(N, slot), 0, 1);
}

/* a tended connection stays so while it waits for something: to be freed, bytes to go, room to read on, a deadline
 * (a repl's session: its lines) */
static int tend_more(const Conn *c) {
    return c->state != CONN_FREE && (c->ended || c->closing || c->lingering || c->held || c->out_at < c->out_n ||
                                     c->repl == REPL_SESSION);
}

/* something on a connection's socket (ev: the poll's words for it): accepted, connected, read, written */
static void conn_ready(Forth *f, int slot, short ev) {
    if (N->conns[slot].state == CONN_FREE) {
        return;
    }
    tend(N, slot);
    switch (N->conns[slot].state) {
    case CONN_LISTENER:
        accept_all(f, slot);
        break;
    case CONN_CONNECTING:
        connect_done(f, slot);
        break;
    default:
        if (N->conns[slot].lingering) {
            linger_read(f, &N->conns[slot]);
            break;
        }
        if ((ev & POLLERR) && N->conns[slot].shut && N->conns[slot].ended) {   /* (one kept for its answer, the
                                                * other end gone altogether now: nothing more to send it) */
            N->conns[slot].shut = 0;
            N->conns[slot].out_at = N->conns[slot].out_n = 0;
        }
        if ((ev & (POLLIN | POLLHUP | POLLERR)) && !N->conns[slot].shut && !N->conns[slot].ended) {
            read_all(f, slot);
        }
#ifdef CLANKFORTH_TLS
        else if ((ev & POLLOUT) && N->conns[slot].ssl != NULL && N->conns[slot].tls_want_write) {
            read_all(f, slot);   /* (a handshake, or a read, that waited to write) */
        }
#endif
        if ((ev & POLLOUT) && N->conns[slot].state == CONN_OPEN && !N->conns[slot].ended) {
            write_some(f, slot);
        }
    }
}

#ifndef CLANKFORTH_EPOLL
/* the sockets polled (N->fds) waited for up to ms, and what is on them seen to; 1 if anything was */
static int sockets_ready(Forth *f, long ms) {
    int n = N->nfds, i, any = 0;

    if (n == 0 || poll_sockets(N->fds, n, (int)(ms > INT32_MAX ? INT32_MAX : ms)) <= 0) {
        return 0;
    }
    for (i = 0; i < n && i < N->nfds; i++) {
        short ev;

        if (i > 0 && N->fds[i - 1].revents != 0) {   /* (the one before freed as it was seen to, the last put in its
                                                       * place with something on it: that one first) */
            i--;
        }
        if ((ev = N->fds[i].revents) == 0) {
            continue;
        }
        N->fds[i].revents = 0;   /* (seen to) */
        any = 1;
#ifndef CLANKFORTH_IOCP
        if (N->fd_conn[i] < 0) {   /* (the wake pair) */
            wake_drain(N);
#ifdef SHARE_PORTS
            share_take(f);   /* (connections handed from the node that bound a shared port) */
#endif
            continue;
        }
#endif
        conn_ready(f, N->fd_conn[i], ev);
    }
    return any;
}
#endif

/* one round: waits up to ms (less if a timer is due sooner) for the sockets or a message from another node, then
 * what happened told to the actors (queued: the next delivery runs their handlers); 1 if anything did. What it does
 * is for the connections tended and the sockets with something on them - not a walk of all there are. */
int forth_net_poll(Forth *f, long ms) {
    long long now = now_ms();
    int i, kept = 0, any = 0;
#ifdef CLANKFORTH_EPOLL
    struct epoll_event events[EPOLL_READY_MAX];
    int n;
#endif

    if (N == NULL) {
        return forth_node_wait(f, ms);
    }
    for (i = 0; i < N->ntend; i++) {
        int slot = N->tend[i];
        Conn *c = &N->conns[slot];

        if (c->state == CONN_FREE) {
            continue;
        }
        /* the ended - told last round - and those whose actor is gone: closed; one closing still sends what it has,
         * its actor gone or not */
        if (c->ended && c->shut && c->repl == REPL_NONE && forth_actor_handling(f, c->owner)) {
            /* (a clean end of its sending, its owner's handler still at work - waiting for a file, another actor:
             *  what that writes is sent as it is written, and the connection kept till the handler has ended. It
             *  was let go here, and the answer with it) */
            if (c->out_at < c->out_n) {
                write_some(f, slot);
            }
            continue;
        }
        if (c->ended && c->repl == REPL_NONE && forth_actor_stopped_mid_work(f, c->owner)) {
            /* (ended - reset, perhaps - with its owner's handler where its slice left it, in the middle of the
             *  connection's input: kept, its bytes where they are, till the handler has gone on and looked) */
            continue;
        }
        if (c->ended && shut_with_output(c)) {   /* (its answer written: sent, then let go) */
            close_after_output(c);
        }
        if (c->ended || (c->repl == REPL_NONE && forth_actor_body(f, c->owner) == NULL && !c->closing) ||
            (c->lingering && now >= c->linger_until)) {
            conn_free(f, c);
            continue;
        }
        if (c->lingering && c->linger_until - now < ms) {
            ms = c->linger_until - now > 0 ? (long)(c->linger_until - now) : 0;
        }
        if (c->in != NULL && c->in_n == 0) {   /* (all its input taken: its buffer let go - here, between the handlers,
                                                * none of which holds net-input's address now - made again as more comes) */
            free(c->in);
            c->in = NULL;
            c->in_cap = 0;
        }
        if (c->state == CONN_OPEN && c->held && c->in_n < c->in_max && !c->lingering) {   /* (one held at its limit,
                                                * with room now: read - what TLS decrypted and kept wakes no poll) */
            c->held = 0;
            read_all(f, slot);
            any = 1;
            ms = 0;
        }
        if (c->state == CONN_OPEN && !c->ended && c->out_at < c->out_n) {   /* (what its handlers wrote since the
                                                * last round: sent now - only a socket that won't take it all is
                                                * asked about, to be told when there is room) */
            write_some(f, slot);
        }
    }
    for (i = 0; i < N->ntend; i++) {   /* (what the poll asks of each, as it is now; those waiting for nothing: off
                                         * the list till something happens to them) */
        int slot = N->tend[i];
        Conn *c = &N->conns[slot];

        if (!polled(N, slot) || tend_more(c)) {
            N->tend[kept++] = slot;
        } else {
            c->tended = 0;
        }
    }
    N->ntend = kept;
    if (N->nheap > 0 && N->timers[N->heap[0]].due - now < ms) {   /* (the soonest timer) */
        ms = N->timers[N->heap[0]].due - now > 0 ? (long)(N->timers[N->heap[0]].due - now) : 0;
    }
#ifdef CLANKFORTH_EPOLL
    n = epoll_wait(N->epoll, events, EPOLL_READY_MAX, (int)(ms > INT32_MAX ? INT32_MAX : ms));
    for (i = 0; i < n; i++) {
        uint64_t what = events[i].data.u64;
        int slot = (int)(uint32_t)what;

        any = 1;
        if (what == EPOLL_WAKE) {
            wake_drain(N);
        } else if (slot < N->nconns && N->conns[slot].generation == (int)(what >> EPOLL_GENERATION_SHIFT)) {   /* (not one freed since) */
            conn_ready(f, slot, epoll_says(events[i].events));
        }
    }
#elif defined(CLANKFORTH_IOCP)
    {
        OVERLAPPED_ENTRY packets[IOCP_ENTRIES];
        ULONG got = 0;
        int edge = 0;

        for (i = 0; i < N->nfds; i++) {   /* (one waiting to connect or to send: looked at again before long) */
            if (N->conns[N->fd_conn[i]].state != CONN_LISTENER && ms > IOCP_RECHECK_MS) {
                ms = IOCP_RECHECK_MS;
            }
        }
        if (sockets_ready(f, 0)) {   /* (the few polled, as they stand: an event wakes the port only for what happens
                                      * from here on) */
            any = 1;
            ms = 0;
        }
        if (!GetQueuedCompletionStatusEx(N->iocp, packets, IOCP_ENTRIES, &got, (DWORD)(ms > INT32_MAX ? INT32_MAX : ms),
                                         FALSE)) {
            got = 0;   /* (nothing came in that time) */
        }
        for (i = 0; i < (int)got; i++) {
            Watch *w = (Watch *)packets[i].lpOverlapped;
            int slot;

            any = 1;
            if (w == NULL) {   /* (a wake, or an edge: the few polled are looked at below) */
                edge |= packets[i].lpCompletionKey == IOCP_EDGE;
#ifdef SHARE_PORTS
                if (packets[i].lpCompletionKey == IOCP_WAKE) {
                    share_take(f);   /* (connections handed from the node that bound a shared port) */
                }
#endif
                continue;
            }
            w->pending = 0;
            if (w->gone) {   /* (its connection closed with this read out: the read ended by that) */
                free(w);
                N->watches_gone--;
                continue;
            }
            slot = w->slot;
            if (slot < N->nconns && N->conns[slot].watch == w) {   /* (its connection's still) */
                conn_ready(f, slot, POLLIN);
            }
        }
        if (edge) {
            any |= sockets_ready(f, 0);
        }
    }
#else
    any |= sockets_ready(f, ms);
#endif
    now = now_ms();
    while (N->nheap > 0 && N->timers[N->heap[0]].due <= now) {   /* (the timers due: the soonest first) */
        int slot = N->heap[0], owner = N->timers[slot].owner;
        Cell id = N->timers[slot].id;

        timer_drop(N, slot);
        tell(f, owner, N->k_timer, id, 0, 1);
        any = 1;
    }
    if (f->t == f->main) {   /* (the repl sessions' lines: from the interpreter's loop, never inside a handler) */
        any |= repl_lines(f);
    }
    for (i = 0; i < N->ntend; i++) {   /* (what was written this round: sent now, not a round later) */
        int slot = N->tend[i];
        const Conn *c = &N->conns[slot];

        if (c->state == CONN_OPEN && !c->ended && c->out_at < c->out_n) {
            write_some(f, slot);
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

/* serve ( -- ) the loop, until `bye`: a round - what can run, runs - then a wait for what comes (forth_round says how
 * long). From the interpreter (a program's last line). */
static void p_serve(Forth *f, Word *w) {
    (void)w;
    if (f->t != f->main) {
        forth_error(f, "serve: only from the interpreter (it runs the tasks and the actors itself)");
    }
    while (!forth_bye(f)) {
        if (forth_shutdown_signalled()) {   /* (told to stop: its words; the nodes' run meanwhile - then it ends) */
            long waited = 0;

            forth_nodes_shutdown_seen();   /* (the nodes asleep: woken to run their words) */
            forth_shutdown_run(f);
            while (forth_nodes_draining(f) > 0 && waited < forth_shutdown_grace()) {
                (void)forth_round(f);
                forth_net_poll(f, DRAIN_POLL_MS);
                waited += DRAIN_POLL_MS;
            }
            (void)forth_round(f);   /* (what the last wait told of is delivered before the program goes on: a
                                     * connection ended then is let go at the next wait, heard or not) */
            return;
        }
        forth_net_poll(f, forth_round(f));   /* (no wait while something can run) */
    }
}

#ifdef CLANKFORTH_TLS
/* ---- TLS ---- */

static void tls_fail(Forth *f, SSL_CTX *ctx, const char *word, const char *what) {
    char why[TLS_ERROR_BYTES];

    ERR_error_string_n(ERR_get_error(), why, sizeof(why));
    ERR_clear_error();
    SSL_CTX_free(ctx);
    forth_error(f, "%s: %s: %s", word, what, why);
}
static void tls_common(SSL_CTX *ctx) {   /* both ends': TLS 1.2 at least, writes that may be partial */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   /* (OpenSSL writes to the socket itself: the other end gone is an error, not a death) */
#endif
}
static const char *path_of(Forth *f, Cell a, Cell n, const char *word, char *buf, size_t size) {
    size_t len;
    const char *p = forth_string_arg(f, a, n, word, &len);

    if (len >= size) {
        forth_error(f, "%s: a path longer than %zu bytes", word, size - 1);
    }
    memcpy(buf, p, len);
    buf[len] = 0;
    return buf;
}

/* tls-listen ( port cert n key n -- listener ) a listener whose connections are TLS: the certificate (its chain, PEM)
 * and its private key from these files */
static void p_tls_listen(Forth *f, Word *w) {
    Cell kn = forth_pop(f), ka = forth_pop(f), cn = forth_pop(f), ca = forth_pop(f), port = forth_pop(f);
    char cert[HOST_BYTES * 4], key[HOST_BYTES * 4];
    SSL_CTX *ctx;
    int slot;

    (void)w;
    path_of(f, ca, cn, "tls-listen", cert, sizeof(cert));
    path_of(f, ka, kn, "tls-listen", key, sizeof(key));
    if ((ctx = SSL_CTX_new(TLS_server_method())) == NULL) {
        tls_fail(f, NULL, "tls-listen", "no context");
    }
    tls_common(ctx);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
        tls_fail(f, ctx, "tls-listen", cert);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
        tls_fail(f, ctx, "tls-listen", key);
    }
    if (port < 0 || port > 65535) {   /* (listen_on's errors, before it: the context not to be left) */
        SSL_CTX_free(ctx);
        forth_error(f, "tls-listen: no port %ld", (long)port);
    }
    slot = listen_try(f, port, self_actor(f, "tls-listen"), 0);
    if (slot < 0) {
        char why[TLS_ERROR_BYTES];

        snprintf(why, sizeof(why), "%s", strerror(-slot));
        SSL_CTX_free(ctx);
        forth_error(f, "tls-listen: port %ld: %s", (long)port, why);
    }
    N->conns[slot].ctx = ctx;
    forth_push(f, conn_handle(N, slot));
}

/* tls-record-max! ( n conn -- ) what is written on the connection goes in TLS records of n bytes at most (0:
 * OpenSSL's own, 16 KB) - a protocol whose packets must each be a record of its own (SQL Server's TDS 8.0) */
static void p_tls_record_max(Forth *f, Word *w) {
    Conn *c = conn_arg(f, forth_pop(f), "tls-record-max!");
    Cell n = forth_pop(f);

    (void)w;
    if (n < 0) {
        forth_error(f, "tls-record-max!: %ld bytes", (long)n);
    }
    c->record_max = (size_t)n;
}

/* tls-ca! ( path n -- ) the certificates tls-connect trusts from now: this file's (PEM); "" the system's */
static void p_tls_ca(Forth *f, Word *w) {
    Cell n = forth_pop(f), a = forth_pop(f);

    (void)w;
    path_of(f, a, n, "tls-ca!", N->ca_file, sizeof(N->ca_file));
    SSL_CTX_free(N->client_ctx);   /* (made again, trusting those, as next wanted) */
    N->client_ctx = NULL;
}

#define ALPN_BYTES 256   /* tls-connect-as's protocols, as ALPN carries them (each its length, then its name) */

/* a TLS connection to the host: the certificate checked against `name` (sent too, for the server to choose by) - or
 * the host itself, name "" - ALPN offering `alpn` (names split by commas; "" none) */
static void tls_connect(Forth *f, Cell a, Cell n, Cell port, Cell na, Cell nn, Cell aa, Cell an, const char *word) {
    char host[HOST_BYTES], name[HOST_BYTES];
    unsigned char ip[16], alpn[ALPN_BYTES];
    size_t alpn_n = 0;
    SSL *ssl;
    Conn *c;
    int slot, is_ip;

    if (nn < 0 || nn >= HOST_BYTES || an < 0 || an >= ALPN_BYTES) {
        forth_error(f, "%s: a name or ALPN too long", word);
    }
    if (an > 0) {   /* (a,b -> 1a1b: each name its length before it) */
        const char *src = forth_addr_n(f, aa, (size_t)an);
        Cell i, from = 0;

        for (i = 0; i <= an; i++) {
            if (i == an || src[i] == ',') {
                Cell len = i - from;

                if (len < 1 || len > 255) {
                    forth_error(f, "%s: an ALPN name empty or too long", word);
                }
                alpn[alpn_n++] = (unsigned char)len;
                memcpy(alpn + alpn_n, src + from, (size_t)len);
                alpn_n += (size_t)len;
                from = i + 1;
            }
        }
    }
    if (N->client_ctx == NULL) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());

        if (ctx == NULL) {
            tls_fail(f, NULL, word, "no context");
        }
        tls_common(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        if (N->ca_file[0] ? SSL_CTX_load_verify_locations(ctx, N->ca_file, NULL) != 1
                          : SSL_CTX_set_default_verify_paths(ctx) != 1) {
            tls_fail(f, ctx, word, N->ca_file[0] ? N->ca_file : "the system's certificates");
        }
        N->client_ctx = ctx;
    }
    if (nn > 0) {
        memcpy(name, forth_addr_n(f, na, (size_t)nn), (size_t)nn);
    }
    name[nn] = '\0';
    slot = connect_to(f, a, n, port, word, host);
    if (nn == 0) {
        memcpy(name, host, sizeof(name));
    }
    c = &N->conns[slot];
    is_ip = inet_pton(AF_INET, name, ip) == 1 || inet_pton(AF_INET6, name, ip) == 1;
    if ((ssl = SSL_new(N->client_ctx)) == NULL || SSL_set_fd(ssl, (int)c->sock) != 1 ||
        (alpn_n > 0 && SSL_set_alpn_protos(ssl, alpn, (unsigned)alpn_n) != 0) ||
        (is_ip ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name) != 1
               : SSL_set_tlsext_host_name(ssl, name) != 1 ||
                     X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), name, 0) != 1)) {
        SSL_free(ssl);
        conn_free(f, c);
        tls_fail(f, NULL, word, name);
    }
    SSL_set_connect_state(ssl);
    c->ssl = ssl;
    c->tls_client = 1;
    forth_push(f, conn_handle(N, slot));
}

/* tls-connect ( host n port -- conn ) as connect, the connection TLS: the server's certificate checked against the
 * trusted ones and the host's name (or address); net-connected once the handshake is done, net-closed if it fails */
static void p_tls_connect(Forth *f, Word *w) {
    Cell port = forth_pop(f), n = forth_pop(f), a = forth_pop(f);

    (void)w;
    tls_connect(f, a, n, port, 0, 0, 0, 0, "tls-connect");
}

/* tls-connect-as ( host n port name n alpn n -- conn ) as tls-connect, the certificate checked against `name` (""
 * the host's) - a server reached at another address than the name it answers to - and ALPN offering `alpn`'s
 * protocols ("h2,http/1.1"; "" none) */
static void p_tls_connect_as(Forth *f, Word *w) {
    Cell an = forth_pop(f), aa = forth_pop(f), nn = forth_pop(f), na = forth_pop(f);
    Cell port = forth_pop(f), n = forth_pop(f), a = forth_pop(f);

    (void)w;
    tls_connect(f, a, n, port, na, nn, aa, an, "tls-connect-as");
}
#endif

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
#ifdef CLANKFORTH_IOCP
    if (N->edge_wait != NULL) {
        UnregisterWaitEx(N->edge_wait, INVALID_HANDLE_VALUE);   /* (waits out a telling under way: none after) */
    }
    for (i = 0; N->iocp != NULL && N->watches_gone > 0 && i < IOCP_DRAIN_TRIES; i++) {   /* (the reads the closes
                                           * ended come back: their Watches freed - one that doesn't is left) */
        OVERLAPPED_ENTRY packets[IOCP_ENTRIES];
        ULONG got = 0, k;

        if (!GetQueuedCompletionStatusEx(N->iocp, packets, IOCP_ENTRIES, &got, IOCP_DRAIN_MS, FALSE)) {
            got = 0;
        }
        for (k = 0; k < got; k++) {
            Watch *w = (Watch *)packets[k].lpOverlapped;

            if (w != NULL && w->gone) {
                free(w);
                N->watches_gone--;
            }
        }
    }
    if (N->edge != NULL) {
        CloseHandle(N->edge);
    }
    if (N->iocp != NULL) {
        CloseHandle(N->iocp);
    }
#else
    sock_close(N->wake[0]);
    sock_close(N->wake[1]);
#endif
#ifdef CLANKFORTH_EPOLL
    if (N->epoll >= 0) {
        close(N->epoll);
    }
#endif
#ifdef CLANKFORTH_TLS
    SSL_CTX_free(N->client_ctx);
#endif
    free(N->conns);
    free(N->tend);
    free(N->timers);
    free(N->heap);
    free(N->fds);
    free(N->fd_conn);
    free(N);
    N = NULL;
}

void forth_net(Forth *f) {
    static const ForthPrim prims[] = {
        {"listen", p_listen, "( port -- listener )"}, {"listen-local", p_listen_local, "( port -- listener )"},
        {"repl-listen", p_repl_listen, "( port -- listener )"}, {"listener-port", p_listener_port, "( listener -- port )"},
        {"connect", p_connect, "( host n port -- conn )"}, {"net-write", p_net_write, "( a n conn -- )"},
        {"net-input", p_net_input, "( conn -- a n )"}, {"net-consume", p_net_consume, "( n conn -- )"},
        {"net-close", p_net_close, "( conn -- )"}, {"net-shut", p_net_shut, "( conn -- )"}, {"net-owner!", p_net_owner, "( actor conn -- )"},
        {"net-ended?", p_net_ended, "( conn -- flag )"}, {"net-gone?", p_net_gone, "( conn -- flag )"}, {"net-pending", p_net_pending, "( conn -- n )"},
        {"conn-peer", p_conn_peer, "( conn -- s len )"}, {"net-why", p_net_why, "( conn -- s len )"},
        {"net-input-max!", p_net_input_max, "( n conn -- )"}, {"net-output-max!", p_net_output_max, "( n conn -- )"}, {"after", p_after, "( ms -- timer )"}, {"net-waits", p_net_waits, "( -- s len )"},
        {"after-together", p_after_together, "( ms step -- timer )"},
        {"cancel-timer", p_cancel_timer, "( timer -- )"}, {"net-poll", p_net_poll, "( ms -- flag )"},
        {"serve", p_serve, "( -- )"},
#ifdef CLANKFORTH_TLS
        {"tls-listen", p_tls_listen, "( port cert n key n -- listener )"},
        {"tls-connect", p_tls_connect, "( host n port -- conn )"}, {"tls-ca!", p_tls_ca, "( path n -- )"},
        {"tls-connect-as", p_tls_connect_as, "( host n port name n alpn n -- conn )"},
        {"tls-record-max!", p_tls_record_max, "( n conn -- )"},
#endif
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
    N->free_conn = N->free_timer = -1;
#ifdef CLANKFORTH_EPOLL
    N->epoll = -1;
#endif
    if (!conns_grow(N) || wake_pair(N->wake) != 0) {
        free(N->tend);
        free(N->fds);
        free(N->fd_conn);
        free(N->conns);
        free(N);
        N = NULL;
        forth_error(f, "forth_net: no socket pair to wake the loop");
    }
#ifdef CLANKFORTH_EPOLL
    {
        struct epoll_event e;

        memset(&e, 0, sizeof(e));
        e.events = EPOLLIN;
        e.data.u64 = EPOLL_WAKE;
        if ((N->epoll = epoll_create1(EPOLL_CLOEXEC)) < 0 || epoll_ctl(N->epoll, EPOLL_CTL_ADD, N->wake[0], &e) != 0) {
            int why = errno;

            forth_net_free(f);   /* (no net half made: a host that goes on has none, and waits as one without) */
            forth_error(f, "forth_net: no epoll to wait for the sockets with (%s)", strerror(why));
        }
    }
#elif defined(CLANKFORTH_IOCP)
    N->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    N->edge = CreateEventW(NULL, FALSE, FALSE, NULL);   /* (set by a socket's event, unset as its wait is told) */
    if (N->iocp == NULL || N->edge == NULL ||
        !RegisterWaitForSingleObject(&N->edge_wait, N->edge, edge_seen, N, INFINITE, WT_EXECUTEINWAITTHREAD)) {
        unsigned long why = (unsigned long)GetLastError();

        forth_net_free(f);   /* (no net half made) */
        forth_error(f, "forth_net: no completion port to wait for the sockets with (error %lu)", why);
    }
#else
    N->fds[0].fd = N->wake[0];   /* (the first of the sockets polled, always) */
    N->fds[0].events = POLLIN;
    N->fds[0].revents = 0;
    N->fd_conn[0] = -1;
    N->nfds = 1;
#endif
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

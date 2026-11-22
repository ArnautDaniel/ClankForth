/* The crypto vocabulary: what a web server's secrets need, fast - SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104),
 * PBKDF2-HMAC-SHA256 (RFC 8018: a password's hash, slow on purpose), bytes from the system's secure random source,
 * and a comparison that takes as long whatever the bytes. In C: a password's hash is a million SHA-256 blocks.
 * Every host has it (USING: crypto ;). Docs: docs/libraries.md, "crypto". */
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S   /* (rand_s: the system's secure random numbers, RtlGenRandom's) */
#endif
#include "forth_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/random.h>
#elif !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#define SHA256_BLOCK 64        /* bytes a block */
#define SHA256_DIGEST 32       /* bytes a digest */
#define SHA256_ROUNDS 64
#define LENGTH_BYTES 8         /* the message's length in bits, at the end of its last block */
#define HMAC_INNER_PAD 0x36
#define HMAC_OUTER_PAD 0x5c
#define RANDOM_MAX (1 << 20)   /* random-bytes: at most this many at once */
#define PBKDF2_LENGTH_MAX 1024 /* a derived key's bytes, at most */
#define PBKDF2_ITERATIONS_MAX 100000000L   /* (more: a mistake - it would hold the node for minutes) */

/* ---- SHA-256 ---- */

typedef struct Sha256 {
    uint32_t h[8];
    uint8_t block[SHA256_BLOCK];
    size_t used;               /* bytes in block */
    uint64_t total;            /* bytes so far */
} Sha256;

static const uint32_t sha256_k[SHA256_ROUNDS] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static const uint32_t sha256_start[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t load_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static void store_be32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

static void sha256_compress(uint32_t h[8], const uint8_t block[SHA256_BLOCK]) {
    uint32_t w[SHA256_ROUNDS], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be32(block + 4 * i);
    }
    for (i = 16; i < SHA256_ROUNDS; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < SHA256_ROUNDS; i++) {
        uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

static void sha256_init(Sha256 *s) {
    memcpy(s->h, sha256_start, sizeof(s->h));
    s->used = 0;
    s->total = 0;
}

static void sha256_add(Sha256 *s, const uint8_t *p, size_t n) {
    s->total += n;
    while (n > 0) {
        size_t take = SHA256_BLOCK - s->used < n ? SHA256_BLOCK - s->used : n;

        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == SHA256_BLOCK) {
            sha256_compress(s->h, s->block);
            s->used = 0;
        }
    }
}

static void sha256_end(Sha256 *s, uint8_t out[SHA256_DIGEST]) {
    uint64_t bits = s->total * 8;
    int i;

    s->block[s->used++] = 0x80;
    if (s->used > SHA256_BLOCK - LENGTH_BYTES) {
        memset(s->block + s->used, 0, SHA256_BLOCK - s->used);
        sha256_compress(s->h, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, SHA256_BLOCK - LENGTH_BYTES - s->used);
    for (i = 0; i < LENGTH_BYTES; i++) {
        s->block[SHA256_BLOCK - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
    sha256_compress(s->h, s->block);
    for (i = 0; i < 8; i++) {
        store_be32(out + 4 * i, s->h[i]);
    }
}

static void sha256(const uint8_t *p, size_t n, uint8_t out[SHA256_DIGEST]) {
    Sha256 s;

    sha256_init(&s);
    sha256_add(&s, p, n);
    sha256_end(&s, out);
}

/* ---- HMAC-SHA256: the key's inner and outer states worked out once (PBKDF2 uses them a million times) ---- */

typedef struct Hmac {
    Sha256 inner, outer;   /* each after its padded key's block */
} Hmac;

static void hmac_init(Hmac *m, const uint8_t *key, size_t n) {
    uint8_t k[SHA256_BLOCK], pad[SHA256_BLOCK];
    int i;

    memset(k, 0, sizeof(k));
    if (n > SHA256_BLOCK) {
        sha256(key, n, k);
    } else {
        memcpy(k, key, n);
    }
    for (i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = k[i] ^ HMAC_INNER_PAD;
    }
    sha256_init(&m->inner);
    sha256_add(&m->inner, pad, SHA256_BLOCK);
    for (i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = k[i] ^ HMAC_OUTER_PAD;
    }
    sha256_init(&m->outer);
    sha256_add(&m->outer, pad, SHA256_BLOCK);
}

static void hmac(const Hmac *key, const uint8_t *p, size_t n, uint8_t out[SHA256_DIGEST]) {
    Sha256 s = key->inner;
    uint8_t inner[SHA256_DIGEST];

    sha256_add(&s, p, n);
    sha256_end(&s, inner);
    s = key->outer;
    sha256_add(&s, inner, SHA256_DIGEST);
    sha256_end(&s, out);
}

/* ---- the words ---- */

static const uint8_t *bytes_arg(Forth *f, size_t *n, const char *word) {   /* ( a n -- ) */
    Cell len = forth_pop(f), a = forth_pop(f);

    return (const uint8_t *)forth_string_arg(f, a, len, word, n);
}
static void push_bytes(Forth *f, const uint8_t *p, size_t n) {   /* ( -- s len ) a new string */
    forth_push(f, (Cell)forth_new_string(f, (const char *)p, n));
    forth_push(f, (Cell)n);
}

/* sha256 ( a n -- s len ) the 32-byte digest, a new string */
static void p_sha256(Forth *f, Word *w) {
    size_t n;
    const uint8_t *p = bytes_arg(f, &n, "sha256");
    uint8_t out[SHA256_DIGEST];

    (void)w;
    sha256(p, n, out);
    push_bytes(f, out, SHA256_DIGEST);
}

/* hmac-sha256 ( key n msg n -- s len ) the message's 32-byte MAC by the key, a new string */
static void p_hmac_sha256(Forth *f, Word *w) {
    size_t mn, kn;
    const uint8_t *m = bytes_arg(f, &mn, "hmac-sha256"), *k = bytes_arg(f, &kn, "hmac-sha256");
    uint8_t out[SHA256_DIGEST];
    Hmac key;

    (void)w;
    hmac_init(&key, k, kn);
    hmac(&key, m, mn, out);
    push_bytes(f, out, SHA256_DIGEST);
}

/* pbkdf2-sha256 ( password n salt n iterations length -- s len ) a key of so many bytes from the password: each
 * block U1 ^ U2 ^ ... of HMACs chained so many times (RFC 8018, 5.2) - the slower, the harder to guess a password by */
static void p_pbkdf2_sha256(Forth *f, Word *w) {
    Cell length = forth_pop(f), iterations = forth_pop(f);
    size_t sn, pn, done = 0;
    const uint8_t *salt = bytes_arg(f, &sn, "pbkdf2-sha256"), *password = bytes_arg(f, &pn, "pbkdf2-sha256");
    uint8_t out[PBKDF2_LENGTH_MAX], u[SHA256_DIGEST], t[SHA256_DIGEST], *first;
    uint32_t block;
    Hmac key;

    (void)w;
    if (iterations < 1 || iterations > PBKDF2_ITERATIONS_MAX) {
        forth_error(f, "pbkdf2-sha256: %ld iterations: 1 to %ld", (long)iterations, PBKDF2_ITERATIONS_MAX);
    }
    if (length < 1 || length > PBKDF2_LENGTH_MAX) {
        forth_error(f, "pbkdf2-sha256: a key of %ld bytes: 1 to %d", (long)length, PBKDF2_LENGTH_MAX);
    }
    if ((first = malloc(sn + 4)) == NULL) {
        forth_error(f, "pbkdf2-sha256: out of memory");
    }
    memcpy(first, salt, sn);
    hmac_init(&key, password, pn);
    for (block = 1; done < (size_t)length; block++) {
        Cell i;
        int j;
        size_t take;

        store_be32(first + sn, block);
        hmac(&key, first, sn + 4, u);
        memcpy(t, u, SHA256_DIGEST);
        for (i = 1; i < iterations; i++) {
            hmac(&key, u, SHA256_DIGEST, u);
            for (j = 0; j < SHA256_DIGEST; j++) {
                t[j] ^= u[j];
            }
        }
        take = (size_t)length - done < SHA256_DIGEST ? (size_t)length - done : SHA256_DIGEST;
        memcpy(out + done, t, take);
        done += take;
    }
    free(first);
    push_bytes(f, out, (size_t)length);
}

/* the system's secure random bytes into buf; an error if it has none to give */
static void secure_random(Forth *f, uint8_t *buf, size_t n) {
#if defined(_WIN32)
    size_t i;

    for (i = 0; i < n; i += sizeof(unsigned int)) {
        unsigned int r;
        size_t take = n - i < sizeof(r) ? n - i : sizeof(r);

        if (rand_s(&r) != 0) {
            forth_error(f, "random-bytes: the system's random source failed");
        }
        memcpy(buf + i, &r, take);
    }
#elif defined(__linux__)
    size_t done = 0;

    while (done < n) {
        ssize_t got = getrandom(buf + done, n - done, 0);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            forth_error(f, "random-bytes: %s", strerror(errno));
        }
        done += (size_t)got;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__)
    (void)f;
    arc4random_buf(buf, n);
#else
    size_t done = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0) {
        forth_error(f, "random-bytes: /dev/urandom: %s", strerror(errno));
    }
    while (done < n) {
        ssize_t got = read(fd, buf + done, n - done);

        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            close(fd);
            forth_error(f, "random-bytes: /dev/urandom gave no more");
        }
        done += (size_t)got;
    }
    close(fd);
#endif
}

/* random-bytes ( n -- s len ) so many bytes from the system's secure random source (not `random`'s, which a seed
 * replays): a session's id, a salt, a key - a new string */
static void p_random_bytes(Forth *f, Word *w) {
    Cell n = forth_pop(f);
    uint8_t *buf;

    (void)w;
    if (n < 0 || n > RANDOM_MAX) {
        forth_error(f, "random-bytes: %ld bytes: 0 to %d", (long)n, RANDOM_MAX);
    }
    if ((buf = malloc((size_t)n + 1)) == NULL) {
        forth_error(f, "random-bytes: out of memory");
    }
    secure_random(f, buf, (size_t)n);   /* (an error here: buf lost - it hasn't happened on a working system) */
    push_bytes(f, buf, (size_t)n);
    free(buf);
}

/* same-bytes? ( a n a2 n2 -- flag ) the same bytes - looked at all, however soon they differ (a MAC or a token
 * compared so takes as long right or wrong: its time tells a guesser nothing) */
static void p_same_bytes(Forth *f, Word *w) {
    size_t bn, an, i;
    const uint8_t *b = bytes_arg(f, &bn, "same-bytes?"), *a = bytes_arg(f, &an, "same-bytes?");
    unsigned differ = an != bn;

    (void)w;
    for (i = 0; i < an && i < bn; i++) {
        differ |= (unsigned)(a[i] ^ b[i]);
    }
    forth_push(f, differ == 0 ? -1 : 0);
}

void forth_crypto_words(Forth *f) {
    static const ForthPrim prims[] = {
        {"sha256", p_sha256, "( a n -- s len )"},
        {"hmac-sha256", p_hmac_sha256, "( key n msg n -- s len )"},
        {"pbkdf2-sha256", p_pbkdf2_sha256, "( password n salt n iterations length -- s len )"},
        {"random-bytes", p_random_bytes, "( n -- s len )"},
        {"same-bytes?", p_same_bytes, "( a n a2 n2 -- flag )"},
    };
    Vocab *was = forth_current(f), *v = forth_vocab(f, "crypto");

    forth_set_current(f, v);   /* scripts say USING: crypto ; */
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_set_current(f, was);
}

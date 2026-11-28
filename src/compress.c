/* The compression vocabulary: DEFLATE (RFC 1951) both ways, and gzip's wrapping (RFC 1952) - what a web server's
 * pages go in (Content-Encoding: gzip) and a WebSocket's messages (permessage-deflate, RFC 7692). The work is zlib's
 * where the build found it (CLANKFORTH_ZLIB); without, its own code: LZ77 over a 32 KB window with hash chains, each
 * block's Huffman codes made for its symbols (limited to 15 bits); inflating as zlib's puff.c does, its lengths and
 * distances checked. Either way a call keeps nothing after it and shares nothing with another: any actor's, on any
 * node, at once. Every host has it (USING: compression ;). Docs: docs/libraries.md, "compression". */
#include "forth_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CLANKFORTH_ZLIB
#include <zlib.h>
#elif defined(CLANKFORTH_NODES)
#include <pthread.h>
#endif

#define WINDOW 32768              /* how far back a match may reach */
#define MIN_MATCH 3
#define MAX_MATCH 258
#define HASH_BITS_MAX 15          /* the places a three-byte run was last seen: by its hash - a table of 2^15 at most ... */
#define HASH_BITS_MIN 8           /* ... and of 2^8 at least: one as big as the input calls for (a short message's small) */
#define CHAIN_MAX 96              /* earlier places tried for a match at each place */
#define NICE_MATCH 128            /* a match this long is taken without looking further */
#define BLOCK_SYMBOLS 32768       /* literals and matches a block holds before it is written */
#define SHORT_BLOCK 2048          /* a block of no more: written in the fixed codes too, the shorter kept */
#define LITLEN_CODES 286          /* literals 0-255, the end of a block 256, lengths 257-285 */
#define DIST_CODES 30
#define CODELEN_CODES 19
#define MAX_BITS 15               /* a code's length at most */
#define CODELEN_MAX_BITS 7        /* ... a code length code's */
#define END_OF_BLOCK 256
#define FIXED_LITLEN 288          /* the fixed codes' (inflating) */
#define FIXED_DIST 30
#define INPUT_MAX ((size_t)1 << 30)   /* bytes deflate takes at once, at most */
#define GZIP_HEADER 10
#define GZIP_TRAILER 8            /* its CRC-32 and length */
#define GZIP_ID1 0x1f
#define GZIP_ID2 0x8b
#define GZIP_DEFLATE 8
#define GZIP_OS_UNKNOWN 255
#define GZ_FHCRC 2                /* gzip's flags: a header CRC, extra fields, a name, a comment */
#define GZ_FEXTRA 4
#define GZ_FNAME 8
#define GZ_FCOMMENT 16
#define CRC32_POLY 0xedb88320u

/* ---- bytes out (and, its own code's, bits into them: least significant first) ---- */
typedef struct Out {
    uint8_t *p;
    size_t n, cap;
    uint32_t bits;
    int nbits;
    int failed;   /* out of memory */
} Out;

static void out_byte(Out *o, uint8_t b) {
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 1024;
        uint8_t *p = realloc(o->p, cap);

        if (p == NULL) {
            o->failed = 1;
            return;
        }
        o->p = p;
        o->cap = cap;
    }
    o->p[o->n++] = b;
}
static void out_le32(Out *o, uint32_t x) {
    int i;

    for (i = 0; i < 4; i++) {
        out_byte(o, (uint8_t)(x >> (8 * i)));
    }
}

/* ---- what is inflated: its bytes, how far they are read, what they made ---- */
typedef struct In {
    const uint8_t *p;
    size_t n, at;
    uint32_t bits;
    int nbits;
    Out out;
    size_t max;   /* the bytes it may make, at most */
    int bad;
    int full;     /* ... bad because it would make more */
} In;

#ifdef CLANKFORTH_ZLIB
/* ---- the work done by zlib: a stream of its own for each call (zlib keeps nothing between calls) ---- */
#define ZLIB_WINDOW_BITS_MIN 9    /* a window of 512 bytes: zlib's least */
#define ZLIB_WINDOW_BITS_MAX 15   /* ... and DEFLATE's most, 32 KB: what inflating must allow for */
#define ZLIB_HASH_UNDER_WINDOW 7  /* zlib's memLevel is its hash table's bits less 7: a table as big as the window */
#define ZLIB_CHUNK ((size_t)1 << 30)   /* bytes given to zlib at a call, at most (its counts are 32 bits) */
#define ZLIB_AT_BLOCK_END 128     /* data_type's bit: inflate stopped where a block ended */
#define INFLATED_FIRST 1024       /* room for what is inflated, to begin with - or ... */
#define INFLATED_GUESS 4          /* ... this many times its input */
#define ZLIB_LEVEL 3               /* how hard zlib looks for matches (1-9; its own default 6). A web server's: every
                                   * page is deflated as it is asked for - a 2.5 KB page took 291,000 instructions
                                   * at 6 and 235,000 at 3 (2% longer), a 62 KB JSON text 2.6 million and 0.93 (10%) */
#define SYNC_TAIL 4               /* a sync flush's last bytes, 00 00 FF FF: left off */
#define IN_PLACE_BYTES 8192       /* what is deflated, or made inflating, where the word stands: under a handler's
                                   * slice of time (a tenth of a millisecond) - more, on a worker */
#define ZLIB_VERSION_TEXT_MAX 24   /* of zlib's version, what compressed-by says ("1.3.1.1-motley") */

/* room for so many more bytes after o's: 0 if out of memory */
static int out_room(Out *o, size_t more) {
    uint8_t *p;

    if (o->cap - o->n >= more) {
        return 1;
    }
    p = realloc(o->p, o->n + more);
    if (p == NULL) {
        return 0;
    }
    o->p = p;
    o->cap = o->n + more;
    return 1;
}
/* the window for n bytes: no bigger than they are (what zlib allocates and clears at each call is by its window -
 * 300 bytes deflated with the 32 KB one took 37 us, with this 7) */
static int window_bits(size_t n) {
    int bits = ZLIB_WINDOW_BITS_MIN;

    while (bits < ZLIB_WINDOW_BITS_MAX && ((size_t)1 << bits) < n) {
        bits++;
    }
    return bits;
}
/* in as DEFLATE onto o: its last block final - or (sync) ended as a sync flush is, its 00 00 FF FF left off
 * (permessage-deflate's form); 0 if out of memory. (n is INPUT_MAX at most: the words see to it.) */
static int deflate_to(Out *o, const uint8_t *in, size_t n, int sync) {
    z_stream z;
    int bits = window_bits(n), done = 0;

    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, ZLIB_LEVEL, Z_DEFLATED, -bits, bits - ZLIB_HASH_UNDER_WINDOW, Z_DEFAULT_STRATEGY) != Z_OK) {
        return 0;
    }
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)n;
    while (!done) {   /* (once, but for a sync flush that deflateBound's room was short for) */
        size_t room = (size_t)deflateBound(&z, (uLong)z.avail_in) + SYNC_TAIL + 1;
        int r;

        if (!out_room(o, room)) {
            deflateEnd(&z);
            return 0;
        }
        z.next_out = o->p + o->n;
        z.avail_out = (uInt)room;
        r = deflate(&z, sync ? Z_SYNC_FLUSH : Z_FINISH);
        o->n += room - z.avail_out;
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) {
            deflateEnd(&z);
            return 0;
        }
        done = sync ? z.avail_out > 0 : r == Z_STREAM_END;   /* (a flush is whole when it left room over) */
    }
    deflateEnd(&z);
    if (sync) {
        o->n -= SYNC_TAIL;
    }
    return 1;
}
/* DEFLATE inflated (at most max bytes made): 1 if it was whole - its final block, or (open) its input ending at a
 * block's end (permessage-deflate's, its 00 00 FF FF put back); s->at where it ended */
static int inflate_from(In *s, int open) {
    z_stream z;
    size_t limit = s->max < SIZE_MAX ? s->max + 1 : s->max;   /* (a byte past max: more is seen, and not made) */
    int whole = 0, at_block_end = 1;   /* (before the first block: where one ends) */

    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -ZLIB_WINDOW_BITS_MAX) != Z_OK) {
        s->bad = 1;
        return 0;
    }
    for (;;) {
        size_t fed = s->n - s->at, room;
        int r;

        if (s->out.n == s->out.cap) {
            size_t cap = s->out.cap ? s->out.cap * 2 : s->n < limit / INFLATED_GUESS ? s->n * INFLATED_GUESS : limit;

            if (cap < INFLATED_FIRST) {
                cap = INFLATED_FIRST;
            }
            if (cap > limit) {
                cap = limit;
            }
            if (cap == s->out.n) {   /* (more than max) */
                s->full = 1;
                break;
            }
            if (!out_room(&s->out, cap - s->out.n)) {   /* (no memory: said - it isn't "not DEFLATE") */
                s->out.failed = 1;
                break;
            }
        }
        room = s->out.cap - s->out.n;
        if (fed > ZLIB_CHUNK) {
            fed = ZLIB_CHUNK;
        }
        if (room > ZLIB_CHUNK) {
            room = ZLIB_CHUNK;
        }
        z.next_in = (Bytef *)(uintptr_t)(s->p + s->at);
        z.avail_in = (uInt)fed;
        z.next_out = s->out.p + s->out.n;
        z.avail_out = (uInt)room;
        r = inflate(&z, Z_NO_FLUSH);
        if (fed != z.avail_in || room != z.avail_out) {   /* (a call with nothing to do says nothing of where the
                                                           * one before stopped: zlib tells it once) */
            at_block_end = (z.data_type & ZLIB_AT_BLOCK_END) != 0;
        }
        s->at += fed - z.avail_in;
        s->out.n += room - z.avail_out;
        if (r == Z_STREAM_END) {
            whole = 1;
            break;
        }
        if (r != Z_OK && r != Z_BUF_ERROR) {   /* (not DEFLATE) */
            break;
        }
        if (s->at == s->n && z.avail_out > 0) {   /* (the input over, and all it makes made) */
            whole = open && at_block_end;
            break;
        }
    }
    inflateEnd(&z);
    if (s->out.n > s->max) {
        whole = 0;
        s->full = 1;
    }
    if (!whole) {
        s->bad = 1;
    }
    return whole;
}
/* a CRC-32 carried on over more bytes (crc: the bytes before's; 0 to begin) */
static uint32_t crc32_more(uint32_t crc, const uint8_t *p, size_t n) { return (uint32_t)crc32_z(crc, p, n); }
#else
#define IN_PLACE_BYTES 4096       /* what is deflated, or made inflating, where the word stands: under a handler's
                                   * slice of time (a tenth of a millisecond) - more, on a worker */
static const uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t codelen_order[CODELEN_CODES] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static void put_bits(Out *o, uint32_t value, int count) {
    o->bits |= value << o->nbits;
    o->nbits += count;
    while (o->nbits >= 8) {
        out_byte(o, (uint8_t)o->bits);
        o->bits >>= 8;
        o->nbits -= 8;
    }
}
static void flush_bits(Out *o) {   /* to the next byte */
    if (o->nbits > 0) {
        out_byte(o, (uint8_t)o->bits);
    }
    o->bits = 0;
    o->nbits = 0;
}

/* ---- Huffman codes for a block's symbols ---- */
/* the code's lengths for these counts: at most max_bits each; a symbol counted none gets none (but a used code
 * has two symbols at least - one alone gets a partner - so every decoder takes it) */
static void code_lengths(const uint32_t *freq, int n, int max_bits, uint8_t *len) {
    int sym[LITLEN_CODES + 2], parent[2 * (LITLEN_CODES + 2)], depth[2 * (LITLEN_CODES + 2)];
    uint32_t weight[2 * (LITLEN_CODES + 2)];
    int alive[2 * (LITLEN_CODES + 2)];
    int bl_count[MAX_BITS + 2] = {0};
    int count = 0, nodes, i, j, k, kept;

    memset(len, 0, (size_t)n);
    for (i = 0; i < n; i++) {
        if (freq[i] > 0) {
            sym[count++] = i;
        }
    }
    if (count == 0) {
        return;
    }
    if (count == 1) {   /* (one symbol: two of length 1, a whole code) */
        len[sym[0]] = 1;
        len[sym[0] == 0 ? 1 : 0] = 1;
        return;
    }
    for (i = 0; i < count; i++) {   /* (a tree of the counts, the two lightest joined each time: n is small) */
        weight[i] = freq[sym[i]];
        alive[i] = 1;
        parent[i] = -1;
    }
    nodes = count;
    for (k = 0; k < count - 1; k++) {
        int a = -1, b = -1;

        for (i = 0; i < nodes; i++) {
            if (!alive[i]) {
                continue;
            }
            if (a < 0 || weight[i] < weight[a]) {
                b = a;
                a = i;
            } else if (b < 0 || weight[i] < weight[b]) {
                b = i;
            }
        }
        alive[a] = alive[b] = 0;
        weight[nodes] = weight[a] + weight[b];
        alive[nodes] = 1;
        parent[nodes] = -1;
        parent[a] = parent[b] = nodes;
        nodes++;
    }
    for (i = nodes - 1; i >= 0; i--) {
        depth[i] = parent[i] < 0 ? 0 : depth[parent[i]] + 1;
    }
    for (i = 0; i < count; i++) {
        bl_count[depth[i] > max_bits ? max_bits : depth[i]]++;
    }
    {   /* (too long: the lengths' counts made right again - miniz's way - their sum of 2^-length back to 1) */
        uint32_t total = 0;

        for (i = max_bits; i > 0; i--) {
            total += (uint32_t)bl_count[i] << (max_bits - i);
        }
        while (total > (1u << max_bits)) {
            bl_count[max_bits]--;
            for (i = max_bits - 1; i > 0; i--) {
                if (bl_count[i] > 0) {
                    bl_count[i]--;
                    bl_count[i + 1] += 2;
                    break;
                }
            }
            total--;
        }
    }
    for (i = 1; i < count; i++) {   /* the symbols by count, most first (a stable insertion sort) */
        int s = sym[i];

        for (j = i; j > 0 && freq[sym[j - 1]] < freq[s]; j--) {
            sym[j] = sym[j - 1];
        }
        sym[j] = s;
    }
    kept = 0;
    for (i = 1; i <= max_bits; i++) {   /* the shortest lengths to the most common */
        for (j = 0; j < bl_count[i]; j++) {
            len[sym[kept++]] = (uint8_t)i;
        }
    }
}
/* each symbol's code, its bits reversed (written least significant first) */
static void codes_of(const uint8_t *len, int n, uint16_t *code) {
    int bl_count[MAX_BITS + 1] = {0}, next[MAX_BITS + 2], i, bits, c = 0;

    for (i = 0; i < n; i++) {
        bl_count[len[i]]++;
    }
    bl_count[0] = 0;
    for (bits = 1; bits <= MAX_BITS; bits++) {
        c = (c + bl_count[bits - 1]) << 1;
        next[bits] = c;
    }
    for (i = 0; i < n; i++) {
        if (len[i] != 0) {
            int v = next[len[i]]++, r = 0, b;

            for (b = 0; b < len[i]; b++) {
                r = (r << 1) | ((v >> b) & 1);
            }
            code[i] = (uint16_t)r;
        }
    }
}

/* ---- a block: its literals and matches, written with codes of their own ---- */
typedef struct Block {
    uint16_t *lit;    /* a literal's byte, or a match's length */
    uint16_t *dist;   /* 0: a literal; else a match's distance */
    int n, cap;       /* (cap: as many as the input could make, BLOCK_SYMBOLS at most) */
} Block;

static int length_code(int len) {   /* 0-28: its code is 257 + this */
    int c = 0;

    while (c < 28 && length_base[c + 1] <= len) {
        c++;
    }
    return c;
}
static int dist_code(int d) {
    int c = 0;

    while (c < 29 && dist_base[c + 1] <= d) {
        c++;
    }
    return c;
}
static void write_block(Out *o, const Block *b, int final) {
    uint32_t lfreq[LITLEN_CODES] = {0}, dfreq[DIST_CODES] = {0}, cfreq[CODELEN_CODES] = {0};
    uint8_t llen[LITLEN_CODES], dlen[DIST_CODES], clen[CODELEN_CODES], all[LITLEN_CODES + DIST_CODES];
    uint16_t lcode[LITLEN_CODES], dcode[DIST_CODES], ccode[CODELEN_CODES];
    uint8_t rle[LITLEN_CODES + DIST_CODES], rle_extra[LITLEN_CODES + DIST_CODES];
    int i, hlit, hdist, hclen, nall, nrle = 0;

    for (i = 0; i < b->n; i++) {
        if (b->dist[i] == 0) {
            lfreq[b->lit[i]]++;
        } else {
            lfreq[257 + length_code(b->lit[i])]++;
            dfreq[dist_code(b->dist[i])]++;
        }
    }
    lfreq[END_OF_BLOCK]++;
    code_lengths(lfreq, LITLEN_CODES, MAX_BITS, llen);
    code_lengths(dfreq, DIST_CODES, MAX_BITS, dlen);
    for (i = 0; i < DIST_CODES && dlen[i] == 0; i++) {
    }
    if (i == DIST_CODES) {   /* (no matches at all: still a distance code, of two) */
        dlen[0] = dlen[1] = 1;
    }
    for (hlit = LITLEN_CODES; hlit > 257 && llen[hlit - 1] == 0; hlit--) {
    }
    for (hdist = DIST_CODES; hdist > 1 && dlen[hdist - 1] == 0; hdist--) {
    }
    memcpy(all, llen, (size_t)hlit);
    memcpy(all + hlit, dlen, (size_t)hdist);
    nall = hlit + hdist;
    for (i = 0; i < nall;) {   /* the lengths, runs of them shortened: 16 the last again, 17 and 18 zeros */
        int run = 1;

        while (i + run < nall && all[i + run] == all[i]) {
            run++;
        }
        if (all[i] == 0 && run >= 3) {
            int take = run > 138 ? 138 : run;

            rle[nrle] = take >= 11 ? 18 : 17;
            rle_extra[nrle++] = (uint8_t)(take >= 11 ? take - 11 : take - 3);
            i += take;
        } else if (all[i] != 0 && run >= 4) {
            int take = run - 1 > 6 ? 6 : run - 1;

            rle[nrle] = all[i];
            rle_extra[nrle++] = 0;
            rle[nrle] = 16;
            rle_extra[nrle++] = (uint8_t)(take - 3);
            i += 1 + take;
        } else {
            rle[nrle] = all[i];
            rle_extra[nrle++] = 0;
            i++;
        }
    }
    for (i = 0; i < nrle; i++) {
        cfreq[rle[i]]++;
    }
    code_lengths(cfreq, CODELEN_CODES, CODELEN_MAX_BITS, clen);
    for (hclen = CODELEN_CODES; hclen > 4 && clen[codelen_order[hclen - 1]] == 0; hclen--) {
    }
    codes_of(llen, LITLEN_CODES, lcode);
    codes_of(dlen, DIST_CODES, dcode);
    codes_of(clen, CODELEN_CODES, ccode);

    put_bits(o, (uint32_t)final, 1);
    put_bits(o, 2, 2);   /* (dynamic codes) */
    put_bits(o, (uint32_t)(hlit - 257), 5);
    put_bits(o, (uint32_t)(hdist - 1), 5);
    put_bits(o, (uint32_t)(hclen - 4), 4);
    for (i = 0; i < hclen; i++) {
        put_bits(o, clen[codelen_order[i]], 3);
    }
    for (i = 0; i < nrle; i++) {
        put_bits(o, ccode[rle[i]], clen[rle[i]]);
        if (rle[i] == 16) {
            put_bits(o, rle_extra[i], 2);
        } else if (rle[i] == 17) {
            put_bits(o, rle_extra[i], 3);
        } else if (rle[i] == 18) {
            put_bits(o, rle_extra[i], 7);
        }
    }
    for (i = 0; i < b->n; i++) {
        if (b->dist[i] == 0) {
            put_bits(o, lcode[b->lit[i]], llen[b->lit[i]]);
        } else {
            int lc = length_code(b->lit[i]), dc = dist_code(b->dist[i]);

            put_bits(o, lcode[257 + lc], llen[257 + lc]);
            put_bits(o, (uint32_t)(b->lit[i] - length_base[lc]), length_extra[lc]);
            put_bits(o, dcode[dc], dlen[dc]);
            put_bits(o, (uint32_t)(b->dist[i] - dist_base[dc]), dist_extra[dc]);
        }
    }
    put_bits(o, lcode[END_OF_BLOCK], llen[END_OF_BLOCK]);
}

/* a block in the fixed codes (no table of its own: what a short message costs least in) */
static void write_fixed_block(Out *o, const Block *b, int final) {
    uint8_t llen[FIXED_LITLEN], dlen[FIXED_DIST];
    uint16_t lcode[FIXED_LITLEN], dcode[FIXED_DIST];
    int i;

    for (i = 0; i < FIXED_LITLEN; i++) {
        llen[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    }
    for (i = 0; i < FIXED_DIST; i++) {
        dlen[i] = 5;
    }
    codes_of(llen, FIXED_LITLEN, lcode);
    codes_of(dlen, FIXED_DIST, dcode);
    put_bits(o, (uint32_t)final, 1);
    put_bits(o, 1, 2);   /* (fixed codes) */
    for (i = 0; i < b->n; i++) {
        if (b->dist[i] == 0) {
            put_bits(o, lcode[b->lit[i]], llen[b->lit[i]]);
        } else {
            int lc = length_code(b->lit[i]), dc = dist_code(b->dist[i]);

            put_bits(o, lcode[257 + lc], llen[257 + lc]);
            put_bits(o, (uint32_t)(b->lit[i] - length_base[lc]), length_extra[lc]);
            put_bits(o, dcode[dc], dlen[dc]);
            put_bits(o, (uint32_t)(b->dist[i] - dist_base[dc]), dist_extra[dc]);
        }
    }
    put_bits(o, lcode[END_OF_BLOCK], llen[END_OF_BLOCK]);
}
/* the block in whichever codes make it shorter: its own, or the fixed ones (each tried) */
static void write_best_block(Out *o, const Block *b, int final) {
    Out dyn = {0}, fix = {0};
    size_t i;

    if (b->n > SHORT_BLOCK) {   /* (a long block: its own codes, always the shorter) */
        write_block(o, b, final);
        return;
    }
    dyn.bits = fix.bits = 0;
    write_block(&dyn, b, final);
    write_fixed_block(&fix, b, final);
    {
        Out *best = (fix.n * 8 + (size_t)fix.nbits <= dyn.n * 8 + (size_t)dyn.nbits) ? &fix : &dyn;

        if (best->failed) {
            o->failed = 1;
        }
        for (i = 0; i < best->n; i++) {   /* (its bytes and leftover bits on after o's own) */
            put_bits(o, best->p[i], 8);
        }
        if (best->nbits > 0) {
            put_bits(o, best->bits, best->nbits);
        }
    }
    free(dyn.p);
    free(fix.p);
}

static uint32_t hash3(const uint8_t *p, int bits) {
    return ((((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) * 2654435761u) >> (32 - bits);
}
/* in as DEFLATE onto o: its last block final - or (sync) none final, ended by an empty stored block's first bits and
 * the bytes to the next one, its 00 00 FF FF left off (permessage-deflate's form); 0 if out of memory */
static int deflate_to(Out *o, const uint8_t *in, size_t n, int sync) {
    int bits = HASH_BITS_MIN;
    size_t i = 0, hash_size;
    int32_t *head, *prev;
    Block block, *b = &block;

    while (bits < HASH_BITS_MAX && ((size_t)1 << bits) < n) {   /* (the tables as big as the input: no bigger) */
        bits++;
    }
    hash_size = (size_t)1 << bits;
    b->cap = n < BLOCK_SYMBOLS ? (int)n + 1 : BLOCK_SYMBOLS;
    head = malloc(hash_size * sizeof(int32_t));
    prev = malloc((n > 0 ? n : 1) * sizeof(int32_t));
    b->lit = malloc((size_t)b->cap * sizeof(uint16_t));
    b->dist = malloc((size_t)b->cap * sizeof(uint16_t));
    if (head == NULL || prev == NULL || b->lit == NULL || b->dist == NULL) {
        free(head);
        free(prev);
        free(b->lit);
        free(b->dist);
        return 0;
    }
    memset(head, 0xff, hash_size * sizeof(int32_t));   /* (-1: none seen) */
    b->n = 0;
    while (i < n) {
        int best = 0, best_dist = 0;

        if (i + MIN_MATCH <= n) {
            uint32_t h = hash3(in + i, bits);
            int32_t cand = head[h];
            int chain = CHAIN_MAX;
            size_t most = n - i < MAX_MATCH ? n - i : MAX_MATCH;

            while (cand >= 0 && i - (size_t)cand <= WINDOW && chain-- > 0) {
                const uint8_t *a = in + cand, *c = in + i;
                int len = 0;

                if (a[best] == c[best]) {   /* (a longer match must have this byte too) */
                    while ((size_t)len < most && a[len] == c[len]) {
                        len++;
                    }
                    if (len > best) {
                        best = len;
                        best_dist = (int)(i - (size_t)cand);
                        if (len >= NICE_MATCH || (size_t)len == most) {
                            break;
                        }
                    }
                }
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = (int32_t)i;
        }
        if (best >= MIN_MATCH) {
            size_t k;

            b->lit[b->n] = (uint16_t)best;
            b->dist[b->n++] = (uint16_t)best_dist;
            for (k = 1; k < (size_t)best; k++) {   /* (the places inside the match seen too) */
                if (i + k + MIN_MATCH <= n) {
                    uint32_t h = hash3(in + i + k, bits);

                    prev[i + k] = head[h];
                    head[h] = (int32_t)(i + k);
                }
            }
            i += (size_t)best;
        } else {
            b->lit[b->n] = in[i++];
            b->dist[b->n++] = 0;
        }
        if (b->n == b->cap && i < n) {
            write_block(o, b, 0);
            b->n = 0;
        }
    }
    if (n == 0 && !sync) {   /* (nothing: a fixed-code block holding only its end) */
        put_bits(o, 1, 1);
        put_bits(o, 1, 2);
        put_bits(o, 0, 7);
    } else if (n > 0) {
        write_best_block(o, b, !sync);
    }
    if (sync) {   /* (an empty stored block, not final: three bits, then to the byte - its LEN and NLEN left off) */
        put_bits(o, 0, 3);
    }
    flush_bits(o);
    free(head);
    free(prev);
    free(b->lit);
    free(b->dist);
    return !o->failed;
}

/* ---- inflating (puff.c's way) ---- */
typedef struct Huff {
    short count[MAX_BITS + 1];
    short symbol[FIXED_LITLEN];
} Huff;

static int get_bits(In *s, int need) {
    uint32_t v = s->bits;

    while (s->nbits < need) {
        if (s->at == s->n) {
            s->bad = 1;
            return 0;
        }
        v |= (uint32_t)s->p[s->at++] << s->nbits;
        s->nbits += 8;
    }
    s->bits = v >> need;
    s->nbits -= need;
    return (int)(v & ((1u << need) - 1));
}
static int decode(In *s, const Huff *h) {
    int code = 0, first = 0, index = 0, len;

    for (len = 1; len <= MAX_BITS; len++) {
        int count;

        code |= get_bits(s, 1);
        if (s->bad) {
            return -1;
        }
        count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->bad = 1;
    return -1;
}
static int construct(Huff *h, const uint8_t *len, int n) {   /* -1: lengths that make no code */
    short offs[MAX_BITS + 1];
    int sym, l, left = 1;

    memset(h->count, 0, sizeof(h->count));
    for (sym = 0; sym < n; sym++) {
        h->count[len[sym]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    for (l = 1; l <= MAX_BITS; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) {
            return -1;
        }
    }
    offs[1] = 0;
    for (l = 1; l < MAX_BITS; l++) {
        offs[l + 1] = (short)(offs[l] + h->count[l]);
    }
    for (sym = 0; sym < n; sym++) {
        if (len[sym] != 0) {
            h->symbol[offs[len[sym]]++] = (short)sym;
        }
    }
    return left;
}
static void out_made(In *s, uint8_t b) {
    if (s->out.n >= s->max) {
        s->bad = 1;
        s->full = 1;
        return;
    }
    out_byte(&s->out, b);
    if (s->out.failed) {
        s->bad = 1;
    }
}
static void codes(In *s, const Huff *lh, const Huff *dh) {   /* a block's literals and matches, to its end */
    for (;;) {
        int sym = decode(s, lh);

        if (s->bad) {
            return;
        }
        if (sym < END_OF_BLOCK) {
            out_made(s, (uint8_t)sym);
        } else if (sym == END_OF_BLOCK) {
            return;
        } else {
            int len, d, dist;
            size_t k;

            sym -= 257;
            if (sym >= 29) {
                s->bad = 1;
                return;
            }
            len = length_base[sym] + get_bits(s, length_extra[sym]);
            d = decode(s, dh);
            if (s->bad || d < 0 || d >= 30) {
                s->bad = 1;
                return;
            }
            dist = dist_base[d] + get_bits(s, dist_extra[d]);
            if (s->bad || (size_t)dist > s->out.n) {
                s->bad = 1;
                return;
            }
            for (k = 0; k < (size_t)len && !s->bad; k++) {
                out_made(s, s->out.p[s->out.n - (size_t)dist]);
            }
        }
        if (s->bad) {
            return;
        }
    }
}
static void stored(In *s) {
    unsigned len, nlen;

    s->bits = 0;
    s->nbits = 0;
    if (s->at + 4 > s->n) {
        s->bad = 1;
        return;
    }
    len = s->p[s->at] | (unsigned)s->p[s->at + 1] << 8;
    nlen = s->p[s->at + 2] | (unsigned)s->p[s->at + 3] << 8;
    s->at += 4;
    if (len != (~nlen & 0xffff) || s->at + len > s->n) {
        s->bad = 1;
        return;
    }
    while (len-- > 0 && !s->bad) {
        out_made(s, s->p[s->at++]);
    }
}
static void fixed(In *s) {
    uint8_t len[FIXED_LITLEN];
    Huff lh, dh;
    int i;

    for (i = 0; i < 144; i++) {
        len[i] = 8;
    }
    for (; i < 256; i++) {
        len[i] = 9;
    }
    for (; i < 280; i++) {
        len[i] = 7;
    }
    for (; i < FIXED_LITLEN; i++) {
        len[i] = 8;
    }
    construct(&lh, len, FIXED_LITLEN);
    for (i = 0; i < FIXED_DIST; i++) {
        len[i] = 5;
    }
    construct(&dh, len, FIXED_DIST);
    codes(s, &lh, &dh);
}
static void dynamic(In *s) {
    uint8_t len[LITLEN_CODES + DIST_CODES];
    Huff lh, dh, ch;
    int nlen, ndist, ncode, i = 0, err;

    nlen = get_bits(s, 5) + 257;
    ndist = get_bits(s, 5) + 1;
    ncode = get_bits(s, 4) + 4;
    if (s->bad || nlen > LITLEN_CODES || ndist > DIST_CODES) {
        s->bad = 1;
        return;
    }
    memset(len, 0, sizeof(len));
    for (i = 0; i < ncode; i++) {
        len[codelen_order[i]] = (uint8_t)get_bits(s, 3);
    }
    if (s->bad || construct(&ch, len, CODELEN_CODES) != 0) {
        s->bad = 1;
        return;
    }
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &ch), rep, l = 0;

        if (s->bad || sym < 0) {
            s->bad = 1;
            return;
        }
        if (sym < 16) {
            len[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (i == 0) {
                s->bad = 1;
                return;
            }
            l = len[i - 1];
            rep = 3 + get_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + get_bits(s, 3);
        } else {
            rep = 11 + get_bits(s, 7);
        }
        if (s->bad || i + rep > nlen + ndist) {
            s->bad = 1;
            return;
        }
        while (rep-- > 0) {
            len[i++] = (uint8_t)l;
        }
    }
    if (len[END_OF_BLOCK] == 0) {
        s->bad = 1;
        return;
    }
    err = construct(&lh, len, nlen);
    if (err < 0 || (err > 0 && nlen - lh.count[0] != 1)) {
        s->bad = 1;
        return;
    }
    err = construct(&dh, len + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dh.count[0] != 1)) {
        s->bad = 1;
        return;
    }
    codes(s, &lh, &dh);
}
/* DEFLATE inflated (at most max bytes made): 1 if it was whole - its final block, or (open) its input ending at a
 * block's end (permessage-deflate's, its 00 00 FF FF put back); s->at where it ended */
static int inflate_from(In *s, int open) {
    int last = 0;

    while (!last) {
        int type;

        if (open && s->at == s->n && s->nbits < 3) {   /* (at a block's end, the input over: that was all) */
            return 1;
        }
        last = get_bits(s, 1);
        type = get_bits(s, 2);
        if (s->bad) {
            return 0;
        }
        if (type == 0) {
            stored(s);
        } else if (type == 1) {
            fixed(s);
        } else if (type == 2) {
            dynamic(s);
        } else {
            return 0;
        }
        if (s->bad) {
            return 0;
        }
    }
    return 1;
}

/* a CRC-32 carried on over more bytes (crc: the bytes before's; 0 to begin), as zlib's crc32 */
static uint32_t crc_table[256];
static void crc_table_make(void) {
    uint32_t k;

    for (k = 0; k < 256; k++) {
        uint32_t v = k;
        int b;

        for (b = 0; b < 8; b++) {
            v = (v & 1) ? CRC32_POLY ^ (v >> 1) : v >> 1;
        }
        crc_table[k] = v;
    }
}
/* the table, made once - by one thread, the others waiting: nodes and the workers ask at once, and one that saw it
 * "made" before its numbers were written (the flag and the table were plain statics) would sum wrongly */
static const uint32_t *crc_table_made(void) {
#ifdef CLANKFORTH_NODES
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    pthread_once(&once, crc_table_make);
#else
    static int made;

    if (!made) {
        crc_table_make();
        made = 1;
    }
#endif
    return crc_table;
}
static uint32_t crc32_more(uint32_t crc, const uint8_t *p, size_t n) {
    const uint32_t *table = crc_table_made();
    uint32_t c = crc ^ 0xffffffffu;
    size_t i;

    for (i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}
#endif
static uint32_t crc32_of(const uint8_t *p, size_t n) { return crc32_more(0, p, n); }

/* ---- what a word makes of its bytes: nothing of a Forth's in it - done on its node, or on a worker ---- */
enum { OP_DEFLATE, OP_DEFLATE_SYNC, OP_GZIP, OP_INFLATE, OP_GUNZIP };

/* the bytes deflated onto o (raw; ended as a sync flush is; or in gzip's form): 0 if out of memory */
static int deflated(int op, const uint8_t *p, size_t n, Out *o) {
    static const uint8_t header[GZIP_HEADER] = {GZIP_ID1, GZIP_ID2, GZIP_DEFLATE, 0, 0, 0, 0, 0, 0, GZIP_OS_UNKNOWN};
    int i;

    if (op == OP_GZIP) {
        for (i = 0; i < GZIP_HEADER; i++) {
            out_byte(o, header[i]);
        }
    }
    if (!deflate_to(o, p, n, op == OP_DEFLATE_SYNC)) {
        o->failed = 1;
    }
    if (op == OP_GZIP) {
        out_le32(o, crc32_of(p, n));
        out_le32(o, (uint32_t)n);
    }
    return !o->failed;
}
/* a gzip member's header at p[*at]: *at past it (the member's blocks begin there); 0 if there is none whole */
static int gzip_header(const uint8_t *p, size_t n, size_t *at) {
    size_t i = *at;
    int flags;

    if (n - i < GZIP_HEADER + GZIP_TRAILER || p[i] != GZIP_ID1 || p[i + 1] != GZIP_ID2 || p[i + 2] != GZIP_DEFLATE) {
        return 0;
    }
    flags = p[i + 3];
    i += GZIP_HEADER;
    if (flags & GZ_FEXTRA) {
        if (n - i < 2) {
            return 0;
        }
        i += 2 + (size_t)(p[i] | p[i + 1] << 8);
    }
    if (flags & GZ_FNAME) {
        while (i < n && p[i++] != 0) {
        }
    }
    if (flags & GZ_FCOMMENT) {
        while (i < n && p[i++] != 0) {
        }
    }
    if (flags & GZ_FHCRC) {
        i += 2;
    }
    if (i > n || n - i < GZIP_TRAILER) {
        return 0;
    }
    *at = i;
    return 1;
}
/* the bytes inflated onto o, at most max made (raw, its input perhaps ending at a block's end; or gzip's form: its
 * members one after another - a file of several is their bytes together - each one's CRC and length checked): 1; 0
 * for what isn't that - bytes after gzip's last member too - or for more than max, *full then saying it was the
 * limit; -1 out of memory */
static int inflated(int op, const uint8_t *p, size_t n, size_t max, Out *o, int *full) {
    size_t at = 0;
    In s;
    int whole;

    *full = 0;
    memset(&s, 0, sizeof(s));
    do {
        size_t made = s.out.n;
        Out out = s.out;

        if (op == OP_GUNZIP && !gzip_header(p, n, &at)) {
            *o = out;
            return 0;
        }
        memset(&s, 0, sizeof(s));
        s.out = out;   /* (a member's bytes after the one before's) */
        s.p = p + at;
        s.n = op == OP_GUNZIP ? n - at - GZIP_TRAILER : n;
        s.max = max;
        whole = inflate_from(&s, op == OP_INFLATE) && !s.bad;
        *o = s.out;
        *full = s.full;
        if (s.out.failed) {
            return -1;
        }
        if (whole && op == OP_GUNZIP) {
            const uint8_t *t = p + at + s.at;   /* (the trailer, after what the blocks took) */
            uint32_t crc = t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
            uint32_t size = t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;

            whole = crc == crc32_of(o->p + made, o->n - made) && size == (uint32_t)(o->n - made);
            at += s.at + GZIP_TRAILER;
        } else {
            at = n;   /* (raw: one stream. After a final block, more is passed over - permessage-deflate's sender may
                       *  end a message so, an empty block after it: RFC 7692, 7.2.3.4) */
        }
    } while (whole && at < n);
    return whole;
}
static int inflates(int op) { return op == OP_INFLATE || op == OP_GUNZIP; }
/* the work as a job, on a worker: arg which, arg2 its limit, in the bytes; out what it made, result whether it did
 * (inflating: 0 for what isn't DEFLATE, or too much) */
static void run_compress(ForthJob *j) {
    Out o = {0};
    int op = (int)j->arg, full;

    if (inflates(op)) {
        j->result = inflated(op, (const uint8_t *)j->in, j->in_n, (size_t)j->arg2, &o, &full);
    } else {
        j->result = deflated(op, (const uint8_t *)j->in, j->in_n, &o) ? 1 : -1;
    }
    if (j->result < 0) {   /* (no memory: said - of what is inflated too: it isn't "not DEFLATE") */
        j->failed = 1;
        snprintf(j->why, sizeof(j->why), "%s: out of memory", j->word);
    }
    if (j->failed || !j->result) {
        free(o.p);
        return;
    }
    j->out = (char *)o.p;
    j->out_n = o.n;
}

/* ---- the words ---- */
static const uint8_t *bytes_arg(Forth *f, size_t *n, const char *word) {   /* ( a n -- ) */
    Cell len = forth_pop(f), a = forth_pop(f);

    return (const uint8_t *)forth_string_arg(f, a, len, word, n);
}
static void push_made(Forth *f, const uint8_t *p, size_t n) {   /* ( -- s len ) a new string of the bytes */
    forth_push(f, (Cell)forth_new_string(f, p != NULL ? (const char *)p : "", n));
    forth_push(f, (Cell)n);
}
/* a word's work: where it stands if that takes no longer than a handler's slice does - or where the task can't wait
 * (the prompt, a host with no slices) - and else on a worker, the handler waiting while its node's other actors go
 * on. Deflating ( a n -- s len ); inflating ( a n max -- s len true | false ). */
static void compress_word(Forth *f, const Word *w, int op, const char *word) {
    ForthJob *j = forth_job_taken(f, word);
    int ok;

    if (j == NULL) {
        Cell max = inflates(op) ? forth_pop(f) : 0;
        size_t n, limit = max > 0 ? (size_t)max : 0;
        const uint8_t *p = bytes_arg(f, &n, word);
        Out o = {0};
        int full = 0, here;

        if (!inflates(op) && n > INPUT_MAX) {
            forth_error(f, "%s: more than %lu bytes", word, (unsigned long)INPUT_MAX);
        }
        /* (what is made inflating isn't known till it is made: up to a slice's worth is made here - most are no
         *  more - and only one that would be longer begun again on a worker) */
        here = inflates(op) ? n < IN_PLACE_BYTES : n <= IN_PLACE_BYTES;
        if (here || !forth_job_may(f, w)) {
            size_t most = here && limit > IN_PLACE_BYTES && forth_job_may(f, w) ? IN_PLACE_BYTES : limit;
            long long since = 0;

            (void)forth_work_steps(&since);
            ok = inflates(op) ? inflated(op, p, n, most, &o, &full) : deflated(op, p, n, &o) ? 1 : -1;
            forth_slice_charge(f, forth_work_steps(&since));   /* (what it took, counted against the handler's slice) */
            if (ok != 0 || !full || most == limit) {
                if (ok <= 0) {
                    free(o.p);
                    if (ok < 0) {
                        forth_error(f, "%s: out of memory", word);
                    }
                    forth_push(f, 0);
                    return;
                }
                push_made(f, o.p, o.n);
                free(o.p);
                if (inflates(op)) {
                    forth_push(f, -1);
                }
                return;
            }
            free(o.p);   /* (more than a slice's worth: all of it on a worker) */
        }
        j = forth_job_new(f, run_compress, word);
        j->in = forth_job_copy(f, j, p, n);
        j->in_n = n;
        j->arg = op;
        j->arg2 = (long long)limit;
        if ((j = forth_job_wait(f, j)) == NULL) {
            return;   /* (threaded: this primitive again as the task goes on, its bytes made) */
        }
    }
    if (j->failed) {
        char why[sizeof(j->why)];

        memcpy(why, j->why, sizeof(why));
        forth_job_free(j);
        forth_error(f, "%s", why);
    }
    ok = (int)j->result;
    if (ok) {
        push_made(f, (const uint8_t *)j->out, j->out_n);
    }
    forth_job_free(j);
    if (inflates(op)) {
        forth_push(f, ok ? -1 : 0);
    }
}
/* deflate ( a n -- s len ) the bytes as DEFLATE (raw: no header), a new string */
static void p_deflate(Forth *f, Word *w) { compress_word(f, w, OP_DEFLATE, "deflate"); }
/* deflate-sync ( a n -- s len ) as deflate, ended as a sync flush is - its last 00 00 FF FF left off: a WebSocket
 * message's (permessage-deflate) */
static void p_deflate_sync(Forth *f, Word *w) { compress_word(f, w, OP_DEFLATE_SYNC, "deflate-sync"); }
/* inflate ( a n max -- s len true | false ) DEFLATE (raw) inflated - at most max bytes made: false for more, or for
 * what isn't DEFLATE. Its input may end at a block's end, unfinished (a WebSocket message's, its 00 00 FF FF put back) */
static void p_inflate(Forth *f, Word *w) { compress_word(f, w, OP_INFLATE, "inflate"); }
/* gzip ( a n -- s len ) the bytes in gzip's form: a .gz file's, Content-Encoding: gzip's */
static void p_gzip(Forth *f, Word *w) { compress_word(f, w, OP_GZIP, "gzip"); }
/* gunzip ( a n max -- s len true | false ) gzip's form undone (at most max bytes made): false for more, for what isn't
 * gzip, or for a CRC or a length that doesn't match */
static void p_gunzip(Forth *f, Word *w) { compress_word(f, w, OP_GUNZIP, "gunzip"); }

/* crc32-more ( crc a n -- crc' ) CRC-32 (zlib's, PNG's) carried on over the bytes - checksums.crc32's */
static void p_crc32_more(Forth *f, Word *w) {
    size_t n;
    const uint8_t *p = bytes_arg(f, &n, "crc32-more");
    Cell crc = forth_pop(f);

    (void)w;
    forth_push(f, (Cell)crc32_more((uint32_t)crc, p, n));
}

/* compressed-by ( -- s len ) whose work this build's compression is: "zlib" and its version ("zlib 1.3.1"), or
 * "clankforth" - its own code, where the build found no zlib or was told to leave it (CLANKFORTH_ZLIB=OFF). A new
 * string. */
static void p_compressed_by(Forth *f, Word *w) {
#ifdef CLANKFORTH_ZLIB
    char by[sizeof("zlib ") + ZLIB_VERSION_TEXT_MAX];
    int n = snprintf(by, sizeof(by), "zlib %.*s", ZLIB_VERSION_TEXT_MAX, zlibVersion());
#else
    static const char by[] = "clankforth";
    int n = (int)sizeof(by) - 1;
#endif

    (void)w;
    forth_push(f, (Cell)forth_new_string(f, by, (size_t)n));
    forth_push(f, (Cell)n);
}

void forth_compress_words(Forth *f) {
    static const ForthPrim prims[] = {
        {"deflate", p_deflate, "( a n -- s len )"},
        {"deflate-sync", p_deflate_sync, "( a n -- s len )"},
        {"inflate", p_inflate, "( a n max -- s len true | false )"},
        {"gzip", p_gzip, "( a n -- s len )"},
        {"gunzip", p_gunzip, "( a n max -- s len true | false )"},
        {"crc32-more", p_crc32_more, "( crc a n -- crc' )"},
        {"compressed-by", p_compressed_by, "( -- s len )"},
    };
    Vocab *was = forth_current(f), *v = forth_vocab(f, "compression");

    forth_set_current(f, v);   /* scripts say USING: compression ; */
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_set_current(f, was);
}

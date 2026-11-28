/* The objects the scripts make: lists, tuples (and their generic words) and strings, each known by its address and
 * made in a region - the system's, kept till `dispose`, or a scratch one freed as it ends. Its words, and the walk
 * a list's combinators are compiled as. See docs/forth.md, "Writing words". */
#include "forth_internal.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- objects: the lists, tuples and strings the scripts make. Each is known by its address (a table: a number or a
 * stale address used as one is an error, not a crash) and made in a region - the system's, kept till `dispose`, or a
 * scratch one, all of whose objects `with-scratch` frees as it ends (but those `rescue`d).
 *
 * Regions that last: an actor's, the parent of its handlers' scratch regions (what a handler `rescue`s, the actor
 * keeps till it dies), and the post - the objects messages carry on their way. An object sent from a scratch region
 * goes into the post (and with it what it holds), counted for each message carrying it; delivered, it goes into the
 * receiving handler's region when the last of them is (forth_objects_post, _take, _drop) ---- */

enum { OBJ_FREE, OBJ_LIST, OBJ_TUPLE, OBJ_STRING, OBJ_MAP, OBJ_BYTES, OBJ_SLICE, OBJ_GONE };   /* (OBJ_GONE: none -
                                                                                               * how many kinds) */
static const char *const object_kinds[] = {"nothing", "a list", "a tuple", "a string", "a map", "bytes", "a slice",
                                           "nothing"};

/* ---- references ----
 * An object is known by a reference: a cell that is no address and no number a program counts with, naming a slot
 * of the objects' table and the object that is in it now.
 *
 *   bits 63-60  REF_MARK (0100: positive, and past every address - `c@` of one is an error, not a fetch)
 *        59-56  its kind (a string, a list ...: what a reference is, said by the cell itself)
 *        55-32  the slot's generation: one on each time the slot is let go
 *        31-0   the slot's number
 *
 * The slot keeps the upper half (its tag) while the object is alive: a reference is that object's if the slot's tag
 * is its own - two loads and a compare, no table searched. One let go, its slot taken by another: the generation
 * differs, and the old reference names nothing (it named whatever came to be at its address). A number is taken for
 * an object only if it has the mark, a slot's number and that slot's generation of the moment - and the numbers read
 * from text (JSON's, string>number's) are never let have the mark: none a stranger sends is a reference.
 * An object's block has its reference in the cell before it (REF: from a block to what names it). ---- */
#define REF_MARK 4u
#define REF_MARK_SHIFT 28       /* (in the tag) */
#define REF_KIND_SHIFT 24
#define REF_GEN_MAX 0xFFFFFFu   /* a slot let go this many times is used no more (its next would be its first again) */
#define REF(p) (((const Cell *)(p))[-1])   /* an object's reference, by its block */
#define REF_BYTES sizeof(Cell)

typedef struct ObjectSlot {
    void *at;           /* its block (the reference in the cell before it) */
    size_t size;        /* its bytes (a string's: its length) */
    uint32_t tag;       /* the upper half of its reference - mark, kind, generation; 0: the slot is free */
    uint32_t gen;       /* its generation (kept while it is free: the next object's) */
    int kind, region;
    int pending;        /* in the post: messages carrying it, not yet delivered */
    int listed_at;      /* where it is in its region's list of what it made; NOT_LISTED in none (free: the next free
                         * slot of its chunk, -1 none) */
    char owned;         /* a map's string key: the map's to let go (dispose refuses) */
    char permanent;     /* the system's for good (json-true ...): never moved into a region, nor let go */
} ObjectSlot;
#define NOT_LISTED (-1)

/* The slots are in chunks that never move (a slot's address is its own for good: nothing held is lost as the table
 * grows, and it grows by a chunk - no table made anew, nothing carried). A chunk whose slots are all free is given
 * back, but for the first few; its generations are kept by the highest of them, for the chunk made there next. */
#define CHUNK_SHIFT 10
#define CHUNK_SLOTS (1 << CHUNK_SHIFT)
#define CHUNKS_KEPT 64          /* chunks kept though all free (65536 slots: a handler's usual objects make no chunk) */
typedef struct SlotChunk {
    int nfree, free_head;       /* its free slots: how many, the first (-1 none; each names the next) */
    int prev, next;             /* among the chunks with a free slot (-1: the first, the last) */
    ObjectSlot slots[CHUNK_SLOTS];
} SlotChunk;

/* a map's entry: its key a cell, or a string by its bytes (the key then the map's own string object) */
typedef struct MapEntry {
    Cell key, value;
    uint64_t hash;
    int text;           /* the key is a string, matched by its bytes */
    int live;           /* (0: deleted - a hole in the order, left till the entries are packed) */
} MapEntry;

/* A map keeps its entries in the order they came (keys, values, map-each go in that order: the same run, the same
 * order) and finds them by an index: a table of entry numbers by hash, open addressing. */
typedef struct Map {
    MapEntry *e;
    Cell n, cap;        /* entries (holes too), room for them */
    Cell live;
    int32_t *index;     /* MAP_EMPTY, MAP_DELETED, or an entry's number */
    Cell slots;         /* a power of 2 */
} Map;

#define MAP_EMPTY (-1)
#define MAP_DELETED (-2)
#define MAP_FIRST_SLOTS 16
#define MAP_ENTRIES_MAX (INT32_MAX / 2)   /* (an entry's number fits the index's int32) */
#define CELL_BYTES_MAX (INTPTR_MAX / 2)   /* the most bytes a byte buffer holds (its sizes stay sums that can't wrap) */

/* a byte buffer: bytes that grow (a file's, a message's) - b-data its address while it doesn't grow */
typedef struct Bytes {
    uint8_t *p;
    Cell n, cap;
} Bytes;

/* A slice: a piece of a string, its bytes where the string's are - no copy (Factor's virtual sequence). It is read as
 * a string is, by every word that reads one, while what it is a piece of lives: that let go, the slice names bytes
 * that are no more - and says so (its base's reference names nothing then), where an address kept would have been
 * read. Or it is memory C lends for a while - what has come on a connection (net.c): base 0, and the slice itself let
 * go by C when the memory moves or goes, so that every cell that named it names nothing. */
typedef struct Slice {
    Cell base;          /* the string (or lent slice) it is a piece of; 0: memory lent by C */
    const uint8_t *p;   /* its bytes: found again by its base each time it is asked for (seq_slot); lent: C's */
    size_t from;        /* where in its base */
} Slice;

typedef struct Region {
    int parent;         /* the region around it: where `rescue` puts an object */
    int used;
    int lasting;        /* an actor's, or the post: no task's to end (forth_regions_unwind stops there) */
    int next_free;      /* (let go: the next region let go before it; 0 none) */
    Cell *made;         /* the objects made in it, or moved in, in that order: 0 where one was let go or moved out */
    int nmade, cap;
    int holes;          /* the 0s among them */
    int scan, kept;     /* (as they are packed, a few at each object made) those looked at, those kept of them */
    int ended;          /* (as it ends) how many of them have been looked at: an end that gave way goes on from there */
    Cell build;         /* the bytes `build` is gathering in it (`%` adds to the innermost), 0 none */
    Cell build_mark;    /* a cell kept with them, for the words that build (html's: an element's start tag open) */
} Region;

/* An object's block is taken from, and let go to, a few short lists by size: most objects are small and live for one
 * handler, and the allocator's own way round for each cost more than everything else done with them. Bounded (the
 * blocks kept are a few hundred KB at most); a larger block, or one more than are kept, is the allocator's. */
#define BREATH_OBJECTS 64    /* objects let go, or moved, between one look at the clock and the next: what they took
                              * is counted against the handler's slice (some take a hundred times what others do) */
#define POOL_STEP 16      /* the sizes: 16, 32 ... bytes */
#define POOL_CLASSES 16   /* (up to 256) */
#define POOL_KEPT 64      /* blocks of a size kept, at most */
static int pool_class(size_t bytes) {
    return bytes > POOL_STEP * POOL_CLASSES ? -1 : bytes == 0 ? 0 : (int)((bytes - 1) / POOL_STEP);
}

struct ForthObjects {
    SlotChunk **chunks; /* the slots, by their number's upper bits (one given back: no_slots, all free for good) */
    uint32_t *gen_base; /* ... each chunk's first generation (one given back and made again goes on from its last) */
    int nchunks, chunks_cap, chunks_held;   /* the entries used, their room; the chunks that are there */
    int roomy;          /* the first chunk with a free slot (-1 none) */
    int nused;          /* the objects alive */
    Region *regions;    /* [0]: the system's */
    int nregions, regions_made;   /* room for them, how many have been used (from 1: the system's is [0]) */
    int free_region;    /* the region let go last (each the next before it); 0 none */
    int post;           /* the post's region (0: not made yet) */
    long made[OBJ_GONE];   /* each kind made, all told (for `.objects`) */
    long freed;            /* the objects freed, all told (forth_objects_freed) */
    long moved;            /* ... moved by rescue (counted: a long move looks at the clock every so many) */
    long long moving_since;   /* ... and when it looked last */
    void *pool[POOL_CLASSES];   /* blocks let go, kept for the next object of their size (each holds the next) */
    int pooled[POOL_CLASSES];   /* how many of each */
    uint64_t hash_seed;   /* s-hash's: this Forth's own, random (0: not made yet) */
    uint64_t map_key[2];  /* the maps' hashes' key: this Forth's own, random - a client that names a map's keys (a
                           * JSON object's) can't choose ones that all hash alike */
};

#define REGIONS_FIRST 16
#define REGION_FIRST_MADE 32
#define REGION_MADE_KEPT 4096 /* a region's list of what it made, kept for its next use when no longer than this */
#define OBJECT_STEPS 20       /* an object made (and, later, let go) counts as this many steps of its handler's slice */
#define OBJECT_DEPTH_MAX 64   /* objects within objects moved with one (a list holding itself stops sooner: moved) */

static struct ForthObjects *objects(Forth *f) {
    if (f->objects == NULL) {
        struct ForthObjects *o = calloc(1, sizeof(struct ForthObjects));
        uint64_t key[2];

        if (o == NULL) {
            forth_error(f, "objects: out of memory");
        }
        o->roomy = -1;
        f->objects = o;
        forth_secure_random(f, (uint8_t *)key, sizeof(key));   /* (it fails: an error, the key zeros till asked again) */
        o->map_key[0] = key[0];
        o->map_key[1] = key[1];
    }
    return f->objects;
}

/* (in this file a pointer is never a cell by a cast's leave: an object's block is pushed as REF(block)) */
static inline Cell cell_given(Cell x) { return x; }
#undef PUSH
#define PUSH(x) cell_push(f, cell_given(x))
/* a slot's generations begin at a number of its own: a reference added to (taken for an address: `a i +`) names
 * another slot, and that one's generation is no likelier the same than any (begun alike, the slot next door's often
 * was - and a sum on a reference was another object, quietly) */
static inline uint32_t gen_salt(uint32_t number) { return (number * 2654435761u) >> 8 & REF_GEN_MAX; }
static SlotChunk no_slots;   /* (a chunk given back: every slot free, none ever taken - a reference there names nothing) */

static inline ObjectSlot *slot_numbered(const struct ForthObjects *o, uint32_t number) {
    return &o->chunks[number >> CHUNK_SHIFT]->slots[number & (CHUNK_SLOTS - 1)];
}
static void chunk_link(struct ForthObjects *o, int c) {   /* (it has a free slot: first among those that do) */
    SlotChunk *k = o->chunks[c];

    k->prev = -1;
    k->next = o->roomy;
    if (o->roomy >= 0) {
        o->chunks[o->roomy]->prev = c;
    }
    o->roomy = c;
}
static void chunk_unlink(struct ForthObjects *o, int c) {
    const SlotChunk *k = o->chunks[c];

    if (k->prev >= 0) {
        o->chunks[k->prev]->next = k->next;
    } else {
        o->roomy = k->next;
    }
    if (k->next >= 0) {
        o->chunks[k->next]->prev = k->prev;
    }
}
/* a chunk more: where one was given back, else after the last. 0: no memory */
static int chunk_new(struct ForthObjects *o) {
    SlotChunk *k;
    int c, i;

    for (c = 0; c < o->nchunks && o->chunks[c] != &no_slots; c++) {
    }
    if (c == o->chunks_cap) {
        int cap = o->chunks_cap ? o->chunks_cap * 2 : CHUNKS_KEPT;
        SlotChunk **chunks = realloc(o->chunks, (size_t)cap * sizeof(*chunks));
        uint32_t *base = chunks != NULL ? realloc(o->gen_base, (size_t)cap * sizeof(*base)) : NULL;

        if (chunks != NULL) {
            o->chunks = chunks;
        }
        if (base == NULL) {
            return 0;
        }
        memset(base + o->chunks_cap, 0, (size_t)(cap - o->chunks_cap) * sizeof(*base));
        o->gen_base = base;
        o->chunks_cap = cap;
    }
    if ((k = malloc(sizeof(SlotChunk))) == NULL) {
        return 0;
    }
    for (i = 0; i < CHUNK_SLOTS; i++) {
        k->slots[i] = (ObjectSlot){.gen = o->gen_base[c], .listed_at = i + 1 < CHUNK_SLOTS ? i + 1 : -1};
    }
    k->nfree = CHUNK_SLOTS;
    k->free_head = 0;
    o->chunks[c] = k;
    if (c == o->nchunks) {
        o->nchunks++;
    }
    o->chunks_held++;
    chunk_link(o, c);
    return 1;
}
/* a free slot, taken: its number in *number. NULL: no memory for more. (Every object made comes here, and every one
 * let go to slot_release: what is rare in them - a chunk made, filled, emptied - is out of their way.) */
static inline ObjectSlot *slot_take(struct ForthObjects *o, uint32_t *number) {
    SlotChunk *k;
    ObjectSlot *sl;
    int c = o->roomy;

    if (c < 0) {
        if (!chunk_new(o)) {
            return NULL;
        }
        c = o->roomy;
    }
    k = o->chunks[c];
    sl = &k->slots[k->free_head];
    *number = (uint32_t)c << CHUNK_SHIFT | (uint32_t)k->free_head;
    k->free_head = sl->listed_at;
    if (--k->nfree == 0) {
        chunk_unlink(o, c);
    }
    return sl;
}
/* a chunk whose slots are all free, given back: its generations kept by the highest of them */
static void chunk_given_back(struct ForthObjects *o, int c) {
    SlotChunk *k = o->chunks[c];
    uint32_t last = 0;
    int i;

    for (i = 0; i < CHUNK_SLOTS; i++) {
        if (k->slots[i].gen > last) {
            last = k->slots[i].gen;
        }
    }
    o->gen_base[c] = last;
    chunk_unlink(o, c);
    o->chunks[c] = &no_slots;
    o->chunks_held--;
    free(k);
}
/* a slot let go: its generation one on - what named its object names nothing now - and free for the next (a chunk
 * all free given back, past the few kept: a burst of a million objects doesn't hold their slots for good) */
static inline void slot_release(struct ForthObjects *o, ObjectSlot *sl, uint32_t number) {
    int c = (int)(number >> CHUNK_SHIFT);
    SlotChunk *k = o->chunks[c];

    sl->tag = 0;
    sl->at = NULL;
    if (sl->gen >= REF_GEN_MAX) {
        return;   /* (its generations all used: left out - forty bytes, once in sixteen million of a slot's objects) */
    }
    sl->gen++;
    sl->listed_at = k->free_head;
    k->free_head = (int)(number & (CHUNK_SLOTS - 1));
    if (++k->nfree == 1) {
        chunk_link(o, c);
    } else if (k->nfree == CHUNK_SLOTS && o->chunks_held > CHUNKS_KEPT) {
        chunk_given_back(o, c);
    }
}
void forth_object_layout(ForthObjectLayout *l) {
    l->chunks = (uint32_t)offsetof(struct ForthObjects, chunks);
    l->nchunks = (uint32_t)offsetof(struct ForthObjects, nchunks);
    l->chunk_shift = CHUNK_SHIFT;
    l->chunk_slots = (uint32_t)offsetof(SlotChunk, slots);
    l->slot_bytes = (uint32_t)sizeof(ObjectSlot);
    l->slot_at = (uint32_t)offsetof(ObjectSlot, at);
    l->slot_tag = (uint32_t)offsetof(ObjectSlot, tag);
    l->bytes_tag = REF_MARK << REF_MARK_SHIFT | (uint32_t)OBJ_BYTES << REF_KIND_SHIFT;
    l->tag_gen_bits = REF_KIND_SHIFT;
    l->bytes_p = (uint32_t)offsetof(Bytes, p);
    l->bytes_n = (uint32_t)offsetof(Bytes, n);
    l->bytes_cap = (uint32_t)offsetof(Bytes, cap);
    l->regions = (uint32_t)offsetof(struct ForthObjects, regions);
    l->region_bytes = (uint32_t)sizeof(Region);
    l->region_build = (uint32_t)offsetof(Region, build);
}
/* x's record, NULL if it is no object: one alive, whose reference x is. (Asked by every word that takes an object,
 * several times a word: written where it is asked - called, it was a tenth of a request's instructions.) */
#if defined(__GNUC__) || defined(__clang__)
#define IN_LINE_ALWAYS __attribute__((always_inline)) inline
#else
#define IN_LINE_ALWAYS inline
#endif
static IN_LINE_ALWAYS ObjectSlot *object_of(Forth *f, Cell x) {
    const struct ForthObjects *o = f->objects;
    uint32_t number = (uint32_t)(UCell)x, tag = (uint32_t)((UCell)x >> 32);
    ObjectSlot *sl;

    if (tag >> REF_MARK_SHIFT != REF_MARK || o == NULL || number >> CHUNK_SHIFT >= (uint32_t)o->nchunks) {
        return NULL;
    }
    sl = slot_numbered(o, number);
    return sl->tag == tag ? sl : NULL;
}
static inline ObjectSlot *object_slot(Forth *f, const void *p) { return object_of(f, REF(p)); }   /* a block's */
/* text a word is given, `a n`: its bytes. a is an address, or - what every word that makes a string gives - a
 * string's reference, the text its first n bytes: seen here with no call (forth_string_arg's own way round, for the
 * words of this file - nearly every text they are given is a string's) */
static inline const char *text_at(Forth *f, Cell a, Cell n, const char *word, size_t *len) {
    const ObjectSlot *sl = object_of(f, a);

    if (sl != NULL && sl->kind == OBJ_STRING && (UCell)n <= sl->size) {
        *len = (size_t)n;
        return sl->at;
    }
    return forth_string_arg(f, a, n, word, len);
}
/* every slot, in turn: sl each one's, alive or free (what looks at them all: counts, the end) */
static ObjectSlot *slot_after(const struct ForthObjects *o, ObjectSlot *sl, int *chunk) {
    for (;;) {
        if (sl != NULL && sl + 1 < o->chunks[*chunk]->slots + CHUNK_SLOTS) {
            return sl + 1;
        }
        do {
            if (++*chunk >= o->nchunks) {
                return NULL;
            }
        } while (o->chunks[*chunk] == &no_slots);
        return o->chunks[*chunk]->slots;
    }
}
#define EACH_SLOT(o, sl, chunk) for ((chunk) = -1, (sl) = slot_after((o), NULL, &(chunk)); (sl) != NULL; (sl) = slot_after((o), (sl), &(chunk)))

/* after many were let go at once (a region's end): the region's own list given back if a burst made it big */
static void objects_shrink(struct ForthObjects *o, Region *r) {
    (void)o;
    if (r->cap > REGION_MADE_KEPT) {
        free(r->made);
        r->made = NULL;
        r->cap = 0;
    }
}

static int object_live(const ObjectSlot *sl) { return sl != NULL && sl->tag != 0; }

/* A region's list of what it made is the objects it has now: one let go, or moved out, leaves a hole there and
 * then (it knows its place). The list had every object the region ever had, to be packed when it was full - and it
 * was grown at half full, so never packed: an actor that kept one object of each message's listed them all, 8 bytes
 * a message for as long as it lived; a handler's loop that made and disposed a million left a million entries.
 * The holes are closed up a few entries at each object made, once they are more than half of it: no step grows
 * with the list (one pass over a big actor's would be milliseconds), and the order stays - a map before the keys
 * it lets go itself. */
#define PACK_AT_ONCE 8   /* entries looked at, at each object made, as the list is packed */
static void region_unlist(struct ForthObjects *o, ObjectSlot *sl) {
    if (sl->listed_at != NOT_LISTED) {
        Region *r = &o->regions[sl->region];

        if (sl->listed_at < r->nmade && r->made[sl->listed_at] == REF(sl->at)) {
            r->made[sl->listed_at] = 0;
            r->holes++;
        }
        sl->listed_at = NOT_LISTED;
    }
}
static void region_pack(Forth *f, Region *r) {
    int most = PACK_AT_ONCE;

    for (; most > 0 && r->scan < r->nmade; most--, r->scan++) {
        Cell x = r->made[r->scan];

        if (x == 0) {
            continue;
        }
        if (r->kept != r->scan) {   /* (down into a hole: where it came from is one now) */
            r->made[r->kept] = x;
            r->made[r->scan] = 0;
            object_of(f, x)->listed_at = r->kept;
        }
        r->kept++;
    }
    if (r->scan == r->nmade) {   /* (all of it looked at: what is past those kept is holes) */
        r->holes -= r->nmade - r->kept;
        r->nmade = r->kept;
        r->scan = r->kept = 0;
    }
}
/* sl listed in its region, the last of what it has (the post lists none: it never ends - what leaves it is let go
 * by forth_objects_drop) */
static void region_note(Forth *f, ObjectSlot *sl) {
    struct ForthObjects *o = f->objects;
    Region *r = &o->regions[sl->region];

    sl->listed_at = NOT_LISTED;
    if (sl->region == o->post) {
        return;
    }
    if (r->holes > r->nmade / 2 && r->ended == 0) {   /* (not as it ends: its end is going through them) */
        region_pack(f, r);
    }
    if (r->nmade == r->cap) {
        int cap = r->cap ? r->cap * 2 : REGION_FIRST_MADE;
        Cell *made = realloc(r->made, (size_t)cap * sizeof(Cell));

        if (made == NULL) {
            forth_error(f, "objects: out of memory");
        }
        r->made = made;
        r->cap = cap;
    }
    sl->listed_at = r->nmade;
    r->made[r->nmade++] = REF(sl->at);
}

static int region_new(Forth *f, int parent, int lasting);

/* the region the task's objects go to now: an actor's handler runs in its actor's region till it makes one, then in a
 * scratch region of its own inside it (opened then: a handler that makes nothing costs none) */
static int task_region(Forth *f, Task *t) {
    const struct ForthObjects *o = f->objects;

    if (t->region > 0 && o->regions[t->region].lasting && t->region != o->post) {
        t->region = region_new(f, t->region, 0);
    }
    return t->region;
}

/* a new object of `bytes`, in the running task's region: its record's size `size` (a string's: its length), its
 * bytes zeroed - or not, for one whose maker writes them all */
static void *object_make(Forth *f, int kind, size_t bytes, size_t size, int zeroed) {
    struct ForthObjects *o = objects(f);
    int region = task_region(f, f->t);
    size_t block = bytes + REF_BYTES;
    uint32_t number = 0;
    ObjectSlot *sl;
    Cell *p;

    /* (an object is a tenth of a microsecond, made and let go - a word of C's, one step: a loop making them ran
     *  its slice of steps for 3 to 15 ms, where the slice is a tenth of one. Counted as the steps of that long) */
    if (f->budget > OBJECT_STEPS && f->turn_slice > 0 && f->depth == f->task_depth) {   /* (forth_slice_charge's
                                                                                         *  rule, here: no call) */
        f->budget -= OBJECT_STEPS;
    }
    if (pool_class(block) >= 0 && o->pool[pool_class(block)] != NULL) {
        int c = pool_class(block);

        p = o->pool[c];
        memcpy(&o->pool[c], p, sizeof(void *));
        o->pooled[c]--;
    } else if ((p = malloc(pool_class(block) >= 0 ? (size_t)(pool_class(block) + 1) * POOL_STEP : block)) == NULL) {
        forth_error(f, "%s: out of memory", object_kinds[kind]);
    }
    if ((sl = slot_take(o, &number)) == NULL) {
        free(p);
        forth_error(f, "%s: out of memory", object_kinds[kind]);
    }
    p++;   /* (past the cell its reference is in) */
    if (zeroed) {
        memset(p, 0, bytes);
    }
    sl->at = p;
    sl->size = size;
    sl->tag = REF_MARK << REF_MARK_SHIFT | (uint32_t)kind << REF_KIND_SHIFT | ((sl->gen + gen_salt(number)) & REF_GEN_MAX);
    sl->kind = kind;
    sl->region = region;
    sl->pending = 0;
    sl->owned = sl->permanent = 0;
    sl->listed_at = NOT_LISTED;
    p[-1] = (Cell)((UCell)sl->tag << 32 | number);
    o->nused++;
    o->made[kind]++;
    if (region > 0) {
        region_note(f, sl);   /* (no memory for the list: the object is there, in its region - let go by hand) */
    }
    return p;
}

/* a new object of `bytes` (zeroed), in the running task's region */
static void *object_new(Forth *f, int kind, size_t bytes) { return object_make(f, kind, bytes, bytes, 1); }

/* a string object of len bytes, zeroed (and a 0 after them): the record's size its length */
static char *string_alloc(Forth *f, size_t len) {
    forth_worked(f, len, WORK_FAST);
    return object_make(f, OBJ_STRING, len + 1, len, 1);
}
/* ... of these bytes (written once: not zeroed first - a page is built of thousands of them) */
static char *string_new(Forth *f, const char *s, size_t len) {
    char *p = object_make(f, OBJ_STRING, len + 1, len, 0);

    forth_worked(f, len, WORK_FAST);
    memcpy(p, s, len);
    p[len] = 0;
    return p;
}

static void *object_arg_looked_up(Forth *f, Cell x, int kind, const char *word);
static inline void *object_arg(Forth *f, Cell x, int kind, const char *word) {
    const ObjectSlot *sl = object_of(f, x);

    if (sl != NULL && sl->kind == kind) {
        return sl->at;
    }
    return object_arg_looked_up(f, x, kind, word);
}
static void *object_arg_looked_up(Forth *f, Cell x, int kind, const char *word) {
    ObjectSlot *sl = object_of(f, x);

    if (sl != NULL && sl->kind != kind) {
        forth_error(f, "%s: not %s: %s", word, object_kinds[kind], object_kinds[sl->kind]);
    }
    if (sl == NULL) {
        forth_error(f, "%s: not %s: %ld", word, object_kinds[kind], (long)x);
    }
    return sl->at;
}

static void map_let_go(Forth *f, Map *m);

static void object_free(Forth *f, ObjectSlot *sl) {
    region_unlist(f->objects, sl);
    if (sl->kind == OBJ_LIST) {
        free(((List *)sl->at)->items);
    } else if (sl->kind == OBJ_MAP) {
        map_let_go(f, sl->at);   /* (its keys' strings with it) */
    } else if (sl->kind == OBJ_BYTES) {
        free(((Bytes *)sl->at)->p);
    }
    {   /* (a string's block is its length and the 0 after it; any block's, the cell its reference is in before) */
        struct ForthObjects *o = f->objects;
        int c = pool_class((sl->kind == OBJ_STRING ? sl->size + 1 : sl->kind == OBJ_SLICE ? sizeof(Slice) : sl->size) + REF_BYTES);
        Cell *block = (Cell *)sl->at - 1;
        uint32_t number = (uint32_t)(UCell)*block;

        if (c >= 0 && o->pooled[c] < POOL_KEPT) {
            memcpy(block, &o->pool[c], sizeof(void *));
            o->pool[c] = block;
            o->pooled[c]++;
        } else {
            free(block);
        }
        sl->kind = OBJ_FREE;
        slot_release(o, sl, number);
        o->nused--;
        o->freed++;
    }
    f->objects_let_go++;
}

/* a new region inside `parent`: the one let go last, else one not used yet (the regions grown for it) */
static int region_new(Forth *f, int parent, int lasting) {
    struct ForthObjects *o = objects(f);
    int i;

    if (o->nregions == 0) {   /* (the system's: [0]) */
        o->regions = calloc(REGIONS_FIRST, sizeof(Region));
        if (o->regions == NULL) {
            forth_error(f, "objects: out of memory");
        }
        o->nregions = REGIONS_FIRST;
        o->regions_made = 1;
        o->regions[0].used = 1;
    }
    if (o->free_region > 0) {
        i = o->free_region;
        o->free_region = o->regions[i].next_free;
    } else {
        if (o->regions_made == o->nregions) {
            Region *r = realloc(o->regions, (size_t)o->nregions * 2 * sizeof(Region));

            if (r == NULL) {
                forth_error(f, "objects: out of memory");
            }
            memset(r + o->nregions, 0, (size_t)o->nregions * sizeof(Region));
            o->regions = r;
            o->nregions *= 2;
        }
        i = o->regions_made++;
    }
    o->regions[i].used = 1;
    o->regions[i].parent = parent;
    o->regions[i].lasting = lasting;
    o->regions[i].next_free = 0;
    o->regions[i].nmade = o->regions[i].holes = o->regions[i].scan = o->regions[i].kept = 0;
    o->regions[i].build = 0;
    o->regions[i].build_mark = 0;
    return i;
}

/* a region ended: what was made in it (or moved in), and not let go or moved out since, freed */
static void region_end(Forth *f, int region, int breathing) {
    struct ForthObjects *o = f->objects;
    Region *r = &o->regions[region];
    long long since = 0;   /* (the clock first looked at after the first few: most regions end before) */
    int at;

    /* (a region of a hundred thousand objects - a long JSON text read, a query's rows - takes ten milliseconds to
     *  let go: ended from inside its task (breathing), that is counted against the slice and gives way as any long
     *  work does. The region stays the task's till it has ended: a task let go meanwhile ends it from `ended` on) */
    for (at = r->ended; at < r->nmade;) {
        Cell x = r->made[at++];
        ObjectSlot *sl = x != 0 ? object_of(f, x) : NULL;

        if (object_live(sl) && sl->region == region) {
            object_free(f, sl);
        }
        if (breathing && at % BREATH_OBJECTS == 0) {
            r->ended = at;   /* (where a task let go while this waits takes it up) */
            if (forth_give_way(f, forth_work_steps(&since))) {
                r = &o->regions[region];   /* (others have had turns: the regions' table may have grown) */
                (void)forth_work_steps(&since);   /* (... and their time isn't this work's) */
            }
        }
    }
    r->ended = 0;
    r->nmade = r->holes = r->scan = r->kept = 0;
    r->used = 0;
    r->lasting = 0;
    r->next_free = o->free_region;
    o->free_region = region;
    objects_shrink(o, r);
}

static int post_region(Forth *f) {   /* (made the first time a message carries an object) */
    struct ForthObjects *o = objects(f);

    if (o->post == 0) {
        o->post = region_new(f, 0, 1);
    }
    return o->post;
}

/* may region r end with a task (a scratch region, or the post as a message is let go)? not the system's, nor an
 * actor's */
static int transient(const struct ForthObjects *o, int r) { return r > 0 && (!o->regions[r].lasting || r == o->post); }

static int region_within(const struct ForthObjects *o, int r, int outer) {   /* r is outer, or a region inside it */
    for (; r > 0; r = o->regions[r].parent) {
        if (r == outer) {
            return 1;
        }
    }
    return 0;
}

static void object_move(Forth *f, ObjectSlot *sl, int to, int from, int depth);
static void object_move_by_task(Forth *f, ObjectSlot *sl, int to, int from);   /* ... by a task's own word: it may give way */
static void object_free_deep(Forth *f, ObjectSlot *sl, int region, int depth);

static void objects_free_all(Forth *f) {
    struct ForthObjects *o = f->objects;
    int i;

    if (o == NULL) {
        return;
    }
    for (i = 0; i < o->nchunks; i++) {
        int k;

        for (k = 0; k < CHUNK_SLOTS; k++) {   /* (a chunk given back as its last is let go: no more of it then) */
            if (o->chunks[i] != &no_slots && o->chunks[i]->slots[k].tag != 0) {
                object_free(f, &o->chunks[i]->slots[k]);
            }
        }
    }
    for (i = 0; i < o->nchunks; i++) {   /* (after every object: a slot let go links its chunk among the others) */
        if (o->chunks[i] != &no_slots) {
            free(o->chunks[i]);
        }
    }
    free(o->chunks);
    free(o->gen_base);
    for (i = 0; i < o->nregions; i++) {
        free(o->regions[i].made);
    }
    for (i = 0; i < POOL_CLASSES; i++) {
        while (o->pool[i] != NULL) {
            void *p = o->pool[i];

            memcpy(&o->pool[i], p, sizeof(void *));
            free(p);
        }
    }
    free(o->regions);
    free(o);
    f->objects = NULL;
}

PRIM(p_dispose) {   /* ( obj -- ) a list, tuple or string let go */
    Cell x = POP();
    ObjectSlot *sl = object_of(f, x);

    if (sl == NULL) {
        forth_error(f, "dispose: not an object: %ld", (long)x);
    }
    if (sl->owned) {
        forth_error(f, "dispose: that string is a map's key (the map lets it go: delete-at, or the map disposed)");
    }
    if (sl->permanent) {
        forth_error(f, "dispose: a permanent object (json-true, json-false, json-null ...): the system's for good");
    }
    object_free(f, sl);
}
static void object_contents(Forth *f, const ObjectSlot *sl, void (*each)(Forth *, ObjectSlot *, void *), void *ctx);
static void make_permanent(Forth *f, ObjectSlot *c, void *ctx) {   /* (it and what it holds) */
    (void)ctx;
    if (!c->permanent) {
        c->permanent = 1;
        region_unlist(f->objects, c);
        c->region = 0;
        object_contents(f, c, make_permanent, NULL);
    }
}
/* ( obj -- obj ) the system's for good, and what it holds: no region takes it, no rescue moves it, dispose refuses -
 * a constant every actor may hold (json's true, false and null: one each, shared) */
PRIM(p_permanent) {
    ObjectSlot *sl = object_of(f, *top(f, 1));

    if (sl == NULL) {
        forth_error(f, "permanent: not an object: %ld", (long)*top(f, 1));
    }
    make_permanent(f, sl, NULL);
}
/* ( obj -- obj ) made in a region: it outlives it, kept by the one around it (a handler's: by its actor) - and what it
 * holds made in that region (or inside it) with it */
PRIM(p_rescue) {
    Cell x = *top(f, 1);
    ObjectSlot *sl = object_of(f, x);

    if (sl == NULL) {
        forth_error(f, "rescue: not an object: %ld", (long)x);
    }
    if (sl->region > 0 && !sl->owned && !sl->permanent) {   /* (a map's key: where its map is) */
        object_move_by_task(f, sl, f->objects->regions[sl->region].parent, sl->region);
    }
}
/* rescue-handler ( obj -- obj ) out of the scratch regions round it, as far as its handler's (or its task's) own:
 * it lasts till the handler ends, whatever `with-scratch` it was made in, and no longer - what a task-variable may
 * hold, a task's own being a handler's (rescued further, to the actor, it would outlive the variable naming it) */
PRIM(p_rescue_handler) {
    struct ForthObjects *o = f->objects;
    Cell x = *top(f, 1);
    ObjectSlot *sl = object_of(f, x);
    int to;

    if (sl == NULL) {
        forth_error(f, "rescue-handler: not an object: %ld", (long)x);
    }
    if (!transient(o, sl->region) || sl->owned || sl->permanent) {   /* (kept already; a map's key: where its map is) */
        return;
    }
    for (to = sl->region; transient(o, o->regions[to].parent); to = o->regions[to].parent) {
    }
    if (to != sl->region) {
        object_move(f, sl, to, 0, 0);
    }
}
PRIM(p_rescue_all) {   /* ( obj -- obj ) out of every region round it: kept till `dispose` - what it holds with it */
    Cell x = *top(f, 1);
    ObjectSlot *sl = object_of(f, x);

    if (sl == NULL) {
        forth_error(f, "rescue-all: not an object: %ld", (long)x);
    }
    if (sl->region > 0 && !sl->owned && !sl->permanent) {   /* (a map's key: where its map is) */
        object_move_by_task(f, sl, 0, 0);   /* (the system's: no region's to let go as it ends) */
    }
}
PRIM(p_object_q) { Cell x = POP(); PUSH(object_of(f, x) != NULL ? -1 : 0); }   /* ( x -- flag ) */
PRIM(p_objects) { PUSH(f->objects != NULL ? f->objects->nused : 0); }   /* ( -- n ) objects alive */
/* timed ( xt -- ) xt run, then how long it took and how many more objects are alive after it - a word's cost at the
 * prompt, a leak seen (run from C: nothing in it may wait) */
PRIM(p_timed) {
    Word *xt = forth_xt_arg(f, POP(), "timed");
    long before = f->objects != NULL ? f->objects->nused : 0;
    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    forth_execute(f, xt);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    forth_printf(f, "%.3f ms, %+ld objects\n",
                 (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6,
                 (f->objects != NULL ? f->objects->nused : 0) - before);
}
PRIM(p_dot_objects) {   /* every kind's count: alive, and made all told */
    long alive[OBJ_GONE] = {0};
    ObjectSlot *sl;
    int i, k;

    if (f->objects != NULL) {
        EACH_SLOT(f->objects, sl, i) {
            if (object_live(sl)) {
                alive[sl->kind]++;
            }
        }
    }
    for (k = OBJ_LIST; k < OBJ_GONE; k++) {
        const char *kind = object_kinds[k];

        if (strncmp(kind, "a ", 2) == 0) {
            kind += 2;   /* ("a list": list) */
        }
        forth_printf(f, "%-10s %ld alive, %ld made\n", kind, alive[k],
                     f->objects != NULL ? f->objects->made[k] : 0L);
    }
}

/* [: ... ;] with-scratch ( xt -- ) - run it in a scratch region: every object made in it is freed as it ends (an error
 * too), but those `rescue`d. It is the prelude's - `(scratch-open) >r execute r> (scratch-close)` - so the code in it
 * may wait (`await`, `yield`): the region is its task's, open till it goes on and ends. An error ends the regions
 * opened since the `catch` (or the task's start) it goes back to; a task let go - killed, started over - ends its. */
static void regions_unwind(Forth *f, Task *t, int to, int breathing) {
    while (t->region > 0 && t->region != to) {
        int region = t->region, parent = f->objects->regions[region].parent;

        if (f->objects->regions[region].lasting) {   /* (an actor's, round its handler's: not the task's to end) */
            t->region = 0;
            break;
        }
        region_end(f, region, breathing);   /* (the task's till it has ended: it may give way on the way) */
        t->region = parent;
    }
}
void forth_regions_unwind(Forth *f, Task *t, int to) {   /* t's regions ended, the latest first, till `to` is current */
    regions_unwind(f, t, to, 0);
}
/* ... by the running task itself, where it goes on after (a with-scratch's end, a handler's): long, it gives way */
void forth_regions_let_go(Forth *f, int to) { regions_unwind(f, f->t, to, 1); }
PRIM(p_scratch_open) {   /* (scratch-open) ( -- region ) a new region, the running task's objects made in it */
    int region = region_new(f, task_region(f, f->t), 0);

    f->t->region = region;
    PUSH(region);
}
PRIM(p_scratch_close) {   /* (scratch-close) ( region -- ) it ended (and any opened in it, left open) */
    Cell region = POP();
    int r = f->t->region;

    while (r > 0 && r != region) {
        r = f->objects->regions[r].parent;
    }
    if (region <= 0 || r != region) {
        forth_error(f, "with-scratch: region %ld is not open in this task", (long)region);
    }
    forth_regions_let_go(f, f->objects->regions[region].parent);
}

/* ---- lists ---- */

static List *list_arg(Forth *f, Cell x) { return object_arg(f, x, OBJ_LIST, "list"); }
static List *new_list(Forth *f) { return object_new(f, OBJ_LIST, sizeof(List)); }
static void list_add(Forth *f, List *l, Cell x) {
    if (l->n == l->cap) {
        Cell cap = l->cap * 2 + 8;
        Cell *items = realloc(l->items, (size_t)cap * sizeof(Cell));

        if (items == NULL) {   /* (the list as it was) */
            forth_error(f, "list: out of memory at %ld items", (long)l->n);
        }
        l->items = items;
        l->cap = cap;
    }
    l->items[l->n++] = x;
}
PRIM(p_list) { PUSH(REF(new_list(f))); }   /* ( -- l ) */
PRIM(p_list_free) {   /* ( l -- ) (dispose, for a list) */
    Cell x = POP();

    list_arg(f, x);
    object_free(f, object_of(f, x));
}
/* ---- sequences (Factor's protocol): a list's cells, a string's bytes, a byte buffer's bytes - the same words for
 * each, and what one makes the kind of what it was given (`map` over a string a string). Lists and bytes grow; a
 * string keeps its length (its object is its address: it can't move) - built in bytes, then made (`build`, `like`).
 * (A slot's record is looked up again after anything is made: the table of them may move as it grows.) ---- */

static void bytes_grown(Forth *f, Bytes *b, Cell more);
/* room in b for `more` bytes after its own. It has it, nearly every time (a byte pushed, a few appended): seen where
 * it is asked; else made (an error leaves the bytes as they were). */
static inline void bytes_room(Forth *f, Bytes *b, Cell more) {
    if (more < 0 || more > b->cap - b->n) {
        bytes_grown(f, b, more);
    }
    forth_worked(f, (size_t)more, WORK_FAST);   /* (bytes about to be put there) */
}
static Bytes *bytes_arg(Forth *f, Cell x, const char *word);

static int text_kind(int kind) { return kind == OBJ_STRING || kind == OBJ_SLICE; }   /* bytes that don't change */
static int byte_kind(int kind) { return kind == OBJ_STRING || kind == OBJ_BYTES || kind == OBJ_SLICE; }
/* a slice's bytes, where they are now: NULL if what it is a piece of was let go */
static const uint8_t *slice_bytes(Forth *f, const ObjectSlot *sl) {
    Slice *s = sl->at;

    if (s->base != 0) {
        const ObjectSlot *b = object_of(f, s->base);

        if (b == NULL || (b->kind == OBJ_SLICE && ((const Slice *)b->at)->base != 0)) {
            return NULL;
        }
        s->p = (b->kind == OBJ_SLICE ? ((const Slice *)b->at)->p : (const uint8_t *)b->at) + s->from;
    }
    return s->p;
}
static ObjectSlot *seq_slot(Forth *f, Cell x, const char *word) {
    ObjectSlot *sl = object_of(f, x);

    if (sl == NULL || (sl->kind != OBJ_LIST && sl->kind != OBJ_STRING && sl->kind != OBJ_BYTES && sl->kind != OBJ_SLICE)) {
        forth_error(f, "%s: not a sequence (a list, a string, bytes): %s", word,
                    sl == NULL ? "no object" : object_kinds[sl->kind]);
    }
    if (sl->kind == OBJ_SLICE && slice_bytes(f, sl) == NULL) {
        forth_error(f, "%s: a slice of a string that was let go", word);
    }
    return sl;
}
static Cell seq_length(const ObjectSlot *sl) {
    return sl->kind == OBJ_LIST ? ((const List *)sl->at)->n
         : sl->kind == OBJ_BYTES ? ((const Bytes *)sl->at)->n : (Cell)sl->size;
}
static uint8_t *seq_bytes(const ObjectSlot *sl) {   /* a string's, a slice's (seq_slot found them) or a buffer's bytes */
    return sl->kind == OBJ_BYTES ? ((Bytes *)sl->at)->p
         : sl->kind == OBJ_SLICE ? (uint8_t *)(uintptr_t)((const Slice *)sl->at)->p : (uint8_t *)sl->at;
}
static Cell seq_index(Forth *f, const ObjectSlot *sl, Cell i, const char *word) {   /* (negative: from the end) */
    Cell n = seq_length(sl);

    if (i < 0) {
        i += n;
    }
    if (i < 0 || i >= n) {
        forth_error(f, "%s: index %ld out of range (length %ld)", word, (long)i, (long)n);
    }
    return i;
}
static Cell seq_get(const ObjectSlot *sl, Cell i) {
    return sl->kind == OBJ_LIST ? ((const List *)sl->at)->items[i] : seq_bytes(sl)[i];
}
static uint8_t byte_of(Forth *f, Cell x, const char *word) {
    if (x < 0 || x > 255) {
        forth_error(f, "%s: %ld isn't a byte (a string's or bytes' items are 0 to 255)", word, (long)x);
    }
    return (uint8_t)x;
}
static void seq_put(Forth *f, ObjectSlot *sl, Cell i, Cell x, const char *word) {
    if (sl->kind == OBJ_LIST) {
        ((List *)sl->at)->items[i] = x;
    } else if (sl->kind == OBJ_SLICE) {
        forth_error(f, "%s: a slice doesn't change (it is a piece of a string, or what came on a connection)", word);
    } else if (sl->kind == OBJ_STRING) {   /* (what is cut from it, kept by it as a key, sent with it, relies on it) */
        forth_error(f, "%s: a string doesn't change (bytes do: `<bytes> like`, changed, then `\" \" like` - or map)", word);
    } else {
        seq_bytes(sl)[i] = byte_of(f, x, word);
    }
}
static void seq_push(Forth *f, ObjectSlot *sl, Cell x, const char *word) {
    if (sl->kind == OBJ_LIST) {
        list_add(f, sl->at, x);
    } else if (sl->kind == OBJ_BYTES) {
        Bytes *b = sl->at;
        uint8_t c = byte_of(f, x, word);

        bytes_room(f, b, 1);
        b->p[b->n++] = c;
    } else {
        forth_error(f, "%s: a string doesn't grow (a list or bytes does; a string is built: build, like)", word);
    }
}
/* from's items after to's own (to a list or bytes; from any sequence - to itself too) */
static void seq_add_all(Forth *f, ObjectSlot *to, const ObjectSlot *from, Cell start, Cell end, const char *word) {
    Cell i;

    if (end <= start) {
        return;   /* (nothing: and a sequence with none may have no bytes to copy from, or to) */
    }
    if (to->kind == OBJ_BYTES && from->kind != OBJ_LIST) {
        Bytes *b = to->at;

        bytes_room(f, b, end - start);   /* (then their address: it may have moved, from being to) */
        memmove(b->p + b->n, seq_bytes(from) + start, (size_t)(end - start));
        b->n += end - start;
        return;
    }
    forth_worked(f, (size_t)(end - start), WORK_ITEMS);
    for (i = start; i < end; i++) {
        seq_push(f, to, seq_get(from, i), word);
    }
}
/* a new list, or bytes (what a string is built in), to gather a sequence of that kind into */
static Cell gatherer(Forth *f, int kind) {   /* (a slice's: as a string's) */
    return kind == OBJ_LIST ? REF(new_list(f)) : REF(object_new(f, OBJ_BYTES, sizeof(Bytes)));
}
/* what was gathered as that kind: bytes made a string (and let go) for a string; else as it is */
static Cell gathered(Forth *f, Cell acc, int kind) {
    Bytes *b;
    char *s;

    if (object_of(f, acc) == NULL) {
        forth_error(f, "map: what was gathered isn't an object: %ld", (long)acc);
    }
    if (!text_kind(kind) || object_of(f, acc)->kind != OBJ_BYTES) {   /* (what is made of a slice: a string) */
        return acc;
    }
    b = object_of(f, acc)->at;
    s = string_alloc(f, (size_t)b->n);
    b = object_of(f, acc)->at;
    memcpy(s, b->p != NULL ? (const char *)b->p : "", (size_t)b->n);
    object_free(f, object_of(f, acc));
    return REF(s);
}

PRIM(p_push) {   /* push ( x seq -- ) onto its end: a list's, bytes' (a string doesn't grow) */
    ObjectSlot *sl = seq_slot(f, POP(), "push");

    seq_push(f, sl, POP(), "push");
}
PRIM(p_pop) {   /* pop ( seq -- x ) its last, taken off */
    ObjectSlot *sl = seq_slot(f, POP(), "pop");
    Cell n = seq_length(sl), x;

    if (sl->kind == OBJ_STRING) {
        forth_error(f, "pop: a string doesn't shrink");
    }
    if (n == 0) {
        forth_error(f, "pop: it is empty");
    }
    x = seq_get(sl, n - 1);
    if (sl->kind == OBJ_LIST) {
        ((List *)sl->at)->n--;
    } else {
        ((Bytes *)sl->at)->n--;
    }
    PUSH(x);
}
PRIM(p_nth) {   /* nth ( i seq -- x ) (negative: from the end) */
    ObjectSlot *sl = seq_slot(f, POP(), "nth");
    Cell i = POP();

    PUSH(seq_get(sl, seq_index(f, sl, i, "nth")));
}
PRIM(p_nth_store) {   /* nth! ( x i seq -- ) */
    ObjectSlot *sl = seq_slot(f, POP(), "nth!");
    Cell i = POP(), x = POP();

    seq_put(f, sl, seq_index(f, sl, i, "nth!"), x, "nth!");
}
PRIM(p_length) { PUSH(seq_length(seq_slot(f, POP(), "length"))); }   /* ( seq -- n ) */
PRIM(p_list_clear) { list_arg(f, POP())->n = 0; }
PRIM(p_clear) {   /* clear ( seq -- ) its items gone: a list's, bytes' */
    ObjectSlot *sl = seq_slot(f, POP(), "clear");

    if (text_kind(sl->kind)) {
        forth_error(f, "clear: a string keeps its length");
    }
    if (sl->kind == OBJ_LIST) {
        ((List *)sl->at)->n = 0;
    } else {
        ((Bytes *)sl->at)->n = 0;
    }
}
PRIM(p_like_acc) {   /* (like-acc) ( seq -- acc ) what map and filter gather into, for a sequence of its kind */
    PUSH(gatherer(f, seq_slot(f, POP(), "map")->kind));
}
PRIM(p_like_done) {   /* (like-done) ( acc seq -- seq' ) what they gathered, as its kind */
    int kind = seq_slot(f, POP(), "map")->kind;
    Cell acc = POP();

    PUSH(gathered(f, acc, kind));
}
PRIM(p_string_copy) {   /* (string-copy) ( s -- s' ) a compiled " text"'s: its copy, made where objects are made now */
    Cell x = POP();
    size_t n;
    const char *s = forth_object_string(f, x, &n);
    char *p;

    if (s == NULL) {
        forth_error(f, "(string-copy): not a string");
    }
    p = string_alloc(f, n);
    memcpy(p, forth_object_string(f, x, &n), n);
    PUSH(REF(p));
}
PRIM(p_like) {   /* like ( seq exemplar -- seq' ) its items as a sequence of the exemplar's kind (of it already: itself) */
    int kind = seq_slot(f, POP(), "like")->kind;
    Cell x = POP(), acc;

    if (seq_slot(f, x, "like")->kind == kind) {
        PUSH(x);
        return;
    }
    acc = gatherer(f, kind);
    seq_add_all(f, object_of(f, acc), object_of(f, x), 0, seq_length(object_of(f, x)), "like");
    PUSH(gathered(f, acc, kind));
}
PRIM(p_append) {   /* append ( a b -- c ) a's items, then b's: a new sequence of a's kind */
    Cell b = POP(), a = POP(), c;
    int kind = seq_slot(f, a, "append")->kind;

    seq_slot(f, b, "append");
    c = gatherer(f, kind);
    seq_add_all(f, object_of(f, c), object_of(f, a), 0, seq_length(object_of(f, a)), "append");
    seq_add_all(f, object_of(f, c), object_of(f, b), 0, seq_length(object_of(f, b)), "append");
    PUSH(gathered(f, c, kind));
}
PRIM(p_append_store) {   /* append! ( a b -- a ) b's items onto a's end, in place (a list or bytes) */
    Cell b = POP(), a = *top(f, 1);
    ObjectSlot *sa = seq_slot(f, a, "append!"), *sb = seq_slot(f, b, "append!");

    if (text_kind(sa->kind)) {
        forth_error(f, "append!: a string doesn't grow (append makes a new one)");
    }
    seq_add_all(f, sa, sb, 0, seq_length(sb), "append!");
}
PRIM(p_subseq) {   /* subseq ( seq from to -- seq' ) the items from `from` up to (not) `to`: a new one, of its kind */
    Cell to = POP(), from = POP(), x = POP(), acc, n;
    int kind = seq_slot(f, x, "subseq")->kind;

    n = seq_length(object_of(f, x));
    if (from < 0 || to < from || to > n) {
        forth_error(f, "subseq: %ld to %ld out of 0 to %ld", (long)from, (long)to, (long)n);
    }
    if (text_kind(kind)) {   /* (a string's, a slice's: a string of those bytes, copied once) */
        PUSH(REF(string_new(f, (const char *)seq_bytes(object_of(f, x)) + from, (size_t)(to - from))));
        return;
    }
    acc = gatherer(f, kind);
    seq_add_all(f, object_of(f, acc), object_of(f, x), from, to, "subseq");
    PUSH(gathered(f, acc, kind));
}
/* ---- slices (Factor's): a piece of a string that is no copy - <slice>, head-slice, tail-slice. What reads a string
 * reads one; what it is a piece of let go, it is an error to read (the bytes are no more). subseq, head and tail copy:
 * a string of its own, that lasts whatever the other does. ---- */
static Cell slice_new(Forth *f, Cell base, Cell from, Cell to, const char *word) {
    const ObjectSlot *b = seq_slot(f, base, word);
    Slice *s;

    if (!text_kind(b->kind)) {
        forth_error(f, "%s: a slice is of a string (%s: subseq makes a piece of its own)", word, object_kinds[b->kind]);
    }
    if (from < 0 || to < from || to > (Cell)b->size) {
        forth_error(f, "%s: %ld to %ld out of 0 to %ld", word, (long)from, (long)to, (long)b->size);
    }
    if (b->kind == OBJ_SLICE && ((const Slice *)b->at)->base != 0) {   /* (a piece of a piece: of the string itself) */
        from += (Cell)((const Slice *)b->at)->from;
        to += (Cell)((const Slice *)b->at)->from;
        base = ((const Slice *)b->at)->base;
        b = object_of(f, base);
    }
    s = object_make(f, OBJ_SLICE, sizeof(Slice), (size_t)(to - from), 1);
    s->base = base;
    s->from = (size_t)from;
    s->p = seq_bytes(b) + from;
    return REF(s);
}
PRIM(p_slice) {   /* <slice> ( from to seq -- slice ) the string's bytes from `from` up to (not) `to` */
    Cell x = POP(), to = POP(), from = POP();

    PUSH(slice_new(f, x, from, to, "<slice>"));
}
PRIM(p_head_slice) {   /* head-slice ( seq n -- slice ) its first n */
    Cell n = POP(), x = POP();

    PUSH(slice_new(f, x, 0, n, "head-slice"));
}
PRIM(p_tail_slice) {   /* tail-slice ( seq n -- slice ) all but its first n */
    Cell n = POP(), x = POP();

    PUSH(slice_new(f, x, n, seq_length(seq_slot(f, x, "tail-slice")), "tail-slice"));
}
/* (for net.c, and any C that lends its memory to the scripts for a while) n bytes at p as a slice, in the running
 * task's region; forth_view_gone as the memory moves or goes - the slice let go, so every cell that named it names
 * nothing (one let go already - its region ended - is none: nothing to do) */
Cell forth_new_view(Forth *f, const void *p, size_t n) {
    Slice *s = object_make(f, OBJ_SLICE, sizeof(Slice), n, 1);

    s->p = p;
    return REF(s);
}
void forth_view_gone(Forth *f, Cell view) {
    ObjectSlot *sl = object_of(f, view);

    if (sl != NULL && sl->kind == OBJ_SLICE && ((const Slice *)sl->at)->base == 0) {
        object_free(f, sl);
    }
}
PRIM(p_reverse) {   /* reverse ( seq -- seq' ) its items last first: a new one, of its kind */
    Cell x = POP(), acc, i;
    int kind = seq_slot(f, x, "reverse")->kind;

    acc = gatherer(f, kind);
    forth_worked(f, (size_t)seq_length(object_of(f, x)), WORK_ITEMS);
    for (i = seq_length(object_of(f, x)) - 1; i >= 0; i--) {
        seq_push(f, object_of(f, acc), seq_get(object_of(f, x), i), "reverse");
    }
    PUSH(gathered(f, acc, kind));
}
PRIM(p_index) {   /* index ( x seq -- i | -1 ) where x first is in it */
    ObjectSlot *sl = seq_slot(f, POP(), "index");
    Cell x = POP(), i, n = seq_length(sl);

    if (byte_kind(sl->kind)) {   /* (a byte in bytes: memchr) */
        const uint8_t *bytes = seq_bytes(sl), *at = x >= 0 && x <= 255 && n > 0 ? memchr(bytes, (int)x, (size_t)n) : NULL;

        forth_worked(f, (size_t)(at != NULL ? at - bytes : n), WORK_FAST);
        PUSH(at != NULL ? (Cell)(at - bytes) : -1);
        return;
    }
    for (i = 0; i < n && seq_get(sl, i) != x; i++) {
    }
    forth_worked(f, (size_t)i, WORK_ITEMS);
    PUSH(i < n ? i : -1);
}
/* ---- a sequence made of another, all of it at once: what a quotation an item would do in forty instructions each,
 * done here in a few - a table looked up, bits cut another way. base64 is the two of them; hex, base32 the same. ---- */
#define REGROUP_BITS_MAX 8        /* an item's bits, at most: a byte's */
#define WHOLE_LONG 16384          /* items: so many made at once are counted against the handler's slice */

/* a new sequence of n bytes, of that kind (a string or bytes): where to write them */
static uint8_t *bytes_made(Forth *f, int kind, Cell n, Cell *made) {
    if (text_kind(kind)) {   /* (of a slice: a string) */
        uint8_t *s = (uint8_t *)string_alloc(f, (size_t)n);

        *made = REF(s);
        return s;
    }
    {
        Bytes *b = object_new(f, OBJ_BYTES, sizeof(Bytes));

        *made = REF(b);
        bytes_room(f, b, n);
        b->n = n;
        return b->p;
    }
}
/* translate ( seq table -- seq' ) each item of seq the table's item at that place - a byte through a table of 256, a
 * digit's value through an alphabet: a new sequence, of seq's kind. An item with no place in the table is an error. */
PRIM(p_translate) {
    Cell table = POP(), x = POP(), made, i, n, m;
    int kind = seq_slot(f, x, "translate")->kind, table_kind = seq_slot(f, table, "translate")->kind;
    long long since = 0;

    n = seq_length(object_of(f, x));
    m = seq_length(object_of(f, table));
    if (n >= WHOLE_LONG) {
        (void)forth_work_steps(&since);
    }
    if (byte_kind(kind) && byte_kind(table_kind)) {   /* (bytes through bytes: no item looked up but in the table) */
        uint8_t *out = bytes_made(f, kind, n, &made);
        const uint8_t *in = seq_bytes(object_of(f, x)), *t = seq_bytes(object_of(f, table));

        for (i = 0; i < n; i++) {
            if (in[i] >= m) {
                Cell bad = in[i];

                object_free(f, object_of(f, made));
                forth_error(f, "translate: %ld has no place in a table of %ld", (long)bad, (long)m);
            }
            out[i] = t[in[i]];
        }
    } else {
        made = gatherer(f, kind);
        for (i = 0; i < n; i++) {
            Cell v = seq_get(object_of(f, x), i);

            if (v < 0 || v >= m) {
                object_free(f, object_of(f, made));
                forth_error(f, "translate: %ld has no place in a table of %ld", (long)v, (long)m);
            }
            seq_push(f, object_of(f, made), seq_get(object_of(f, table), v), "translate");
        }
        made = gathered(f, made, kind);
    }
    if (n >= WHOLE_LONG) {
        forth_slice_charge(f, forth_work_steps(&since));
    }
    PUSH(made);
}
/* regroup ( seq from to -- seq' ) seq's items, each of `from` bits, as items of `to` bits - the same bits in their
 * order, cut another way (8 6: bytes as base64's sextets; 6 8 back; 8 4: as hex digits' values): a new sequence, of
 * seq's kind (a string or bytes). Bits left over at the end are filled out with zeros to a last item when the cut
 * is finer (to < from), and left out when it is coarser - so `8 6 regroup 6 8 regroup` gives the bytes back. An item
 * that doesn't fit `from` bits is an error. */
PRIM(p_regroup) {
    Cell to = POP(), from = POP(), x = POP(), made, i, n, k = 0, room;
    int kind = seq_slot(f, x, "regroup")->kind, bits = 0;
    uint32_t acc = 0;
    const uint8_t *in;
    uint8_t *out;
    long long since = 0;

    if (!byte_kind(kind)) {
        forth_error(f, "regroup: a string's or bytes' items (a list's aren't bytes)");
    }
    if (from < 1 || from > REGROUP_BITS_MAX || to < 1 || to > REGROUP_BITS_MAX) {
        forth_error(f, "regroup: items of %ld bits as items of %ld: 1 to %d each", (long)from, (long)to, REGROUP_BITS_MAX);
    }
    n = seq_length(object_of(f, x));
    if (n > CELL_BYTES_MAX / REGROUP_BITS_MAX) {
        forth_error(f, "regroup: too many items");
    }
    if (n >= WHOLE_LONG) {
        (void)forth_work_steps(&since);
    }
    room = to < from ? (n * from + to - 1) / to : n * from / to;   /* (the last bits: an item more, or left out) */
    out = bytes_made(f, kind, room, &made);
    in = seq_bytes(object_of(f, x));
    for (i = 0; i < n; i++) {
        if (in[i] >> from != 0) {
            Cell bad = in[i];

            object_free(f, object_of(f, made));
            forth_error(f, "regroup: %ld doesn't fit %ld bits", (long)bad, (long)from);
        }
        acc = acc << from | in[i];
        bits += (int)from;
        while (bits >= to) {
            bits -= (int)to;
            out[k++] = (uint8_t)(acc >> bits & ((1u << to) - 1));
        }
        acc &= (1u << bits) - 1;
    }
    if (bits > 0 && to < from) {   /* (cut finer: the last bits, zeros after them) */
        out[k++] = (uint8_t)(acc << (to - bits) & ((1u << to) - 1));
    }
    if (n >= WHOLE_LONG) {
        forth_slice_charge(f, forth_work_steps(&since));
    }
    PUSH(made);
}

/* where sub first is in seq, from `from` on (its items: the same values; a byte kind's against a byte kind's by bytes) */
static Cell seq_search(const ObjectSlot *s, const ObjectSlot *sub, Cell from) {
    Cell n = seq_length(s), m = seq_length(sub), i, j;

    if (m == 0) {
        return from <= n ? from : -1;
    }
    if (from > n - m) {   /* (no room for it from there - and no sum past a cell's largest) */
        return -1;
    }
    for (i = from; i <= n - m; i++) {
        for (j = 0; j < m && seq_get(s, i + j) == seq_get(sub, j); j++) {
        }
        if (j == m) {
            return i;
        }
    }
    return -1;
}
static size_t find_in(const char *s, size_t n, const char *t, size_t m, size_t from);
static Cell searched(Forth *f, const ObjectSlot *s, const ObjectSlot *sub, Cell from) {   /* ... and counted */
    Cell at;

    if (byte_kind(s->kind) && byte_kind(sub->kind) && seq_length(sub) > 0) {   /* (bytes in bytes: the C library's way -
                                                                               *  a head's end found in a request) */
        Cell n = seq_length(s);

        if (from > n) {
            return -1;
        }
        at = (Cell)find_in((const char *)seq_bytes(s), (size_t)n, (const char *)seq_bytes(sub),
                           (size_t)seq_length(sub), (size_t)from);
        forth_worked(f, (size_t)((at < n ? at : n) - from), WORK_FAST);
        return at < n ? at : -1;
    }
    at = seq_search(s, sub, from);

    forth_worked(f, (size_t)((at < 0 ? seq_length(s) : at) - (from < seq_length(s) ? from : seq_length(s))), WORK_ITEMS);
    return at;
}
/* ---- for what reads a protocol: a class of bytes run through, digits read - from a place in a sequence on, no piece
 * of it made. A class: a table of 256 bytes (BYTE_VALUES), not 0 at each byte that is of it. ---- */
#define DIGIT_BASE_MAX 16   /* the bases digits>number reads: 2 to 16 */
/* span ( seq table from -- i ) where the run of bytes of the class, begun at `from`, ends (from: none there; the
 * sequence's length: all the rest) */
PRIM(p_span) {
    Cell from = POP(), table = POP();
    const ObjectSlot *sl = seq_slot(f, POP(), "span");
    const uint8_t *of = forth_addr_n(f, table, 256), *bytes;
    Cell n = seq_length(sl), i;

    if (!byte_kind(sl->kind)) {
        forth_error(f, "span: a string's or bytes' (%s)", object_kinds[sl->kind]);
    }
    if (from < 0 || from > n) {
        forth_error(f, "span: from %ld of %ld", (long)from, (long)n);
    }
    bytes = seq_bytes(sl);
    for (i = from; i < n && of[bytes[i]]; i++) {
    }
    forth_worked(f, (size_t)(i - from), WORK_BYTES);
    PUSH(i);
}
/* digits>number ( seq from to base -- u true | false ) the digits from `from` up to `to` as a number in the base
 * (letters of either case past 9): false for none, for one that is no digit of the base - a sign, a space - and for a
 * number past a cell's largest. What a length, a chunk's size, a port is read with: nothing but its digits. */
PRIM(p_digits_to_number) {
    Cell base = POP(), to = POP(), from = POP();
    const ObjectSlot *sl = seq_slot(f, POP(), "digits>number");
    const uint8_t *bytes;
    UCell u = 0;
    Cell i;

    if (!byte_kind(sl->kind)) {
        forth_error(f, "digits>number: a string's or bytes' (%s)", object_kinds[sl->kind]);
    }
    if (from < 0 || to < from || to > seq_length(sl) || base < 2 || base > DIGIT_BASE_MAX) {
        forth_error(f, "digits>number: %ld to %ld of %ld, base %ld", (long)from, (long)to, (long)seq_length(sl), (long)base);
    }
    bytes = seq_bytes(sl);
    for (i = from; i < to; i++) {
        int c = bytes[i], d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : base;

        if (d >= base || u > ((UCell)INTPTR_MAX - (UCell)d) / (UCell)base) {
            PUSH(0);
            return;
        }
        u = u * (UCell)base + (UCell)d;
    }
    if (to == from) {
        PUSH(0);
        return;
    }
    PUSH((Cell)u);
    PUSH(-1);
}
/* be> le> ( seq from to -- u ) the bytes from `from` up to `to` as one number, the first the most significant (be>)
 * or the least (le>): a frame's length, a packet's fields - eight bytes at most */
static void bytes_number(Forth *f, int big, const char *word) {
    Cell to = POP(), from = POP();
    const ObjectSlot *sl = seq_slot(f, POP(), word);
    const uint8_t *bytes;
    UCell u = 0;
    Cell i;

    if (!byte_kind(sl->kind)) {
        forth_error(f, "%s: a string's or bytes' (%s)", word, object_kinds[sl->kind]);
    }
    if (from < 0 || to < from || to > seq_length(sl) || to - from > (Cell)sizeof(Cell)) {
        forth_error(f, "%s: %ld to %ld of %ld (a cell's bytes at most)", word, (long)from, (long)to, (long)seq_length(sl));
    }
    bytes = seq_bytes(sl);
    for (i = from; i < to; i++) {
        u = big ? u << 8 | bytes[i] : u | (UCell)bytes[i] << (8 * (i - from));
    }
    PUSH((Cell)u);
}
PRIM(p_be_number) { bytes_number(f, 1, "be>"); }
PRIM(p_le_number) { bytes_number(f, 0, "le>"); }
/* seq-xor ( seq key -- seq' ) each byte xor the key's byte at its place, the key gone round as often as it takes: a
 * new one of seq's kind (a WebSocket frame unmasked, a token's pad taken off) */
PRIM(p_seq_xor) {
    Cell k = POP(), x = POP(), made;
    const ObjectSlot *key = seq_slot(f, k, "seq-xor"), *sl = seq_slot(f, x, "seq-xor");
    Cell n = seq_length(sl), kn = seq_length(key), i;
    uint8_t *out;
    const uint8_t *in, *kb;

    if (!byte_kind(sl->kind) || !byte_kind(key->kind)) {
        forth_error(f, "seq-xor: strings' or bytes' (%s, %s)", object_kinds[sl->kind], object_kinds[key->kind]);
    }
    if (kn == 0 && n > 0) {
        forth_error(f, "seq-xor: a key of no bytes");
    }
    out = bytes_made(f, sl->kind, n, &made);
    in = seq_bytes(seq_slot(f, x, "seq-xor"));   /* (after what was made: found again) */
    kb = seq_bytes(seq_slot(f, k, "seq-xor"));
    for (i = 0; i < n; i++) {
        out[i] = in[i] ^ kb[i % kn];
    }
    forth_worked(f, (size_t)n, WORK_BYTES);
    PUSH(made);
}
PRIM(p_subseq_index) {   /* subseq-index ( seq sub -- i | -1 ) where sub first is in it */
    ObjectSlot *sub = seq_slot(f, POP(), "subseq-index"), *s = seq_slot(f, POP(), "subseq-index");

    PUSH(searched(f, s, sub, 0));
}
PRIM(p_subseq_index_from) {   /* subseq-index-from ( seq sub from -- i | -1 ) the same, looking from `from` on */
    Cell from = POP();
    ObjectSlot *sub = seq_slot(f, POP(), "subseq-index-from"), *s = seq_slot(f, POP(), "subseq-index-from");

    PUSH(from < 0 ? -1 : searched(f, s, sub, from));
}
PRIM(p_seq_equal) {   /* seq= ( a b -- flag ) the same items, in the same order (whatever their kinds) */
    ObjectSlot *b = seq_slot(f, POP(), "seq="), *a = seq_slot(f, POP(), "seq=");
    Cell n = seq_length(a), i;

    for (i = 0; n == seq_length(b) && i < n && seq_get(a, i) == seq_get(b, i); i++) {
    }
    forth_worked(f, (size_t)i, WORK_ITEMS);
    PUSH(n == seq_length(b) && i == n ? -1 : 0);
}

/* ---- build: a string made a piece at a time - Factor's make. `[: ... ;] build` runs the code in a scratch region of
 * its own (what it makes besides freed as it ends), `%` and its kin adding to the bytes being built there; the
 * string made of them given in the region around. The bytes are found by the region the task is in: each task its
 * own (one may wait in the middle: another's build is in its own region), and an error that leaves the build leaves
 * nothing behind (its region ended with it). ---- */

PRIM(p_build_open) {   /* (build-open) ( -- region ) */
    int region = region_new(f, task_region(f, f->t), 0);
    Cell b;

    f->t->region = region;
    b = REF(object_new(f, OBJ_BYTES, sizeof(Bytes)));
    f->objects->regions[region].build = b;
    PUSH(region);
}
static int build_region(Forth *f, const char *word) {   /* the innermost build's region */
    const struct ForthObjects *o = f->objects;
    int r = f->t->region;

    while (o != NULL && r > 0 && !o->regions[r].lasting) {
        if (o->regions[r].build != 0) {
            return r;
        }
        r = o->regions[r].parent;
    }
    forth_error(f, "%s: only inside build ([: ... ;] build)", word);
}
static Cell building(Forth *f, const char *word) {   /* (the region first: there may be no objects yet at all) */
    int r = build_region(f, word);

    return f->objects->regions[r].build;
}
PRIM(p_build_mark) {   /* ( -- x ) */
    int r = build_region(f, "build-mark@");

    PUSH(f->objects->regions[r].build_mark);
}
PRIM(p_build_mark_store) {   /* build-mark! ( x -- ) a cell kept with the string being built (0 as it begins) */
    Cell x = POP();
    int r = build_region(f, "build-mark!");

    f->objects->regions[r].build_mark = x;
}
PRIM(p_build_at) {   /* build-at ( -- b n ) the string being built: which (one build's bytes, while it lasts), how long so far */
    Cell b = building(f, "build-at");

    PUSH(b);
    PUSH(bytes_arg(f, b, "build-at")->n);
}
PRIM(p_build_close) {   /* (build-close) ( region -- s ) what was built: a string, made in the region around; it ended */
    Cell region = POP();
    struct ForthObjects *o = f->objects;
    int r = f->t->region, parent, was;
    Bytes *b;
    char *s;

    while (r > 0 && r != region) {
        r = o->regions[r].parent;
    }
    if (region <= 0 || r != region || o->regions[region].build == 0) {
        forth_error(f, "build: region %ld is no build open in this task", (long)region);
    }
    parent = o->regions[region].parent;
    was = f->t->region;
    f->t->region = parent;   /* (the string the region around's) */
    s = string_alloc(f, (size_t)((Bytes *)object_of(f, o->regions[region].build)->at)->n);
    f->t->region = was;
    b = object_of(f, f->objects->regions[region].build)->at;
    memcpy(s, b->p != NULL ? (const char *)b->p : "", (size_t)b->n);
    forth_regions_unwind(f, f->t, parent);
    PUSH(REF(s));
}
PRIM(p_pct) {   /* % ( seq -- ) a string's or bytes' bytes onto what is being built (a list's items: each a byte) */
    Cell x = POP(), b = building(f, "%");

    seq_slot(f, x, "%");
    seq_add_all(f, object_of(f, b), object_of(f, x), 0, seq_length(object_of(f, x)), "%");
}
PRIM(p_pct_s) {   /* %s ( a n -- ) the bytes of addr len onto what is being built */
    Bytes *b = bytes_arg(f, building(f, "%s"), "%s");
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, "%s", &len);

    if (len == 0) {
        return;
    }
    bytes_room(f, b, (Cell)len);
    memcpy(b->p + b->n, s, len);
    b->n += (Cell)len;
}
/* A whole number's digits, written here and not by printf: a number in an answer's head, a count in a page, was a
 * format string made, read back and run through the C library's whole machinery - several times, for format. */
#define NUMBER_TEXT_MAX 24   /* a cell's digits at most (20 in decimal), and its sign */
#define DECIMAL 10
#define HEX 16

/* u's digits in `base` (hex: capitals if upper), a '-' before them if negative, at out: how many bytes */
static size_t number_text(char *out, UCell u, int negative, int base, int upper) {
    const char *digit = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char back[NUMBER_TEXT_MAX];
    size_t n = 0, len = 0;

    do {
        back[n++] = digit[u % (UCell)base];
        u /= (UCell)base;
    } while (u != 0);
    if (negative) {
        out[len++] = '-';
    }
    while (n > 0) {
        out[len++] = back[--n];
    }
    return len;
}
/* ... a cell's, in decimal (the most negative too: its size taken unsigned) */
static size_t cell_text(char *out, Cell x) { return number_text(out, x < 0 ? (UCell)0 - (UCell)x : (UCell)x, x < 0, DECIMAL, 0); }

PRIM(p_pct_n) {   /* %n ( n -- ) the number in decimal onto what is being built */
    Bytes *b = bytes_arg(f, building(f, "%n"), "%n");
    char digits[NUMBER_TEXT_MAX];
    int len = (int)cell_text(digits, POP());

    bytes_room(f, b, len);
    memcpy(b->p + b->n, digits, (size_t)len);
    b->n += len;
}
PRIM(p_pct_c) {   /* %c ( c -- ) a byte onto what is being built */
    Bytes *b = bytes_arg(f, building(f, "%c"), "%c");
    uint8_t c = byte_of(f, POP(), "%c");

    bytes_room(f, b, 1);
    b->p[b->n++] = c;
}
/* { a b c } - the values put on the stack in between, as a new list */
PRIM(p_brace_open) {
    Task *t = f->t;

    if (t->nmarks >= 16) {
        forth_error(f, "{ nested too deep");
    }
    t->marks[t->nmarks++] = t->sp;
}
PRIM(p_brace_close) {
    Task *t = f->t;
    List *l;
    int i, from;

    if (t->nmarks == 0) {
        forth_error(f, "} without {");
    }
    from = t->marks[--t->nmarks];
    if (from > t->sp) {
        forth_error(f, "}: the stack lost items since {");
    }
    l = new_list(f);
    for (i = from; i < t->sp; i++) {
        list_add(f, l, t->ds[i]);   /* (an error here leaks l: out of memory, a lost list matters little) */
    }
    t->sp = from;
    PUSH(REF(l));
}

/* ---- a sequence walked by a combinator compiled inline (`[: ... ;] each`: lay_walk). The walk is cells on the
 * return stack (WALK_CELLS: forth_internal.h) - so the quotation runs on the data stack as its word left it. An item
 * was a lookup of the sequence each turn: 160 instructions an item, where a do loop's c@ is 23 - so a walk was the
 * slowest way to read a string. Now where the sequence was found is kept with the walk, and good while no object
 * has been let go since (the objects' count of those says): the item is read from there. Let go meanwhile, or
 * shorter, is an error still - found the slow way. ---- */

static Cell *walk_cells(Forth *f, int under) {   /* the walk's cells (under: cells a word keeps beneath them) */
    Task *t = f->t;

    if (t->rp < WALK_CELLS + under) {
        forth_error(f, "each: the list being walked is gone from the return stack");
    }
    return &t->rs[t->rp - WALK_CELLS];
}
static int walk_kind(int kind) { return kind == OBJ_LIST ? WALK_LIST : kind == OBJ_BYTES ? WALK_BYTES : WALK_STRING; }
static void walk_found(Forth *f, Cell *walk, const ObjectSlot *sl) {   /* where the sequence is, kept with its walk */
    walk[WALK_STAMP] = f->objects_let_go;
    walk[WALK_KIND] = walk_kind(sl->kind);
    if (sl->kind == OBJ_LIST) {
        List *l = sl->at;

        walk[WALK_DATA] = (Cell)&l->items;
        walk[WALK_COUNT] = (Cell)&l->n;
    } else if (sl->kind == OBJ_BYTES) {
        Bytes *b = sl->at;

        walk[WALK_DATA] = (Cell)&b->p;
        walk[WALK_COUNT] = (Cell)&b->n;
    } else {
        walk[WALK_DATA] = (Cell)seq_bytes(sl);
        walk[WALK_COUNT] = 0;
    }
}
/* the walk's item i, the sequence looked up: where it is kept for the items after. What is at its address must be
 * what the walk began on - its kind, and a string its length: let go, another object may have been made where it
 * was (the objects' blocks are used again at once), and the walk's cells say how to read the one it began on - a
 * list read as a string's bytes to the list's length, map's bytes written as a list's cells. */
static Cell walk_item_found(Forth *f, Cell *walk, Cell i) {
    ObjectSlot *sl = seq_slot(f, walk[WALK_SEQ], "each");

    if (walk_kind(sl->kind) != walk[WALK_KIND] ||
        (text_kind(sl->kind) && (Cell)sl->size != walk[WALK_LENGTH])) {
        forth_error(f, "each: the sequence was let go as it was walked (another object is where it was)");
    }
    if (i < 0 || i >= seq_length(sl)) {
        forth_error(f, "each: the sequence lost items as it was walked (item %ld of %ld)", (long)i,
                    (long)seq_length(sl));
    }
    walk_found(f, walk, sl);
    return seq_get(sl, i);
}
static inline Cell walk_item(Forth *f, Cell *walk, Cell i) {
    if (walk[WALK_STAMP] == f->objects_let_go && i >= 0) {   /* (no object let go since: it is where it was) */
        if (walk[WALK_KIND] == WALK_STRING) {
            if (i < walk[WALK_LENGTH]) {   /* (a string keeps its length) */
                return ((const uint8_t *)walk[WALK_DATA])[i];
            }
        } else if (i < *(const Cell *)walk[WALK_COUNT]) {
            return walk[WALK_KIND] == WALK_LIST ? (*(Cell *const *)walk[WALK_DATA])[i]
                                                : (*(uint8_t *const *)walk[WALK_DATA])[i];
        }
    }
    return walk_item_found(f, walk, i);
}
PRIM(p_each_begin) {   /* (each-begin) ( seq -- ) */
    Cell x = POP(), *walk;
    const ObjectSlot *sl = seq_slot(f, x, "each");
    int k;

    rpush(f, x);
    rpush(f, seq_length(sl));
    for (k = WALK_INDEX; k < WALK_CELLS; k++) {
        rpush(f, 0);
    }
    walk = walk_cells(f, 0);
    walk_found(f, walk, sl);
}
PRIM(p_each_next) {   /* (each-next) out ( -- x ), or - no more - to out; the index then past the length: none found */
    Cell *walk = walk_cells(f, 0);

    if (walk[WALK_INDEX] < walk[WALK_LENGTH]) {
        PUSH(walk_item(f, walk, walk[WALK_INDEX]));
        walk[WALK_INDEX]++;
        IP++;
    } else {
        walk[WALK_INDEX] = walk[WALK_LENGTH] + 1;
        IP = (Cell *)*IP;
    }
}
PRIM(p_each_item) {   /* (each-item) ( -- x ) the item (each-next) gave last */
    Cell *walk = walk_cells(f, 0);

    PUSH(walk_item(f, walk, walk[WALK_INDEX] - 1));
}
PRIM(p_each_keep) {   /* (each-keep) ( x -- ) onto what is gathered under the walk (map's, filter's: a list or bytes) */
    Cell *walk = walk_cells(f, 1), x = POP();
    ObjectSlot *sl = seq_slot(f, walk[-1], "map");

    seq_push(f, sl, x, "map");
    if (sl->kind == OBJ_LIST && walk[WALK_KIND] == WALK_LIST) {   /* (where it is: the items after are put there,
                                                                  *  with no call - it is the walk's own, let go by none) */
        List *l = sl->at;

        walk[WALK_KEEP_DATA] = (Cell)&l->items;
        walk[WALK_KEEP_COUNT] = (Cell)&l->n;
        walk[WALK_KEEP_ROOM] = (Cell)&l->cap;
    } else if (sl->kind == OBJ_BYTES && walk[WALK_KIND] != WALK_LIST) {
        Bytes *b = sl->at;

        walk[WALK_KEEP_DATA] = (Cell)&b->p;
        walk[WALK_KEEP_COUNT] = (Cell)&b->n;
        walk[WALK_KEEP_ROOM] = (Cell)&b->cap;
    }
}
PRIM(p_each_end) {
    (void)walk_cells(f, 0);
    f->t->rp -= WALK_CELLS;
}
PRIM(p_each_found) {   /* (each-found) ( -- x true | false ) the walk over: the item it stopped at, if it did */
    Cell *walk = walk_cells(f, 0);
    int found = walk[WALK_INDEX] <= walk[WALK_LENGTH];
    Cell x = found ? walk_item(f, walk, walk[WALK_INDEX] - 1) : 0;

    f->t->rp -= WALK_CELLS;
    if (found) {
        PUSH(x);
    }
    PUSH(found ? -1 : 0);
}

/* ---- tuples (Factor's): TUPLE: point x y ; - a class, its constructor <point> ( x y -- p ), its predicate point?,
 * and for each slot accessors in the vocabulary `accessors` (x>> ( p -- x ), >>x ( p x -- p ), change-x ( p xt -- p
 * )). A tuple is an object: its class, then a cell a slot. GENERIC: area ( shape -- n ) and M: circle area ... ;
 * dispatch on the class of the object on top, up its parents to `object`'s. ---- */

#define TUPLE_SLOTS_MAX 32
#define CONSTRUCTOR_EFFECT_BYTES 256   /* a constructor's stack comment, each stack's half */
#define CONSTRUCTOR_ROOM_KEPT (2 * TUPLE_SLOTS_MAX)   /* (in it, room for " x" for every slot: names until then) */

typedef struct TupleClass {
    char name[WORD_NAME_MAX + 1];
    struct TupleClass *parent;   /* NULL: `object`, the root */
    int nslots;
    int slot[TUPLE_SLOTS_MAX];   /* each slot's name (its index among the slots' names): the parent's first */
    struct TupleClass *next;     /* every class */
} TupleClass;

typedef struct SlotName {
    char name[WORD_NAME_MAX + 1];
    int is_float;                /* F: a float's (its accessors take and give floats) */
} SlotName;

typedef struct Method {
    TupleClass *cls;
    Word *xt;
    struct Method *next;
} Method;

typedef struct Generic {
    Method *methods;
    char effect[128];            /* its stack comment: each method's effect */
    char name[WORD_NAME_MAX + 1];
} Generic;

struct ForthTuples {
    TupleClass object;           /* the root: every class's last parent */
    TupleClass *classes;
    SlotName *names;
    int nnames, cap;
    Generic **generics;
    int ngenerics, gcap;
};

static struct ForthTuples *tuples(Forth *f) {
    if (f->tuples == NULL) {
        f->tuples = calloc(1, sizeof(struct ForthTuples));
        if (f->tuples == NULL) {
            forth_error(f, "tuples: out of memory");
        }
        snprintf(f->tuples->object.name, sizeof(f->tuples->object.name), "object");
    }
    return f->tuples;
}

static void tuples_free(Forth *f) {
    struct ForthTuples *t = f->tuples;
    int i;

    if (t == NULL) {
        return;
    }
    while (t->classes != NULL) {
        TupleClass *c = t->classes;

        t->classes = c->next;
        free(c);
    }
    for (i = 0; i < t->ngenerics; i++) {
        while (t->generics[i]->methods != NULL) {
            Method *m = t->generics[i]->methods;

            t->generics[i]->methods = m->next;
            free(m);
        }
        free(t->generics[i]);
    }
    free(t->generics);
    free(t->names);
    free(t);
    f->tuples = NULL;
}

static TupleClass *class_arg(Forth *f, Cell x, const char *word) {
    struct ForthTuples *t = tuples(f);
    TupleClass *c;

    if ((TupleClass *)x == &t->object) {
        return &t->object;
    }
    for (c = t->classes; c != NULL && (Cell)c != x; c = c->next) {
    }
    if (c == NULL) {
        forth_error(f, "%s: not a tuple class", word);
    }
    return c;
}

static Cell *tuple_arg(Forth *f, Cell x, const char *word) { return object_arg(f, x, OBJ_TUPLE, word); }

static int is_a(const TupleClass *c, const TupleClass *of) {   /* (`object` is every class's last parent) */
    for (; c != NULL; c = c->parent) {
        if (c == of) {
            return 1;
        }
    }
    return 0;
}

/* the slot's place in a tuple of class c (its cell after the class), -1 if it has none of that name */
static int slot_at(const TupleClass *c, int name) {
    int i;

    for (i = 0; i < c->nslots; i++) {
        if (c->slot[i] == name) {
            return i + 1;
        }
    }
    return -1;
}

static int slot_name(Forth *f, const char *s, size_t n, int is_float) {
    struct ForthTuples *t = tuples(f);
    int i;

    for (i = 0; i < t->nnames; i++) {
        if (strlen(t->names[i].name) == n && strncasecmp(t->names[i].name, s, n) == 0) {
            if (t->names[i].is_float != is_float) {
                forth_error(f, "TUPLE: slot %.*s is a %s elsewhere (its accessors take one kind)", (int)n, s,
                            t->names[i].is_float ? "float" : "cell");
            }
            return i;
        }
    }
    if (t->nnames == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        SlotName *names = realloc(t->names, (size_t)cap * sizeof(SlotName));

        if (names == NULL) {
            forth_error(f, "tuples: out of memory");
        }
        t->names = names;
        t->cap = cap;
    }
    snprintf(t->names[t->nnames].name, sizeof(t->names[0].name), "%.*s", (int)n, s);
    t->names[t->nnames].is_float = is_float;
    return -(++t->nnames);   /* (new: its accessors to be made) */
}

static Cell *tuple_slot(Forth *f, Word *w, Cell obj) {   /* the accessor w's slot in obj */
    Cell *t = tuple_arg(f, obj, w->name);
    TupleClass *c = (TupleClass *)t[0];
    int at = slot_at(c, (int)w->body[0]);

    if (at < 0) {
        forth_error(f, "%s: %s has no slot %s", w->name, c->name, f->tuples->names[w->body[0]].name);
    }
    return &t[at];
}
static void doreader(Forth *f, Word *w) {   /* x>> ( obj -- x ) */
    Cell *slot = tuple_slot(f, w, POP());

    if (f->tuples->names[w->body[0]].is_float) {
        Float x;

        memcpy(&x, slot, sizeof(x));
        FPUSH(x);
    } else {
        PUSH(*slot);
    }
}
static void dowriter(Forth *f, Word *w) {   /* >>x ( obj x -- obj ) */
    if (f->tuples->names[w->body[0]].is_float) {
        Float x = FPOP();

        memcpy(tuple_slot(f, w, *top(f, 1)), &x, sizeof(x));
    } else {
        Cell x = POP();

        *tuple_slot(f, w, *top(f, 1)) = x;
    }
}
static void dochanger(Forth *f, Word *w) {   /* change-x ( obj xt -- obj ): its value through xt ( x -- x' ) */
    Word *x = forth_xt_arg(f, POP(), w->name);
    Cell obj = *top(f, 1);
    int is_float = f->tuples->names[w->body[0]].is_float;
    Cell *slot = tuple_slot(f, w, obj);
    Float v;

    if (is_float) {
        memcpy(&v, slot, sizeof(v));
        FPUSH(v);
    } else {
        PUSH(*slot);
    }
    forth_execute(f, x);
    slot = tuple_slot(f, w, obj);   /* (xt may have done anything: looked up again) */
    if (is_float) {
        v = FPOP();
        memcpy(slot, &v, sizeof(v));
    } else {
        *slot = POP();
    }
}
static void doconstructor(Forth *f, Word *w) {   /* <point> ( slots -- obj ): its slots in order, floats' from F: */
    TupleClass *c = (TupleClass *)w->body[0];
    Cell *t;
    int i, cells = 0, floats = 0;

    for (i = 0; i < c->nslots; i++) {   /* (its slots there first: an underflow makes nothing) */
        f->tuples->names[c->slot[i]].is_float ? floats++ : cells++;
    }
    if (f->t->sp - f->t->sfloor < cells) {
        forth_error(f, "stack underflow");
    }
    if (f->t->fp - f->t->ffloor < floats) {
        forth_error(f, "float stack underflow");
    }
    t = object_new(f, OBJ_TUPLE, (size_t)(c->nslots + 1) * sizeof(Cell));
    t[0] = (Cell)c;
    for (i = c->nslots - 1; i >= 0; i--) {   /* (the last on top) */
        if (f->tuples->names[c->slot[i]].is_float) {
            Float x = FPOP();

            memcpy(&t[i + 1], &x, sizeof(x));
        } else {
            t[i + 1] = POP();
        }
    }
    PUSH(REF(t));
}
static void dopredicate(Forth *f, Word *w) {   /* point? ( x -- flag ) */
    ObjectSlot *sl = object_of(f, POP());

    PUSH(sl != NULL && sl->kind == OBJ_TUPLE && is_a((TupleClass *)((Cell *)sl->at)[0], (TupleClass *)w->body[0]) ? -1
                                                                                                                  : 0);
}

/* a word in the vocabulary `accessors` (made once for each slot's name) */
static void accessor(Forth *f, const char *fmt, const char *name, Code code, int slot, const char *effect) {
    char buf[WORD_NAME_MAX + 16];
    Word *w;

    snprintf(buf, sizeof(buf), fmt, name);
    if (strlen(buf) > WORD_NAME_MAX) {
        forth_error(f, "TUPLE: %s: a name too long for its accessors", name);
    }
    w = forth_new_word(f, buf, strlen(buf), code);
    forth_comma(f, slot);
    if (effect != NULL) {
        forth_set_effect(f, w, effect);
    } else {
        w->effect.kind = EFFECT_HIGHER;
        w->effect_tried = 1;
    }
}

/* TUPLE: name [< parent] slot ... [F: slot ...] ; */
PRIM(p_tuple) {
    struct ForthTuples *t = tuples(f);
    size_t len, m;
    const char *name = forth_parse_required(f, &len, "TUPLE:");
    TupleClass *c = calloc(1, sizeof(TupleClass));
    char buf[WORD_NAME_MAX + 8], effect[CONSTRUCTOR_EFFECT_BYTES], feffect[CONSTRUCTOR_EFFECT_BYTES];
    int is_float = 0, i, k, din = 0, fin = 0;
    Vocab *current = f->m.current;
    Word *made;

    if (c == NULL) {
        forth_error(f, "TUPLE: out of memory");
    }
    if (len + 2 > WORD_NAME_MAX) {
        free(c);
        forth_error(f, "TUPLE: name too long: %.*s", (int)len, name);
    }
    snprintf(c->name, sizeof(c->name), "%.*s", (int)len, name);
    c->parent = &t->object;
    c->next = t->classes;
    t->classes = c;
    for (;;) {
        const char *s = forth_parse_name(f, &m);

        if (m == 0) {
            forth_error(f, "TUPLE: %s without ;", c->name);
        }
        if (m == 1 && s[0] == ';') {
            break;
        }
        if (m == 1 && s[0] == '\\') {   /* comments, as enum: has them */
            (void)forth_parse_until(f, '\n', &m);
            forth_source(f)->line++;
            continue;
        }
        if (m == 1 && s[0] == '(') {
            (void)forth_parse_until(f, ')', &m);
            continue;
        }
        if (m == 1 && s[0] == '<' && c->nslots == 0 && c->parent == &t->object) {   /* < parent: its slots first */
            Word *pw = forth_parse_find(f, "TUPLE: <");

            if (forth_code_of(pw) != CODE_CONSTANT) {
                forth_error(f, "TUPLE: %s < %s: not a tuple class", c->name, pw->name);
            }
            c->parent = class_arg(f, pw->body[0], "TUPLE: <");
            memcpy(c->slot, c->parent->slot, sizeof(c->slot));
            c->nslots = c->parent->nslots;
            continue;
        }
        if (m == 2 && strncasecmp(s, "F:", 2) == 0) {
            is_float = 1;
            continue;
        }
        if (c->nslots == TUPLE_SLOTS_MAX) {
            forth_error(f, "TUPLE: %s has too many slots", c->name);
        }
        k = slot_name(f, s, m, is_float);
        if (k < 0) {   /* a name new to the slots: its accessors */
            const char *sn;

            k = -k - 1;
            sn = t->names[k].name;
            f->m.current = forth_vocab(f, "accessors");   /* (not used by this file for it: USING: accessors) */
            f->m.current->state = VOCAB_LOADED;
            accessor(f, "%s>>", sn, doreader, k, is_float ? "( obj -- ) ( F: -- F: x )" : "( obj -- x )");
            accessor(f, ">>%s", sn, dowriter, k, is_float ? "( obj -- obj ) ( F: x -- )" : "( obj x -- obj )");
            accessor(f, "change-%s", sn, dochanger, k, NULL);
            f->m.current = current;
        }
        if (slot_at(c, k) >= 0) {
            forth_error(f, "TUPLE: %s has two slots %.*s", c->name, (int)m, s);
        }
        c->slot[c->nslots++] = k;
    }
    {   /* defined again in its vocabulary as it was (a reload): the same class - its tuples, its methods kept */
        Word *old = forth_in_vocab(current, c->name, strlen(c->name));
        TupleClass *was = old != NULL && forth_code_of(old) == CODE_CONSTANT ? (TupleClass *)old->body[0] : NULL;
        TupleClass *k2;

        for (k2 = c->next; was != NULL && k2 != NULL && k2 != was; k2 = k2->next) {
        }
        if (was != NULL && k2 == was && was->parent == c->parent && was->nslots == c->nslots &&
            memcmp(was->slot, c->slot, (size_t)c->nslots * sizeof(c->slot[0])) == 0) {
            t->classes = c->next;
            free(c);
            c = was;
        }
    }
    forth_constant(f, c->name, (Cell)c);   /* the class */
    effect[0] = feffect[0] = 0;
    for (i = 0; i < c->nslots; i++) {   /* the constructor's effect: its slots in, each on its stack */
        char *e = t->names[c->slot[i]].is_float ? feffect : effect;
        size_t at = strlen(e), cap = t->names[c->slot[i]].is_float ? sizeof(feffect) : sizeof(effect);
        const char *name = t->names[c->slot[i]].name;

        /* (by its name - or, past the room for names, `x`: the count is what is checked, never cut short) */
        snprintf(e + at, cap - at, " %s", at + strlen(name) + 1 + CONSTRUCTOR_ROOM_KEPT < cap ? name : "x");
        t->names[c->slot[i]].is_float ? fin++ : din++;
    }
    snprintf(buf, sizeof(buf), "<%s>", c->name);
    made = forth_new_word(f, buf, strlen(buf), doconstructor);
    forth_comma(f, (Cell)c);
    {
        char full[2 * CONSTRUCTOR_EFFECT_BYTES + 32];

        snprintf(full, sizeof(full), "(%s -- obj )%s%s%s", effect, fin > 0 ? " ( F:" : "", feffect, fin > 0 ? " -- )" : "");
        forth_set_effect(f, made, full);
    }
    (void)din;
    snprintf(buf, sizeof(buf), "%s?", c->name);
    made = forth_new_word(f, buf, strlen(buf), dopredicate);
    forth_comma(f, (Cell)c);
    forth_set_effect(f, made, "( x -- flag )");
}

static void dogeneric(Forth *f, Word *w) {   /* the method for the class of the tuple on top (or its parents') */
    Generic *g = (Generic *)w->body[0];
    Cell *t = tuple_arg(f, *top(f, 1), g->name);
    TupleClass *c;
    Method *m;

    for (c = (TupleClass *)t[0]; c != NULL; c = c->parent) {
        for (m = g->methods; m != NULL; m = m->next) {
            if (m->cls == c) {
                m->xt->code(f, m->xt);   /* (in its place, as a deferred word runs its word) */
                return;
            }
        }
    }
    forth_error(f, "%s: no method for %s", g->name, ((TupleClass *)t[0])->name);
}

/* GENERIC: name ( stack comment ) - a word whose method is the class's of the tuple on top */
/* a generic defined again in its vocabulary (a reload): the same one, its methods from other vocabularies kept - its
 * own vocabulary's let go, to be defined again with it (NULL: not one) */
static Generic *generic_again(Forth *f, const char *name, size_t len) {
    Word *old = forth_in_vocab(f->m.current, name, len);
    Generic *g;
    Method **m;

    if (old == NULL || old->code != dogeneric) {
        return NULL;
    }
    g = (Generic *)old->body[0];
    for (m = &g->methods; *m != NULL;) {
        Vocab *v = (*m)->xt->vocab;

        if (v == f->m.current || v->parent == f->m.current) {   /* (its own, or its <PRIVATE part's) */
            Method *gone = *m;

            *m = gone->next;
            free(gone);
        } else {
            m = &(*m)->next;
        }
    }
    g->effect[0] = 0;
    return g;
}

static Generic *generic_new(Forth *f, const char *name, size_t len) {
    struct ForthTuples *t = tuples(f);
    Generic *g = calloc(1, sizeof(Generic));

    if (g == NULL) {
        forth_error(f, "GENERIC: out of memory");
    }
    if (t->ngenerics == t->gcap) {
        int cap = t->gcap ? t->gcap * 2 : 32;
        Generic **gs = realloc(t->generics, (size_t)cap * sizeof(Generic *));

        if (gs == NULL) {
            free(g);
            forth_error(f, "GENERIC: out of memory");
        }
        t->generics = gs;
        t->gcap = cap;
    }
    t->generics[t->ngenerics++] = g;
    snprintf(g->name, sizeof(g->name), "%.*s", (int)len, name);
    return g;
}

PRIM(p_generic) {
    size_t len, m;
    const char *name = forth_parse_required(f, &len, "GENERIC:"), *s;
    Generic *g = generic_again(f, name, len);
    Word *made;

    if (g == NULL) {
        g = generic_new(f, name, len);
    }
    made = forth_new_word(f, name, len, dogeneric);
    forth_comma(f, (Cell)g);
    {   /* its stack comment, if one follows: its effect, and its methods' */
        Source *src = forth_source(f);
        size_t pos = src->pos;

        s = forth_parse_name(f, &m);
        if (m == 1 && s[0] == '(') {
            s = forth_parse_until(f, ')', &m);
            while (m > 0 && isspace((unsigned char)s[m - 1])) {
                m--;
            }
            snprintf(g->effect, sizeof(g->effect), "( %.*s )", (int)m, s);
            if (forth_set_effect(f, made, g->effect) != 0) {
                g->effect[0] = 0;
            }
        } else {
            src->pos = pos;
        }
    }
}

/* M: class generic ... ; - the generic's method for that class (and its children, but those with their own) */
PRIM(p_method) {
    Word *cw = forth_parse_find(f, "M:"), *gw = forth_parse_find(f, "M:"), *def;
    TupleClass *c;
    Generic *g;
    Method *m;
    char name[2 * WORD_NAME_MAX + 4], why[200];

    if (forth_code_of(cw) != CODE_CONSTANT) {
        forth_error(f, "M: %s: not a tuple class", cw->name);
    }
    c = class_arg(f, cw->body[0], "M:");
    if (gw->code != dogeneric) {
        forth_error(f, "M: %s: not a generic word", gw->name);
    }
    g = (Generic *)gw->body[0];
    forth_execute(f, forth_find(f, ":noname", 7));
    def = (Word *)POP();
    f->top_sp = f->t->sp;
    snprintf(name, sizeof(name), "%s:%s", c->name, g->name);   /* (its name, for the call chain: not findable) */
    memcpy(def->name, name, WORD_NAME_MAX);
    def->name[WORD_NAME_MAX] = 0;
    for (m = g->methods; m != NULL && m->cls != c; m = m->next) {
    }
    if (m == NULL) {
        if ((m = calloc(1, sizeof(Method))) == NULL) {
            forth_error(f, "M: out of memory");
        }
        m->cls = c;
        m->next = g->methods;
        g->methods = m;
    }
    m->xt = def;   /* (again: a script reloaded, the newer) */
    if (g->effect[0]) {
        snprintf(why, sizeof(why), "%s's is %s", g->name, g->effect);
        forth_declare_effect(f, g->effect, why);
    }
}

PRIM(p_class_of) { PUSH(tuple_arg(f, POP(), "class-of")[0]); }   /* ( obj -- class ) */
PRIM(p_instance) {   /* ( x class -- flag ) a tuple of that class, or of one of its children */
    TupleClass *c = class_arg(f, POP(), "instance?");
    ObjectSlot *sl = object_of(f, POP());

    PUSH(sl != NULL && sl->kind == OBJ_TUPLE && is_a((TupleClass *)((Cell *)sl->at)[0], c) ? -1 : 0);
}
PRIM(p_dot_tuple) {   /* ( obj -- ) its class and each slot */
    Cell *t = tuple_arg(f, POP(), ".tuple");
    TupleClass *c = (TupleClass *)t[0];
    int i;

    forth_printf(f, "%s", c->name);
    for (i = 0; i < c->nslots; i++) {
        const SlotName *sn = &f->tuples->names[c->slot[i]];

        if (sn->is_float) {
            Float x;

            memcpy(&x, &t[i + 1], sizeof(x));
            forth_printf(f, " %s: %g", sn->name, x);
        } else {
            forth_printf(f, " %s: %s", sn->name, forth_format_number(f, t[i + 1], 0));
        }
    }
    forth_type(f, " ", 1);
}
static void *clone_container(Forth *f, int kind, void *from);

PRIM(p_clone) {   /* ( obj -- obj' ) a copy, in this region: a list's items, a tuple's slots, a string's bytes */
    ObjectSlot *sl = object_of(f, POP());
    void *from, *copy;
    size_t size;
    int kind;
    Cell i;

    if (sl == NULL) {
        forth_error(f, "clone: not an object");
    }
    from = sl->at;   /* (taken now: a new object may move the table) */
    size = sl->size;
    kind = sl->kind;
    if (kind == OBJ_STRING) {
        copy = string_new(f, (const char *)from, size);
    } else if (kind == OBJ_SLICE) {   /* (a copy of a piece: a string of its own - it lasts whatever the other does) */
        const uint8_t *bytes = slice_bytes(f, sl);

        if (bytes == NULL) {
            forth_error(f, "clone: a slice of a string that was let go");
        }
        copy = string_new(f, (const char *)bytes, size);
    } else if (kind == OBJ_LIST) {
        copy = new_list(f);
        for (i = 0; i < ((List *)from)->n; i++) {
            list_add(f, copy, ((List *)from)->items[i]);
        }
    } else if (kind == OBJ_MAP || kind == OBJ_BYTES) {
        copy = clone_container(f, kind, from);
    } else {
        copy = object_new(f, kind, size);
        memcpy(copy, from, size);
    }
    PUSH(REF(copy));
}

/* ---- strings: ( addr len ) as ever; those these words make are objects (their addr), in the region's - freed by
 * `dispose`, or the scratch region's end. A list of strings holds the objects: `string` gives one's addr len. ---- */

static void push_string(Forth *f, const char *s, size_t len) {
    char *p = string_new(f, s, len);

    PUSH(REF(p));
    PUSH(len);
}
PRIM(p_to_string) {   /* >string ( addr len -- s len ) a copy that lasts (an object) */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, ">string", &len);

    push_string(f, s, len);
}
PRIM(p_string) {   /* string ( s -- s len ) a string object as text: itself, and its length */
    Cell x = POP();

    if (object_of(f, x) == NULL || object_of(f, x)->kind != OBJ_SLICE) {   /* (a slice: text as a string is) */
        (void)object_arg(f, x, OBJ_STRING, "string");
    }
    PUSH(x);   /* (its reference is text wherever text is taken, and what dispose lets go; s-bytes: its address) */
    PUSH(object_of(f, x)->size);
}
/* s-bytes ( a n -- addr n ) text's bytes, by their address: for the words that read memory (c@, move). Text is
 * `a n` - and a is an address, or a string's (a byte buffer's) reference: what `string` and every word that makes
 * a string give. The reference is no address; text by its address already is given back as it is. (The address is
 * the bytes' while the string lives, and a buffer's while it doesn't grow.) */
PRIM(p_s_bytes) {
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, "s-bytes", &len);

    PUSH(len > 0 || FORTH_IS_REF(a) ? (Cell)s : a);
    PUSH(n);
}
static void push_kind_q(Forth *f, int kind) {   /* ( x -- flag ) an object of that kind */
    ObjectSlot *sl = object_of(f, POP());

    PUSH(sl != NULL && sl->kind == kind ? -1 : 0);
}
PRIM(p_string_q) {   /* string? ( x -- flag ) a string - or a slice: a piece of one is one to every word that reads */
    const ObjectSlot *sl = object_of(f, POP());

    PUSH(sl != NULL && text_kind(sl->kind) ? -1 : 0);
}
PRIM(p_slice_q) { push_kind_q(f, OBJ_SLICE); }   /* slice? ( x -- flag ) */
PRIM(p_list_q) { push_kind_q(f, OBJ_LIST); }
PRIM(p_map_q) { push_kind_q(f, OBJ_MAP); }
PRIM(p_bytes_q) { push_kind_q(f, OBJ_BYTES); }
PRIM(p_s_plus) {   /* s+ ( a1 n1 a2 n2 -- a n ) the two together, a new string */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2;
    const char *s1 = text_at(f, a1, n1, "s+", &l1), *s2 = text_at(f, a2, n2, "s+", &l2);
    char *p = string_alloc(f, l1 + l2);

    memcpy(p, s1, l1);
    memcpy(p + l1, s2, l2);
    PUSH(REF(p));
    PUSH(l1 + l2);
}
PRIM(p_n_to_s) {   /* n>s ( n -- a n ) its digits in the base, a new string */
    const char *d = forth_format_number(f, POP(), 0);

    push_string(f, d, strlen(d));
}
PRIM(p_f_to_string) {   /* f>string ( F: x -- ) ( -- a n ) as f. prints it, a new string */
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", FPOP());

    push_string(f, buf, (size_t)n);
}
/* f>exact ( F: x -- ) ( -- a n ) the fewest digits that read back as the same x (15, else 17): what JSON and a page
 * write - a price, a time - where f>string's 6 would change it; a new string */
PRIM(p_f_to_exact) {
    char buf[64];
    double x = FPOP();
    int n = snprintf(buf, sizeof(buf), "%.15g", x);

    if (strtod(buf, NULL) != x) {
        n = snprintf(buf, sizeof(buf), "%.17g", x);
    }
    push_string(f, buf, (size_t)n);
}
PRIM(p_string_to_number) {   /* string>number ( a n -- n flag ) a whole number in the base ($ # % too); 0 false if not */
    Cell n = POP(), a = POP(), x = 0;
    size_t len;
    const char *s = text_at(f, a, n, "string>number", &len);
    int ok = len > 0 && forth_parse_int(f, s, len, &x) && !FORTH_IS_REF(x);   /* (no number read from text has a
                                                                             *  reference's mark: objects.c, References) */

    PUSH(ok ? x : 0);
    PUSH(ok ? -1 : 0);
}
PRIM(p_string_to_float) {   /* string>float ( a n -- flag ) ( F: -- x ) a float (1.5 2e): 0e false if not */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, "string>float", &len);
    Float x = 0;
    int ok = len > 0 && forth_parse_float(s, len, &x);

    FPUSH(ok ? x : 0.0);
    PUSH(ok ? -1 : 0);
}
/* t in s, from `from`: where, or n. Its first byte is looked for (memchr), the rest compared only where that is -
 * compared at every byte, a request's lines found were a tenth of its time. */
static size_t find_in(const char *s, size_t n, const char *t, size_t m, size_t from) {
    size_t i = from;
    const char *p;

    if (m == 0) {
        return from <= n ? from : n;
    }
    while (m <= n && i + m <= n && (p = memchr(s + i, t[0], n - m - i + 1)) != NULL) {
        i = (size_t)(p - s);
        if (memcmp(p, t, m) == 0) {
            return i;
        }
        i++;
    }
    return n;
}
PRIM(p_s_find) {   /* s-find ( a n a2 n2 -- i ) where the second is in the first, -1 if not */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2, at;
    const char *s1 = text_at(f, a1, n1, "s-find", &l1), *s2 = text_at(f, a2, n2, "s-find", &l2);

    at = l2 == 0 ? 0 : find_in(s1, l1, s2, l2, 0);   /* (nothing is found at the start) */
    forth_worked(f, at, WORK_FAST);
    PUSH(l2 == 0 || at < l1 ? (Cell)at : -1);
}
PRIM(p_s_under) {   /* s-under ( a n c -- i ) where its first byte under c is, -1 if none (c 32: a control character) */
    Cell c = POP(), n = POP(), a = POP();
    size_t len, i;
    const uint8_t *s = (const uint8_t *)text_at(f, a, n, "s-under", &len);

    for (i = 0; i < len && (Cell)s[i] >= c; i++) {
    }
    forth_worked(f, i, WORK_BYTES);
    PUSH(i < len ? (Cell)i : -1);
}
#define BYTE_VALUES 256   /* a table with an entry for each byte there can be */

/* s-marked ( a n table -- i ) where its first byte marked in the table is - n if none. The table: 256 bytes, one for
 * each byte there can be, not 0 for those looked for. What writing text out escaped needs - JSON's quotes and
 * control characters, HTML's & < > - is the runs between such bytes: found here, not a byte at a time in Forth. */
PRIM(p_s_marked) {
    Cell table = POP(), n = POP(), a = POP();
    size_t len, i;
    const uint8_t *s = (const uint8_t *)text_at(f, a, n, "s-marked", &len);
    const uint8_t *marked = forth_addr_n(f, table, BYTE_VALUES);

    for (i = 0; i < len && !marked[s[i]]; i++) {
    }
    forth_worked(f, i, WORK_BYTES);
    PUSH((Cell)i);
}
/* crlf-lines? ( a n -- flag ) its lines end CR LF and nothing else: no CR but before an LF, no LF but after a CR, and
 * no NUL - a request's head as every server and proxy between reads it the same (http's check, each request) */
PRIM(p_crlf_lines_q) {
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, "crlf-lines?", &len), *end = s + len, *p;
    int clean = memchr(s, 0, len) == NULL;

    forth_worked(f, len, WORK_FAST);
    for (p = s; clean && (p = memchr(p, '\n', (size_t)(end - p))) != NULL; p++) {
        clean = p > s && p[-1] == '\r';
    }
    for (p = s; clean && (p = memchr(p, '\r', (size_t)(end - p))) != NULL; p++) {
        clean = p + 1 < end && p[1] == '\n';
    }
    PUSH(clean ? -1 : 0);
}
PRIM(p_s_slice) {   /* s-slice ( a n i len -- a' n' ) its len bytes from i (kept to the string): no copy */
    Cell k = POP(), i = POP(), n = POP(), a = POP();

    if (n < 0) {
        forth_error(f, "s-slice: a negative length (%ld)", (long)n);
    }
    if (FORTH_IS_REF(a)) {   /* (a string by its reference: the slice is of its bytes, by their address) */
        size_t len;

        a = (Cell)text_at(f, a, n, "s-slice", &len);
    }
    i = i < 0 ? 0 : i > n ? n : i;
    k = k < 0 ? 0 : k > n - i ? n - i : k;
    PUSH((Cell)((UCell)a + (UCell)i));
    PUSH(k);
}
PRIM(p_s_trim) {   /* s-trim ( a n -- a' n' ) without the spaces at its ends: no copy */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, "s-trim", &len);

    while (len > 0 && isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        len--;
    }
    PUSH(len > 0 || FORTH_IS_REF(a) ? (Cell)s : a);   /* (by its address: a piece of a string is no string) */
    PUSH(len);
}
static int ends_with(Forth *f, int at_start, const char *word) {   /* s-starts? s-ends? ( a n a2 n2 -- flag ) */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2;
    const char *s1 = text_at(f, a1, n1, word, &l1), *s2 = text_at(f, a2, n2, word, &l2);

    return l2 <= l1 && memcmp(at_start ? s1 : s1 + l1 - l2, s2, l2) == 0;
}
PRIM(p_s_starts) { PUSH(ends_with(f, 1, "s-starts?") ? -1 : 0); }
PRIM(p_s_ends) { PUSH(ends_with(f, 0, "s-ends?") ? -1 : 0); }
static void cased(Forth *f, int upper, const char *word) {   /* s-upper s-lower ( a n -- a' n ) a new string */
    Cell n = POP(), a = POP();
    size_t len, i;
    const char *s = text_at(f, a, n, word, &len);
    char *p = string_new(f, s, len);

    forth_worked(f, len, WORK_BYTES);
    for (i = 0; i < len; i++) {
        p[i] = (char)(upper ? toupper((unsigned char)p[i]) : tolower((unsigned char)p[i]));
    }
    PUSH(REF(p));
    PUSH(len);
}
PRIM(p_s_upper) { cased(f, 1, "s-upper"); }
PRIM(p_s_lower) { cased(f, 0, "s-lower"); }
PRIM(p_s_split) {   /* s-split ( a n a2 n2 -- l ) the pieces between each of the second: a list of strings */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2, at = 0, next;
    const char *s1 = text_at(f, a1, n1, "s-split", &l1), *s2 = text_at(f, a2, n2, "s-split", &l2);
    List *l = new_list(f);

    if (l2 == 0) {
        forth_error(f, "s-split: nothing to split at");
    }
    for (;;) {
        next = find_in(s1, l1, s2, l2, at);
        list_add(f, l, REF(string_new(f, s1 + at, next - at)));
        if (next >= l1) {
            break;
        }
        at = next + l2;
    }
    forth_worked(f, l1, WORK_FAST);
    PUSH(REF(l));
}
PRIM(p_s_join) {   /* s-join ( l a n -- a' n' ) a list of strings, the second between each, a new string */
    Cell n = POP(), a = POP();
    List *l = list_arg(f, POP());
    size_t sep, total = 0, at = 0;
    const char *s = text_at(f, a, n, "s-join", &sep);
    char *p;
    Cell i;

    for (i = 0; i < l->n; i++) {
        object_arg(f, l->items[i], OBJ_STRING, "s-join");
        total += object_of(f, l->items[i])->size + (i > 0 ? sep : 0);
    }
    p = string_alloc(f, total);
    for (i = 0; i < l->n; i++) {
        ObjectSlot *sl = object_of(f, l->items[i]);

        if (i > 0) {
            memcpy(p + at, s, sep);
            at += sep;
        }
        memcpy(p + at, sl->at, sl->size);
        at += sl->size;
    }
    PUSH(REF(p));
    PUSH(total);
}

/* ---- objects moved between regions: rescued, sent in a message, delivered ---- */

/* each cell an object holds that may be an object too: a list's items, a tuple's cell slots, a map's values and keys
 * (not its own string keys: the map's, kept with it). `each` is called with each's record. */
static void object_contents(Forth *f, const ObjectSlot *sl, void (*each)(Forth *f, ObjectSlot *c, void *ctx),
                            void *ctx) {
    ObjectSlot *c;

    if (sl->kind == OBJ_LIST) {
        const List *l = sl->at;
        Cell i;

        for (i = 0; i < l->n; i++) {
            if ((c = object_of(f, l->items[i])) != NULL) {
                each(f, c, ctx);
            }
        }
    } else if (sl->kind == OBJ_TUPLE) {
        const Cell *t = sl->at;
        const TupleClass *cls = (const TupleClass *)t[0];
        int i;

        for (i = 0; i < cls->nslots; i++) {
            if (!f->tuples->names[cls->slot[i]].is_float && (c = object_of(f, t[i + 1])) != NULL) {
                each(f, c, ctx);
            }
        }
    } else if (sl->kind == OBJ_SLICE) {   /* (the string it is a piece of: kept, or sent, with it) */
        if ((c = object_of(f, ((const Slice *)sl->at)->base)) != NULL) {
            each(f, c, ctx);
        }
    } else if (sl->kind == OBJ_MAP) {
        const Map *m = sl->at;
        Cell i;

        for (i = 0; i < m->n; i++) {
            if (!m->e[i].live) {
                continue;
            }
            if (!m->e[i].text && (c = object_of(f, m->e[i].key)) != NULL) {
                each(f, c, ctx);
            }
            if ((c = object_of(f, m->e[i].value)) != NULL) {
                each(f, c, ctx);
            }
        }
    }
}

typedef struct Moving {
    int to, from, depth;
    int breathing;   /* by its task's own word (rescue): a long move counted against the slice, giving way */
} Moving;
static void object_move_as(Forth *f, ObjectSlot *sl, const Moving *as);

static void move_content(Forth *f, ObjectSlot *c, void *ctx) {
    const Moving *m = ctx;
    const struct ForthObjects *o = f->objects;

    if (c->region == m->to || c->region == 0) {
        return;
    }
    if (c->pending > 0 && c->region == o->post) {   /* (another message carries it too: that one's arrival moves it) */
        return;
    }
    if (m->from == 0 ? transient(o, c->region) : region_within(o, c->region, m->from)) {
        Moving in = *m;

        in.depth++;
        object_move_as(f, c, &in);
    }
}

/* sl into region `to`, and what it holds from region `from` (or inside it; 0: from any region that ends) */
static void object_move_as(Forth *f, ObjectSlot *sl, const Moving *m) {
    struct ForthObjects *o = f->objects;

    if (m->depth > OBJECT_DEPTH_MAX) {
        forth_error(f, "objects inside objects deeper than %d, moved with one", OBJECT_DEPTH_MAX);
    }
    region_unlist(o, sl);
    sl->region = m->to;   /* (first: a list holding itself is moved already as its items are looked at) */
    if (m->to > 0) {
        region_note(f, sl);
    }
    object_contents(f, sl, move_content, (void *)(uintptr_t)m);
    /* (a tree of a hundred thousand objects takes ten milliseconds to move: counted, and the task gives way - here,
     *  with nothing of the table's held: others' turns may grow it. What is half moved is the task's own) */
    if (m->breathing && ++o->moved % BREATH_OBJECTS == 0 && forth_give_way(f, forth_work_steps(&o->moving_since))) {
        (void)forth_work_steps(&o->moving_since);   /* (others have had turns: their time isn't this work's) */
    }
}
static void object_move(Forth *f, ObjectSlot *sl, int to, int from, int depth) {
    Moving m = {to, from, depth, 0};

    object_move_as(f, sl, &m);
}
/* ... by the running task's own word (rescue, rescue-all, rescue-actor): a long move counted against its slice, and
 * giving way in its middle - with what is the task's own, a scratch region of its handler's: nobody else has it. One
 * an actor keeps, sent here by its address, is its owner's to let go at any turn: moved as others had turns, the move
 * went on in what the owner had freed. That one is moved in a step. */
static void object_move_by_task(Forth *f, ObjectSlot *sl, int to, int from) {
    const struct ForthObjects *o = f->objects;
    Moving m = {to, from, 0, 0};

    m.breathing = sl->region > 0 && !o->regions[sl->region].lasting && region_within(o, f->t->region, sl->region);
    f->objects->moving_since = 0;   /* (the clock first looked at after the first few: most are no more) */
    object_move_as(f, sl, &m);
}

static void free_content(Forth *f, ObjectSlot *c, void *ctx) {
    const Moving *m = ctx;

    if (c->region == m->from && !(c->pending > 0 && c->region == f->objects->post)) {   /* (not one another message
                                                                                        * carries: its arrival) */
        object_free_deep(f, c, m->from, m->depth + 1);
    }
}

/* sl let go, and what it holds in that region (a message's objects, never delivered) */
static void object_free_deep(Forth *f, ObjectSlot *sl, int region, int depth) {
    Moving m = {0, region, depth, 0};

    if (depth > OBJECT_DEPTH_MAX) {
        return;   /* (what is deeper stays: freed with the Forth) */
    }
    region_unlist(f->objects, sl);
    sl->region = 0;   /* (first: one holding itself isn't freed twice) */
    object_contents(f, sl, free_content, &m);
    object_free(f, sl);
}

void forth_objects_post(Forth *f, const Cell *args, int n) {
    int i;

    for (i = 0; i < n; i++) {
        ObjectSlot *sl = object_of(f, args[i]);

        if (sl == NULL || sl->owned || sl->permanent || !transient(f->objects, sl->region)) {   /* (kept - the system's, an actor's,
                                                                                * a map's key: as it is) */
            continue;
        }
        if (sl->region != f->objects->post) {
            object_move(f, sl, post_region(f), 0, 0);
        }
        sl->pending++;
    }
}

/* a message's objects delivered (to) or not (to < 0: let go): each in the post, its last message, goes into region
 * `to` (or is freed) - one taken out of the post meanwhile (rescued by an earlier receiver) stays where it is */
static void objects_arrive(Forth *f, const Cell *args, int n, int to) {
    int i;

    for (i = 0; i < n; i++) {
        ObjectSlot *sl = object_of(f, args[i]);

        if (sl == NULL || sl->pending == 0 || --sl->pending > 0 || sl->region != f->objects->post) {
            continue;
        }
        if (to < 0) {
            object_free_deep(f, sl, f->objects->post, 0);
        } else {
            object_move(f, sl, to, f->objects->post, 0);
        }
    }
}
void forth_objects_take(Forth *f, Task *t, const Cell *args, int n) {
    int i;

    for (i = 0; i < n; i++) {   /* (any object there: the handler's region opened for it) */
        if ((UCell)args[i] >= LOWEST_ADDRESS && object_of(f, args[i]) != NULL) {
            objects_arrive(f, args, n, task_region(f, t));
            return;
        }
    }
}
void forth_objects_drop(Forth *f, const Cell *args, int n) { objects_arrive(f, args, n, -1); }

/* x moved into region `to` (0: the system's), with what it holds from any region that ends; an error if it is no object */
void forth_object_keep(Forth *f, Cell x, int to, const char *word) {
    ObjectSlot *sl = object_of(f, x);

    if (sl == NULL) {
        forth_error(f, "%s: not an object: %ld", word, (long)x);
    }
    if (sl->region != to && !sl->owned && !sl->permanent && (sl->region == 0 || transient(f->objects, sl->region))) {
        object_move_by_task(f, sl, to, 0);   /* (not a map's key: its map's, freed with it) */
    }
}

/* x as it reads in a listing, if it is an object: a string "in quotes" (cut short ...), list(3), map(2), bytes(10),
 * a tuple by its class name{slots}; 0 if it isn't one */
int forth_object_describe(Forth *f, Cell x, char *buf, size_t n) {
    const ObjectSlot *sl = object_of(f, x);

    if (sl == NULL || n == 0) {
        return 0;
    }
    switch (sl->kind) {
    case OBJ_STRING: case OBJ_SLICE: {
        int room = (int)n - 6;   /* (the quotes, an ellipsis, the end) */
        int len = (int)sl->size < room ? (int)sl->size : room;
        const char *text = sl->kind == OBJ_SLICE ? (const char *)slice_bytes(f, sl) : (const char *)sl->at;

        if (text == NULL) {
            snprintf(buf, n, "a slice of a string let go");
            break;
        }
        snprintf(buf, n, "\"%.*s%s\"", len > 0 ? len : 0, text, len < (int)sl->size ? "..." : "");
        break;
    }
    case OBJ_LIST:
        snprintf(buf, n, "list(%ld)", (long)((const List *)sl->at)->n);
        break;
    case OBJ_MAP:
        snprintf(buf, n, "map(%ld)", (long)((const Map *)sl->at)->live);
        break;
    case OBJ_BYTES:
        snprintf(buf, n, "bytes(%ld)", (long)((const Bytes *)sl->at)->n);
        break;
    default:
        snprintf(buf, n, "%s{%d}", ((const TupleClass *)((const Cell *)sl->at)[0])->name,
                 ((const TupleClass *)((const Cell *)sl->at)[0])->nslots);
    }
    return 1;
}

/* an actor's region (inside the system's): its handlers' scratch regions are opened inside it, so what one rescues
 * the actor keeps; it lasts till forth_region_close */
int forth_region_lasting(Forth *f) { return region_new(f, 0, 1); }
void forth_region_close(Forth *f, int region) {
    if (region > 0 && f->objects != NULL && f->objects->regions[region].used) {
        region_end(f, region, 0);
    }
}

/* the objects alive in a region and those inside it, by kind (counts[OBJ_LIST ..]); their number */
static long region_counts(Forth *f, int region, long *counts) {
    struct ForthObjects *o = f->objects;
    long n = 0;
    ObjectSlot *sl;
    int i;

    if (o == NULL) {
        return 0;
    }
    EACH_SLOT(o, sl, i) {
        if (object_live(sl) && sl->region > 0 && region_within(o, sl->region, region)) {
            counts[sl->kind]++;
            n++;
        }
    }
    return n;
}
/* the bytes an object takes: its own (a string's, a tuple's slots) and what it holds (a list's items, a map's entries
 * and index, a byte buffer's room) - the heap's own bookkeeping not counted */
static size_t object_bytes(const ObjectSlot *sl) {
    size_t n = sl->kind == OBJ_SLICE ? sizeof(Slice) : sl->size;   /* (a slice's bytes are its string's) */

    if (sl->kind == OBJ_LIST) {
        n += (size_t)((const List *)sl->at)->cap * sizeof(Cell);
    } else if (sl->kind == OBJ_MAP) {
        const Map *m = sl->at;

        n += (size_t)m->cap * sizeof(MapEntry) + (size_t)m->slots * sizeof(int32_t);
    } else if (sl->kind == OBJ_BYTES) {
        n += (size_t)((const Bytes *)sl->at)->cap;
    }
    return n;
}
/* the bytes the objects of the region (and those inside it) take: an actor's memory, near enough */
size_t forth_region_bytes(Forth *f, int region) {
    struct ForthObjects *o = f->objects;
    size_t n = 0;
    ObjectSlot *sl;
    int i;

    if (o == NULL || region <= 0) {
        return 0;
    }
    EACH_SLOT(o, sl, i) {
        if (object_live(sl) && sl->region > 0 && region_within(o, sl->region, region)) {
            n += object_bytes(sl);
        }
    }
    return n;
}
long forth_region_objects(Forth *f, int region) {
    long counts[OBJ_GONE] = {0};

    return region > 0 ? region_counts(f, region, counts) : 0;
}
long forth_objects_freed(Forth *f) { return f->objects != NULL ? f->objects->freed : 0; }
void forth_region_print(Forth *f, int region) {   /* (`.actor-objects`: by kind, as `.objects`) */
    long counts[OBJ_GONE] = {0};
    int k;

    if (region > 0) {
        region_counts(f, region, counts);
    }
    for (k = OBJ_LIST; k < OBJ_GONE; k++) {
        const char *kind = object_kinds[k];

        forth_printf(f, "%-10s %ld\n", strncmp(kind, "a ", 2) == 0 ? kind + 2 : kind, counts[k]);
    }
}

/* ---- setting up ---- */

/* ---- objects a message carries to another node (nodes.c): copied out as bytes, made again there - the receiver's,
 * kept till `dispose` (as `rescue-all` keeps one) ---- */

#define PACK_DEPTH_MAX 32   /* objects within objects one may carry (a list holding itself is an error) */
enum { PACK_CELL = 'c', PACK_STRING = 's', PACK_LIST = 'l', PACK_TUPLE = 't', PACK_BYTES = 'b', PACK_MAP = 'm' };

/* (the maps' and byte buffers' own, further down) a map's entry made again: its key a string's bytes or a cell */
static void map_put_unpacked(Forth *f, Map *m, int is_text, const char *text, size_t len, Cell key, Cell value);

static void pack_bytes(Forth *f, ForthBytes *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = (b->cap + n) * 2;
        uint8_t *x = realloc(b->bytes, cap);

        if (x == NULL) {
            forth_error(f, "send: out of memory for what goes to another node");
        }
        b->bytes = x;
        b->cap = cap;
    }
    memcpy(b->bytes + b->n, p, n);
    b->n += n;
}
static void pack_tag(Forth *f, ForthBytes *b, int tag) {
    uint8_t t = (uint8_t)tag;

    pack_bytes(f, b, &t, 1);
}
static void pack_cell(Forth *f, ForthBytes *b, Cell x) { pack_bytes(f, b, &x, sizeof(x)); }

static void pack_object(Forth *f, ForthBytes *b, const ObjectSlot *sl, int depth);
static void pack_value(Forth *f, ForthBytes *b, Cell x, int is_float, int depth) {
    const ObjectSlot *sl = is_float ? NULL : object_of(f, x);

    if (sl != NULL) {
        pack_object(f, b, sl, depth);
    } else {
        pack_tag(f, b, PACK_CELL);
        pack_cell(f, b, x);
    }
}
static void pack_object(Forth *f, ForthBytes *b, const ObjectSlot *sl, int depth) {
    if (depth > PACK_DEPTH_MAX) {
        forth_error(f, "send: objects inside objects deeper than %d, for another node (a list holding itself?)",
                    PACK_DEPTH_MAX);
    }
    if (sl->kind == OBJ_STRING) {
        pack_tag(f, b, PACK_STRING);
        pack_cell(f, b, (Cell)sl->size);
        pack_bytes(f, b, sl->at, sl->size);
    } else if (sl->kind == OBJ_SLICE) {   /* (to another node: its bytes, a string there) */
        const uint8_t *bytes = slice_bytes(f, sl);

        if (bytes == NULL) {
            forth_error(f, "send: a slice of a string that was let go");
        }
        pack_tag(f, b, PACK_STRING);
        pack_cell(f, b, (Cell)sl->size);
        pack_bytes(f, b, bytes, sl->size);
    } else if (sl->kind == OBJ_BYTES) {
        const Bytes *x = sl->at;

        pack_tag(f, b, PACK_BYTES);
        pack_cell(f, b, x->n);
        pack_bytes(f, b, x->p, (size_t)x->n);
    } else if (sl->kind == OBJ_MAP) {   /* (its live entries in order: a string key by its bytes, else its cell) */
        const Map *m = sl->at;
        Cell i;

        pack_tag(f, b, PACK_MAP);
        pack_cell(f, b, m->live);
        for (i = 0; i < m->n; i++) {
            const ObjectSlot *key;
            uint8_t is_text;

            if (!m->e[i].live) {
                continue;
            }
            is_text = (uint8_t)(m->e[i].text != 0);
            pack_bytes(f, b, &is_text, 1);
            if (is_text) {
                key = object_of(f, m->e[i].key);
                pack_cell(f, b, (Cell)key->size);
                pack_bytes(f, b, key->at, key->size);
            } else {
                pack_value(f, b, m->e[i].key, 0, depth + 1);
            }
            pack_value(f, b, m->e[i].value, 0, depth + 1);
        }
    } else if (sl->kind == OBJ_LIST) {
        const List *l = sl->at;
        Cell i;

        pack_tag(f, b, PACK_LIST);
        pack_cell(f, b, l->n);
        for (i = 0; i < l->n; i++) {
            pack_value(f, b, l->items[i], 0, depth + 1);
        }
    } else {
        const Cell *t = sl->at;
        const TupleClass *c = (const TupleClass *)t[0];
        uint8_t len = (uint8_t)strlen(c->name);
        int i;

        pack_tag(f, b, PACK_TUPLE);
        pack_bytes(f, b, &len, 1);
        pack_bytes(f, b, c->name, len);
        pack_cell(f, b, c->nslots);
        for (i = 0; i < c->nslots; i++) {
            pack_value(f, b, t[i + 1], f->tuples->names[c->slot[i]].is_float, depth + 1);
        }
    }
}

int forth_object_pack(Forth *f, Cell x, ForthBytes *b) {
    const ObjectSlot *sl = object_of(f, x);

    if (sl == NULL) {
        return 0;
    }
    pack_object(f, b, sl, 0);
    return 1;
}

typedef struct Unpacking {
    const uint8_t *p, *end;
} Unpacking;

static int unpack_bytes(Unpacking *u, void *out, size_t n) {
    if ((size_t)(u->end - u->p) < n) {
        return 0;
    }
    memcpy(out, u->p, n);
    u->p += n;
    return 1;
}

static TupleClass *class_named(Forth *f, const char *name) {
    TupleClass *c;

    for (c = tuples(f)->classes; c != NULL && strcmp(c->name, name) != 0; c = c->next) {
    }
    return c;
}

/* one value: made (make) or only followed, to see it can be (its classes here, as many slots) - 0 if it can't */
static int unpack_value(Forth *f, Unpacking *u, int make, Cell *out) {
    uint8_t tag;
    Cell n, i;

    if (!unpack_bytes(u, &tag, 1)) {
        return 0;
    }
    if (tag == PACK_CELL) {
        return unpack_bytes(u, out, sizeof(Cell));
    }
    if (tag == PACK_TUPLE) {
        char name[WORD_NAME_MAX + 1];
        uint8_t len;
        TupleClass *c;
        Cell *t = NULL;

        if (!unpack_bytes(u, &len, 1) || len > WORD_NAME_MAX || !unpack_bytes(u, name, len)) {
            return 0;
        }
        name[len] = 0;
        if (!unpack_bytes(u, &n, sizeof(n)) || (c = class_named(f, name)) == NULL || c->nslots != n) {
            return 0;   /* (no such class here, or another of that name) */
        }
        if (make) {
            t = object_new(f, OBJ_TUPLE, (size_t)(c->nslots + 1) * sizeof(Cell));
            t[0] = (Cell)c;
        }
        for (i = 0; i < n; i++) {
            Cell x = 0;

            if (!unpack_value(f, u, make, &x)) {
                return 0;
            }
            if (make) {
                t[i + 1] = x;
            }
        }
        *out = t != NULL ? REF(t) : 0;   /* (only asked whether it fits: none made) */
        return 1;
    }
    if (!unpack_bytes(u, &n, sizeof(n)) || n < 0) {
        return 0;
    }
    if (tag == PACK_STRING) {
        if ((Cell)(u->end - u->p) < n) {
            return 0;
        }
        if (make) {
            *out = REF(string_new(f, (const char *)u->p, (size_t)n));
        }
        u->p += n;
        return 1;
    }
    if (tag == PACK_LIST) {
        List *l = make ? new_list(f) : NULL;

        for (i = 0; i < n; i++) {
            Cell x = 0;

            if (!unpack_value(f, u, make, &x)) {
                return 0;
            }
            if (make) {
                list_add(f, l, x);
            }
        }
        *out = l != NULL ? REF(l) : 0;
        return 1;
    }
    if (tag == PACK_BYTES) {
        if ((Cell)(u->end - u->p) < n) {
            return 0;
        }
        if (make) {
            Bytes *b = object_new(f, OBJ_BYTES, sizeof(Bytes));

            bytes_room(f, b, n);
            if (n > 0) {
                memcpy(b->p, u->p, (size_t)n);
            }
            b->n = n;
            *out = REF(b);
        }
        u->p += n;
        return 1;
    }
    if (tag == PACK_MAP) {
        Map *m = make ? object_new(f, OBJ_MAP, sizeof(Map)) : NULL;

        for (i = 0; i < n; i++) {
            uint8_t is_text;
            const char *text = NULL;
            Cell len = 0, key = 0, value = 0;

            if (!unpack_bytes(u, &is_text, 1)) {
                return 0;
            }
            if (is_text) {
                if (!unpack_bytes(u, &len, sizeof(len)) || len < 0 || (Cell)(u->end - u->p) < len) {
                    return 0;
                }
                text = (const char *)u->p;
                u->p += len;
            } else if (!unpack_value(f, u, make, &key)) {
                return 0;
            }
            if (!unpack_value(f, u, make, &value)) {
                return 0;
            }
            if (make) {
                map_put_unpacked(f, m, is_text, text, (size_t)len, key, value);
            }
        }
        *out = m != NULL ? REF(m) : 0;
        return 1;
    }
    return 0;
}

/* a string's or a byte buffer's bytes, by its reference (the text a word is given as `ref len`); NULL: x is neither */
const char *forth_object_bytes(Forth *f, Cell x, size_t *len) {
    const ObjectSlot *sl = object_of(f, x);

    if (sl != NULL && sl->kind == OBJ_STRING) {
        *len = sl->size;
        return sl->at;
    }
    if (sl != NULL && sl->kind == OBJ_BYTES) {
        const Bytes *b = sl->at;

        *len = (size_t)b->n;
        return b->p != NULL ? (const char *)b->p : "";
    }
    if (sl != NULL && sl->kind == OBJ_SLICE) {
        *len = sl->size;
        return (const char *)slice_bytes(f, sl);   /* (NULL: its string was let go) */
    }
    return NULL;
}
const char *forth_object_string(Forth *f, Cell x, size_t *len) {
    ObjectSlot *sl = object_of(f, x);

    if (sl == NULL || sl->kind != OBJ_STRING) {
        return NULL;
    }
    *len = sl->size;
    return sl->at;
}

int forth_object_fits(Forth *f, const uint8_t **p, const uint8_t *end) {
    Unpacking look = {*p, end};
    Cell x = 0;

    if (!unpack_value(f, &look, 0, &x)) {
        return 0;
    }
    *p = look.p;
    return 1;
}

int forth_object_unpack(Forth *f, const uint8_t **p, const uint8_t *end, Cell *out) {
    Unpacking look = {*p, end}, u = {*p, end};
    int region = f->t->region;
    Cell x = 0;

    if (!unpack_value(f, &look, 0, &x)) {
        return 0;
    }
    f->t->region = post_region(f);   /* (on its way: the receiving handler's as it is delivered - forth_objects_take) */
    unpack_value(f, &u, 1, out);
    f->t->region = region;
    *p = u.p;
    return 1;
}


/* ---- maps: <map> ( -- m ), its keys cells or strings. A key that is a string object - or one given as addr len to
 * the s- words - is matched by its bytes, and the map keeps a copy (its own string: `keys` gives it, `dispose`
 * refuses it); any other key by its cell (a number, an object by identity). Values are cells, as a list's items. ---- */

typedef struct Key {
    Cell cell;
    const char *text;   /* (a string key's bytes) */
    size_t len;
    uint64_t hash;
    int is_text;
} Key;

#define HASH_OFFSET 14695981039346656037ull   /* FNV-1a, 64 bits */
#define HASH_PRIME 1099511628211ull

/* A map's string key hashed: SipHash-1-3 by the Forth's own random key. (It was FNV-1a, the same everywhere: 65,000
 * keys made to agree in its low bits - found once, good against every server - and a JSON object of them took 3.7
 * seconds to read where 65,000 others take 0.04, the node held half a second at a time.) Eight bytes a round: no
 * slower than a byte at a time was for the short keys maps mostly have. */
#define SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND                                                                                                   \
    do {                                                                                                            \
        v0 += v1; v1 = SIP_ROTL(v1, 13); v1 ^= v0; v0 = SIP_ROTL(v0, 32);                                           \
        v2 += v3; v3 = SIP_ROTL(v3, 16); v3 ^= v2;                                                                  \
        v0 += v3; v3 = SIP_ROTL(v3, 21); v3 ^= v0;                                                                  \
        v2 += v1; v1 = SIP_ROTL(v1, 17); v1 ^= v2; v2 = SIP_ROTL(v2, 32);                                           \
    } while (0)
static uint64_t hash_bytes(Forth *f, const char *s, size_t n) {
    const uint64_t *key = objects(f)->map_key;
    uint64_t v0 = key[0] ^ 0x736f6d6570736575ull, v1 = key[1] ^ 0x646f72616e646f6dull;
    uint64_t v2 = key[0] ^ 0x6c7967656e657261ull, v3 = key[1] ^ 0x7465646279746573ull, m;
    size_t i, whole = n & ~(size_t)7;

    for (i = 0; i < whole; i += 8) {
        memcpy(&m, s + i, sizeof(m));
        v3 ^= m;
        SIP_ROUND;
        v0 ^= m;
    }
    m = (uint64_t)n << 56;   /* (the last bytes, and how many there were in all) */
    for (i = whole; i < n; i++) {
        m |= (uint64_t)(uint8_t)s[i] << (8 * (i - whole));
    }
    v3 ^= m;
    SIP_ROUND;
    v0 ^= m;
    v2 ^= 0xff;
    SIP_ROUND;
    SIP_ROUND;
    SIP_ROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
static uint64_t hash_cell(Cell x) {   /* (splitmix64's finish: every bit of the cell in every bit of the hash) */
    uint64_t z = (uint64_t)x;

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static Key text_key(Forth *f, const char *s, size_t n) { return (Key){0, s, n, hash_bytes(f, s, n), 1}; }
/* ... a number's, or an object's by its address: with the key too (numbers a client names are keys as well) */
static uint64_t hash_key_cell(Forth *f, Cell x) { return hash_cell((Cell)((uint64_t)x ^ objects(f)->map_key[0])); }

static Key cell_key(Forth *f, Cell x) {   /* (a string object: by its bytes) */
    ObjectSlot *sl = object_of(f, x);

    if (sl != NULL && sl->kind == OBJ_STRING) {
        return text_key(f, sl->at, sl->size);
    }
    if (sl != NULL && sl->kind == OBJ_SLICE && slice_bytes(f, sl) != NULL) {
        return text_key(f, (const char *)slice_bytes(f, sl), sl->size);
    }
    return (Key){x, NULL, 0, hash_key_cell(f, x), 0};
}

static Map *map_arg(Forth *f, Cell x, const char *word) { return object_arg(f, x, OBJ_MAP, word); }

static const char *entry_text(Forth *f, const MapEntry *e, size_t *len) {
    ObjectSlot *sl = object_of(f, e->key);

    *len = sl != NULL ? sl->size : 0;
    return sl != NULL ? sl->at : "";
}

/* the index slot that holds k's entry; -1 if it has none */
static Cell map_slot(Forth *f, const Map *m, const Key *k) {
    Cell i, mask = m->slots - 1;

    if (m->slots == 0) {
        return -1;
    }
    for (i = (Cell)(k->hash & (uint64_t)mask);; i = (i + 1) & mask) {
        int32_t at = m->index[i];
        const MapEntry *e;

        if (at == MAP_EMPTY) {
            return -1;
        }
        if (at == MAP_DELETED) {
            continue;
        }
        e = &m->e[at];
        if (e->hash == k->hash && e->text == k->is_text) {
            size_t len;
            const char *t;

            if (!k->is_text) {
                if (e->key == k->cell) {
                    return i;
                }
            } else if ((t = entry_text(f, e, &len)) != NULL && len == k->len && memcmp(t, k->text, len) == 0) {
                return i;
            }
        }
    }
}

static void index_put(Map *m, Cell entry) {
    Cell i, mask = m->slots - 1;

    for (i = (Cell)(m->e[entry].hash & (uint64_t)mask); m->index[i] >= 0; i = (i + 1) & mask) {
    }
    m->index[i] = (int32_t)entry;
}

/* room for one more: the entries packed (their holes gone, their order kept) and the index made again, larger if it
 * must be. An error (out of memory) leaves the map as it was. */
static void map_room(Forth *f, Map *m) {
    Cell slots = MAP_FIRST_SLOTS, i, j;
    int32_t *index;

    if (m->n == m->cap) {
        Cell cap = m->cap ? m->cap * 2 : MAP_FIRST_SLOTS;
        MapEntry *e;

        if (m->live >= MAP_ENTRIES_MAX) {
            forth_error(f, "map: %ld entries, the most a map has", (long)m->live);
        }
        if ((e = realloc(m->e, (size_t)cap * sizeof(MapEntry))) == NULL) {
            forth_error(f, "map: out of memory at %ld entries", (long)m->live);
        }
        m->e = e;
        m->cap = cap;
    }
    if (m->slots > 0 && (m->n + 1) * 4 <= m->slots * 3) {
        return;
    }
    while ((m->live + 1) * 2 > slots) {
        slots *= 2;
    }
    if ((index = malloc((size_t)slots * sizeof(int32_t))) == NULL) {
        forth_error(f, "map: out of memory at %ld entries", (long)m->live);
    }
    for (i = j = 0; i < m->n; i++) {   /* (packed: the order kept) */
        if (m->e[i].live) {
            m->e[j++] = m->e[i];
        }
    }
    m->n = j;
    free(m->index);
    m->index = index;
    m->slots = slots;
    for (i = 0; i < slots; i++) {
        m->index[i] = MAP_EMPTY;
    }
    for (i = 0; i < m->n; i++) {
        index_put(m, i);
    }
}

static void map_put(Forth *f, Map *m, const Key *k, Cell value) {
    Cell at = map_slot(f, m, k), key = k->cell;
    MapEntry *e;

    if (at >= 0) {
        m->e[m->index[at]].value = value;
        return;
    }
    map_room(f, m);
    if (k->is_text) {   /* (its own copy of the key: made in the system's region, whatever region runs) */
        int region = f->t->region;
        char *copy;

        f->t->region = 0;
        copy = string_new(f, k->text, k->len);
        f->t->region = region;
        object_slot(f, copy)->owned = 1;
        key = REF(copy);
    }
    e = &m->e[m->n];
    *e = (MapEntry){key, value, k->hash, k->is_text, 1};
    index_put(m, m->n);
    m->n++;
    m->live++;
}

static void map_put_unpacked(Forth *f, Map *m, int is_text, const char *text, size_t len, Cell key, Cell value) {
    Key k = is_text ? text_key(f, text, len) : (Key){key, NULL, 0, hash_key_cell(f, key), 0};

    map_put(f, m, &k, value);
}

static void let_go_key(Forth *f, const MapEntry *e) {   /* a string key's copy (gone already as the system ends) */
    ObjectSlot *sl = e->text ? object_of(f, e->key) : NULL;

    if (sl != NULL) {
        sl->owned = 0;
        object_free(f, sl);
    }
}

static void map_delete(Forth *f, Map *m, const Key *k) {
    Cell at = map_slot(f, m, k);
    MapEntry *e;

    if (at < 0) {
        return;
    }
    e = &m->e[m->index[at]];
    m->index[at] = MAP_DELETED;
    let_go_key(f, e);
    e->live = 0;
    m->live--;
}

static void map_clear(Forth *f, Map *m) {
    Cell i;

    for (i = 0; i < m->n; i++) {
        if (m->e[i].live) {
            let_go_key(f, &m->e[i]);
        }
    }
    forth_worked(f, (size_t)m->n + (size_t)m->slots / 2, WORK_ITEMS);
    m->n = m->live = 0;
    for (i = 0; i < m->slots; i++) {
        m->index[i] = MAP_EMPTY;
    }
}

static void map_let_go(Forth *f, Map *m) {
    map_clear(f, m);
    free(m->e);
    free(m->index);
}

static void push_found(Forth *f, Map *m, Cell at) {   /* ( -- value true | false ) */
    if (at >= 0) {
        PUSH(m->e[m->index[at]].value);
        PUSH(-1);
    } else {
        PUSH(0);
    }
}

PRIM(p_map_new) { PUSH(REF(object_new(f, OBJ_MAP, sizeof(Map)))); }   /* <map> ( -- m ) */
PRIM(p_at) {   /* at ( key m -- value true | false ) */
    Map *m = map_arg(f, POP(), "at");
    Key k = cell_key(f, POP());

    push_found(f, m, map_slot(f, m, &k));
}
PRIM(p_at_store) {   /* at! ( value key m -- ) */
    Map *m = map_arg(f, POP(), "at!");
    Key k = cell_key(f, POP());

    map_put(f, m, &k, POP());
}
PRIM(p_key_q) {   /* key? ( key m -- flag ) */
    Map *m = map_arg(f, POP(), "key?");
    Key k = cell_key(f, POP());

    PUSH(map_slot(f, m, &k) >= 0 ? -1 : 0);
}
PRIM(p_delete_at) {   /* delete-at ( key m -- ) (none there: nothing) */
    Map *m = map_arg(f, POP(), "delete-at");
    Key k = cell_key(f, POP());

    map_delete(f, m, &k);
}
static Key text_arg(Forth *f, const char *word) {   /* ( addr len -- ) */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, word, &len);

    return text_key(f, s, len);
}
PRIM(p_s_at) {   /* s-at ( addr len m -- value true | false ) */
    Map *m = map_arg(f, POP(), "s-at");
    Key k = text_arg(f, "s-at");

    push_found(f, m, map_slot(f, m, &k));
}
PRIM(p_s_at_store) {   /* s-at! ( value addr len m -- ) */
    Map *m = map_arg(f, POP(), "s-at!");
    Key k = text_arg(f, "s-at!");

    map_put(f, m, &k, POP());
}
/* fields>map ( a n max m -- flag ) An internet message's header fields - HTTP's, a MIME part's: lines `name: value`
 * with CR LF between them - put in the map m by name, lower-cased, each value without the spaces round it; a name
 * said again has both values, joined by ", ". False (m as far as it got) if there are more than max lines, a line has
 * no name, or a name holds what RFC 9110's token may not: a space before the colon above all (a smuggler's trick).
 * In C because it is every request a server reads: in Forth, a third of a browser's request's time. */
#define FIELD_NAME_BYTES 64   /* a name lower-cased here on the stack; a longer one in a string of its own */
static int field_name_char(unsigned char c) {   /* (RFC 9110's tchar; not isalnum: no locale has a say) */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}
PRIM(p_fields_to_map) {
    Map *m = map_arg(f, POP(), "fields>map");
    Cell max = POP(), n = POP(), a = POP(), lines = 0;
    size_t len, at = 0, end, i;
    const char *s = text_at(f, a, n, "fields>map", &len);

    for (; len > 0; at = end + 2) {
        const char *line = s + at, *colon, *v, *v_end;
        char small[FIELD_NAME_BYTES], *name = small, *value;
        size_t name_len, v_len;
        Cell slot;
        Key k;

        end = find_in(s, len, "\r\n", 2, at);
        colon = memchr(line, ':', end - at);
        if (++lines > max || colon == NULL || colon == line) {
            PUSH(0);
            return;
        }
        name_len = (size_t)(colon - line);
        if (name_len > sizeof(small)) {
            name = string_alloc(f, name_len);
        }
        for (i = 0; i < name_len; i++) {
            unsigned char c = (unsigned char)line[i];

            if (!field_name_char(c)) {
                PUSH(0);
                return;
            }
            name[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        for (v = colon + 1, v_end = s + end; v < v_end && isspace((unsigned char)*v); v++) {
        }
        for (; v_end > v && isspace((unsigned char)v_end[-1]); v_end--) {
        }
        v_len = (size_t)(v_end - v);
        k = text_key(f, name, name_len);
        if ((slot = map_slot(f, m, &k)) >= 0) {   /* (said before: both) */
            Cell said = m->e[m->index[slot]].value;
            const char *was = object_arg(f, said, OBJ_STRING, "fields>map");
            size_t was_len = object_of(f, said)->size;

            value = string_alloc(f, was_len + 2 + v_len);
            memcpy(value, was, was_len);
            memcpy(value + was_len, ", ", 2);
            memcpy(value + was_len + 2, v, v_len);
        } else {
            value = string_new(f, v, v_len);
        }
        map_put(f, m, &k, REF(value));
        if (end >= len) {
            break;
        }
    }
    PUSH(-1);
}
PRIM(p_s_key_q) {   /* s-key? ( addr len m -- flag ) */
    Map *m = map_arg(f, POP(), "s-key?");
    Key k = text_arg(f, "s-key?");

    PUSH(map_slot(f, m, &k) >= 0 ? -1 : 0);
}
PRIM(p_s_delete_at) {   /* s-delete-at ( addr len m -- ) */
    Map *m = map_arg(f, POP(), "s-delete-at");
    Key k = text_arg(f, "s-delete-at");

    map_delete(f, m, &k);
}
/* s-key-after ( addr len m -- s true | false ) the key that comes after that one in the map's order: a new string
 * of it; false if that one is the last, isn't there, or the next isn't a string. (With s-move-before: an entry taken
 * out put back where it was - a change undone.) */
PRIM(p_s_key_after) {
    Map *m = map_arg(f, POP(), "s-key-after");
    Key k = text_arg(f, "s-key-after");
    Cell at = map_slot(f, m, &k), i;

    for (i = at >= 0 ? m->index[at] + 1 : m->n; i < m->n && !m->e[i].live; i++) {
    }
    if (i < m->n && m->e[i].text) {
        size_t len;
        const char *text = entry_text(f, &m->e[i], &len);

        PUSH(REF(string_new(f, text, len)));
        PUSH(-1);
    } else {
        PUSH(0);
    }
}
/* s-move-before ( addr len addr2 len2 m -- ) the first key's entry moved to just before the second's, the others'
 * order kept (either not there: nothing). It looks at every entry: for what is seldom done. */
PRIM(p_s_move_before) {
    Map *m = map_arg(f, POP(), "s-move-before");
    Key before = text_arg(f, "s-move-before"), k = text_arg(f, "s-move-before");
    Cell at = map_slot(f, m, &k), to = map_slot(f, m, &before), from, i;
    MapEntry moved;

    if (at < 0 || to < 0 || at == to) {
        return;
    }
    from = m->index[at];
    to = m->index[to];
    moved = m->e[from];
    if (from > to) {
        memmove(&m->e[to + 1], &m->e[to], (size_t)(from - to) * sizeof(MapEntry));
        m->e[to] = moved;
    } else {
        memmove(&m->e[from], &m->e[from + 1], (size_t)(to - from - 1) * sizeof(MapEntry));
        m->e[to - 1] = moved;
    }
    for (i = 0; i < m->slots; i++) {   /* (the entries have other numbers: the index made again) */
        m->index[i] = MAP_EMPTY;
    }
    for (i = 0; i < m->n; i++) {
        if (m->e[i].live) {
            index_put(m, i);
        }
    }
}
/* s-hash ( addr len -- n ) a number of the text: the same text, the same number - while this Forth runs (each has a
 * seed of its own: no one outside can make two texts of one number, nor is the number another run's). For telling
 * whether a text is the one seen before without keeping it; quick (a few instructions a byte, where SHA-256 takes
 * sixty). Not for secrets, nor to be kept in a file. */
PRIM(p_s_hash) {
    struct ForthObjects *o = objects(f);
    Key k = text_arg(f, "s-hash");
    uint64_t h;
    size_t i;

    while (o->hash_seed == 0) {
        forth_secure_random(f, (uint8_t *)&o->hash_seed, sizeof(o->hash_seed));
    }
    h = HASH_OFFSET ^ o->hash_seed;
    forth_worked(f, k.len, WORK_BYTES);
    for (i = 0; i < k.len; i++) {
        h = (h ^ (uint8_t)k.text[i]) * HASH_PRIME;
    }
    PUSH(hash_cell((Cell)h));
}
PRIM(p_map_size) { PUSH(map_arg(f, POP(), "map-size")->live); }   /* ( m -- n ) */
PRIM(p_map_clear) { map_clear(f, map_arg(f, POP(), "map-clear")); }   /* ( m -- ) */
static void map_list(Forth *f, int values) {   /* ( m -- l ) its keys or its values, in their order */
    Cell x = POP();
    Map *m = map_arg(f, x, values ? "values" : "keys");
    List *l = new_list(f);
    Cell i;

    for (i = 0; i < m->n; i++) {
        if (m->e[i].live) {
            list_add(f, l, values ? m->e[i].value : m->e[i].key);
        }
    }
    forth_worked(f, (size_t)m->n, WORK_ITEMS);
    PUSH(REF(l));
}
PRIM(p_keys) { map_list(f, 0); }
PRIM(p_values) { map_list(f, 1); }
/* map-each ( m xt -- ) xt ( key value -- ) for each entry, in their order. The map may have its values changed as
 * it is walked (at! of a key it has), not entries added or taken away: an error. */
PRIM(p_map_each) {
    Word *xt = forth_xt_arg(f, POP(), "map-each");
    Cell x = POP(), i, n, live;
    Map *m = map_arg(f, x, "map-each");

    for (i = 0, n = m->n, live = m->live; i < n; i++) {   /* (a map all of whose entries were taken away before: none walked) */
        m = map_arg(f, x, "map-each");
        if (m->n != n || m->live != live) {
            forth_error(f, "map-each: entries added or taken away as the map was walked");
        }
        if (!m->e[i].live) {
            continue;
        }
        PUSH(m->e[i].key);
        PUSH(m->e[i].value);
        forth_execute(f, xt);
    }
}

/* ---- bytes: <bytes> ( -- b ), bytes that grow. b-data gives their address for the memory words (w@ l@ move type
 * ...): good till the bytes grow. ---- */

static Bytes *bytes_arg(Forth *f, Cell x, const char *word) { return object_arg(f, x, OBJ_BYTES, word); }

static void bytes_grown(Forth *f, Bytes *b, Cell more) {
    if (more < 0 || more > CELL_BYTES_MAX - b->n) {
        forth_error(f, "bytes: %ld more is too many", (long)more);
    }
    if (b->n + more > b->cap) {
        Cell cap = b->cap ? b->cap : 64;
        uint8_t *p;

        while (cap < b->n + more) {
            cap = cap > CELL_BYTES_MAX / 2 ? CELL_BYTES_MAX : cap * 2;
        }
        if ((p = realloc(b->p, (size_t)cap)) == NULL) {
            forth_error(f, "bytes: out of memory for %ld", (long)(b->n + more));
        }
        b->p = p;
        b->cap = cap;
    }
}
static Cell byte_index(Forth *f, const Bytes *b, Cell i, const char *word) {
    if (i < 0 || i >= b->n) {
        forth_error(f, "%s: index %ld out of range (length %ld)", word, (long)i, (long)b->n);
    }
    return i;
}
PRIM(p_bytes_new) { PUSH(REF(object_new(f, OBJ_BYTES, sizeof(Bytes)))); }   /* <bytes> ( -- b ) */
PRIM(p_b_length) { PUSH(bytes_arg(f, POP(), "b-length")->n); }   /* ( b -- n ) */
PRIM(p_b_fetch) {   /* b@ ( i b -- c ) */
    Bytes *b = bytes_arg(f, POP(), "b@");
    Cell i = POP();

    PUSH(b->p[byte_index(f, b, i, "b@")]);
}
PRIM(p_b_store) {   /* b! ( c i b -- ) */
    Bytes *b = bytes_arg(f, POP(), "b!");
    Cell i = POP(), c = POP();

    b->p[byte_index(f, b, i, "b!")] = (uint8_t)c;
}
PRIM(p_b_push) {   /* b-push ( c b -- ) */
    Bytes *b = bytes_arg(f, POP(), "b-push");
    Cell c = POP();

    bytes_room(f, b, 1);
    b->p[b->n++] = (uint8_t)c;
}
/* b-append-seq ( seq from to b -- ) a string's or bytes' bytes from `from` up to `to`, after b's own: a piece of what
 * came put by - no slice made of it, no copy but the one (a chunk's data, a packet's payload) */
PRIM(p_b_append_seq) {
    Cell x = POP(), to = POP(), from = POP();
    const ObjectSlot *sl = seq_slot(f, POP(), "b-append-seq");
    Bytes *b = bytes_arg(f, x, "b-append-seq");

    if (!byte_kind(sl->kind)) {
        forth_error(f, "b-append-seq: a string's or bytes' (%s)", object_kinds[sl->kind]);
    }
    if (from < 0 || to < from || to > seq_length(sl)) {
        forth_error(f, "b-append-seq: %ld to %ld of %ld", (long)from, (long)to, (long)seq_length(sl));
    }
    if (to == from) {
        return;
    }
    if (sl->at == (void *)b) {   /* (its own bytes: where they are after it has room) */
        bytes_room(f, b, to - from);
        memmove(b->p + b->n, b->p + from, (size_t)(to - from));
    } else {
        bytes_room(f, b, to - from);
        memcpy(b->p + b->n, seq_bytes(sl) + from, (size_t)(to - from));
    }
    b->n += to - from;
}
PRIM(p_b_append) {   /* b-append ( addr len b -- ) those bytes after its own (its own too: b b-data b b-append) */
    Bytes *b = bytes_arg(f, POP(), "b-append");
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = text_at(f, a, n, "b-append", &len);
    Cell inside;

    if (len == 0) {
        return;   /* (nothing: and b may have no bytes yet to copy to) */
    }
    inside = b->p != NULL && (const uint8_t *)s >= b->p && (const uint8_t *)s < b->p + b->cap
             ? (Cell)((const uint8_t *)s - b->p) : -1;

    bytes_room(f, b, (Cell)len);
    memmove(b->p + b->n, inside >= 0 ? b->p + inside : (const uint8_t *)s, len);   /* (moved with them if theirs) */
    b->n += (Cell)len;
}
PRIM(p_b_data) {   /* b-data ( b -- addr len ) */
    Bytes *b = bytes_arg(f, POP(), "b-data");

    if (b->p == NULL) {   /* (an address even with none: a 0-length string's) */
        bytes_room(f, b, 1);
    }
    PUSH((Cell)b->p);
    PUSH(b->n);
}
PRIM(p_b_to_string) {   /* b>string ( b -- s len ) a new string of its bytes */
    Bytes *b = bytes_arg(f, POP(), "b>string");
    char *p = string_alloc(f, (size_t)b->n);

    memcpy(p, b->p != NULL ? (const char *)b->p : "", (size_t)b->n);
    PUSH(REF(p));
    PUSH(b->n);
}
PRIM(p_b_clear) { bytes_arg(f, POP(), "b-clear")->n = 0; }   /* ( b -- ) */
PRIM(p_b_resize) {   /* b-resize ( n b -- ) so many bytes: cut, or grown with zeros */
    Bytes *b = bytes_arg(f, POP(), "b-resize");
    Cell n = POP();

    if (n < 0) {
        forth_error(f, "b-resize: a negative length (%ld)", (long)n);
    }
    if (n > b->n) {
        bytes_room(f, b, n - b->n);
        memset(b->p + b->n, 0, (size_t)(n - b->n));
    }
    b->n = n;
}

static void *clone_container(Forth *f, int kind, void *from) {   /* (clone's: a map's entries, a bytes' bytes) */
    if (kind == OBJ_BYTES) {
        Bytes *src = from, *b = object_new(f, OBJ_BYTES, sizeof(Bytes));

        bytes_room(f, b, src->n);
        if (src->n > 0) {
            memcpy(b->p, src->p, (size_t)src->n);
        }
        b->n = src->n;
        return b;
    } else {
        Map *src = from, *m = object_new(f, OBJ_MAP, sizeof(Map));
        Cell i;

        for (i = 0; i < src->n; i++) {
            if (src->e[i].live) {
                Key k = src->e[i].text ? cell_key(f, src->e[i].key) : (Key){src->e[i].key, NULL, 0, src->e[i].hash, 0};

                map_put(f, m, &k, src->e[i].value);
            }
        }
        return m;
    }
}

/* ---- sort: a list's items in place - numbers (sort), strings by their bytes (sort-strings), or as xt says
 * (sort-by: xt ( a b -- flag ) true when a goes before b). Stable: items alike keep their order. ---- */

typedef struct Sorting {
    Forth *f;
    Word *xt;           /* (sort-by's) */
    int kind;           /* SORT_* */
} Sorting;

enum { SORT_NUMBERS, SORT_STRINGS, SORT_BY };

static int sort_before(Sorting *s, Cell a, Cell b) {   /* does a go before b? */
    Forth *f = s->f;

    if (s->kind == SORT_NUMBERS) {
        return a < b;
    }
    if (s->kind == SORT_STRINGS) {
        ObjectSlot *x = object_of(f, a), *y = object_of(f, b);
        size_t n;
        int c;

        if (x == NULL || x->kind != OBJ_STRING || y == NULL || y->kind != OBJ_STRING) {
            forth_error(f, "sort-strings: an item that isn't a string");
        }
        n = x->size < y->size ? x->size : y->size;
        c = memcmp(x->at, y->at, n);
        return c < 0 || (c == 0 && x->size < y->size);
    }
    PUSH(a);
    PUSH(b);
    forth_execute(f, s->xt);
    return POP() != 0;
}

static void merge_sort(Sorting *s, Cell *a, Cell *tmp, Cell n) {
    Cell mid = n / 2, i = 0, j = mid, k = 0;

    if (n < 2) {
        return;
    }
    merge_sort(s, a, tmp, mid);
    merge_sort(s, a + mid, tmp, n - mid);
    while (i < mid && j < n) {   /* (the right one first only if it goes before: stable) */
        tmp[k++] = sort_before(s, a[j], a[i]) ? a[j++] : a[i++];
    }
    while (i < mid) {
        tmp[k++] = a[i++];
    }
    while (j < n) {
        tmp[k++] = a[j++];
    }
    memcpy(a, tmp, (size_t)n * sizeof(Cell));
}

/* the items sorted as a copy (sort-by's code may do anything meanwhile), then put back if the list is as it was */
static void sort_list(Forth *f, Cell x, int kind, Word *xt, const char *word) {
    List *l = object_arg(f, x, OBJ_LIST, word);
    Cell n = l->n;
    Cell *volatile items = malloc((size_t)(n > 0 ? n : 1) * 2 * sizeof(Cell));
    jmp_buf jb, *prev = f->catch;
    Sorting s = {f, xt, kind};

    if (items == NULL) {
        forth_error(f, "%s: out of memory for %ld items", word, (long)n);
    }
    if (setjmp(jb) != 0) {   /* (an error in xt, or an item not a string: the copy let go, the error passed on) */
        free(items);
        f->catch = prev;
        longjmp(*prev, 1);
    }
    f->catch = &jb;
    if (n > 0) {   /* (none: no address to copy from) */
        memcpy(items, l->items, (size_t)n * sizeof(Cell));
    }
    {   /* (n items sorted: each compared with another about log2 n times - a step each) */
        size_t compared = 0, k;

        for (k = (size_t)n; k > 1; k /= 2) {
            compared += (size_t)n;
        }
        forth_worked(f, compared, 1);
    }
    merge_sort(&s, items, items + n, n);
    f->catch = prev;
    l = (List *)object_of(f, x);
    if (l == NULL || object_of(f, x)->kind != OBJ_LIST || ((List *)object_of(f, x)->at)->n != n) {
        free(items);
        forth_error(f, "%s: the list changed as it was sorted", word);
    }
    if (n > 0) {   /* (... nor one to copy back to) */
        memcpy(((List *)object_of(f, x)->at)->items, items, (size_t)n * sizeof(Cell));
    }
    free(items);
}
PRIM(p_sort) { sort_list(f, POP(), SORT_NUMBERS, NULL, "sort"); }   /* ( l -- ) */
PRIM(p_sort_strings) { sort_list(f, POP(), SORT_STRINGS, NULL, "sort-strings"); }   /* ( l -- ) */
PRIM(p_sort_by) {   /* ( l xt -- ) */
    Word *xt = forth_xt_arg(f, POP(), "sort-by");

    sort_list(f, POP(), SORT_BY, xt, "sort-by");
}


/* ---- UTF-8: a string's bytes as characters (code points). A byte that begins no character - or an overlong form,
 * a surrogate, past U+10FFFF - is one character, U+FFFD (the replacement), so every string can be walked. ---- */

#define UTF8_REPLACEMENT 0xFFFD
#define UNICODE_MAX 0x10FFFF
#define SURROGATE_FIRST 0xD800
#define SURROGATE_LAST 0xDFFF

/* the character at s (n > 0 bytes): its code point, and how many bytes it took (*used) */
static Cell utf8_decode(const uint8_t *s, size_t n, size_t *used) {
    static const Cell least[4] = {0, 0x80, 0x800, 0x10000};   /* (the smallest each length may hold: no overlongs) */
    size_t len, i;
    Cell c;

    *used = 1;
    if (s[0] < 0x80) {
        return s[0];
    }
    len = s[0] >= 0xF0 && s[0] < 0xF8 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 0;
    if (len == 0 || len > n || (s[0] >= 0xF8)) {
        return UTF8_REPLACEMENT;
    }
    c = s[0] & (0x7F >> len);
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return UTF8_REPLACEMENT;
        }
        c = c << 6 | (s[i] & 0x3F);
    }
    if (c < least[len - 1] || c > UNICODE_MAX || (c >= SURROGATE_FIRST && c <= SURROGATE_LAST)) {
        return UTF8_REPLACEMENT;
    }
    *used = len;
    return c;
}

/* c's bytes into out (4 at most): how many; 0 if c is no character */
static int utf8_encode(Cell c, uint8_t *out) {
    if (c < 0 || c > UNICODE_MAX || (c >= SURROGATE_FIRST && c <= SURROGATE_LAST)) {
        return 0;
    }
    if (c < 0x80) {
        out[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (uint8_t)(0xC0 | c >> 6);
        out[1] = (uint8_t)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (uint8_t)(0xE0 | c >> 12);
        out[1] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
        out[2] = (uint8_t)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | c >> 18);
    out[1] = (uint8_t)(0x80 | (c >> 12 & 0x3F));
    out[2] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
    out[3] = (uint8_t)(0x80 | (c & 0x3F));
    return 4;
}

static const uint8_t *text_arg2(Forth *f, const char *word, size_t *len) {   /* ( a n -- ) */
    Cell n = POP(), a = POP();

    return (const uint8_t *)text_at(f, a, n, word, len);
}

PRIM(p_utf8_length) {   /* utf8-length ( a n -- count ) its characters */
    size_t len, used;
    const uint8_t *s = text_arg2(f, "utf8-length", &len);
    Cell count = 0;

    forth_worked(f, len, WORK_BYTES);
    for (; len > 0; s += used, len -= used, count++) {
        (void)utf8_decode(s, len, &used);
    }
    PUSH(count);
}
PRIM(p_utf8_next) {   /* utf8-next ( a n -- c a' n' ) its first character, and the rest */
    size_t len, used;
    const uint8_t *s = text_arg2(f, "utf8-next", &len);
    Cell c;

    if (len == 0) {
        forth_error(f, "utf8-next: the string is empty");
    }
    c = utf8_decode(s, len, &used);
    PUSH(c);
    PUSH((Cell)(s + used));
    PUSH((Cell)(len - used));
}
PRIM(p_utf8_valid) {   /* utf8-valid? ( a n -- flag ) no byte out of place */
    size_t len, used;
    const uint8_t *s = text_arg2(f, "utf8-valid?", &len);
    int ok = 1;

    forth_worked(f, len, WORK_BYTES);
    for (; ok && len > 0; s += used, len -= used) {
        ok = utf8_decode(s, len, &used) != UTF8_REPLACEMENT || (used == 3 && s[0] == 0xEF);   /* (U+FFFD itself) */
    }
    PUSH(ok ? -1 : 0);
}
/* ---- byte runs counted, for a parser that would loop a byte at a time (json's): how many leading bytes are ... ---- */
static size_t run_of(Forth *f, const char *word, int (*in_run)(uint8_t)) {   /* ( a n -- count ) */
    size_t len, i;
    const uint8_t *s = text_arg2(f, word, &len);

    for (i = 0; i < len && in_run(s[i]); i++) {
    }
    forth_worked(f, i, WORK_BYTES);
    return i;
}
static int plain_byte(uint8_t c) { return c != '"' && c != '\\' && c >= 32; }
static int blank_byte(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int digit_byte(uint8_t c) { return c >= '0' && c <= '9'; }
PRIM(p_scan_plain) { PUSH(run_of(f, "scan-plain", plain_byte)); }    /* ( a n -- n' ) not " \ nor a control byte */
PRIM(p_skip_blanks) { PUSH(run_of(f, "skip-blanks", blank_byte)); }  /* ( a n -- n' ) space, tab, LF, CR */
PRIM(p_scan_digits) { PUSH(run_of(f, "scan-digits", digit_byte)); }  /* ( a n -- n' ) 0-9 */
/* ---- JSON read (the json vocabulary's json>): the text into the language's objects - an object a map, an array a
 * list, a string a string, a whole number a cell, true false and null the vocabulary's three, a number with a
 * fraction or an exponent (or past a cell) what its word makes of a float. In Forth it took 250 instructions a byte;
 * what it reads here it made there, and says the same of text that isn't JSON: what was expected, at which byte.
 * The objects are made in the region that runs (json>'s scratch region: an error lets them go with it). ---- */

#define JSON_DEPTH_HARD_MAX 512     /* arrays and objects inside each other, at most, whatever json-depth-max says
                                     * (each is a call here, on the task's own stack) */
#define JSON_READ_DEPTH_MAX 40      /* ... and read, at most: what is read is copied and freed by words of Forth's
                                     * (json-copy, json-free: 42 deep fits a task's return stack), and kept inside
                                     * the program's own lists and maps (objects are moved OBJECT_DEPTH_MAX deep) */
#define JSON_CELL_DIGITS 19         /* a whole number of more digits is past a cell: a float */
#define JSON_BREATH_BYTES 1024    /* read between one look at the clock and the next: what the reading took is counted
                                   * against the handler's slice (a tenth of a slice's time, or so) */
#define JSON_NUMBER_BYTES 1025      /* a number's text that is read as a float, and the 0 after it: no longer */
#define HIGH_SURROGATE 0xD800
#define LOW_SURROGATE 0xDC00
#define SURROGATE_SPAN 0x400
#define PAST_BMP 0x10000

typedef struct JsonIn {
    Forth *f;
    const uint8_t *s;
    size_t n, pos;
    int depth, depth_max;
    Cell null, yes, no;      /* json-null, json-true, json-false */
    Word *make_float;        /* <json-float> ( F: x -- v ) */
    size_t counted;          /* the bytes read when the clock was looked at last ... */
    long long since;         /* ... and its time then */
} JsonIn;

static Cell json_value(JsonIn *j);

static void json_fail(JsonIn *j, const char *what) {
    forth_error(j->f, "json: %s at byte %zu", what, j->pos);
}
static int json_peek(const JsonIn *j) { return j->pos < j->n ? j->s[j->pos] : -1; }   /* -1 at the end */
static void json_blanks(JsonIn *j) {
    while (j->pos < j->n && blank_byte(j->s[j->pos])) {
        j->pos++;
    }
}
static void json_expect(JsonIn *j, char c) {
    json_blanks(j);
    if (json_peek(j) != c) {
        forth_error(j->f, "json: '%c' expected at byte %zu", c, j->pos);
    }
    j->pos++;
}
static void json_word(JsonIn *j, const char *word, Cell *v, Cell is) {   /* the literal's letters there: its value */
    size_t m = strlen(word);

    if (j->n - j->pos < m || memcmp(j->s + j->pos, word, m) != 0) {
        json_fail(j, "a value expected");
    }
    j->pos += m;
    *v = is;
}
static Cell json_hex4(JsonIn *j) {   /* \uXXXX's four digits */
    Cell u = 0;
    int k;

    for (k = 0; k < 4; k++) {
        int c = json_peek(j), lower = c | 32;

        if (c >= '0' && c <= '9') {
            u = u << 4 | (c - '0');
        } else if (c >= 0 && lower >= 'a' && lower <= 'f') {
            u = u << 4 | (lower - 'a' + 10);
        } else {
            json_fail(j, "\\u wants four hex digits");
        }
        j->pos++;
    }
    return u;
}
/* what comes after a backslash, as bytes (out: where to put them, NULL to only count them): how many */
static size_t json_escape(JsonIn *j, uint8_t *out) {
    int c = json_peek(j);
    uint8_t u8[4];
    Cell u;
    int k;

    j->pos++;
    switch (c) {
    case '"': case '\\': case '/': u8[0] = (uint8_t)c; break;
    case 'b': u8[0] = 8; break;
    case 'f': u8[0] = 12; break;
    case 'n': u8[0] = 10; break;
    case 'r': u8[0] = 13; break;
    case 't': u8[0] = 9; break;
    case 'u':
        u = json_hex4(j);
        if (u >= HIGH_SURROGATE && u < LOW_SURROGATE) {   /* (a pair: the low half next) */
            Cell lo;

            if (json_peek(j) != '\\') {
                json_fail(j, "a surrogate's other half expected");
            }
            j->pos++;
            if (json_peek(j) != 'u') {
                json_fail(j, "a surrogate's other half expected");
            }
            j->pos++;
            lo = json_hex4(j);
            if (lo < LOW_SURROGATE || lo >= LOW_SURROGATE + SURROGATE_SPAN) {
                json_fail(j, "a surrogate's other half expected");
            }
            u = ((u - HIGH_SURROGATE) << 10 | (lo - LOW_SURROGATE)) + PAST_BMP;
        }
        if ((k = utf8_encode(u, u8)) == 0) {   /* (a low half alone) */
            forth_error(j->f, "json: \\u%04lx is no character (half of a pair, alone) at byte %zu", (long)u, j->pos);
        }
        if (out != NULL) {
            memcpy(out, u8, (size_t)k);
        }
        return (size_t)k;
    default:
        json_fail(j, "a known escape expected");
    }
    if (out != NULL) {
        out[0] = u8[0];
    }
    return 1;
}
/* a string's bytes, after its opening quote and to its closing one (taken): how many they are, written to out if
 * there is one. Read twice - counted (what isn't a string said), then written into a string of that length. */
static size_t json_text(JsonIn *j, uint8_t *out) {
    size_t len = 0;

    for (;;) {
        size_t k = 0;
        int c;

        while (j->pos + k < j->n && plain_byte(j->s[j->pos + k])) {
            k++;
        }
        if (out != NULL) {
            memcpy(out + len, j->s + j->pos, k);
        }
        len += k;
        j->pos += k;
        c = json_peek(j);
        if (c == '"') {
            j->pos++;
            return len;
        }
        if (c < 0) {
            json_fail(j, "the string's end expected");
        }
        if (c < 32) {
            json_fail(j, "a control character in a string");
        }
        j->pos++;   /* (the backslash) */
        len += json_escape(j, out != NULL ? out + len : NULL);
    }
}
/* a string, after its opening quote. *plain: its bytes are the text's own, so many (no object made: an object's
 * key); else a string object */
static char *json_string(JsonIn *j, size_t *len, int key) {
    size_t start = j->pos, k = 0;
    char *s;

    while (start + k < j->n && plain_byte(j->s[start + k])) {
        k++;
    }
    if (start + k < j->n && j->s[start + k] == '"') {   /* (most strings: no escape - made from the bytes at once) */
        j->pos = start + k + 1;
        *len = k;
        return key ? (char *)(uintptr_t)(j->s + start) : string_new(j->f, (const char *)j->s + start, k);
    }
    *len = json_text(j, NULL);
    s = string_alloc(j->f, *len);
    j->pos = start;
    (void)json_text(j, (uint8_t *)s);
    return s;
}
static void json_digits(JsonIn *j) {   /* one or more */
    size_t k = 0;

    while (j->pos + k < j->n && digit_byte(j->s[j->pos + k])) {
        k++;
    }
    if (k == 0) {
        json_fail(j, "a digit expected");
    }
    j->pos += k;
}
/* a number's text - JSON's grammar, seen to already - as a float (forth_parse_float is the Forth's own: it takes 63
 * characters, and digits alone are no float to it) */
static Cell json_float(JsonIn *j, const char *text, size_t n) {
    Forth *f = j->f;
    char buf[JSON_NUMBER_BYTES], *end;
    Float x;

    if (n >= sizeof(buf)) {   /* (strtod reads every digit: a megabyte of them would be the node's for seconds) */
        json_fail(j, "a number of too many characters");
    }
    memcpy(buf, text, n);
    buf[n] = 0;
    x = strtod(buf, &end);
    if (end != buf + n) {
        json_fail(j, "a number expected");
    }
    if (isinf(x)) {   /* (1e999: no float holds it - and written back it would be null) */
        json_fail(j, "a number too big for a float");
    }
    FPUSH(x);
    j->make_float->code(f, j->make_float);
    return POP();
}
static Cell json_number(JsonIn *j) {
    static const char cell_max[] = "9223372036854775807", cell_min[] = "9223372036854775808";
    size_t start = j->pos, len, digits, i;
    const char *a = (const char *)j->s + start;
    int is_float = 0, sign;
    UCell v = 0;

    if (json_peek(j) == '-') {
        j->pos++;
    }
    if (json_peek(j) == '0') {
        j->pos++;
        if (json_peek(j) >= '0' && json_peek(j) <= '9') {
            json_fail(j, "no leading zeros");
        }
    } else {
        json_digits(j);
    }
    if (json_peek(j) == '.') {
        j->pos++;
        json_digits(j);
        is_float = 1;
    }
    if (json_peek(j) == 'e' || json_peek(j) == 'E') {
        j->pos++;
        if (json_peek(j) == '+' || json_peek(j) == '-') {
            j->pos++;
        }
        json_digits(j);
        is_float = 1;
    }
    len = j->pos - start;
    sign = a[0] == '-';
    digits = len - (size_t)sign;
    if (is_float) {
        return json_float(j, a, len);
    }
    if (digits > JSON_CELL_DIGITS ||
        (digits == JSON_CELL_DIGITS && memcmp(a + sign, sign ? cell_min : cell_max, JSON_CELL_DIGITS) > 0)) {
        return json_float(j, a, len);   /* (digits past a cell: a float) */
    }
    for (i = (size_t)sign; i < len; i++) {
        v = v * 10 + (UCell)(a[i] - '0');
    }
    /* (a cell with a reference's mark may be an object's - to every word that walks a tree: >json would write its
     *  text, json-free let it go. No number read from text is let have the mark: one that would - 4.6 to 5.8
     *  million million million - is a float, as one past a cell is. So no number a stranger sends is a reference) */
    if (!sign && FORTH_IS_REF(v)) {
        return json_float(j, a, len);
    }
    return (Cell)(sign ? (UCell)0 - v : v);
}
/* a long text read doesn't hold its node: after a list's item or a map's entry, every so many bytes, the time they
 * took is counted against the handler's slice - and, the slice spent, the task gives way here and goes on at its
 * next turn (what is read so far is its own objects: nothing half made is left to another) */
static void json_breathe(JsonIn *j) {
    if (j->pos - j->counted >= JSON_BREATH_BYTES) {
        j->counted = j->pos;
        if (forth_give_way(j->f, forth_work_steps(&j->since))) {
            (void)forth_work_steps(&j->since);   /* (others have had turns: their time isn't this reading's) */
        }
    }
}
static void json_nested(JsonIn *j) {
    if (++j->depth > j->depth_max) {
        json_fail(j, "nested too deeply");
    }
}
static Cell json_array(JsonIn *j) {   /* after the [ */
    List *l;

    json_nested(j);
    l = new_list(j->f);
    json_blanks(j);
    if (json_peek(j) == ']') {
        j->pos++;
    } else {
        for (;;) {
            list_add(j->f, l, json_value(j));
            json_breathe(j);
            json_blanks(j);
            if (json_peek(j) != ',') {
                json_expect(j, ']');
                break;
            }
            j->pos++;
        }
    }
    j->depth--;
    return REF(l);
}
static Cell json_object(JsonIn *j) {   /* after the { */
    Map *m;

    json_nested(j);
    m = object_new(j->f, OBJ_MAP, sizeof(Map));
    json_blanks(j);
    if (json_peek(j) == '}') {
        j->pos++;
    } else {
        for (;;) {
            const char *key;
            size_t len;
            Cell v;

            json_expect(j, '"');
            key = json_string(j, &len, 1);
            json_expect(j, ':');
            v = json_value(j);
            map_put_unpacked(j->f, m, 1, key, len, 0, v);
            json_breathe(j);
            json_blanks(j);
            if (json_peek(j) != ',') {
                json_expect(j, '}');
                break;
            }
            j->pos++;
        }
    }
    j->depth--;
    return REF(m);
}
static Cell json_value(JsonIn *j) {
    Cell v = 0;
    size_t len;
    int c;

    json_blanks(j);
    c = json_peek(j);
    j->pos += c == '{' || c == '[' || c == '"';
    switch (c) {
    case '{': return json_object(j);
    case '[': return json_array(j);
    case '"': return REF(json_string(j, &len, 0));
    case 't': json_word(j, "true", &v, j->yes); return v;
    case 'f': json_word(j, "false", &v, j->no); return v;
    case 'n': json_word(j, "null", &v, j->null); return v;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            return json_number(j);
        }
        json_fail(j, "a value expected");
    }
    return v;
}
/* (json>) ( addr len null true false float-xt depth-max -- v ) the JSON text read: its value, json's three literals
 * and its word that makes a float's object given (the vocabulary's own: json> gives them) */
PRIM(p_json_read) {
    JsonIn j;
    Cell max = POP(), v;
    size_t len;

    memset(&j, 0, sizeof(j));
    j.f = f;
    j.make_float = forth_xt_arg(f, POP(), "(json>)");
    j.no = POP();
    j.yes = POP();
    j.null = POP();
    j.s = text_arg2(f, "(json>)", &len);
    j.n = len;
    if (len > JSON_BREATH_BYTES && forth_may_give_way(f)) {
        /* (it may give way as it reads, and others' handlers run: a copy of its own is read - the text given may
         *  be let go by another meanwhile. Freed with the scratch json> reads in) */
        j.s = (const uint8_t *)string_new(f, (const char *)j.s, len);
    }
    j.depth_max = max < 0 ? 0 : max > JSON_READ_DEPTH_MAX ? JSON_READ_DEPTH_MAX : (int)max;
    (void)forth_work_steps(&j.since);
    v = json_value(&j);
    json_blanks(&j);
    if (j.pos < j.n) {
        json_fail(&j, "the text's end expected");
    }
    forth_slice_charge(f, forth_work_steps(&j.since));
    PUSH(v);
}

/* ---- JSON write (the json vocabulary's >json): the language's objects as JSON's text - a map an object (its keys
 * strings; a number's its digits), a list an array, a string a string (what isn't UTF-8 in it as U+FFFD: what comes
 * out is JSON, and a browser's), bytes "0x" and their hex, a whole number its digits, json's float its shortest
 * exact digits, its three literals their names. Written into a byte buffer of the task's own, giving way as a long
 * value is written (json_out_breathe): the value is asked for anew at each item, since another's turn may have let
 * it go. ---- */
#define JSON_OUT_BREATH_BYTES 1024   /* written between one look at the clock and the next */
#define JSON_STRING_BREATH 16384     /* ... of one long string */
#define JSON_FLOAT_BYTES 40          /* a float's digits, at most (%.17g: 24) */
#define JSON_CELL_BYTES 24           /* a cell's (20, and the sign) */

typedef struct JsonOut {
    Forth *f;
    Bytes *b;                /* what is written so far: an object of the task's own (it lasts a give-way) */
    Cell null, yes, no;      /* json-null, json-true, json-false */
    const TupleClass *float_class;   /* json-float */
    size_t counted;          /* the bytes written when the clock was looked at last ... */
    long long since;         /* ... and its time then */
} JsonOut;

static void json_out(JsonOut *j, const char *s, size_t n) {
    if (n == 0) {   /* (nothing: and no bytes yet, perhaps, to copy none onto) */
        return;
    }
    bytes_room(j->f, j->b, (Cell)n);
    memcpy(j->b->p + j->b->n, s, n);
    j->b->n += (Cell)n;
}
static void json_out_byte(JsonOut *j, char c) { json_out(j, &c, 1); }
/* what can't be written: said, what was written so far let go first (at the prompt nothing else would) */
static _Noreturn void json_out_fail(JsonOut *j, const char *what) {
    ObjectSlot *sl = object_slot(j->f, j->b);

    if (sl != NULL) {
        object_free(j->f, sl);
    }
    forth_error(j->f, ">json: %s", what);
}
/* (a long value written doesn't hold its node: what the writing took is counted against the handler's slice, and
 *  - the slice spent - the task gives way here, between two items. What it holds across that is its own buffer) */
static void json_out_breathe(JsonOut *j) {
    if ((size_t)j->b->n - j->counted >= JSON_OUT_BREATH_BYTES) {
        j->counted = (size_t)j->b->n;
        if (forth_give_way(j->f, forth_work_steps(&j->since))) {
            (void)forth_work_steps(&j->since);   /* (others have had turns: their time isn't this writing's) */
        }
    }
}
/* v's block, v an object of this kind still (it was when its writing began: another's turn may have let it go) */
static void *json_out_still(JsonOut *j, Cell v, int kind) {
    const ObjectSlot *sl = object_of(j->f, v);

    if (sl == NULL || sl->kind != kind) {
        json_out_fail(j, "a list or a map let go as it was written (another actor's, not copied first?)");
    }
    return sl->at;
}
/* a string object's text between quotes: " \\ and the control characters as JSON writes them, a byte that is no
 * UTF-8 as U+FFFD; a long one a piece at a time */
static void json_out_string(JsonOut *j, Cell v) {
    static const char hex[] = "0123456789abcdef";
    size_t i = 0, next = JSON_STRING_BREATH;

    json_out_byte(j, '"');
    for (;;) {
        const ObjectSlot *sl = object_of(j->f, v);
        const uint8_t *s;
        size_t n, run;

        if (sl == NULL || !text_kind(sl->kind) || (sl->kind == OBJ_SLICE && slice_bytes(j->f, sl) == NULL)) {
            json_out_fail(j, "a string let go as it was written (another actor's, not copied first?)");
        }
        s = seq_bytes(sl);
        n = sl->size;
        for (; i < n && i < next; i += run) {
            uint8_t c = s[i];

            for (run = 0; i + run < n && s[i + run] < 0x80 && plain_byte(s[i + run]); run++) {
            }
            if (run > 0) {   /* (most of most strings: as they are) */
                json_out(j, (const char *)s + i, run);
            } else if (c >= 0x80) {
                Cell ch = utf8_decode(s + i, n - i, &run);

                if (ch == UTF8_REPLACEMENT && !(run == 3 && c == 0xEF)) {   /* (a bad byte: one at a time, replaced) */
                    json_out(j, "\xEF\xBF\xBD", 3);
                    run = 1;
                } else {
                    json_out(j, (const char *)s + i, run);
                }
            } else {
                char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};

                run = 1;
                switch (c) {
                case '"': json_out(j, "\\\"", 2); break;
                case '\\': json_out(j, "\\\\", 2); break;
                case '\n': json_out(j, "\\n", 2); break;
                case '\r': json_out(j, "\\r", 2); break;
                case '\t': json_out(j, "\\t", 2); break;
                default: json_out(j, esc, sizeof(esc));
                }
            }
        }
        if (i >= n) {
            break;
        }
        next = i + JSON_STRING_BREATH;
        json_out_breathe(j);
    }
    json_out_byte(j, '"');
}
static void json_out_number(JsonOut *j, Cell n) {   /* its digits, in decimal */
    char buf[JSON_CELL_BYTES], *end = buf + sizeof(buf), *p = end;
    UCell u = n < 0 ? (UCell)0 - (UCell)n : (UCell)n;

    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (n < 0) {
        *--p = '-';
    }
    json_out(j, p, (size_t)(end - p));
}
/* a float: the fewest digits that read back the same (as f>exact), a .0 after those that would read as a whole
 * number; what JSON has no number for (an infinity, not a number) null */
static void json_out_float(JsonOut *j, const Cell *t) {
    char buf[JSON_FLOAT_BYTES];
    Float x;
    int n;

    memcpy(&x, &t[1], sizeof(x));
    if (x - x != 0) {
        json_out(j, "null", 4);
        return;
    }
    n = snprintf(buf, sizeof(buf), "%.15g", (double)x);
    if (strtod(buf, NULL) != (double)x) {
        n = snprintf(buf, sizeof(buf), "%.17g", (double)x);
    }
    json_out(j, buf, (size_t)n);
    if (memchr(buf, '.', (size_t)n) == NULL && memchr(buf, 'e', (size_t)n) == NULL) {
        json_out(j, ".0", 2);
    }
}
static void json_out_value(JsonOut *j, Cell v, int depth) {
    static const char hex[] = "0123456789abcdef";
    Forth *f = j->f;
    const ObjectSlot *sl;
    Cell i;
    int kind, first = 1;

    if (v == j->null || v == j->yes || v == j->no) {
        const char *name = v == j->null ? "null" : v == j->yes ? "true" : "false";

        json_out(j, name, strlen(name));
        return;
    }
    if ((sl = object_of(f, v)) == NULL) {
        json_out_number(j, v);
        return;
    }
    kind = sl->kind;
    if (depth > JSON_DEPTH_HARD_MAX) {
        json_out_fail(j, "lists and maps inside each other deeper than 512 (one that holds itself?)");
    }
    if (text_kind(kind)) {
        json_out_string(j, v);
    } else if (kind == OBJ_LIST) {
        json_out_byte(j, '[');
        for (i = 0;; i++) {
            const List *l = json_out_still(j, v, OBJ_LIST);

            if (i >= l->n) {
                break;
            }
            if (i > 0) {
                json_out_byte(j, ',');
            }
            json_out_value(j, l->items[i], depth + 1);
            json_out_breathe(j);
        }
        json_out_byte(j, ']');
    } else if (kind == OBJ_MAP) {
        json_out_byte(j, '{');
        for (i = 0;; i++) {
            const Map *m = json_out_still(j, v, OBJ_MAP);
            Cell value;

            if (i >= m->n) {
                break;
            }
            if (!m->e[i].live) {
                continue;
            }
            if (!first) {
                json_out_byte(j, ',');
            }
            first = 0;
            value = m->e[i].value;
            if (m->e[i].text) {
                json_out_string(j, m->e[i].key);
            } else {   /* (a number's key: its digits, as a string) */
                json_out_byte(j, '"');
                json_out_number(j, m->e[i].key);
                json_out_byte(j, '"');
            }
            json_out_byte(j, ':');
            json_out_value(j, value, depth + 1);
            json_out_breathe(j);
        }
        json_out_byte(j, '}');
    } else if (kind == OBJ_BYTES) {   /* (a binary column's: "0x" and their hex, as SQL Server writes them) */
        const Bytes *b = sl->at;

        json_out(j, "\"0x", 3);
        bytes_room(f, j->b, b->n * 2 + 1);
        for (i = 0; i < b->n; i++) {
            j->b->p[j->b->n++] = (uint8_t)hex[b->p[i] >> 4];
            j->b->p[j->b->n++] = (uint8_t)hex[b->p[i] & 15];
        }
        json_out_byte(j, '"');
    } else {   /* (a tuple) */
        const Cell *t = sl->at;

        if (!is_a((const TupleClass *)t[0], j->float_class)) {
            json_out_fail(j, "a tuple that isn't JSON's");
        }
        json_out_float(j, t);
    }
}
/* (>json) ( v null true false float-class -- s len ) the value as JSON's text, a new string: json's three literals
 * and its float's class given (the vocabulary's own: >json gives them) */
PRIM(p_json_write) {
    JsonOut j;
    Cell v, out;
    char *s;

    memset(&j, 0, sizeof(j));
    j.f = f;
    j.float_class = class_arg(f, POP(), "(>json)");
    j.no = POP();
    j.yes = POP();
    j.null = POP();
    v = POP();
    j.b = object_new(f, OBJ_BYTES, sizeof(Bytes));
    out = REF(j.b);
    (void)forth_work_steps(&j.since);
    json_out_value(&j, v, 0);
    forth_slice_charge(f, forth_work_steps(&j.since));
    s = string_new(f, j.b->p != NULL ? (const char *)j.b->p : "", (size_t)j.b->n);
    PUSH(REF(s));
    PUSH(j.b->n);
    object_free(f, object_of(f, out));
}

/* utf8-clean ( a n -- s len ) the text with each byte out of place as U+FFFD - what a browser takes as it is (a JSON
 * string, a WebSocket's text: one bad byte there fails the whole socket) - a new string */
PRIM(p_utf8_clean) {
    size_t len, used;
    const uint8_t *s = text_arg2(f, "utf8-clean", &len);
    uint8_t *out = malloc(len * 3 + 1), *o = out;   /* (each bad byte three, at most) */

    forth_worked(f, len, WORK_BYTES);
    if (out == NULL) {
        forth_error(f, "utf8-clean: out of memory");
    }
    for (; len > 0; s += used, len -= used) {
        Cell c = utf8_decode(s, len, &used);

        if (c == UTF8_REPLACEMENT && !(used == 3 && s[0] == 0xEF)) {   /* (a bad byte: one at a time, replaced) */
            used = 1;
            *o++ = 0xEF;
            *o++ = 0xBF;
            *o++ = 0xBD;
        } else {
            memcpy(o, s, used);
            o += used;
        }
    }
    push_string(f, (const char *)out, (size_t)(o - out));
    free(out);
}
PRIM(p_utf8_slice) {   /* utf8-slice ( a n i len -- a' n' ) its len characters from character i (kept to it) */
    Cell k = POP(), i = POP();
    size_t len, used, at = 0, from;
    const uint8_t *s = text_arg2(f, "utf8-slice", &len);
    Cell count;

    for (count = 0; count < i && at < len; count++, at += used) {
        (void)utf8_decode(s + at, len - at, &used);
    }
    from = at;
    for (count = 0; count < k && at < len; count++, at += used) {
        (void)utf8_decode(s + at, len - at, &used);
    }
    PUSH((Cell)(s + from));
    PUSH((Cell)(at - from));
}
static int char_bytes(Forth *f, Cell c, uint8_t *out, const char *word) {
    int n = utf8_encode(c, out);

    if (n == 0) {
        forth_error(f, "%s: %ld is no character (0 .. $10FFFF, not a surrogate)", word, (long)c);
    }
    return n;
}
PRIM(p_char_to_string) {   /* char>string ( c -- s len ) its UTF-8, a new string */
    uint8_t b[4];
    int n = char_bytes(f, POP(), b, "char>string");

    push_string(f, (const char *)b, (size_t)n);
}
PRIM(p_b_push_char) {   /* b-push-char ( c b -- ) its UTF-8 after the bytes */
    Bytes *b = bytes_arg(f, POP(), "b-push-char");
    uint8_t u[4];
    int n = char_bytes(f, POP(), u, "b-push-char");

    bytes_room(f, b, n);
    memcpy(b->p + b->n, u, (size_t)n);
    b->n += n;
}

/* ---- format ( args a n -- s len ): the text with each {} replaced by an argument, taken from the stack in order
 * (the first the deepest). {} or {d} a number, {u} unsigned, {x} {X} hex, {c} a character (UTF-8), {s} a string
 * (addr len: two cells), {f} a float (from the float stack); before the letter, - (to the left), 0 (zeros), a
 * width, .precision (a float's digits). {{ and }} are braces. ---- */

#define FORMAT_WIDTH_MAX 256   /* a width or precision no larger (the text's room is counted as it is made) */

typedef struct Spec {
    int left, zeros, width, precision;   /* (precision -1: none) */
    char kind;
} Spec;

/* the spec after a {, up to its } (p past it); 0 if it isn't one */
static int format_spec(const char **p, const char *end, Spec *sp) {
    const char *q = *p;

    memset(sp, 0, sizeof(*sp));
    sp->precision = -1;
    if (q < end && *q == '-') {
        sp->left = 1;
        q++;
    }
    if (q < end && *q == '0') {
        sp->zeros = 1;
        q++;
    }
    while (q < end && isdigit((unsigned char)*q) && sp->width < FORMAT_WIDTH_MAX) {
        sp->width = sp->width * 10 + (*q++ - '0');
    }
    if (q < end && *q == '.') {
        q++;
        sp->precision = 0;
        while (q < end && isdigit((unsigned char)*q) && sp->precision < FORMAT_WIDTH_MAX) {
            sp->precision = sp->precision * 10 + (*q++ - '0');
        }
    }
    sp->kind = q < end && strchr("dusxXcf", *q) != NULL && *q != 0 ? *q++ : 'd';
    if (q >= end || *q != '}' || sp->width > FORMAT_WIDTH_MAX || sp->precision > FORMAT_WIDTH_MAX) {
        return 0;
    }
    *p = q + 1;
    return 1;
}

/* the text made (out NULL: only measured - and its arguments checked, so an error leaves nothing to free); its
 * length. args: the cells, in order; floats likewise. */
static size_t format_text(Forth *f, const char *fmt, size_t n, const Cell *args, const Float *floats, char *out) {
    const char *p = fmt, *end = fmt + n;
    size_t len = 0;
    int ai = 0, fi = 0;
    char piece[2 * FORMAT_WIDTH_MAX + 64], spec[32], digits[NUMBER_TEXT_MAX];

    while (p < end) {
        Spec sp;
        size_t k;
        const char *body = piece;

        if ((*p == '{' || *p == '}') && p + 1 < end && p[1] == *p) {   /* {{ }} */
            if (out != NULL) {
                out[len] = *p;
            }
            len++;
            p += 2;
            continue;
        }
        if (*p != '{') {
            if (out != NULL) {
                out[len] = *p;
            }
            len++;
            p++;
            continue;
        }
        p++;
        if (!format_spec(&p, end, &sp)) {
            forth_error(f, "format: a { not closed by } (or a spec it can't read): \"%.*s\"", (int)n, fmt);
        }
        if (sp.kind == 's') {   /* (a string: any length, padded here) */
            size_t slen, pad;
            const char *str = text_at(f, args[ai], args[ai + 1], "format", &slen);

            ai += 2;
            pad = (size_t)sp.width > slen ? (size_t)sp.width - slen : 0;
            if (out != NULL) {
                memset(out + len + (sp.left ? slen : 0), ' ', pad);
                memcpy(out + len + (sp.left ? 0 : pad), str, slen);
            }
            len += slen + pad;
            continue;
        }
        if (sp.kind == 'f') {   /* (a float: the C library's to write) */
            snprintf(spec, sizeof(spec), "%%%s%s%d", sp.left ? "-" : "", sp.zeros ? "0" : "", sp.width);
            snprintf(spec + strlen(spec), sizeof(spec) - strlen(spec), ".%dg", sp.precision >= 0 ? sp.precision : 6);
            if (sp.precision >= 0) {
                spec[strlen(spec) - 1] = 'f';
            }
            k = (size_t)snprintf(piece, sizeof(piece), spec, (double)floats[fi++]);
        } else if (sp.kind == 'c') {
            uint8_t u[4];
            int cn = char_bytes(f, args[ai++], u, "format");

            memcpy(piece, u, (size_t)cn);
            k = (size_t)cn;
        } else {   /* a whole number: its sign, then - in its width - zeros before its digits, or spaces round them */
            Cell x = args[ai++];
            int negative = (sp.kind == 'd') && x < 0, hex = sp.kind == 'x' || sp.kind == 'X';
            size_t nd = number_text(digits, negative ? (UCell)0 - (UCell)x : (UCell)x, 0, hex ? HEX : DECIMAL,
                                    sp.kind == 'X');
            size_t pad = (size_t)sp.width > nd + (size_t)negative ? (size_t)sp.width - nd - (size_t)negative : 0;
            int zeros = sp.zeros && !sp.left;   /* (to the left: spaces after, as printf has it) */

            k = 0;
            if (!sp.left && !zeros) {
                memset(piece, ' ', pad);
                k = pad;
            }
            if (negative) {
                piece[k++] = '-';
            }
            if (zeros) {
                memset(piece + k, '0', pad);
                k += pad;
            }
            memcpy(piece + k, digits, nd);
            k += nd;
            if (sp.left) {
                memset(piece + k, ' ', pad);
                k += pad;
            }
        }
        if (k >= sizeof(piece)) {
            k = sizeof(piece) - 1;
        }
        if (out != NULL) {
            memcpy(out + len, body, k);
        }
        len += k;
    }
    return len;
}

PRIM(p_format) {
    Cell n = POP(), a = POP();
    size_t len, out_len;
    const char *fmt = text_at(f, a, n, "format", &len), *p = fmt, *end = fmt + len;
    int cells = 0, floats = 0;
    Task *t = f->t;
    char *out;

    while (p < end) {   /* (what it takes from the stacks) */
        Spec sp;

        if ((*p == '{' || *p == '}') && p + 1 < end && p[1] == *p) {
            p += 2;
        } else if (*p++ == '{') {
            if (!format_spec(&p, end, &sp)) {
                forth_error(f, "format: a { not closed by } (or a spec it can't read): \"%.*s\"", (int)len, fmt);
            }
            cells += sp.kind == 's' ? 2 : sp.kind == 'f' ? 0 : 1;
            floats += sp.kind == 'f';
        }
    }
    if (t->sp - t->sfloor < cells) {
        forth_error(f, "format: %d cells for its {}s, and the stack has %d", cells, t->sp - t->sfloor);
    }
    if (t->fp - t->ffloor < floats) {
        forth_error(f, "format: %d floats for its {f}s, and the float stack has %d", floats, t->fp - t->ffloor);
    }
    out_len = format_text(f, fmt, len, &t->ds[t->sp - cells], &t->fs[t->fp - floats], NULL);
    out = string_alloc(f, out_len);
    format_text(f, fmt, len, &t->ds[t->sp - cells], &t->fs[t->fp - floats], out);
    t->sp -= cells;
    t->fp -= floats;
    PUSH(REF(out));
    PUSH((Cell)out_len);
}

Cell forth_new_string(Forth *f, const char *s, size_t len) { return REF(string_new(f, s, len)); }
Cell forth_new_string_kept(Forth *f, const char *s, size_t len) {
    int region = f->t->region;
    char *p;

    f->t->region = 0;   /* (made in the system's region, whatever region runs) */
    p = string_new(f, s, len);
    f->t->region = region;
    return REF(p);
}
Cell forth_new_list(Forth *f) { return REF(new_list(f)); }
void forth_list_push(Forth *f, Cell l, Cell x) { list_add(f, object_arg(f, l, OBJ_LIST, "list"), x); }
Cell forth_new_bytes(Forth *f, const void *data, size_t n) {
    Bytes *b = object_new(f, OBJ_BYTES, sizeof(Bytes));

    bytes_room(f, b, (Cell)n);
    if (n > 0) {
        memcpy(b->p, data, n);
    }
    b->n = (Cell)n;
    return REF(b);
}
const uint8_t *forth_bytes_of(Forth *f, Cell x, size_t *n, const char *word) {
    Bytes *b = bytes_arg(f, x, word);

    *n = (size_t)b->n;
    return b->p != NULL ? b->p : (const uint8_t *)"";
}

void forth_objects_words(Forth *f) {
    static const ForthPrim prims[] = {
        {"list", p_list, "( -- l )"}, {"list-free", p_list_free, "( l -- )"}, {"push", p_push, "( x seq -- )"},
        {"pop", p_pop, "( seq -- x )"}, {"nth", p_nth, "( i seq -- x )"}, {"nth!", p_nth_store, "( x i seq -- )"},
        {"length", p_length, "( seq -- n )"}, {"list-clear", p_list_clear, "( l -- )"}, {"{", p_brace_open, NULL},
        {"clear", p_clear, "( seq -- )"}, {"like", p_like, "( seq exemplar -- seq' )"},
        {"(like-acc)", p_like_acc, "( seq -- acc )"}, {"(like-done)", p_like_done, "( acc seq -- seq' )"},
        {"append", p_append, "( a b -- c )"}, {"append!", p_append_store, "( a b -- a )"},
        {"be>", p_be_number, "( seq from to -- u )"}, {"le>", p_le_number, "( seq from to -- u )"},
        {"seq-xor", p_seq_xor, "( seq key -- seq' )"},
        {"b-append-seq", p_b_append_seq, "( seq from to b -- )"},
        {"span", p_span, "( seq table from -- i )"}, {"digits>number", p_digits_to_number, "( seq from to base -- u true | false )"},
        {"<slice>", p_slice, "( from to seq -- slice )"}, {"head-slice", p_head_slice, "( seq n -- slice )"},
        {"tail-slice", p_tail_slice, "( seq n -- slice )"}, {"slice?", p_slice_q, "( x -- flag )"},
        {"subseq", p_subseq, "( seq from to -- seq' )"}, {"reverse", p_reverse, "( seq -- seq' )"},
        {"index", p_index, "( x seq -- i )"}, {"translate", p_translate, "( seq table -- seq' )"},
        {"regroup", p_regroup, "( seq from to -- seq' )"}, {"subseq-index", p_subseq_index, "( seq sub -- i )"},
        {"subseq-index-from", p_subseq_index_from, "( seq sub from -- i )"}, {"seq=", p_seq_equal, "( a b -- flag )"},
        {"(build-open)", p_build_open, "( -- region )"}, {"(build-close)", p_build_close, "( region -- s )"},
        {"%", p_pct, "( seq -- )"}, {"%s", p_pct_s, "( a n -- )"}, {"%n", p_pct_n, "( n -- )"}, {"%c", p_pct_c, "( c -- )"},
        {"build-mark@", p_build_mark, "( -- x )"}, {"build-mark!", p_build_mark_store, "( x -- )"},
        {"build-at", p_build_at, "( -- b n )"},
        {"}", p_brace_close, NULL}, {"dispose", p_dispose, "( obj -- )"}, {"rescue", p_rescue, "( obj -- obj )"},
        {"rescue-all", p_rescue_all, "( obj -- obj )"}, {"rescue-handler", p_rescue_handler, "( obj -- obj )"}, {"permanent", p_permanent, "( obj -- obj )"}, {"object?", p_object_q, "( x -- flag )"},
        {"objects", p_objects, "( -- n )"}, {".objects", p_dot_objects, "( -- )"}, {"timed", p_timed, "( xt -- )"},
        {"(scratch-open)", p_scratch_open, "( -- region )"}, {"(scratch-close)", p_scratch_close, "( region -- )"},
        {"TUPLE:", p_tuple, "( -- )"}, {"GENERIC:", p_generic, "( -- )"},
        {"M:", p_method, "( -- )"}, {"class-of", p_class_of, "( obj -- class )"},
        {"instance?", p_instance, "( x class -- flag )"}, {".tuple", p_dot_tuple, "( obj -- )"},
        {"clone", p_clone, "( obj -- obj' )"}, {">string", p_to_string, "( addr len -- s len )"},
        {"string", p_string, "( s -- addr len )"}, {"string?", p_string_q, "( x -- flag )"}, {"list?", p_list_q, "( x -- flag )"},
        {"map?", p_map_q, "( x -- flag )"}, {"bytes?", p_bytes_q, "( x -- flag )"},
        {"s+", p_s_plus, "( a1 n1 a2 n2 -- a n )"}, {"n>s", p_n_to_s, "( n -- a len )"},
        {"f>string", p_f_to_string, "( -- a len ) ( F: x -- )"}, {"f>exact", p_f_to_exact, "( -- a len ) ( F: x -- )"},
        {"string>number", p_string_to_number, "( a len -- n flag )"},
        {"string>float", p_string_to_float, "( a len -- flag ) ( F: -- F: x )"},
        {"s-find", p_s_find, "( a n a2 n2 -- i )"}, {"s-under", p_s_under, "( a n c -- i )"},
        {"s-marked", p_s_marked, "( a n table -- i )"}, {"crlf-lines?", p_crlf_lines_q, "( a n -- flag )"}, {"s-bytes", p_s_bytes, "( a n -- addr n )"}, {"s-slice", p_s_slice, "( a n i len -- a' n' )"},
        {"s-trim", p_s_trim, "( a n -- a' n' )"}, {"s-starts?", p_s_starts, "( a n a2 n2 -- flag )"},
        {"s-ends?", p_s_ends, "( a n a2 n2 -- flag )"}, {"s-upper", p_s_upper, "( a n -- a' n )"},
        {"s-lower", p_s_lower, "( a n -- a' n )"}, {"s-split", p_s_split, "( a n a2 n2 -- l )"},
        {"s-join", p_s_join, "( l a n -- a' n' )"},
        {"<map>", p_map_new, "( -- m )"}, {"at", p_at, "( key m -- value true | false )"},
        {"at!", p_at_store, "( value key m -- )"}, {"key?", p_key_q, "( key m -- flag )"},
        {"delete-at", p_delete_at, "( key m -- )"}, {"s-at", p_s_at, "( a n m -- value true | false )"},
        {"s-at!", p_s_at_store, "( value a n m -- )"}, {"fields>map", p_fields_to_map, "( a n max m -- flag )"}, {"s-key?", p_s_key_q, "( a n m -- flag )"},
        {"s-delete-at", p_s_delete_at, "( a n m -- )"}, {"s-hash", p_s_hash, "( a n -- n )"},
        {"(json>)", p_json_read, "( a n null true false xt max -- v )"}, {"(>json)", p_json_write, "( v null true false class -- s len )"}, {"s-key-after", p_s_key_after, "( a n m -- s true | false )"},
        {"s-move-before", p_s_move_before, "( a n a2 n2 m -- )"}, {"map-size", p_map_size, "( m -- n )"},
        {"map-clear", p_map_clear, "( m -- )"}, {"keys", p_keys, "( m -- l )"}, {"values", p_values, "( m -- l )"},
        {"map-each", p_map_each, "( m xt -- )"},
        {"<bytes>", p_bytes_new, "( -- b )"}, {"b-length", p_b_length, "( b -- n )"}, {"b@", p_b_fetch, "( i b -- c )"},
        {"b!", p_b_store, "( c i b -- )"}, {"b-push", p_b_push, "( c b -- )"}, {"b-append", p_b_append, "( a n b -- )"},
        {"b-data", p_b_data, "( b -- a n )"}, {"b>string", p_b_to_string, "( b -- s len )"},
        {"b-clear", p_b_clear, "( b -- )"}, {"b-resize", p_b_resize, "( n b -- )"},
        {"utf8-length", p_utf8_length, "( a n -- count )"}, {"utf8-next", p_utf8_next, "( a n -- c a' n' )"},
        {"utf8-valid?", p_utf8_valid, "( a n -- flag )"}, {"utf8-clean", p_utf8_clean, "( a n -- s len )"},
        {"scan-plain", p_scan_plain, "( a n -- n' )"}, {"skip-blanks", p_skip_blanks, "( a n -- n' )"},
        {"scan-digits", p_scan_digits, "( a n -- n' )"}, {"utf8-slice", p_utf8_slice, "( a n i len -- a' n' )"},
        {"char>string", p_char_to_string, "( c -- s len )"}, {"b-push-char", p_b_push_char, "( c b -- )"},
        {"format", p_format, NULL},
        {"sort", p_sort, "( l -- )"}, {"sort-strings", p_sort_strings, "( l -- )"}, {"sort-by", p_sort_by, "( l xt -- )"},
    };

    f->w_string_copy = forth_prim_word(f, "(string-copy)", p_string_copy, WORD_COMPILED_ONLY, 0);
    forth_set_effect(f, f->w_string_copy, "( s -- s' )");
    f->w_each_begin = forth_prim_word(f, "(each-begin)", p_each_begin, WORD_COMPILED_ONLY, 0);
    f->w_each_next = forth_prim_word(f, "(each-next)", p_each_next, WORD_COMPILED_ONLY, 1);
    f->w_each_item = forth_prim_word(f, "(each-item)", p_each_item, WORD_COMPILED_ONLY, 0);
    f->w_each_keep = forth_prim_word(f, "(each-keep)", p_each_keep, WORD_COMPILED_ONLY, 0);
    f->w_each_end = forth_prim_word(f, "(each-end)", p_each_end, WORD_COMPILED_ONLY, 0);
    f->w_each_found = forth_prim_word(f, "(each-found)", p_each_found, WORD_COMPILED_ONLY, 0);
    forth_set_effect(f, f->w_each_begin, "( l -- )");
    forth_set_effect(f, f->w_each_item, "( -- x )");
    forth_set_effect(f, f->w_each_keep, "( x -- )");
    forth_set_effect(f, f->w_each_end, "( -- )");
    forth_set_effect(f, f->w_each_found, "( -- x true | false )");
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_constant(f, "object", (Cell)&tuples(f)->object);   /* (the root class: `M: object g` is every tuple's) */
    forth_vocab(f, "accessors")->state = VOCAB_LOADED;   /* (the tuples' accessors: there before any TUPLE: makes one) */
}

void forth_objects_free(Forth *f) {
    objects_free_all(f);
    tuples_free(f);
}

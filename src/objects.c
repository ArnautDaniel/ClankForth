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

/* ---- objects: the lists, tuples and strings the scripts make. Each is known by its address (a table: a number or a
 * stale address used as one is an error, not a crash) and made in a region - the system's, kept till `dispose`, or a
 * scratch one, all of whose objects `with-scratch` frees as it ends (but those `rescue`d) ---- */

enum { OBJ_FREE, OBJ_LIST, OBJ_TUPLE, OBJ_STRING, OBJ_MAP, OBJ_BYTES, OBJ_GONE };   /* (OBJ_GONE: a slot freed) */
static const char *const object_kinds[] = {"nothing", "a list", "a tuple", "a string", "a map", "bytes", "nothing"};

typedef struct ObjectSlot {
    void *at;
    size_t size;        /* its bytes (a string's: its length) */
    int kind, region;
    int owned;          /* a map's string key: the map's to let go (dispose refuses) */
} ObjectSlot;

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

typedef struct Region {
    int parent;         /* the region around it: where `rescue` puts an object */
    int used;
    void **made;        /* the objects made in it (some since let go or rescued: looked up as it ends) */
    int nmade, cap;
} Region;

struct ForthObjects {
    ObjectSlot *slots;  /* open addressing by address */
    int nslots, nused, ngone;
    Region *regions;    /* [0]: the system's */
    int nregions;
    long made[OBJ_GONE];   /* each kind made, all told (for `.objects`) */
};

#define OBJECTS_FIRST 1024

static struct ForthObjects *objects(Forth *f) {
    if (f->objects == NULL) {
        f->objects = calloc(1, sizeof(struct ForthObjects));
        if (f->objects == NULL) {
            forth_error(f, "objects: out of memory");
        }
    }
    return f->objects;
}

static unsigned object_hash(const void *p) { return (unsigned)(((uintptr_t)p >> 3) * 2654435761u); }

static ObjectSlot *object_slot(struct ForthObjects *o, const void *p) {   /* its slot, or the free one it would take */
    unsigned k;
    ObjectSlot *gone = NULL;

    if (o->nslots == 0) {
        return NULL;
    }
    for (k = object_hash(p) & (unsigned)(o->nslots - 1);; k = (k + 1) & (unsigned)(o->nslots - 1)) {
        ObjectSlot *sl = &o->slots[k];

        if (sl->kind == OBJ_FREE) {
            return gone != NULL ? gone : sl;
        }
        if (sl->kind == OBJ_GONE) {
            if (gone == NULL) {
                gone = sl;
            }
        } else if (sl->at == p) {
            return sl;
        }
    }
}

static void objects_grow(Forth *f, struct ForthObjects *o) {
    ObjectSlot *old = o->slots;
    int n = o->nslots, i;

    o->nslots = n ? n * 2 : OBJECTS_FIRST;
    if (o->nused * 2 < n) {   /* (mostly gone: the same size, swept) */
        o->nslots = n;
    }
    o->slots = calloc((size_t)o->nslots, sizeof(ObjectSlot));
    if (o->slots == NULL) {
        o->slots = old;
        o->nslots = n;
        forth_error(f, "objects: out of memory");
    }
    o->nused = 0;
    o->ngone = 0;
    for (i = 0; i < n; i++) {
        if (old[i].kind != OBJ_FREE && old[i].kind != OBJ_GONE) {
            *object_slot(o, old[i].at) = old[i];
            o->nused++;
        }
    }
    free(old);
}

static void region_note(Forth *f, Region *r, void *p) {
    if (r->nmade == r->cap) {
        int cap = r->cap ? r->cap * 2 : 32;
        void **made = realloc(r->made, (size_t)cap * sizeof(void *));

        if (made == NULL) {
            forth_error(f, "objects: out of memory");
        }
        r->made = made;
        r->cap = cap;
    }
    r->made[r->nmade++] = p;
}

/* a new object of `bytes` (zeroed), in the running task's region */
static void *object_new(Forth *f, int kind, size_t bytes) {
    struct ForthObjects *o = objects(f);
    int region = f->t->region;
    ObjectSlot *sl;
    void *p;

    if ((o->nused + o->ngone + 1) * 4 >= o->nslots * 3) {
        objects_grow(f, o);
    }
    if ((p = calloc(1, bytes > 0 ? bytes : 1)) == NULL) {
        forth_error(f, "%s: out of memory", object_kinds[kind]);
    }
    sl = object_slot(o, p);
    if (sl->kind == OBJ_GONE) {
        o->ngone--;
    }
    *sl = (ObjectSlot){p, bytes, kind, region, 0};
    o->nused++;
    o->made[kind]++;
    if (region > 0) {
        region_note(f, &o->regions[region], p);
    }
    return p;
}

/* a string object of len bytes, zeroed (and a 0 after them): the record's size its length */
static char *string_alloc(Forth *f, size_t len) {
    char *p = object_new(f, OBJ_STRING, len + 1);

    object_slot(f->objects, p)->size = len;
    return p;
}
static char *string_new(Forth *f, const char *s, size_t len) { return memcpy(string_alloc(f, len), s, len); }

static ObjectSlot *object_of(Forth *f, Cell x) {   /* x's record, NULL if it is no object */
    ObjectSlot *sl;

    if (f->objects == NULL || (UCell)x < LOWEST_ADDRESS || x < 0) {
        return NULL;
    }
    sl = object_slot(f->objects, (const void *)x);
    return sl != NULL && sl->kind != OBJ_FREE && sl->kind != OBJ_GONE ? sl : NULL;
}

/* x as an object of that kind: an error if it isn't one */
static void *object_arg(Forth *f, Cell x, int kind, const char *word) {
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
    if (sl->kind == OBJ_LIST) {
        free(((List *)sl->at)->items);
    } else if (sl->kind == OBJ_MAP) {
        map_let_go(f, sl->at);   /* (its keys' strings with it; sl may have moved: looked up again) */
        sl = object_slot(f->objects, sl->at);
    } else if (sl->kind == OBJ_BYTES) {
        free(((Bytes *)sl->at)->p);
    }
    free(sl->at);
    sl->at = NULL;
    sl->kind = OBJ_GONE;
    f->objects->nused--;
    f->objects->ngone++;
}

static int region_new(Forth *f) {
    struct ForthObjects *o = objects(f);
    int i;

    if (o->nregions == 0) {   /* (the system's: [0]) */
        o->regions = calloc(16, sizeof(Region));
        if (o->regions == NULL) {
            forth_error(f, "objects: out of memory");
        }
        o->nregions = 16;
        o->regions[0].used = 1;
    }
    for (i = 1; i < o->nregions && o->regions[i].used; i++) {
    }
    if (i == o->nregions) {
        Region *r = realloc(o->regions, (size_t)o->nregions * 2 * sizeof(Region));

        if (r == NULL) {
            forth_error(f, "objects: out of memory");
        }
        memset(r + o->nregions, 0, (size_t)o->nregions * sizeof(Region));
        o->regions = r;
        o->nregions *= 2;
    }
    o->regions[i].used = 1;
    o->regions[i].parent = f->t->region;
    o->regions[i].nmade = 0;
    return i;
}

/* a scratch region ended: what was made in it, and not let go or rescued since, freed */
static void region_end(Forth *f, int region) {
    struct ForthObjects *o = f->objects;
    Region *r = &o->regions[region];
    int i;

    for (i = 0; i < r->nmade; i++) {
        ObjectSlot *sl = object_slot(o, r->made[i]);

        if (sl != NULL && sl->kind != OBJ_FREE && sl->kind != OBJ_GONE && sl->region == region) {
            object_free(f, sl);
        }
    }
    r->nmade = 0;
    r->used = 0;
}

static void objects_free_all(Forth *f) {
    struct ForthObjects *o = f->objects;
    int i;

    if (o == NULL) {
        return;
    }
    for (i = 0; i < o->nslots; i++) {
        if (o->slots[i].kind != OBJ_FREE && o->slots[i].kind != OBJ_GONE) {
            object_free(f, &o->slots[i]);
        }
    }
    for (i = 0; i < o->nregions; i++) {
        free(o->regions[i].made);
    }
    free(o->regions);
    free(o->slots);
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
    object_free(f, sl);
}
PRIM(p_rescue) {   /* ( obj -- obj ) made in a scratch region: it outlives it, kept by the one around it */
    Cell x = *top(f, 1);
    ObjectSlot *sl = object_of(f, x);
    int parent;

    if (sl == NULL) {
        forth_error(f, "rescue: not an object: %ld", (long)x);
    }
    if (sl->region > 0) {
        parent = f->objects->regions[sl->region].parent;
        sl->region = parent;
        if (parent > 0) {
            region_note(f, &f->objects->regions[parent], sl->at);
        }
    }
}
PRIM(p_rescue_all) {   /* ( obj -- obj ) out of every scratch region round it: kept till `dispose` (a message's) */
    Cell x = *top(f, 1);
    ObjectSlot *sl = object_of(f, x);

    if (sl == NULL) {
        forth_error(f, "rescue-all: not an object: %ld", (long)x);
    }
    sl->region = 0;   /* (the system's: the regions' lists of what they made skip it as they end) */
}
PRIM(p_object_q) { Cell x = POP(); PUSH(object_of(f, x) != NULL ? -1 : 0); }   /* ( x -- flag ) */
PRIM(p_objects) { PUSH(f->objects != NULL ? f->objects->nused : 0); }   /* ( -- n ) objects alive */
PRIM(p_dot_objects) {   /* every kind's count: alive, and made all told */
    long alive[OBJ_GONE] = {0};
    int i, k;

    for (i = 0; f->objects != NULL && i < f->objects->nslots; i++) {
        k = f->objects->slots[i].kind;
        if (k != OBJ_FREE && k != OBJ_GONE) {
            alive[k]++;
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
void forth_regions_unwind(Forth *f, Task *t, int to) {   /* t's regions ended, the latest first, till `to` is current */
    while (t->region > 0 && t->region != to) {
        int region = t->region;

        t->region = f->objects->regions[region].parent;
        region_end(f, region);
    }
}
PRIM(p_scratch_open) {   /* (scratch-open) ( -- region ) a new region, the running task's objects made in it */
    int region = region_new(f);

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
    forth_regions_unwind(f, f->t, f->objects->regions[region].parent);
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
PRIM(p_list) { PUSH(new_list(f)); }   /* ( -- l ) */
PRIM(p_list_free) {   /* ( l -- ) (dispose, for a list) */
    Cell x = POP();

    list_arg(f, x);
    object_free(f, object_of(f, x));
}
PRIM(p_push) { List *l = list_arg(f, POP()); list_add(f, l, POP()); }   /* ( x l -- ) */
PRIM(p_pop) {   /* ( l -- x ) the last */
    List *l = list_arg(f, POP());

    if (l->n == 0) {
        forth_error(f, "pop: the list is empty");
    }
    PUSH(l->items[--l->n]);
}
static Cell index_arg(Forth *f, List *l, Cell i) {
    if (i < 0) {
        i += l->n;   /* -1: the last */
    }
    if (i < 0 || i >= l->n) {
        forth_error(f, "list index %ld out of range (length %ld)", (long)i, (long)l->n);
    }
    return i;
}
PRIM(p_nth) { List *l = list_arg(f, POP()); Cell i = POP(); PUSH(l->items[index_arg(f, l, i)]); }   /* ( i l -- x ) */
PRIM(p_nth_store) {   /* ( x i l -- ) */
    List *l = list_arg(f, POP());
    Cell i = POP(), x = POP();

    l->items[index_arg(f, l, i)] = x;
}
PRIM(p_length) { PUSH(list_arg(f, POP())->n); }   /* ( l -- n ) */
PRIM(p_list_clear) { list_arg(f, POP())->n = 0; }
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
    PUSH(l);
}

/* ---- a list walked by a combinator compiled inline (`[: ... ;] each`: lay_walk). The walk is three cells on the
 * return stack - the list, its length as the walk began, the next item's index - so the quotation runs on the data
 * stack as its word left it. The list is looked up each turn: let go meanwhile, or shorter, is an error. ---- */

static Cell *walk_cells(Forth *f, int under) {   /* the walk's cells (under: cells a word keeps beneath them) */
    Task *t = f->t;

    if (t->rp < WALK_CELLS + under) {
        forth_error(f, "each: the list being walked is gone from the return stack");
    }
    return &t->rs[t->rp - WALK_CELLS];
}
static Cell walk_item(Forth *f, const Cell *walk, Cell i) {
    List *l = list_arg(f, walk[0]);

    if (i < 0 || i >= l->n) {
        forth_error(f, "each: the list lost items as it was walked (item %ld of %ld)", (long)i, (long)l->n);
    }
    return l->items[i];
}
PRIM(p_each_begin) {   /* (each-begin) ( l -- ) */
    Cell x = POP();
    List *l = list_arg(f, x);

    rpush(f, x);
    rpush(f, l->n);
    rpush(f, 0);
}
PRIM(p_each_next) {   /* (each-next) out ( -- x ), or - no more - to out; the index then past the length: none found */
    Cell *walk = walk_cells(f, 0);

    if (walk[2] < walk[1]) {
        PUSH(walk_item(f, walk, walk[2]));
        walk[2]++;
        IP++;
    } else {
        walk[2] = walk[1] + 1;
        IP = (Cell *)*IP;
    }
}
PRIM(p_each_item) {   /* (each-item) ( -- x ) the item (each-next) gave last */
    Cell *walk = walk_cells(f, 0);

    PUSH(walk_item(f, walk, walk[2] - 1));
}
PRIM(p_each_keep) {   /* (each-keep) ( x -- ) onto the list kept under the walk (map's, filter's) */
    Cell *walk = walk_cells(f, 1), x = POP();

    list_add(f, list_arg(f, walk[-1]), x);
}
PRIM(p_each_end) {
    (void)walk_cells(f, 0);
    f->t->rp -= WALK_CELLS;
}
PRIM(p_each_found) {   /* (each-found) ( -- x true | false ) the walk over: the item it stopped at, if it did */
    Cell *walk = walk_cells(f, 0);
    int found = walk[2] <= walk[1];
    Cell x = found ? walk_item(f, walk, walk[2] - 1) : 0;

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
    PUSH(t);
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
PRIM(p_generic) {
    struct ForthTuples *t = tuples(f);
    size_t len, m;
    const char *name = forth_parse_required(f, &len, "GENERIC:"), *s;
    Generic *g = calloc(1, sizeof(Generic));
    Word *made;

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
    PUSH(copy);
}

/* ---- strings: ( addr len ) as ever; those these words make are objects (their addr), in the region's - freed by
 * `dispose`, or the scratch region's end. A list of strings holds the objects: `string` gives one's addr len. ---- */

static void push_string(Forth *f, const char *s, size_t len) {
    char *p = string_new(f, s, len);

    PUSH(p);
    PUSH(len);
}
PRIM(p_to_string) {   /* >string ( addr len -- s len ) a copy that lasts (an object) */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = forth_string_arg(f, a, n, ">string", &len);

    push_string(f, s, len);
}
PRIM(p_string) {   /* string ( s -- addr len ) a string object's bytes */
    Cell x = POP();
    char *p = object_arg(f, x, OBJ_STRING, "string");

    PUSH(p);
    PUSH(object_of(f, x)->size);
}
PRIM(p_string_q) { ObjectSlot *sl = object_of(f, POP()); PUSH(sl != NULL && sl->kind == OBJ_STRING ? -1 : 0); }
PRIM(p_s_plus) {   /* s+ ( a1 n1 a2 n2 -- a n ) the two together, a new string */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2;
    const char *s1 = forth_string_arg(f, a1, n1, "s+", &l1), *s2 = forth_string_arg(f, a2, n2, "s+", &l2);
    char *p = string_alloc(f, l1 + l2);

    memcpy(p, s1, l1);
    memcpy(p + l1, s2, l2);
    PUSH(p);
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
PRIM(p_string_to_number) {   /* string>number ( a n -- n flag ) a whole number in the base ($ # % too); 0 false if not */
    Cell n = POP(), a = POP(), x = 0;
    size_t len;
    const char *s = forth_string_arg(f, a, n, "string>number", &len);
    int ok = len > 0 && forth_parse_int(f, s, len, &x);

    PUSH(ok ? x : 0);
    PUSH(ok ? -1 : 0);
}
PRIM(p_string_to_float) {   /* string>float ( a n -- flag ) ( F: -- x ) a float (1.5 2e): 0e false if not */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = forth_string_arg(f, a, n, "string>float", &len);
    Float x = 0;
    int ok = len > 0 && forth_parse_float(s, len, &x);

    FPUSH(ok ? x : 0.0);
    PUSH(ok ? -1 : 0);
}
static size_t find_in(const char *s, size_t n, const char *t, size_t m, size_t from) {   /* t in s: where, or n */
    size_t i;

    for (i = from; m <= n && i + m <= n; i++) {
        if (memcmp(s + i, t, m) == 0) {
            return i;
        }
    }
    return n;
}
PRIM(p_s_find) {   /* s-find ( a n a2 n2 -- i ) where the second is in the first, -1 if not */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2, at;
    const char *s1 = forth_string_arg(f, a1, n1, "s-find", &l1), *s2 = forth_string_arg(f, a2, n2, "s-find", &l2);

    at = l2 == 0 ? 0 : find_in(s1, l1, s2, l2, 0);   /* (nothing is found at the start) */
    PUSH(l2 == 0 || at < l1 ? (Cell)at : -1);
}
PRIM(p_s_slice) {   /* s-slice ( a n i len -- a' n' ) its len bytes from i (kept to the string): no copy */
    Cell k = POP(), i = POP(), n = POP(), a = POP();

    if (n < 0) {
        forth_error(f, "s-slice: a negative length (%ld)", (long)n);
    }
    i = i < 0 ? 0 : i > n ? n : i;
    k = k < 0 ? 0 : k > n - i ? n - i : k;
    PUSH((Cell)((UCell)a + (UCell)i));
    PUSH(k);
}
PRIM(p_s_trim) {   /* s-trim ( a n -- a' n' ) without the spaces at its ends: no copy */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = forth_string_arg(f, a, n, "s-trim", &len);

    while (len > 0 && isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        len--;
    }
    PUSH(len > 0 ? (Cell)s : a);
    PUSH(len);
}
static int ends_with(Forth *f, int at_start, const char *word) {   /* s-starts? s-ends? ( a n a2 n2 -- flag ) */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2;
    const char *s1 = forth_string_arg(f, a1, n1, word, &l1), *s2 = forth_string_arg(f, a2, n2, word, &l2);

    return l2 <= l1 && memcmp(at_start ? s1 : s1 + l1 - l2, s2, l2) == 0;
}
PRIM(p_s_starts) { PUSH(ends_with(f, 1, "s-starts?") ? -1 : 0); }
PRIM(p_s_ends) { PUSH(ends_with(f, 0, "s-ends?") ? -1 : 0); }
static void cased(Forth *f, int upper, const char *word) {   /* s-upper s-lower ( a n -- a' n ) a new string */
    Cell n = POP(), a = POP();
    size_t len, i;
    const char *s = forth_string_arg(f, a, n, word, &len);
    char *p = string_new(f, s, len);

    for (i = 0; i < len; i++) {
        p[i] = (char)(upper ? toupper((unsigned char)p[i]) : tolower((unsigned char)p[i]));
    }
    PUSH(p);
    PUSH(len);
}
PRIM(p_s_upper) { cased(f, 1, "s-upper"); }
PRIM(p_s_lower) { cased(f, 0, "s-lower"); }
PRIM(p_s_split) {   /* s-split ( a n a2 n2 -- l ) the pieces between each of the second: a list of strings */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t l1, l2, at = 0, next;
    const char *s1 = forth_string_arg(f, a1, n1, "s-split", &l1), *s2 = forth_string_arg(f, a2, n2, "s-split", &l2);
    List *l = new_list(f);

    if (l2 == 0) {
        forth_error(f, "s-split: nothing to split at");
    }
    for (;;) {
        next = find_in(s1, l1, s2, l2, at);
        list_add(f, l, (Cell)string_new(f, s1 + at, next - at));
        if (next >= l1) {
            break;
        }
        at = next + l2;
    }
    PUSH(l);
}
PRIM(p_s_join) {   /* s-join ( l a n -- a' n' ) a list of strings, the second between each, a new string */
    Cell n = POP(), a = POP();
    List *l = list_arg(f, POP());
    size_t sep, total = 0, at = 0;
    const char *s = forth_string_arg(f, a, n, "s-join", &sep);
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
    PUSH(p);
    PUSH(total);
}

/* ---- setting up ---- */

/* ---- objects a message carries to another node (nodes.c): copied out as bytes, made again there - the receiver's,
 * kept till `dispose` (as `rescue-all` keeps one) ---- */

#define PACK_DEPTH_MAX 32   /* objects within objects one may carry (a list holding itself is an error) */
enum { PACK_CELL = 'c', PACK_STRING = 's', PACK_LIST = 'l', PACK_TUPLE = 't' };

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
        *out = (Cell)t;
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
            *out = (Cell)string_new(f, (const char *)u->p, (size_t)n);
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
        *out = (Cell)l;
        return 1;
    }
    return 0;
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
    f->t->region = 0;   /* (the system's: kept, whatever region the delivery runs in) */
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

static uint64_t hash_bytes(const char *s, size_t n) {
    uint64_t h = HASH_OFFSET;
    size_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ (uint8_t)s[i]) * HASH_PRIME;
    }
    return h;
}

static uint64_t hash_cell(Cell x) {   /* (splitmix64's finish: every bit of the cell in every bit of the hash) */
    uint64_t z = (uint64_t)x;

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static Key text_key(const char *s, size_t n) { return (Key){0, s, n, hash_bytes(s, n), 1}; }

static Key cell_key(Forth *f, Cell x) {   /* (a string object: by its bytes) */
    ObjectSlot *sl = object_of(f, x);

    if (sl != NULL && sl->kind == OBJ_STRING) {
        return text_key(sl->at, sl->size);
    }
    return (Key){x, NULL, 0, hash_cell(x), 0};
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
        object_slot(f->objects, copy)->owned = 1;
        key = (Cell)copy;
    }
    e = &m->e[m->n];
    *e = (MapEntry){key, value, k->hash, k->is_text, 1};
    index_put(m, m->n);
    m->n++;
    m->live++;
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

PRIM(p_map_new) { PUSH(object_new(f, OBJ_MAP, sizeof(Map))); }   /* <map> ( -- m ) */
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
    const char *s = forth_string_arg(f, a, n, word, &len);

    return text_key(s, len);
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
    PUSH(l);
}
PRIM(p_keys) { map_list(f, 0); }
PRIM(p_values) { map_list(f, 1); }
/* map-each ( m xt -- ) xt ( key value -- ) for each entry, in their order. The map may have its values changed as
 * it is walked (at! of a key it has), not entries added or taken away: an error. */
PRIM(p_map_each) {
    Word *xt = forth_xt_arg(f, POP(), "map-each");
    Cell x = POP(), i, n;
    Map *m = map_arg(f, x, "map-each");

    for (i = 0, n = m->n; i < n; i++) {
        m = map_arg(f, x, "map-each");
        if (m->n != n || (m->live == 0 && n > 0)) {
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

static void bytes_room(Forth *f, Bytes *b, Cell more) {   /* (an error leaves them as they were) */
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
PRIM(p_bytes_new) { PUSH(object_new(f, OBJ_BYTES, sizeof(Bytes))); }   /* <bytes> ( -- b ) */
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
PRIM(p_b_append) {   /* b-append ( addr len b -- ) those bytes after its own (its own too: b b-data b b-append) */
    Bytes *b = bytes_arg(f, POP(), "b-append");
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = forth_string_arg(f, a, n, "b-append", &len);
    Cell inside = b->p != NULL && (const uint8_t *)s >= b->p && (const uint8_t *)s < b->p + b->cap
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
    PUSH(b->p);
    PUSH(b->n);
}
PRIM(p_b_to_string) {   /* b>string ( b -- s len ) a new string of its bytes */
    Bytes *b = bytes_arg(f, POP(), "b>string");
    char *p = string_alloc(f, (size_t)b->n);

    memcpy(p, b->p != NULL ? (const char *)b->p : "", (size_t)b->n);
    PUSH(p);
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
    memcpy(items, l->items, (size_t)n * sizeof(Cell));
    merge_sort(&s, items, items + n, n);
    f->catch = prev;
    l = (List *)object_of(f, x);
    if (l == NULL || object_of(f, x)->kind != OBJ_LIST || ((List *)object_of(f, x)->at)->n != n) {
        free(items);
        forth_error(f, "%s: the list changed as it was sorted", word);
    }
    memcpy(((List *)object_of(f, x)->at)->items, items, (size_t)n * sizeof(Cell));
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

    return (const uint8_t *)forth_string_arg(f, a, n, word, len);
}

PRIM(p_utf8_length) {   /* utf8-length ( a n -- count ) its characters */
    size_t len, used;
    const uint8_t *s = text_arg2(f, "utf8-length", &len);
    Cell count = 0;

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

    for (; ok && len > 0; s += used, len -= used) {
        ok = utf8_decode(s, len, &used) != UTF8_REPLACEMENT || (used == 3 && s[0] == 0xEF);   /* (U+FFFD itself) */
    }
    PUSH(ok ? -1 : 0);
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
    char piece[2 * FORMAT_WIDTH_MAX + 64], spec[32];

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
            const char *str = forth_string_arg(f, args[ai], args[ai + 1], "format", &slen);

            ai += 2;
            pad = (size_t)sp.width > slen ? (size_t)sp.width - slen : 0;
            if (out != NULL) {
                memset(out + len + (sp.left ? slen : 0), ' ', pad);
                memcpy(out + len + (sp.left ? 0 : pad), str, slen);
            }
            len += slen + pad;
            continue;
        }
        snprintf(spec, sizeof(spec), "%%%s%s%d", sp.left ? "-" : "", sp.zeros ? "0" : "", sp.width);
        if (sp.kind == 'f') {
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
        } else {
            static const char *const conv[] = {"lld", "llu", "llx", "llX"};
            int which = sp.kind == 'u' ? 1 : sp.kind == 'x' ? 2 : sp.kind == 'X' ? 3 : 0;

            snprintf(spec + strlen(spec), sizeof(spec) - strlen(spec), "%s", conv[which]);
            k = which == 0 ? (size_t)snprintf(piece, sizeof(piece), spec, (long long)args[ai])
                           : (size_t)snprintf(piece, sizeof(piece), spec, (unsigned long long)(UCell)args[ai]);
            ai++;
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
    const char *fmt = forth_string_arg(f, a, n, "format", &len), *p = fmt, *end = fmt + len;
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
    PUSH(out);
    PUSH((Cell)out_len);
}

char *forth_new_string(Forth *f, const char *s, size_t len) { return string_new(f, s, len); }
Cell forth_new_list(Forth *f) { return (Cell)new_list(f); }
void forth_list_push(Forth *f, Cell l, Cell x) { list_add(f, object_arg(f, l, OBJ_LIST, "list"), x); }
Cell forth_new_bytes(Forth *f, const void *data, size_t n) {
    Bytes *b = object_new(f, OBJ_BYTES, sizeof(Bytes));

    bytes_room(f, b, (Cell)n);
    if (n > 0) {
        memcpy(b->p, data, n);
    }
    b->n = (Cell)n;
    return (Cell)b;
}
const uint8_t *forth_bytes_of(Forth *f, Cell x, size_t *n, const char *word) {
    Bytes *b = bytes_arg(f, x, word);

    *n = (size_t)b->n;
    return b->p != NULL ? b->p : (const uint8_t *)"";
}

void forth_objects_words(Forth *f) {
    static const ForthPrim prims[] = {
        {"list", p_list, "( -- l )"}, {"list-free", p_list_free, "( l -- )"}, {"push", p_push, "( x l -- )"},
        {"pop", p_pop, "( l -- x )"}, {"nth", p_nth, "( i l -- x )"}, {"nth!", p_nth_store, "( x i l -- )"},
        {"length", p_length, "( l -- n )"}, {"list-clear", p_list_clear, "( l -- )"}, {"{", p_brace_open, NULL},
        {"}", p_brace_close, NULL}, {"dispose", p_dispose, "( obj -- )"}, {"rescue", p_rescue, "( obj -- obj )"},
        {"rescue-all", p_rescue_all, "( obj -- obj )"}, {"object?", p_object_q, "( x -- flag )"},
        {"objects", p_objects, "( -- n )"}, {".objects", p_dot_objects, "( -- )"},
        {"(scratch-open)", p_scratch_open, "( -- region )"}, {"(scratch-close)", p_scratch_close, "( region -- )"},
        {"TUPLE:", p_tuple, "( -- )"}, {"GENERIC:", p_generic, "( -- )"},
        {"M:", p_method, "( -- )"}, {"class-of", p_class_of, "( obj -- class )"},
        {"instance?", p_instance, "( x class -- flag )"}, {".tuple", p_dot_tuple, "( obj -- )"},
        {"clone", p_clone, "( obj -- obj' )"}, {">string", p_to_string, "( addr len -- s len )"},
        {"string", p_string, "( s -- addr len )"}, {"string?", p_string_q, "( x -- flag )"},
        {"s+", p_s_plus, "( a1 n1 a2 n2 -- a n )"}, {"n>s", p_n_to_s, "( n -- a len )"},
        {"f>string", p_f_to_string, "( -- a len ) ( F: x -- )"},
        {"string>number", p_string_to_number, "( a len -- n flag )"},
        {"string>float", p_string_to_float, "( a len -- flag ) ( F: -- F: x )"},
        {"s-find", p_s_find, "( a n a2 n2 -- i )"}, {"s-slice", p_s_slice, "( a n i len -- a' n' )"},
        {"s-trim", p_s_trim, "( a n -- a' n' )"}, {"s-starts?", p_s_starts, "( a n a2 n2 -- flag )"},
        {"s-ends?", p_s_ends, "( a n a2 n2 -- flag )"}, {"s-upper", p_s_upper, "( a n -- a' n )"},
        {"s-lower", p_s_lower, "( a n -- a' n )"}, {"s-split", p_s_split, "( a n a2 n2 -- l )"},
        {"s-join", p_s_join, "( l a n -- a' n' )"},
        {"<map>", p_map_new, "( -- m )"}, {"at", p_at, "( key m -- value true | false )"},
        {"at!", p_at_store, "( value key m -- )"}, {"key?", p_key_q, "( key m -- flag )"},
        {"delete-at", p_delete_at, "( key m -- )"}, {"s-at", p_s_at, "( a n m -- value true | false )"},
        {"s-at!", p_s_at_store, "( value a n m -- )"}, {"s-key?", p_s_key_q, "( a n m -- flag )"},
        {"s-delete-at", p_s_delete_at, "( a n m -- )"}, {"map-size", p_map_size, "( m -- n )"},
        {"map-clear", p_map_clear, "( m -- )"}, {"keys", p_keys, "( m -- l )"}, {"values", p_values, "( m -- l )"},
        {"map-each", p_map_each, "( m xt -- )"},
        {"<bytes>", p_bytes_new, "( -- b )"}, {"b-length", p_b_length, "( b -- n )"}, {"b@", p_b_fetch, "( i b -- c )"},
        {"b!", p_b_store, "( c i b -- )"}, {"b-push", p_b_push, "( c b -- )"}, {"b-append", p_b_append, "( a n b -- )"},
        {"b-data", p_b_data, "( b -- a n )"}, {"b>string", p_b_to_string, "( b -- s len )"},
        {"b-clear", p_b_clear, "( b -- )"}, {"b-resize", p_b_resize, "( n b -- )"},
        {"utf8-length", p_utf8_length, "( a n -- count )"}, {"utf8-next", p_utf8_next, "( a n -- c a' n' )"},
        {"utf8-valid?", p_utf8_valid, "( a n -- flag )"}, {"utf8-slice", p_utf8_slice, "( a n i len -- a' n' )"},
        {"char>string", p_char_to_string, "( c -- s len )"}, {"b-push-char", p_b_push_char, "( c b -- )"},
        {"format", p_format, NULL},
        {"sort", p_sort, "( l -- )"}, {"sort-strings", p_sort_strings, "( l -- )"}, {"sort-by", p_sort_by, "( l xt -- )"},
    };

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

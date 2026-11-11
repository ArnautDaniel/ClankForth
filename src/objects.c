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

enum { OBJ_FREE, OBJ_LIST, OBJ_TUPLE, OBJ_STRING, OBJ_GONE };   /* (OBJ_GONE: a slot freed, for probes to pass) */
static const char *const object_kinds[] = {"nothing", "a list", "a tuple", "a string", "nothing"};

typedef struct ObjectSlot {
    void *at;
    size_t size;        /* its bytes (a string's: its length) */
    int kind, region;
} ObjectSlot;

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
    *sl = (ObjectSlot){p, bytes, kind, region};
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

    if (sl == NULL || sl->kind != kind) {
        forth_error(f, "%s: not %s: %s%ld", word, object_kinds[kind], sl != NULL ? object_kinds[sl->kind] : "",
                    sl != NULL ? 0L : (long)x);
    }
    return sl->at;
}

static void object_free(Forth *f, ObjectSlot *sl) {
    if (sl->kind == OBJ_LIST) {
        free(((List *)sl->at)->items);
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
        forth_printf(f, "%-10s %ld alive, %ld made\n", object_kinds[k] + 2, alive[k],
                     f->objects != NULL ? f->objects->made[k] : 0L);
    }
}

/* [: ... ;] with-scratch ( xt -- ) - run it in a scratch region: every object made in it is freed as it ends (an error
 * too), but those `rescue`d. It can't wait (it is C's: as `catch`). */
static void in_region(Forth *f, Word *x, Task *t, int region) {   /* x run with t's objects made in region */
    jmp_buf jb, *prev = f->catch;
    int outer = t->region;

    if (setjmp(jb) != 0) {   /* (an error: the region ended, and the error passed on) */
        f->catch = prev;
        t->region = outer;
        region_end(f, region);
        longjmp(*prev, 1);
    }
    f->catch = &jb;
    t->region = region;
    forth_execute(f, x);
    f->catch = prev;
    t->region = outer;
    region_end(f, region);
}
PRIM(p_with_scratch) {
    Word *x = forth_xt_arg(f, POP(), "with-scratch");

    in_region(f, x, f->t, region_new(f));
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
    Cell *t = object_new(f, OBJ_TUPLE, (size_t)(c->nslots + 1) * sizeof(Cell));
    int i;

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
    char buf[WORD_NAME_MAX + 8], effect[256], feffect[128];
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

        snprintf(e + at, cap - at, " %s", t->names[c->slot[i]].name);
        t->names[c->slot[i]].is_float ? fin++ : din++;
    }
    snprintf(buf, sizeof(buf), "<%s>", c->name);
    made = forth_new_word(f, buf, strlen(buf), doconstructor);
    forth_comma(f, (Cell)c);
    {
        char full[512];

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

void forth_objects_words(Forth *f) {
    static const ForthPrim prims[] = {
        {"list", p_list, "( -- l )"}, {"list-free", p_list_free, "( l -- )"}, {"push", p_push, "( x l -- )"},
        {"pop", p_pop, "( l -- x )"}, {"nth", p_nth, "( i l -- x )"}, {"nth!", p_nth_store, "( x i l -- )"},
        {"length", p_length, "( l -- n )"}, {"list-clear", p_list_clear, "( l -- )"}, {"{", p_brace_open, NULL},
        {"}", p_brace_close, NULL}, {"dispose", p_dispose, "( obj -- )"}, {"rescue", p_rescue, "( obj -- obj )"},
        {"rescue-all", p_rescue_all, "( obj -- obj )"}, {"object?", p_object_q, "( x -- flag )"},
        {"objects", p_objects, "( -- n )"}, {".objects", p_dot_objects, "( -- )"},
        {"with-scratch", p_with_scratch, NULL}, {"TUPLE:", p_tuple, "( -- )"}, {"GENERIC:", p_generic, "( -- )"},
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

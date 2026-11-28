/* The Forth system's core: the inner interpreter, the outer interpreter (text -> actions), the dictionary and its
 * vocabularies, the compiler and the core primitives, tasks. See forth.h for the model and docs/forth.md for the
 * dialect. Beside it, each a file of its own over forth_internal.h: the objects the scripts make (objects.c), the
 * stack checker (effects.c), the actor kernel (actors.c). */
#include "forth_internal.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- stacks (forth_internal.h has them, for every file of the system: these are the hosts') ---- */

void forth_push(Forth *f, Cell x) { cell_push(f, x); }
Cell forth_pop(Forth *f) { return cell_pop(f); }
void forth_fpush(Forth *f, Float x) { float_push(f, x); }
Float forth_fpop(Forth *f) { return float_pop(f); }

/* ---- output and errors ---- */

void forth_type(Forth *f, const char *s, size_t n) {
    f->out(f->out_ctx, s, n);
}

void forth_printf(Forth *f, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        forth_type(f, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

static void stdout_output(void *ctx, const char *s, size_t n) {
    (void)ctx;
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

void forth_set_output(Forth *f, OutputFn out, void *ctx) {
    f->out = out != NULL ? out : stdout_output;
    f->out_ctx = ctx;
}

void forth_get_output(const Forth *f, OutputFn *out, void **ctx) {
    *out = f->out;
    *ctx = f->out_ctx;
}

/* ---- the system as it stands, for a host (forth.h: its state is not the host's to reach into) ---- */

void forth_set_reader(Forth *f, ForthReader reader, void *ctx) {
    f->reader = reader;
    f->reader_ctx = ctx;
}
int forth_compiling(const Forth *f) { return f->compiling != 0; }
int forth_bye(const Forth *f) { return f->bye; }
Task *forth_task(Forth *f) { return f->t; }
Vocab *forth_current(const Forth *f) { return f->m.current; }
Word *forth_latest(const Forth *f) { return f->latest; }
long forth_set_budget(Forth *f, long steps) {
    long was = (long)f->budget;

    f->budget = steps;
    return was;
}
long forth_set_slice(Forth *f, long steps) {
    long was = f->slice;

    f->slice = steps > 0 ? steps : 0;
    return was;
}
long forth_errors(const Forth *f) { return f->errors; }
long forth_warnings(const Forth *f) { return f->warnings; }
const char *forth_last_error(const Forth *f) { return f->error; }
const char *forth_last_error_chain(const Forth *f) { return f->error_chain; }
int forth_error_repeated(const Forth *f) { return f->quiet; }
void forth_forget_error(Forth *f) {
    f->said[0] = 0;
    f->repeats = 0;
}
void forth_set_task_failed(Forth *f, void (*task_failed)(Forth *f, Task *t)) { f->task_failed = task_failed; }

#define ERROR_REPEATS_SAID 300   /* (a repeated error's count: each 300, ten seconds of frames) */
#define BACKTRACE_BYTES 1024

/* A threaded task's `catch` (one it may wait inside): what it puts back if xt fails, and xt's code - the task's,
 * not C's stack's, so the turn's interpreter runs it and a wait leaves it and comes back. */
#define CATCH_FRAMES_MAX 16   /* catches within each other a task may wait in (more: as C's, no waiting there) */

/* the interpreter's state as a catch began, reckoned from where the turn began (f->turn_...): its sources and views
 * past the turn's, and the definition and compiling flag - the turn's own (the definition a host was in the middle
 * of as the turn began: never the catch's to drop), or those of its own */
typedef struct CatchPlace {
    int nsrc, views;
    int def_turns, compiling_turns;
    Word *defining;
    Cell compiling;
} CatchPlace;

struct ForthCatch {
    Cell *ip;               /* where the word with the catch goes on */
    int sp, rp, fp, lp, nmarks, region;
    CatchPlace place;
    Cell code[2];           /* xt, then (catch-end) */
};

#define BASE_MIN 2
#define BASE_MAX 36   /* (the digits 0-9, a-z) */
#define BASE_DEFAULT 10
#define TASK_VARS FORTH_TASK_VARS   /* task-variables a system may have: each task its own cell of each */
#define NUM_TEXT_BYTES 24    /* a whole number's digits and sign, at most: 20 for the most negative */
#define TEXT_BYTES 256       /* $+'s string, at most */
struct ForthTaskSys {       /* a task's own, the system's (Task's `sys`) */
    struct ForthCatch catches[CATCH_FRAMES_MAX];
    int ncatch;
    /* what is each running piece of Forth's own, not the node's - another actor's handler may run between any two
     * of its words (its slice over), so none of these may be shared: */
    Cell base;                /* number base (`base`) */
    char error[256];          /* the last error raised in it (`error-message`: catch's, not another's since) */
    Cell vars[TASK_VARS];     /* its task-variables' cells */
    char num[NUM_TEXT_BYTES]; /* n>str's digits (the next n>str's buffer too) */
    char text[TEXT_BYTES];    /* $clear $+ $@'s string */
    Cell text_n;
    ForthJob *job;            /* the job it waits for (jobs.c), or done and not yet taken; NULL none */
    long long wake_ms;        /* sleep-ms in a handler: when it goes on (monotonic ms); 0 none */
    int gave_way;             /* its slice was over: it goes on between frames, not only at the next */
};
/* the task's own (made the first time it is wanted: a task that never needs one has none); NULL out of memory */
static struct ForthTaskSys *task_sys_or_null(Task *t) {
    if (t->sys == NULL && (t->sys = calloc(1, sizeof(*t->sys))) != NULL) {
        t->sys->base = BASE_DEFAULT;
    }
    return t->sys;
}
static struct ForthTaskSys *task_sys(Forth *f, Task *t);
ForthJob **forth_task_job(Forth *f, Task *t) { return &task_sys(f, t)->job; }
ForthJob **forth_task_job_if(Task *t) { return t->sys != NULL ? &t->sys->job : NULL; }
int forth_task_sys_ok(Forth *f) { return task_sys_or_null(f->t) != NULL; }
static struct ForthTaskSys *task_sys(Forth *f, Task *t) {
    struct ForthTaskSys *sys = task_sys_or_null(t);

    if (sys == NULL) {
        forth_error(f, "out of memory for a task's own state");
    }
    return sys;
}

/* an error raised now is a catch's to say (not printed) */
static int caught_here(const Forth *f) {
    return f->silent > 0 || (f->t != NULL && f->t->sys != NULL && f->t->sys->ncatch > 0);
}

static void flush_held(Forth *f, int may_raise);
_Noreturn void forth_error(Forth *f, const char *fmt, ...) {
    va_list ap;

    flush_held(f, 0);   /* (said, not raised: an error is under way) */
    va_start(ap, fmt);
    vsnprintf(f->error, sizeof(f->error), fmt, ap);
    va_end(ap);
    if (f->t != NULL && task_sys_or_null(f->t) != NULL) {   /* (the task's own copy: error-message's) */
        memcpy(f->t->sys->error, f->error, sizeof(f->error));
    }
    f->thrown = f->throwing != 0 ? f->throwing : -1;   /* (what catch gives: throw's value, or -1 for the system's) */
    f->throwing = 0;
    f->quiet = 0;
    f->error_chain[0] = 0;
    if (!caught_here(f)) {
        forth_backtrace(f, f->error_chain, sizeof(f->error_chain));
    }
    if (caught_here(f)) {
        /* inside `catch`: the caller decides what to say */
    } else {
        char line[sizeof(f->said) + 300];

        f->errors++;
        if (f->nsrc > 0) {
            snprintf(line, sizeof(line), "error: %s (%s:%d)", f->error, f->src[f->nsrc - 1].name, f->src[f->nsrc - 1].line);
        } else {
            snprintf(line, sizeof(line), "error: %s", f->error);
        }
        /* the same error again and again (a handler failing each frame): said once, then a count
         * now and then, and when another comes (the same message: where it is read from is only
         * the outer source's line - a test's, the frames run from it) */
        if (strcmp(f->error, f->said) == 0) {
            f->quiet = 1;
            if (++f->repeats % ERROR_REPEATS_SAID == 0) {
                forth_printf(f, "(the error above: %ld times more)\n", f->repeats);
            }
        } else {
            if (f->repeats > 0) {
                forth_printf(f, "(the error above: %ld times more)\n", f->repeats);
            }
            snprintf(f->said, sizeof(f->said), "%s", f->error);
            f->repeats = 0;
            forth_printf(f, "\n%s\n", line);
            if (f->error_chain[0]) {
                forth_printf(f, "  in %s\n", f->error_chain);
            }
        }
    }
    if (f->catch == NULL) {
        abort();
    }
    longjmp(*f->catch, 1);
}

/* ---- the dictionary ---- */

static void align_here(Forth *f) {
    UCell a = (UCell)f->here;

    a = (a + sizeof(Cell) - 1) & ~(UCell)(sizeof(Cell) - 1);
    f->here = (uint8_t *)a;
}

static void *allot(Forth *f, size_t n) {
    void *p = f->here;

    if (n > (size_t)(f->end - f->here)) {
        forth_error(f, "dictionary full");
    }
    f->here += n;
    return p;
}

static void comma(Forth *f, Cell x) {
    align_here(f);
    *(Cell *)allot(f, sizeof(Cell)) = x;
}

static void comma_float(Forth *f, Float x) {
    Cell c[FCELLS];
    size_t i;

    memset(c, 0, sizeof(c));
    memcpy(c, &x, sizeof(x));
    for (i = 0; i < FCELLS; i++) {
        comma(f, c[i]);
    }
}

/* which of the dictionary's cells begin a word (a bit each): how a token from the stack is known for one. A word
 * begins on a cell; space given back takes its words' bits with it. */
static size_t word_map_bytes(size_t dict_bytes) { return dict_bytes / sizeof(Cell) / CHAR_BIT + 1; }
static size_t dict_cell(const Forth *f, const uint8_t *p) { return (size_t)(p - f->mem) / sizeof(Cell); }
static void mark_word(Forth *f, const Word *w) {
    size_t k = dict_cell(f, (const uint8_t *)w);

    f->word_map[k / CHAR_BIT] |= (uint8_t)(1u << (k % CHAR_BIT));
}
static int is_word(const Forth *f, Cell x) {
    const uint8_t *p = (const uint8_t *)x;
    size_t k;

    if (p < f->mem || p >= f->here || (UCell)x % sizeof(Cell) != 0) {
        return 0;
    }
    k = dict_cell(f, p);
    return (f->word_map[k / CHAR_BIT] >> (k % CHAR_BIT)) & 1;
}
static void table_remove(Vocab *v, Word *w);

/* the dictionary given back down to p: the words that began there are words no more - out of their vocabularies'
 * lists and tables too (the space is written over next: a list left through it would lead anywhere) */
static void here_back(Forth *f, uint8_t *p) {
    size_t k = dict_cell(f, p + sizeof(Cell) - 1), end = dict_cell(f, f->here + sizeof(Cell) - 1);
    Vocab *v;

    for (v = f->vocabs; v != NULL; v = v->next) {   /* (a vocabulary's list: newest first, so the highest first) */
        while (v->latest != NULL && (uint8_t *)v->latest >= p) {
            if (v->latest->len > 0) {
                table_remove(v, v->latest);
            }
            v->latest = v->latest->link;
        }
    }
    if (f->latest != NULL && (uint8_t *)f->latest >= p) {
        f->latest = f->m.current->latest;
    }

    for (; k < end; k++) {
        f->word_map[k / CHAR_BIT] &= (uint8_t)~(1u << (k % CHAR_BIT));
    }
    f->here = p;
    forth_native_forgot(f, p);
    for (k = 0, end = 0; k < (size_t)f->ntokens; k++) {   /* (what the checker knew of the words gone) */
        if ((const uint8_t *)f->tokens[k].w < p) {
            f->tokens[end++] = f->tokens[k];
        }
    }
    f->ntokens = (int)end;
    for (k = 0, end = 0; k < (size_t)f->nshared; k++) {
        if ((const uint8_t *)f->shared[k].w < p) {
            f->shared[end++] = f->shared[k];
        } else {
            free(f->shared[k].stored);
            free(f->shared[k].fetched);
        }
    }
    if ((int)end != f->nshared) {
        f->nshared = (int)end;
        forth_shared_pruned(f);
    }
    for (k = 0, end = 0; k < (size_t)f->nmeant_shared; k++) {
        if ((const uint8_t *)f->meant_shared[k] < p) {
            f->meant_shared[end++] = f->meant_shared[k];
        }
    }
    f->nmeant_shared = (int)end;
}

static Word *in_vocab(Vocab *v, const char *name, size_t len);
static Word *in_vocab_hashed(Vocab *v, const char *name, size_t len, uint32_t h);
static uint32_t name_hash(const char *name, size_t len);
static void table_add(Forth *f, Vocab *v, Word *w);
static int name_beyond_core(const Forth *f, uint32_t h);
static int same_name(const char *a, const char *b, size_t n);
static void docol(Forth *f, Word *w);
static void doforward(Forth *f, Word *w);
static int is_colon(const Word *w);

static Word *new_word(Forth *f, const char *name, size_t len, Code code) {
    Word *w;

    if (len > WORD_NAME_MAX) {
        forth_error(f, "name too long: %.*s", (int)len, name);
    }
    forth_flush_warning(f);
    if (f->defining != NULL) {   /* (its header would land in the middle of the definition's code) */
        forth_error(f, "%.*s: defined while %.*s is being compiled", (int)len, name, (int)f->defining->len,
                    f->defining->name);
    }
    if (!f->m.listener && len > 0 && name_beyond_core(f, name_hash(name, len))) {
        /* (a word named as one of a vocabulary it uses is its own to define: two vocabularies may each have a word of
         * a name - vocabulary:word says which. What is said is a bare name that could be either: `ambiguous`.) */
        Word *older = f->m.current != f->core ? in_vocab(f->m.current, name, len) : NULL;

        /* (its own: the later wins on - but for one a reload replaces, from before it began) */
        if (older != NULL && !(f->reloading > 0 && (const uint8_t *)older < f->reload_mark)) {
            forth_warn(f, "%.*s is defined again in %s", (int)len, name, f->m.current->name);
        }
    }
    align_here(f);
    w = allot(f, sizeof(Word));
    memset(w, 0, sizeof(Word));
    mark_word(f, w);
    w->vocab = f->m.current;
    w->link = f->m.current->latest;
    w->code = code;
    w->len = (uint8_t)len;
    memcpy(w->name, name, len);
    f->m.current->latest = w;
    f->latest = w;
    if (len > 0) {   /* (a nameless one - :noname, a handler - is never looked up) */
        table_add(f, f->m.current, w);
    }
    return w;
}

/* ---- a word defined again by a reload (`reload`) or at the prompt: the old one forwards to it, so whatever was
 * compiled for the old one - other vocabularies' words, actors' handlers - runs the new one from its next call (a
 * call under way finishes as it began). Not a word that calls the one it replaces (`: f f 1+ ;`, a wrapper): that one
 * would call itself - so its old callers keep the old one, and it is said. ---- */

/* where the next word begins (or here) - not a quotation's headless word inside the body, which a branch jumps over
 * (`[: ... ;]`: branch past, the word, its code): the body goes on after it */
static const Cell *body_end(const Forth *f, const Word *w) {
    const Cell *p = w->body;

    while ((const uint8_t *)p < f->here) {
        if (is_word(f, (Cell)p)) {
            const Cell *target = p - 2 >= w->body && (Word *)p[-2] == f->w_branch ? (const Cell *)p[-1] : NULL;

            if (target == NULL || target <= p || (const uint8_t *)target > f->here) {
                break;
            }
            p = target;   /* (a quotation inside: on past it) */
            continue;
        }
        p++;
    }
    return p;
}

static int calls(const Forth *f, const Word *w, const Word *x) {   /* does colon definition w's code name x? */
    const Cell *p, *end = body_end(f, w);

    for (p = w->body; p < end; p++) {
        if (*p == (Cell)x) {
            return 1;
        }
    }
    return 0;
}

static long callers_of(const Forth *f, const Word *x) {   /* the calls of x compiled in the colon definitions there are */
    const uint8_t *p = f->mem;
    long n = 0;
    int colon = 0;

    while (p < f->here) {
        if (is_word(f, (Cell)p)) {
            const Word *w = (const Word *)p;

            colon = is_colon(w) || w->code == doforward;
            p = (const uint8_t *)w->body;
            continue;
        }
        if (colon && *(const Cell *)p == (Cell)x) {
            n++;
        }
        p += sizeof(Cell);
    }
    return n;
}

static void forward(Forth *f, Word *old, Word *to) {
    if (is_colon(to) && calls(f, to, old)) {
        forth_printf(f, "(%s:%s calls the %s it replaces: what was compiled for that one keeps it)\n", to->vocab->name,
                     to->name, to->name);
        return;
    }
    forth_effect_changed(f, old, to, callers_of(f, old));
    if (old->code == forth_donative) {   /* (machine code calling it: on to the new one's - a stub if it has none) */
        Word *target = to;

        if (to->code != forth_donative) {
            align_here(f);
            target = allot(f, sizeof(Word));
            memset(target, 0, sizeof(Word));
            mark_word(f, target);
            target->code = docol;
            target->vocab = to->vocab;
            snprintf(target->name, sizeof(target->name), "%s", to->name);   /* (its length 0: found by no one) */
            comma(f, (Cell)to);
            comma(f, (Cell)f->w_exit);
            forth_native_compile(f, target, (const Cell *)f->here);
        }
        if (!forth_native_forward(f, old, target)) {
            forth_printf(f, "(%s:%s: machine code compiled for the old one keeps it)\n", to->vocab->name, to->name);
        }
    }
    old->code = doforward;
    old->does = (Cell *)to;
    old->effect = to->effect;
    old->effect_tried = to->effect_tried;
    forth_native_redefined(f, old);   /* (machine code with old's words written into it: made again, calling it) */
}

/* every word of v made before `mark` that a newer word of its name, made since, hides: forwarded to the newest
 * (one forwarded already forwards on through the one it went to) */
static void forward_hidden(Forth *f, Vocab *v, const uint8_t *mark) {
    Word *w;

    for (w = v->latest; w != NULL && (const uint8_t *)w >= mark; w = w->link) {
        Word *x;

        if (w->len == 0 || (w->flags & WORD_HIDDEN) || in_vocab(v, w->name, w->len) != w) {
            continue;   /* (nameless, or not the newest of its name) */
        }
        for (x = w->same; x != NULL; x = x->same) {
            if ((const uint8_t *)x < mark && x->len == w->len && same_name(x->name, w->name, w->len) &&
                x->code != doforward) {
                forward(f, x, w);
            }
        }
    }
}

static void forward_all_hidden(Forth *f, const uint8_t *mark) {   /* (the prompt's: any vocabulary it defined in) */
    Vocab *v;

    if (f->here == mark) {
        return;
    }
    for (v = f->vocabs; v != NULL; v = v->next) {
        if (v != f->core && v->latest != NULL && (const uint8_t *)v->latest >= mark) {
            forward_hidden(f, v, mark);
        }
    }
}

/* names are case-insensitive: ASCII letters folded (as the "C" locale's tolower does, without the call) */
static unsigned char fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c; }

static int same_name(const char *a, const char *b, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (fold((unsigned char)a[i]) != fold((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

/* ---- each vocabulary's table: its named words by name, so a lookup is a probe, not a walk of every word ---- */

#define TABLE_FIRST_SLOTS 64
#define TABLE_LOAD 2   /* words per slot before the table doubles */

static uint32_t name_hash(const char *name, size_t len) {
    uint32_t h = 2166136261u;   /* (FNV-1a, over the folded name) */
    size_t i;

    for (i = 0; i < len; i++) {
        h = (h ^ fold((unsigned char)name[i])) * 16777619u;
    }
    return h;
}

static Word **slot_of(Vocab *v, const char *name, size_t len) {
    return &v->table[name_hash(name, len) & (uint32_t)(v->slots - 1)];
}

/* ---- the names beyond the core: a bit for each name's hash, set as a vocabulary other than the core gets a word of
 * that name (never cleared: a word dropped leaves its bit, and a lookup only looks a little further for it). A
 * source's lookup walks its own vocabulary, each it uses, then the core - where most of what it says is found (`dup`,
 * `+`), or nowhere (a number) - so a name with its bit clear skips the walk. ---- */

#define NAME_BITS_LOG2 18   /* 2^18 bits (32 KiB): a few in a hundred set by a host's tens of thousands of words */
#define NAME_BITS_BYTES ((size_t)1 << (NAME_BITS_LOG2 - 3))

static uint32_t name_bit(uint32_t h) { return h >> (32 - NAME_BITS_LOG2); }   /* (the hash's best mixed bits) */
static void note_name_beyond_core(Forth *f, uint32_t h) {
    f->names_beyond_core[name_bit(h) / CHAR_BIT] |= (uint8_t)(1u << (name_bit(h) % CHAR_BIT));
}
static int name_beyond_core(const Forth *f, uint32_t h) {
    return (f->names_beyond_core[name_bit(h) / CHAR_BIT] >> (name_bit(h) % CHAR_BIT)) & 1;
}

/* the table twice the size: its words put back oldest first, so each slot stays newest first */
static void table_grow(Forth *f, Vocab *v) {
    int slots = v->slots ? v->slots * 2 : TABLE_FIRST_SLOTS, n = 0, i;
    Word **table = calloc((size_t)slots, sizeof(Word *)), **order, *w;

    order = malloc((size_t)(v->nwords + 1) * sizeof(Word *));
    if (table == NULL || order == NULL) {
        free(table);
        free(order);
        forth_error(f, "vocabulary %s: out of memory", v->name);
    }
    for (w = v->latest; w != NULL; w = w->link) {
        if (w->len > 0) {
            order[n++] = w;
        }
    }
    free(v->table);
    v->table = table;
    v->slots = slots;
    for (i = n - 1; i >= 0; i--) {
        Word **slot = slot_of(v, order[i]->name, order[i]->len);

        order[i]->same = *slot;
        *slot = order[i];
    }
    free(order);
}

static void table_add(Forth *f, Vocab *v, Word *w) {
    Word **slot;

    if (v != f->core) {
        note_name_beyond_core(f, name_hash(w->name, w->len));
    }
    if (v->nwords >= v->slots * TABLE_LOAD) {
        v->nwords++;   /* (counted first: the grown table takes it in, from the vocabulary's list) */
        table_grow(f, v);
        return;
    }
    slot = slot_of(v, w->name, w->len);
    w->same = *slot;
    *slot = w;
    v->nwords++;
}

static void table_remove(Vocab *v, Word *w) {
    Word **p;

    if (v->slots == 0 || w->len == 0) {
        return;
    }
    for (p = slot_of(v, w->name, w->len); *p != NULL; p = &(*p)->same) {
        if (*p == w) {
            *p = w->same;
            v->nwords--;
            return;
        }
    }
}

/* ---- vocabularies ---- */

static Vocab *vocab_find(Forth *f, const char *name, size_t len) {
    Vocab *v;

    for (v = f->vocabs; v != NULL; v = v->next) {
        if (strlen(v->name) == len && same_name(v->name, name, len)) {
            return v;
        }
    }
    return NULL;
}

Vocab *forth_vocab(Forth *f, const char *name) {
    Vocab *v = vocab_find(f, name, strlen(name));

    if (v == NULL) {
        v = calloc(1, sizeof(Vocab));
        if (v == NULL) {
            forth_error(f, "vocabulary %s: out of memory", name);
        }
        snprintf(v->name, sizeof(v->name), "%s", name);
        v->next = f->vocabs;
        f->vocabs = v;
    }
    return v;
}

Vocab *forth_find_vocab(Forth *f, const char *name) {
    return vocab_find(f, name, strlen(name));
}

static void order_add(Manifest *m, Vocab *v) {
    int i;

    for (i = 0; i < m->norder; i++) {
        if (m->order[i] == v) {
            return;
        }
    }
    if (m->norder < ORDER_MAX) {
        m->order[m->norder++] = v;
    } else {
        fprintf(stderr, "forth: a file uses more than %d vocabularies - %s left out (ORDER_MAX)\n", ORDER_MAX, v->name);
    }
}

void forth_set_current(Forth *f, Vocab *v) {
    f->m.current = v;
    order_add(&f->m, v);
}

void forth_add_root(Forth *f, const char *dir) {
    if (f->nroots < (int)(sizeof(f->roots) / sizeof(f->roots[0]))) {
        snprintf(f->roots[f->nroots++], sizeof(f->roots[0]), "%s", dir);
    }
}

/* a source's starting view: only the core; definitions into `scratch` until IN: */
static Manifest fresh_manifest(Forth *f, int listener) {
    Manifest m;

    memset(&m, 0, sizeof(m));
    m.current = forth_vocab(f, "scratch");
    m.order[m.norder++] = m.current;
    m.listener = listener;
    return m;
}

/* (h: the name's hash - a lookup through many vocabularies works it out once) */
static Word *in_vocab_hashed(Vocab *v, const char *name, size_t len, uint32_t h) {
    Word *w;

    if (v->slots == 0 || len == 0) {
        return NULL;
    }
    for (w = v->table[h & (uint32_t)(v->slots - 1)]; w != NULL; w = w->same) {
        if (w->len == len && !(w->flags & WORD_HIDDEN) && same_name(w->name, name, len)) {
            return w;
        }
    }
    return NULL;
}

static Word *in_vocab(Vocab *v, const char *name, size_t len) { return in_vocab_hashed(v, name, len, name_hash(name, len)); }

/* the one word of that name among vocabularies (an error if two have it) */
static Word *unique(Forth *f, Word *found, Word *w, const char *name, size_t len) {
    if (found != NULL && w != NULL && found != w) {
        forth_error(f, "%.*s is in both %s and %s: say %s:%.*s or %s:%.*s", (int)len, name, found->vocab->name,
                    w->vocab->name, found->vocab->name, (int)len, name, w->vocab->name, (int)len, name);
    }
    return found != NULL ? found : w;
}

/* Lookup: `vocab:name` exactly; else the source's own vocabulary, then the ones it uses - the
 * last in its USING: first, so a source's nearer vocabularies hide the general ones' names -
 * then (at the prompt) every loaded one (exactly one may have the name), then the core */
Word *forth_find(Forth *f, const char *name, size_t len) {
    Word *found = NULL, *w;
    Vocab *v;
    size_t i;
    uint32_t h;
    int k;

    for (i = 1; i + 1 < len; i++) {
        if (name[i] == ':') {
            v = vocab_find(f, name, i);
            if (v != NULL) {
                return in_vocab(v, name + i + 1, len - i - 1);
            }
            break;
        }
    }
    h = name_hash(name, len);
    if (!name_beyond_core(f, h)) {   /* (no vocabulary but the core has it: most words, and every number) */
        return in_vocab_hashed(f->core, name, len, h);
    }
    if ((w = in_vocab_hashed(f->m.current, name, len, h)) != NULL) {
        return w;
    }
    for (k = f->m.norder - 1; k >= 0; k--) {   /* (no ambiguity: the nearest wins) */
        if (f->m.order[k] != f->core && f->m.order[k] != f->m.current &&
            (w = in_vocab_hashed(f->m.order[k], name, len, h)) != NULL) {
            return w;
        }
    }
    if (found == NULL && f->m.listener) {
        for (v = f->vocabs; v != NULL; v = v->next) {
            if (v != f->core) {
                found = unique(f, found, in_vocab_hashed(v, name, len, h), name, len);
            }
        }
    }
    return found != NULL ? found : in_vocab_hashed(f->core, name, len, h);
}

static int effects_differ(const ForthEffect *a, const ForthEffect *b) {
    return a->kind != b->kind || a->in != b->in || a->out != b->out || a->fin != b->fin || a->fout != b->fout;
}
/* two vocabularies that are one source's: the same, or a vocabulary and its <PRIVATE half (name, name.private) */
static int same_source(const Vocab *a, const Vocab *b) {
    return a == b || a->parent == b || b->parent == a || (a->parent != NULL && a->parent == b->parent);
}
/* Another word of that name in sight than w (the one the bare name found), where the reader is owed which is meant.
 * Two vocabularies a source uses may each have a word of a name: the bare name is then ambiguous - the one later in
 * USING: is taken, and that is said, whatever their effects. And the core's word behind a used vocabulary's, if their
 * stack effects differ (a library's `build` taken where the core's was meant).
 * NULL if none. A source's own word is no ambiguity: it defined it, and its own win (as over the core's). Nor is a
 * qualified name (vocabulary:word): it means the one it says. */
static Word *ambiguous(Forth *f, Word *w, const char *name, size_t len) {
    uint32_t h = name_hash(name, len);
    Word *x;
    int k;

    if (memchr(name, ':', len) != NULL || !name_beyond_core(f, h) || w->vocab == f->core ||
        same_source(w->vocab, f->m.current) || strcmp(w->vocab->name, "messages") == 0) {
        return NULL;
    }
    for (k = f->m.norder - 1; k >= 0; k--) {
        Vocab *v = f->m.order[k];

        if (v != f->core && !same_source(v, w->vocab) && !same_source(v, f->m.current) &&
            strcmp(v->name, "messages") != 0 && (x = in_vocab_hashed(v, name, len, h)) != NULL && x != w) {
            return x;
        }
    }
    x = in_vocab_hashed(f->core, name, len, h);
    return x != NULL && x != w && w->effect.kind != EFFECT_UNKNOWN && x->effect.kind != EFFECT_UNKNOWN &&
           effects_differ(&w->effect, &x->effect) ? x : NULL;
}

/* the word defined right after w (where w's body ends), or NULL if none */
static Word *word_after(Forth *f, Word *w) {
    Word *best = NULL, *x;
    Vocab *v;

    for (v = f->vocabs; v != NULL; v = v->next) {
        for (x = v->latest; x != NULL; x = x->link) {
            if (x > w && (best == NULL || x < best)) {
                best = x;
            }
        }
    }
    return best;
}

/* ---- the call chain: the words running, from the return stacks and where each nesting began ---- */

#define BACKTRACE_WORDS 24

/* a return address (or a task's next cell): inside the dictionary, just after a word's call */
static int code_address(Forth *f, const Cell *p) {
    const Word *w;

    if ((const uint8_t *)p <= f->mem || (const uint8_t *)p >= f->here || ((uintptr_t)p % sizeof(Cell)) != 0) {
        return 0;
    }
    w = (const Word *)p[-1];
    return (const uint8_t *)w >= f->mem && (const uint8_t *)w < f->here && ((uintptr_t)w % sizeof(Cell)) == 0 &&
           w->code != NULL;
}

/* the word whose body holds p: the nearest header before it */
static const Word *word_holding(Forth *f, const void *p) {
    const Word *best = NULL, *x;
    Vocab *v;

    for (v = f->vocabs; v != NULL; v = v->next) {
        for (x = v->latest; x != NULL; x = x->link) {
            if ((const void *)x < p && (best == NULL || x > best)) {
                best = x;
            }
        }
    }
    return best;
}

typedef struct Chain {
    char *buf;
    size_t n, k;
    int words;
} Chain;

static void chain_add(Forth *f, Chain *c, const Cell *p) {
    const Word *w;
    const char *name;

    if (!code_address(f, p) || (w = word_holding(f, p - 1)) == NULL) {
        return;
    }
    if (c->words == BACKTRACE_WORDS) {
        name = "...";
    } else if (c->words > BACKTRACE_WORDS) {
        return;
    } else {
        name = w->len > 0 || w->name[0] ? w->name : ":noname";   /* (a handler: named, though not findable) */
    }
    if (c->k < c->n) {
        c->k += (size_t)snprintf(c->buf + c->k, c->n - c->k, "%s%s", c->words > 0 ? " <- " : "", name);
        if (c->k >= c->n) {
            c->k = c->n - 1;
        }
    }
    c->words++;
}

size_t forth_backtrace(Forth *f, char *buf, size_t n) {
    Chain c = {buf, n, 0, 0};
    Task *t = f->t;
    Cell *ip;
    int level, top;

    if (n == 0) {
        return 0;
    }
    buf[0] = 0;
    if (t == NULL) {
        return 0;
    }
    ip = t->ip;
    top = t->rp;
    for (level = f->depth;; level--) {   /* each nesting: its words, innermost first, then whose called it */
        const Nesting *at = level > 0 && level <= NESTING_MAX ? &f->nesting[level - 1] : NULL;
        int bottom = at != NULL && at->t == t ? at->rp : 0, i;

        chain_add(f, &c, ip);
        for (i = top - 1; i >= bottom && i < RSTACK_CELLS; i--) {
            chain_add(f, &c, (const Cell *)t->rs[i]);
        }
        if (at == NULL || at->t == NULL) {
            break;
        }
        t = at->t;
        ip = at->ip;
        top = at->rp;
    }
    return c.k;
}

PRIM(p_backtrace) {   /* ( -- addr len ) the words running: this one's caller first */
    static _Thread_local char chain[BACKTRACE_BYTES];
    size_t n = forth_backtrace(f, chain, sizeof(chain));

    PUSH(chain);
    PUSH(n);
}

static _Thread_local Forth *sCrashForth;   /* (each thread's: a node's crash names its own words) */

static void crash_report(const char *what) {
    char chain[BACKTRACE_BYTES];

    if (sCrashForth != NULL && forth_backtrace(sCrashForth, chain, sizeof(chain)) > 0) {
        fprintf(stderr, "%s, in the words %s\n", what, chain);
    }
}

#ifdef FORTH_ASAN
/* (the address sanitizer's report: called by it as it reports - its own signal handlers kept) */
void __asan_on_error(void);
void __asan_on_error(void) { crash_report("the sanitizer's report below"); }
#else
static void on_crash(int sig) {
    crash_report(sig == SIGSEGV ? "crashed (a bad address)" : sig == SIGFPE ? "crashed (arithmetic)" : "crashed");
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

#if !defined(FORTH_ASAN) && !defined(_WIN32)
#define CRASH_STACK_BYTES (64 * 1024)   /* the report's own stack: a task's stack overflowed is no place to run it */

static void catch_signal(int sig) {   /* on_crash for sig, run on the alternate stack */
    struct sigaction plain;

    memset(&plain, 0, sizeof(plain));
    plain.sa_handler = on_crash;
    sigemptyset(&plain.sa_mask);
    plain.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigaction(sig, &plain, NULL);
}
#endif

void forth_report_crashes(Forth *f) {
    sCrashForth = f;
#if !defined(FORTH_ASAN) && !defined(_WIN32)   /* (ASan has signal stacks of its own, and frees the thread's as its) */
    {
        static _Thread_local void *alternate;   /* (each thread's, made once; the thread's to the end) */
        stack_t ss;

        if (alternate == NULL && (alternate = malloc(CRASH_STACK_BYTES)) != NULL) {
            memset(&ss, 0, sizeof(ss));
            ss.ss_sp = alternate;
            ss.ss_size = CRASH_STACK_BYTES;
            sigaltstack(&ss, NULL);
        }
        catch_signal(SIGSEGV);
        catch_signal(SIGFPE);
        catch_signal(SIGILL);
        catch_signal(SIGBUS);
    }
#elif !defined(FORTH_ASAN)
    signal(SIGSEGV, on_crash);
    signal(SIGFPE, on_crash);
    signal(SIGILL, on_crash);
#endif
}

/* ---- the inner interpreter ---- */

static long long monotonic_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int forth_task_may_wait(Forth *f, const Word *w) {
    if (f->slice <= 0 || !forth_actor_handler_task(f) || f->depth != f->task_depth || f->defining != f->turn_defining ||
        task_sys_or_null(f->t) == NULL) {
        return 0;
    }
    /* (threaded, the primitive runs again as the task goes on - from the cell before where it goes on: so only if
     * that cell is it, called directly, not through execute) */
    return forth_native_in_coroutine(f) || (f->t->ip != NULL && (const Word *)f->t->ip[-1] == w);
}
int forth_task_sleep(Forth *f, long ms) {
    struct ForthTaskSys *sys = task_sys(f, f->t);

    sys->wake_ms = monotonic_ms() + ms;
    f->yielded = 1;
    if (forth_native_in_coroutine(f)) {
        do {   /* (not ready till its time: it shouldn't go on sooner, but if it does, it waits again) */
            f->yielded = 1;
            forth_native_suspend(f);
        } while (monotonic_ms() < sys->wake_ms);
        sys->wake_ms = 0;
        return 1;
    }
    f->t->ip--;   /* (threaded: sleep-ms again as it goes on - forth_task_slept) */
    return 1;
}
int forth_task_slept(Forth *f) {
    struct ForthTaskSys *sys = f->t->sys;

    if (sys == NULL || sys->wake_ms == 0 || monotonic_ms() < sys->wake_ms) {
        return 0;
    }
    sys->wake_ms = 0;
    return 1;
}
long long forth_task_wake_ms(const Task *t) { return t->sys != NULL ? t->sys->wake_ms : 0; }
/* it waits so many frames (`wait`): where nothing counts them, for ever */
int forth_task_waits_frames(const Task *t) { return t->state == TASK_WAITING; }
int forth_task_urgent(Forth *f, const Task *t) {
    const struct ForthTaskSys *sys = t->sys;

    return forth_task_ready(f, t) && sys != NULL && (sys->gave_way || sys->job != NULL || sys->wake_ms != 0);
}

/* the running task may give way here: a sliced turn's, at its own level (not inside code C called - atomically,
 * map-each's, a sort's comparison - nor while a definition is being made) */
static int may_give_way(const Forth *f) {
    return f->turn_slice > 0 && f->t != f->main && f->depth == f->task_depth && f->defining == f->turn_defining;
}
/* work a single step did that took the time of many (a text compressed, JSON read - a word of C's is one step
 * however long it works): counted against the turn's slice, which ends that much sooner - a handler doing such work
 * over and over gives way as one doing it in Forth would. Only where the turn could give way now: steps taken where
 * it can't count toward the limit that is an error (TASK_BUDGET), and this is no loop that never ends. */
void forth_slice_charge(Forth *f, size_t steps) {
    if (f->budget > 0 && may_give_way(f)) {
        f->budget = (size_t)f->budget > steps ? f->budget - (Cell)steps : 0;
    }
}
/* its slice is over (f->budget run out): it gives way - as `yield`: the next turn goes on here - if it may; if it
 * can't, another slice, till TASK_BUDGET steps have gone with no place to give way (a loop that never ends: stopped).
 * 0: go on now; 1: it gave way (threaded: the interpreter's loop ends; with machine code, this returns when it goes on) */
#define TASK_BUDGET 20000000L
int forth_slice_over(Forth *f) {
    if (f->turn_slice <= 0) {
        forth_ran_too_long(f);
    }
    if (may_give_way(f)) {
        if (task_sys_or_null(f->t) != NULL) {   /* (out of memory: it goes on at the next frame instead) */
            f->t->sys->gave_way = 1;
        }
        f->unsliced = 0;
        f->budget = f->turn_slice;   /* (as it goes on: a fresh slice - with machine code, its next turn's) */
        f->yielded = 1;
        forth_native_suspend(f);
        return f->yielded;
    }
    f->unsliced += f->turn_slice;
    if (f->unsliced >= TASK_BUDGET) {
        forth_ran_too_long(f);
    }
    f->budget = f->turn_slice;
    return 0;
}

_Noreturn void forth_ran_too_long(Forth *f) {
    if (f->t == f->main) {   /* (text being interpreted: the host's limit - forth_set_eval_budget) */
        forth_error(f, "ran too long (a loop that never ends?)");
    }
    forth_error(f, "task %s ran too long without yielding", f->t->name);
}

/* run threaded code from IP until it stops (IP = NULL: `halt`) or the task yields */
static void run(Forth *f) {
    while (IP != NULL && !f->yielded) {
        Word *w;

        if (f->budget >= 0 && --f->budget < 0 && forth_slice_over(f)) {
            break;   /* (it gave way before this word: the next turn runs it) */
        }
        w = (Word *)*IP++;
        w->code(f, w);
    }
}

void forth_run(Forth *f) { run(f); }

/* (machine code calling a word that went on in threaded code - a colon definition left threaded, `execute` of one:
 * that code run until it comes back to where the machine code goes on) */
void forth_run_to(Forth *f, Cell *stop) {
    int floor = f->t->rp - 1;   /* (the cell where it comes back to stop: taken off as it does) */

    while (IP != stop && IP != NULL) {
        Word *w;

        if (f->budget >= 0 && --f->budget < 0) {
            forth_slice_over(f);   /* (with machine code under it: a way given returns here, as it goes on) */
        }
        w = (Word *)*IP++;
        w->code(f, w);
        if (f->t->rp < floor) {   /* (it took the place its caller goes on from - `r> drop` - and went on there) */
            forth_error(f, "%s: went back past its caller, which is machine code (it can't follow: a word that "
                           "takes its caller's return address runs only among threaded words)", w->name);
        }
    }
}

void forth_execute(Forth *f, Word *w) {
    Cell *saved = IP;
    Cell code[2];

    code[0] = (Cell)w;
    code[1] = (Cell)f->w_halt;
    if (f->depth < NESTING_MAX) {   /* (for the call chain: whose words called this) */
        f->nesting[f->depth] = (Nesting){f->t, saved, f->t->rp};
    }
    IP = code;
    f->depth++;
    run(f);
    f->depth--;
    IP = saved;
}

/* code fields */

static void docol(Forth *f, Word *w) {   /* a colon definition: run its body */
    rpush(f, (Cell)IP);
    IP = w->body;
}

/* a colon definition: threaded code, run by docol - or translated to machine code too (native.c) */
static int is_colon(const Word *w) { return w->code == docol || w->code == forth_donative; }

/* a word defined again by a reload or at the prompt (forward, below): in its place, the newer one (its does) - so
 * every call compiled for it, threaded or machine code, goes on there */
static void doforward(Forth *f, Word *w) {
    Word *to = (Word *)w->does;

    to->code(f, to);
}

static void dovar(Forth *f, Word *w) {   /* CREATE: the body's address */
    PUSH(w->body);
}

static void docon(Forth *f, Word *w) {
    PUSH(w->body[0]);
}

static void dofcon(Forth *f, Word *w) {
    Float x;

    memcpy(&x, w->body, sizeof(x));
    FPUSH(x);
}

static void dodoes(Forth *f, Word *w) {   /* made by CREATE ... DOES>: body address, then the DOES> code */
    PUSH(w->body);
    rpush(f, (Cell)IP);
    IP = w->does;
}

static Word *prim_word(Forth *f, const char *name, Code code, int flags, int operands) {
    Word *w = new_word(f, name, strlen(name), code);

    w->flags = (uint8_t)flags;
    w->operands = (uint8_t)operands;
    return w;
}

Word *forth_prim(Forth *f, const char *name, Code code) {
    return prim_word(f, name, code, 0, 0);
}

Word *forth_constant(Forth *f, const char *name, Cell value) {
    Word *w = new_word(f, name, strlen(name), docon);

    comma(f, value);
    return w;
}

Word *forth_fconstant(Forth *f, const char *name, Float value) {
    Word *w = new_word(f, name, strlen(name), dofcon);

    comma_float(f, value);
    return w;
}

/* ---- threaded-code primitives (the compiler lays these down) ---- */

PRIM(p_lit) { PUSH(*IP++); }
PRIM(p_flit) {
    Float x;

    memcpy(&x, IP, sizeof(x));
    IP += FCELLS;
    FPUSH(x);
}
PRIM(p_litstring) {   /* ( -- addr len ): the length, then the bytes */
    Cell n = *IP++;

    PUSH(IP);
    PUSH(n);
    IP += CELLS_FOR((size_t)n);
}
PRIM(p_exit) { IP = (Cell *)rpop(f); }

/* ---- locals ({: a b F: x -- :}): a frame on the return stack, made at the first {: and dropped
 * as the word leaves; the names compile to fetches from it, `to name` to a store. Each {: takes
 * its values with stores, the last named first. A float local's slot holds the float's bits. ---- */
_Static_assert(sizeof(Float) <= sizeof(Cell), "a float local needs a cell of its own");
PRIM(p_locals) {   /* (locals) n: a frame of n, zeroed */
    Cell n = *IP++, j;
    Task *t = f->t;

    rpush(f, t->lp);
    t->lp = t->rp;
    for (j = 0; j < n; j++) {
        rpush(f, 0);
    }
}
PRIM(p_unlocals) {   /* the frame dropped (before the word returns) */
    Task *t = f->t;

    t->rp = t->lp;
    t->lp = (int)rpop(f);
}
PRIM(p_local_fetch) { Cell i = *IP++; PUSH(f->t->rs[f->t->lp + i]); }
PRIM(p_local_store) { Cell i = *IP++; f->t->rs[f->t->lp + i] = POP(); }
PRIM(p_flocal_fetch) {
    Float x;

    memcpy(&x, &f->t->rs[f->t->lp + *IP++], sizeof x);
    FPUSH(x);
}
PRIM(p_flocal_store) {
    Float x = FPOP();

    memcpy(&f->t->rs[f->t->lp + *IP++], &x, sizeof x);
}
/* ---- quotations' frames, and closures: a quotation using its word's locals. Every quotation begins with
 * (qframe): the frame of its own locals, under it the frame of the word it uses the locals of (a closure's call says
 * which: closure_lp) - so `outer@ k i` finds local i k frames out. One that needs no frame has (nop1) there. ---- */
PRIM(p_qframe) {   /* (qframe) n: a quotation's frame of n, zeroed; the frame it reaches out to under it */
    Task *t = f->t;
    Cell n = *IP++, j;

    rpush(f, t->closure_lp - 1);   /* (-1: none) */
    t->closure_lp = 0;
    rpush(f, t->lp);
    t->lp = t->rp;
    for (j = 0; j < n; j++) {
        rpush(f, 0);
    }
}
PRIM(p_unqframe) {   /* a quotation's frame dropped, and what it reached out to */
    Task *t = f->t;

    t->rp = t->lp;
    t->lp = (int)rpop(f);
    (void)rpop(f);
}
PRIM(p_nop1) { IP++; }   /* (a quotation's place for a frame it doesn't need) */
/* the frame k out from this quotation's: its word's (or an enclosing quotation's) */
static Cell *outer_frame(Forth *f, Cell k) {
    Task *t = f->t;
    Cell lp = t->lp;

    while (k-- > 0) {
        if (lp < 2 || lp > t->rp || (lp = t->rs[lp - 2]) < 0) {
            forth_error(f, "a quotation's word's locals: not there (the quotation run without its word)");
        }
    }
    return &t->rs[lp];
}
PRIM(p_outer_fetch) { Cell k = *IP++, i = *IP++; PUSH(outer_frame(f, k)[i]); }   /* (outer@) k i */
PRIM(p_outer_store) { Cell k = *IP++, i = *IP++, x = POP(); outer_frame(f, k)[i] = x; }
PRIM(p_fouter_fetch) {
    Cell k = *IP++, i = *IP++;
    Float x;

    memcpy(&x, &outer_frame(f, k)[i], sizeof x);
    FPUSH(x);
}
PRIM(p_fouter_store) {
    Cell k = *IP++, i = *IP++;
    Float x = FPOP();

    memcpy(&outer_frame(f, k)[i], &x, sizeof x);
}

/* a closure: the head of a word, made in its word's frame (CLOSURE_CELLS from cell `slot`) as the quotation's literal
 * is reached - its quotation (link) and that frame (does) - so it lasts as long as the frame, and costs nothing */
#define CLOSURE_CELLS 7
_Static_assert(offsetof(Word, name) + 1 <= CLOSURE_CELLS * sizeof(Cell), "a closure holds a word's head");
static void doclosure(Forth *f, Word *w);
static int live_closure(Forth *f, const Word *w) {   /* (in this task's frames now, and still a closure) */
    const Cell *c = (const Cell *)w;

    return c >= f->t->rs && c + CLOSURE_CELLS <= f->t->rs + f->t->rp && w->code == doclosure;
}
static void doclosure(Forth *f, Word *w) {
    Task *t = f->t;

    if (!live_closure(f, w)) {
        forth_error(f, "a quotation using its word's locals runs only while that word does (not kept, nor spawned)");
    }
    t->closure_lp = (int)(Cell)w->does + 1;
    if (((Word *)w->link)->code == forth_donative) {   /* (its quotation's machine code, run in its place) */
        forth_donative(f, (Word *)w->link);
        return;
    }
    rpush(f, (Cell)IP);
    IP = ((Word *)w->link)->body;
}
PRIM(p_closure) {   /* (closure) slot q: a closure of q made in this frame, left */
    Cell slot = *IP++;
    Word *q = (Word *)*IP++, *c = (Word *)&f->t->rs[f->t->lp + slot];

    memset(c, 0, CLOSURE_CELLS * sizeof(Cell));
    c->code = doclosure;
    c->link = q;
    c->does = (Cell *)(Cell)f->t->lp;
    PUSH(c);
}
PRIM(p_closure_call) {   /* (closure-call) q: q run with this frame's locals (a combinator compiled inline) */
    Word *q = (Word *)*IP++;

    f->t->closure_lp = f->t->lp + 1;
    if (q->code == forth_donative) {
        forth_donative(f, q);
        return;
    }
    rpush(f, (Cell)IP);
    IP = q->body;
}

/* a local's name: its index among this definition's (or quotation's) locals, -1 if it isn't one */
static int local_in(const struct ForthLocals *l, const char *s, size_t n) {
    int i;

    for (i = 0; i < l->n; i++) {
        if (strlen(l->names[i]) == n && strncasecmp(l->names[i], s, n) == 0) {
            return i;
        }
    }
    return -1;
}
static int local_index(Forth *f, const char *s, size_t n) { return local_in(&f->locals, s, n); }
PRIM(p_halt) { IP = NULL; }
PRIM(p_branch) { IP = (Cell *)*IP; }
PRIM(p_0branch) {
    if (POP() == 0) {
        IP = (Cell *)*IP;
    } else {
        IP++;
    }
}
static void dotaskvar(Forth *f, Word *w);
/* (does>): the latest word runs the rest of this definition; return now. The does> code of `constant`, `value`
 * (@) and `task-variable` (@ (task-var)) run in C instead - machine code calls C at once, where a does> word's
 * threaded code costs a trip through the inner interpreter at each use */
PRIM(p_does) {
    Word *made = f->latest;

    made->code = dodoes;
    made->does = IP;
    if ((Word *)IP[0] == f->w_fetch && (Word *)IP[1] == f->w_exit) {
        made->code = docon;   /* (its body's cell: a value's `to` writes it there, read each time) */
    } else if ((Word *)IP[0] == f->w_fetch && (Word *)IP[1] == f->w_task_var && (Word *)IP[2] == f->w_exit) {
        made->code = dotaskvar;
    }
    IP = (Cell *)rpop(f);
}
static int live_closure(Forth *f, const Word *w);
/* an execution token from the stack: a number there (a value fetched again), or an address no word begins at, is an
 * error, not a crash. Every word is in the dictionary (the word map says where), but for a closure (in its word's
 * frame): one of those must be still there. */
static Word *xt_arg(Forth *f, Cell x, const char *word) {
    if (x == 0) {
        forth_error(f, "%s: null", word);
    }
    if ((UCell)x < LOWEST_ADDRESS || x < 0 || (UCell)x % sizeof(Cell) != 0) {   /* (every word is cell-aligned) */
        forth_error(f, "%s: not a word: %ld", word, (long)x);
    }
    if ((const uint8_t *)x >= f->mem && (const uint8_t *)x < f->end) {
        if (!is_word(f, x)) {
            forth_error(f, "%s: not a word: no word begins at that address in the dictionary", word);
        }
    } else if (!live_closure(f, (const Word *)x)) {
        forth_error(f, "%s: not a word (a quotation using its word's locals, after that word returned?)", word);
    }
    if (((const Word *)x)->flags & WORD_COMPILED_ONLY) {   /* (it reads the cells after it where it is compiled) */
        forth_error(f, "%s: %s is the compiler's word, only in compiled code", word, ((const Word *)x)->name);
    }
    if ((const Word *)x == f->w_exit || (const Word *)x == f->w_leave) {   /* (they end the definition, or the loop,
                                        * they are compiled in: run by another word, they ended that one's - or, the
                                        * definition machine code, went where nothing was) */
        forth_error(f, "%s: %s ends the definition or the loop it is written in, not one it is run from", word,
                    ((const Word *)x)->name);
    }
    return (Word *)x;
}
PRIM(p_execute) {   /* not nested: jump into the word as the inner interpreter would */
    Word *x = xt_arg(f, POP(), "execute");

    x->code(f, x);
}

PRIM(p_jump) {   /* ( xt -- ) leave the running word for xt: a tail call (the event scripts' goto: the
                  * return stack doesn't grow however long a chain of them runs). Not from a word with
                  * locals (an error as it compiles) or a loop open: their frames stay. */
    Word *x = xt_arg(f, POP(), "jump");

    IP = (Cell *)rpop(f);
    x->code(f, x);
}

PRIM(p_abort_q) {   /* ( flag addr len -- ) an error with that message when flag is true (abort") */
    Cell n = POP(), a = POP();

    if (POP()) {
        size_t len;
        const char *msg = forth_string_arg(f, a, n, "abort\"", &len);

        forth_error(f, "%.*s", (int)len, msg);
    }
}

/* counted loops: the return stack holds the exit address, the limit and the index */
PRIM(p_do) {
    Cell exit = *IP++, start = POP(), limit = POP();

    rpush(f, exit);
    rpush(f, limit);
    rpush(f, start);
}
PRIM(p_qdo) {
    Cell exit = *IP++, start = POP(), limit = POP();

    if (start == limit) {
        IP = (Cell *)exit;
        return;
    }
    rpush(f, exit);
    rpush(f, limit);
    rpush(f, start);
}
static void loop_step(Forth *f, Cell n) {
    Task *t = f->t;
    Cell d, nd;

    if (t->rp < 3) {
        forth_error(f, "loop: not in a do loop");
    }
    d = (Cell)((UCell)t->rs[t->rp - 1] - (UCell)t->rs[t->rp - 2]);   /* (wrapping, as the sums do) */
    nd = (Cell)((UCell)d + (UCell)n);
    t->rs[t->rp - 1] = (Cell)((UCell)t->rs[t->rp - 1] + (UCell)n);
    if (((d ^ nd) & (d ^ n)) < 0) {   /* crossed the line between limit - 1 and limit (the sign changed, and not
                                       * by index - limit wrapping round past the largest number) */
        t->rp -= 3;
        IP++;
    } else {
        IP = (Cell *)*IP;
    }
}
PRIM(p_loop) { loop_step(f, 1); }
PRIM(p_ploop) { loop_step(f, POP()); }
PRIM(p_i) {
    if (f->t->rp < 3) {
        forth_error(f, "i: not in a do loop");
    }
    PUSH(f->t->rs[f->t->rp - 1]);
}
PRIM(p_j) {
    if (f->t->rp < 6) {
        forth_error(f, "j: not in a nested do loop");
    }
    PUSH(f->t->rs[f->t->rp - 4]);
}
PRIM(p_leave) {
    if (f->t->rp < 3) {
        forth_error(f, "leave: not in a do loop");
    }
    IP = (Cell *)f->t->rs[f->t->rp - 3];
    f->t->rp -= 3;
}
PRIM(p_unloop) {
    if (f->t->rp < 3) {
        forth_error(f, "unloop: not in a do loop");
    }
    f->t->rp -= 3;
}

/* ---- stack words ---- */

PRIM(p_dup) { Cell a = *top(f, 1); PUSH(a); }
PRIM(p_drop) { (void)POP(); }
PRIM(p_swap) { Cell *s = top(f, 2), a = s[0]; s[0] = s[-1]; s[-1] = a; }
PRIM(p_over) { Cell a = top(f, 2)[-1]; PUSH(a); }
PRIM(p_rot) { Cell *s = top(f, 3), a = s[-2]; s[-2] = s[-1]; s[-1] = s[0]; s[0] = a; }
PRIM(p_mrot) { Cell *s = top(f, 3), a = s[0]; s[0] = s[-1]; s[-1] = s[-2]; s[-2] = a; }
PRIM(p_nip) { Cell a = POP(); (void)POP(); PUSH(a); }
PRIM(p_tuck) { Cell b = POP(), a = POP(); PUSH(b); PUSH(a); PUSH(b); }
PRIM(p_2dup) { Cell *s = top(f, 2), a = s[-1], b = s[0]; PUSH(a); PUSH(b); }
PRIM(p_2drop) { (void)POP(); (void)POP(); }
PRIM(p_2swap) {
    Cell d = POP(), c = POP(), b = POP(), a = POP();

    PUSH(c); PUSH(d); PUSH(a); PUSH(b);
}
PRIM(p_2over) { Cell *s = top(f, 4), a = s[-3], b = s[-2]; PUSH(a); PUSH(b); }
PRIM(p_qdup) { Cell a = *top(f, 1); if (a != 0) PUSH(a); }
PRIM(p_pick) {
    Cell n = POP();

    if (n < 0 || n >= f->t->sp - f->t->sfloor) {
        forth_error(f, "pick: out of range");
    }
    PUSH(f->t->ds[f->t->sp - 1 - n]);
}
PRIM(p_depth) { PUSH(f->t->sp - f->t->sfloor); }   /* (above the floor: a message handler's arguments) */
PRIM(p_tor) { rpush(f, POP()); }
PRIM(p_rfrom) { PUSH(rpop(f)); }
PRIM(p_rfetch) {
    if (f->t->rp < 1) {
        forth_error(f, "return stack underflow");
    }
    PUSH(f->t->rs[f->t->rp - 1]);
}
PRIM(p_rdrop) { (void)rpop(f); }

/* ---- arithmetic and logic ---- */

#define BINOP(name, expr) PRIM(name) { Cell b = POP(), a = POP(); PUSH(expr); }
/* (Forth's sums wrap around: done unsigned, which C defines) */
BINOP(p_add, (Cell)((UCell)a + (UCell)b))
BINOP(p_sub, (Cell)((UCell)a - (UCell)b))
BINOP(p_mul, (Cell)((UCell)a * (UCell)b))
BINOP(p_and, a & b)
BINOP(p_or, a | b)
BINOP(p_xor, a ^ b)
/* shifts: by the cell's width or more (a negative count too) every bit goes; arshift fills with the sign
 * (C defines neither: done here) */
static Cell shift_left(Cell a, Cell n) { return (UCell)n >= CELL_BITS ? 0 : (Cell)((UCell)a << n); }
static Cell shift_right(Cell a, Cell n) { return (UCell)n >= CELL_BITS ? 0 : (Cell)((UCell)a >> n); }
static Cell shift_arith(Cell a, Cell n) {
    if ((UCell)n >= CELL_BITS) {
        return a < 0 ? -1 : 0;
    }
    return a < 0 ? ~(~a >> n) : a >> n;   /* (~a is the non-negative one) */
}
BINOP(p_lshift, shift_left(a, b))
BINOP(p_rshift, shift_right(a, b))
BINOP(p_arshift, shift_arith(a, b))
BINOP(p_min, a < b ? a : b)
BINOP(p_max, a > b ? a : b)
BINOP(p_eq, a == b ? -1 : 0)
BINOP(p_ne, a != b ? -1 : 0)
BINOP(p_lt, a < b ? -1 : 0)
BINOP(p_gt, a > b ? -1 : 0)
BINOP(p_le, a <= b ? -1 : 0)
BINOP(p_ge, a >= b ? -1 : 0)
BINOP(p_ult, (UCell)a < (UCell)b ? -1 : 0)
BINOP(p_ugt, (UCell)a > (UCell)b ? -1 : 0)

static Cell divisor(Forth *f, Cell b) {
    if (b == 0) {
        forth_error(f, "division by zero");
    }
    return b;
}
/* floored division, as Forth-2012 recommends: the remainder has the divisor's sign */
static void floored(Forth *f, Cell a, Cell b, Cell *q, Cell *r) {
    divisor(f, b);
    if (b == -1) {   /* (the smallest / -1 overflows in C: it wraps round to itself, as negate does) */
        *q = (Cell)(0 - (UCell)a);
        *r = 0;
        return;
    }
    *q = a / b;
    *r = a % b;
    if (*r != 0 && (*r < 0) != (b < 0)) {
        *q -= 1;
        *r += b;
    }
}
PRIM(p_div) { Cell b = POP(), a = POP(), q, r; floored(f, a, b, &q, &r); PUSH(q); }
PRIM(p_mod) { Cell b = POP(), a = POP(), q, r; floored(f, a, b, &q, &r); PUSH(r); }
PRIM(p_divmod) { Cell b = POP(), a = POP(), q, r; floored(f, a, b, &q, &r); PUSH(r); PUSH(q); }
PRIM(p_muldiv) {   /* a * b / c with a wide intermediate */
    Cell c = POP(), b = POP(), a = POP();
    __int128 p = (__int128)a * b;   /* (twice a cell's bits: the product can't overflow) */

    divisor(f, c);
    PUSH((Cell)(UCell)(unsigned __int128)(p / c));   /* (a quotient too large for a cell wraps) */
}
/* unsigned, and twice a cell wide (a double: its low cell, its high cell over it - Forth's order) */
PRIM(p_udiv) { UCell b = (UCell)divisor(f, POP()), a = (UCell)POP(); PUSH((Cell)(a / b)); }   /* u/ ( u1 u2 -- q ) */
PRIM(p_umod) { UCell b = (UCell)divisor(f, POP()), a = (UCell)POP(); PUSH((Cell)(a % b)); }   /* umod ( u1 u2 -- r ) */
PRIM(p_udivmod) {   /* u/mod ( u1 u2 -- r q ) */
    UCell b = (UCell)divisor(f, POP()), a = (UCell)POP();

    PUSH((Cell)(a % b));
    PUSH((Cell)(a / b));
}
static void push_double(Forth *f, unsigned __int128 d) {
    PUSH((Cell)(UCell)d);
    PUSH((Cell)(UCell)(d >> CELL_BITS));
}
PRIM(p_umstar) {   /* um* ( u1 u2 -- lo hi ) */
    UCell b = (UCell)POP(), a = (UCell)POP();

    push_double(f, (unsigned __int128)a * b);
}
PRIM(p_mstar) {   /* m* ( n1 n2 -- lo hi ) */
    Cell b = POP(), a = POP();

    push_double(f, (unsigned __int128)((__int128)a * b));
}
PRIM(p_umdivmod) {   /* um/mod ( lo hi u -- r q ) a quotient past a cell is an error */
    UCell u = (UCell)divisor(f, POP()), hi = (UCell)POP(), lo = (UCell)POP();
    unsigned __int128 d = (unsigned __int128)hi << CELL_BITS | lo;

    if (hi >= u) {
        forth_error(f, "um/mod: the quotient doesn't fit a cell");
    }
    PUSH((Cell)(UCell)(d % u));
    PUSH((Cell)(UCell)(d / u));
}
PRIM(p_negate) { Cell *s = top(f, 1); *s = (Cell)(0 - (UCell)*s); }
PRIM(p_abs) { Cell *s = top(f, 1); if (*s < 0) *s = (Cell)(0 - (UCell)*s); }
PRIM(p_invert) { Cell *s = top(f, 1); *s = ~*s; }
PRIM(p_1add) { Cell *s = top(f, 1); *s = (Cell)((UCell)*s + 1); }
PRIM(p_1sub) { Cell *s = top(f, 1); *s = (Cell)((UCell)*s - 1); }
PRIM(p_0eq) { Cell *s = top(f, 1); *s = *s == 0 ? -1 : 0; }
PRIM(p_0ne) { Cell *s = top(f, 1); *s = *s != 0 ? -1 : 0; }
PRIM(p_0lt) { Cell *s = top(f, 1); *s = *s < 0 ? -1 : 0; }
PRIM(p_0gt) { Cell *s = top(f, 1); *s = *s > 0 ? -1 : 0; }

/* ---- memory ---- */

/* ---- memory lent to the scripts (forth_guard): checked in a debug build ---- */

void forth_guard(Forth *f, const void *start, size_t n, size_t span, const char *name) {
    int i;

    if (f->nguards == f->guards_cap) {
        int cap = f->guards_cap ? f->guards_cap * 2 : 256;
        Guard *g = realloc(f->guards, (size_t)cap * sizeof(Guard));

        if (g == NULL) {
            return;   /* (unchecked, then) */
        }
        f->guards = g;
        f->guards_cap = cap;
    }
    for (i = f->nguards; i > 0 && f->guards[i - 1].start > (const uint8_t *)start; i--) {
        f->guards[i] = f->guards[i - 1];
    }
    f->guards[i] = (Guard){start, n, span > n ? span : n, name};
    f->nguards++;
}

#ifndef NDEBUG
static void guard_check(Forth *f, Cell a, size_t size) {
    const uint8_t *p = (const uint8_t *)a;
    int lo = 0, hi = f->nguards - 1, at = -1;

    if (f->nguards == 0 || (p >= f->mem && p < f->end)) {   /* (the dictionary's: variables, the most) */
        return;
    }
    while (lo <= hi) {   /* the last guard starting at or before p */
        int mid = (lo + hi) / 2;

        if (f->guards[mid].start <= p) {
            at = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (at >= 0) {
        const Guard *g = &f->guards[at];
        size_t off = (size_t)(p - g->start);

        if (off < g->span && off + size > g->n) {
            forth_error(f, "past the end of %s (its %zu bytes): %zu at byte %zu", g->name, g->n, size, off);
        }
    }
}
#endif

static void *addr_n(Forth *f, Cell a, size_t size);
static void *addr(Forth *f, Cell a) {
    /* (a number where an address belongs - a value fetched again, a field's offset used alone - is
     * an error here, not a crash) */
    if ((UCell)a < LOWEST_ADDRESS || a < 0) {
        forth_error(f, a == 0 ? "null address" : "not an address: %ld (a number where an address belongs?)", (long)a);
    }
    return (void *)a;
}
/* an address the scripts reach size bytes at */
static void *addr_n(Forth *f, Cell a, size_t size) {
    uint8_t *p = addr(f, a);

    if (p >= f->mem && p < f->end && size > (size_t)(f->end - p)) {
        forth_error(f, "past the dictionary's end: %zu bytes with %zu left", size, (size_t)(f->end - p));
    }
#ifndef NDEBUG
    guard_check(f, a, size);
#else
    (void)size;
#endif
    return p;
}
/* a count of bytes from the stack: a negative one is a mistake, not a huge count */
static size_t length_arg(Forth *f, Cell n, const char *word) {
    if (n < 0) {
        forth_error(f, "%s: a negative length (%ld)", word, (long)n);
    }
    return (size_t)n;
}
/* a string from the stack ( addr len ), checked as a fetch of its bytes would be */
static const char *string_arg(Forth *f, Cell a, Cell n, const char *word, size_t *len) {
    *len = length_arg(f, n, word);
    return *len > 0 ? addr_n(f, a, *len) : "";
}
const char *forth_string_arg(Forth *f, Cell a, Cell n, const char *word, size_t *len) {
    return string_arg(f, a, n, word, len);
}
/* (cells anywhere: an address needn't be aligned - a structure's field, a byte table's) */
PRIM(p_fetch) { Cell a = POP(), x; memcpy(&x, addr_n(f, a, sizeof(x)), sizeof(x)); PUSH(x); }
PRIM(p_store) { Cell a = POP(), x = POP(); memcpy(addr_n(f, a, sizeof(x)), &x, sizeof(x)); }
PRIM(p_pstore) {
    Cell a = POP(), x = POP(), y;
    void *p = addr_n(f, a, sizeof(y));

    memcpy(&y, p, sizeof(y));
    y = (Cell)((UCell)y + (UCell)x);
    memcpy(p, &y, sizeof(y));
}
PRIM(p_cfetch) { Cell a = POP(); PUSH(*(uint8_t *)addr_n(f, a, 1)); }
PRIM(p_cstore) { Cell a = POP(), x = POP(); *(uint8_t *)addr_n(f, a, 1) = (uint8_t)x; }
PRIM(p_wfetch) { Cell a = POP(); uint16_t v; memcpy(&v, addr_n(f, a, 2), 2); PUSH(v); }
PRIM(p_swfetch) { Cell a = POP(); int16_t v; memcpy(&v, addr_n(f, a, 2), 2); PUSH(v); }
PRIM(p_wstore) { Cell a = POP(), x = POP(); uint16_t v = (uint16_t)x; memcpy(addr_n(f, a, 2), &v, 2); }
PRIM(p_lfetch) { Cell a = POP(); uint32_t v; memcpy(&v, addr_n(f, a, 4), 4); PUSH(v); }
PRIM(p_slfetch) { Cell a = POP(); int32_t v; memcpy(&v, addr_n(f, a, 4), 4); PUSH(v); }
PRIM(p_lstore) { Cell a = POP(), x = POP(); uint32_t v = (uint32_t)x; memcpy(addr_n(f, a, 4), &v, 4); }
PRIM(p_move) {   /* ( src dst n -- ) */
    Cell n = POP(), d = POP(), s = POP();

    if (n > 0) {
        memmove(addr_n(f, d, (size_t)n), addr_n(f, s, (size_t)n), (size_t)n);
    }
}
PRIM(p_fill) {   /* ( addr n byte -- ) */
    Cell c = POP(), n = POP(), a = POP();

    if (n > 0) {
        memset(addr_n(f, a, (size_t)n), (int)c, (size_t)n);
    }
}
PRIM(p_cells) { Cell *s = top(f, 1); *s = (Cell)((UCell)*s * sizeof(Cell)); }   /* (wrapping, as `*` does) */
PRIM(p_floats) { Cell *s = top(f, 1); *s = (Cell)((UCell)*s * sizeof(Float)); }
PRIM(p_here) { align_here(f); PUSH(f->here); }
PRIM(p_allot) {   /* ( n -- ) a negative n gives space back, down to the dictionary's start */
    Cell n = POP();

    if (n >= 0) {
        allot(f, (size_t)n);
    } else if ((UCell)0 - (UCell)n > (size_t)(f->here - (f->fence != NULL ? f->fence : f->mem))) {
        forth_error(f, "allot: %ld is past the system's own words (or the dictionary's start)", (long)n);
    } else {
        here_back(f, f->here - (size_t)((UCell)0 - (UCell)n));
    }
}
PRIM(p_unused) { PUSH(f->end - f->here); }   /* ( -- n ) the dictionary's bytes still free */
PRIM(p_comma) { comma(f, POP()); }
PRIM(p_ccomma) { *(uint8_t *)allot(f, 1) = (uint8_t)POP(); }
PRIM(p_fcomma) { comma_float(f, FPOP()); }
PRIM(p_align) { align_here(f); }
PRIM(p_compare) {   /* ( a1 n1 a2 n2 -- -1|0|1 ) */
    Cell n2 = POP(), a2 = POP(), n1 = POP(), a1 = POP();
    size_t len1, len2;
    const char *s1 = string_arg(f, a1, n1, "compare", &len1), *s2 = string_arg(f, a2, n2, "compare", &len2);
    size_t n = len1 < len2 ? len1 : len2;
    int c = n > 0 ? memcmp(s1, s2, n) : 0;

    PUSH(c != 0 ? (c < 0 ? -1 : 1) : (len1 < len2 ? -1 : len1 > len2 ? 1 : 0));
}

/* ---- floats ---- */

#define FBINOP(name, expr) PRIM(name) { Float b = FPOP(), a = FPOP(); FPUSH(expr); }
#define FUNOP(name, expr) PRIM(name) { Float a = FPOP(); FPUSH(expr); }
#define FCMP(name, expr) PRIM(name) { Float b = FPOP(), a = FPOP(); PUSH((expr) ? -1 : 0); }
FBINOP(p_fadd, a + b)
FBINOP(p_fsub, a - b)
FBINOP(p_fmul, a * b)
FBINOP(p_fdiv, a / b)
FBINOP(p_fmin, a < b ? a : b)
FBINOP(p_fmax, a > b ? a : b)
FBINOP(p_fatan2, atan2(a, b))
FBINOP(p_fpow, pow(a, b))
FUNOP(p_fnegate, -a)
FUNOP(p_fabs, fabs(a))
FUNOP(p_fsqrt, sqrt(a))
FUNOP(p_fsin, sin(a))
FUNOP(p_fcos, cos(a))
FUNOP(p_ftan, tan(a))
FUNOP(p_fasin, asin(a))
FUNOP(p_facos, acos(a))
FUNOP(p_fexp, exp(a))
FUNOP(p_fln, log(a))
FUNOP(p_ffloor, floor(a))
FUNOP(p_fround, round(a))
FCMP(p_flt, a < b)
FCMP(p_fgt, a > b)
FCMP(p_fle, a <= b)
FCMP(p_fge, a >= b)
FCMP(p_feq, a == b)
PRIM(p_f0eq) { PUSH(FPOP() == 0.0 ? -1 : 0); }
PRIM(p_f0lt) { PUSH(FPOP() < 0.0 ? -1 : 0); }
PRIM(p_stof) { FPUSH((Float)POP()); }
PRIM(p_3stof) {   /* ( x y z -- ) ( F: -- x y z ) a point to the float stack */
    Cell z = POP(), y = POP(), x = POP();
    forth_fpush(f, (Float)x);
    forth_fpush(f, (Float)y);
    forth_fpush(f, (Float)z);
}
/* a float as a cell: truncated toward zero; one too large for a cell is the largest (the smallest), not-a-number 0
 * (C defines none of these: done here) */
#define CELL_MAX INTPTR_MAX
#define CELL_MIN INTPTR_MIN
#define CELL_RANGE 0x1p63   /* the first float past the largest cell: -CELL_RANGE is the smallest cell itself */
_Static_assert(sizeof(Cell) * CHAR_BIT == 64, "CELL_RANGE is a 64-bit cell's");
static Cell float_to_cell(Float x) {
    if (isnan(x)) {
        return 0;
    }
    return x >= CELL_RANGE ? CELL_MAX : x <= -CELL_RANGE ? CELL_MIN : (Cell)x;
}
PRIM(p_3ftos) {   /* ( F: x y z -- ) ( -- x y z ) and back, as whole numbers */
    Float z = forth_fpop(f), y = forth_fpop(f), x = forth_fpop(f);
    PUSH(float_to_cell(x));
    PUSH(float_to_cell(y));
    PUSH(float_to_cell(z));
}
PRIM(p_ftos) { PUSH(float_to_cell(FPOP())); }   /* truncates toward zero */
PRIM(p_fdup) { Float a = FPOP(); FPUSH(a); FPUSH(a); }
PRIM(p_fdrop) { (void)FPOP(); }
PRIM(p_fswap) { Float b = FPOP(), a = FPOP(); FPUSH(b); FPUSH(a); }
PRIM(p_fover) { Float b = FPOP(), a = FPOP(); FPUSH(a); FPUSH(b); FPUSH(a); }
PRIM(p_frot) { Float c = FPOP(), b = FPOP(), a = FPOP(); FPUSH(b); FPUSH(c); FPUSH(a); }
PRIM(p_fdepth) { PUSH(f->t->fp - f->t->ffloor); }   /* (above the floor, as `depth`) */
PRIM(p_ffetch) { Float x; memcpy(&x, addr_n(f, POP(), sizeof(x)), sizeof(x)); FPUSH(x); }
PRIM(p_fstore) { Float x = FPOP(); memcpy(addr_n(f, POP(), sizeof(x)), &x, sizeof(x)); }
PRIM(p_sffetch) { float x; memcpy(&x, addr_n(f, POP(), 4), 4); FPUSH(x); }
PRIM(p_sfstore) { float x = (float)FPOP(); memcpy(addr_n(f, POP(), 4), &x, 4); }

/* ---- output ---- */

/* the number base: `base` may hold anything; one that isn't a base counts as ten (so `10 base !` puts it right) */
static Cell number_base(const Forth *f) {
    Cell b = f->t != NULL && f->t->sys != NULL ? f->t->sys->base : BASE_DEFAULT;

    return b >= BASE_MIN && b <= BASE_MAX ? b : BASE_DEFAULT;
}

/* x in the current base */
static const char *format_number(Forth *f, Cell x, int is_unsigned) {
    char *p = f->numbuf + sizeof(f->numbuf) - 1;
    UCell u = is_unsigned || x >= 0 ? (UCell)x : (UCell)0 - (UCell)x;
    UCell base = (UCell)number_base(f);

    *p = 0;
    do {
        UCell d = u % base;

        *--p = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        u /= base;
    } while (u != 0);
    if (!is_unsigned && x < 0) {
        *--p = '-';
    }
    return p;
}
PRIM(p_dot) { forth_printf(f, "%s ", format_number(f, POP(), 0)); }
PRIM(p_udot) { forth_printf(f, "%s ", format_number(f, POP(), 1)); }
PRIM(p_fdot) { forth_printf(f, "%g ", FPOP()); }
PRIM(p_dots) {
    int i;

    forth_printf(f, "<%d> ", f->t->sp - f->t->sfloor);
    for (i = f->t->sfloor; i < f->t->sp; i++) {
        forth_printf(f, "%s ", format_number(f, f->t->ds[i], 0));
    }
    if (f->t->fp > f->t->ffloor) {
        forth_printf(f, " F: ");
        for (i = f->t->ffloor; i < f->t->fp; i++) {
            forth_printf(f, "%g ", f->t->fs[i]);
        }
    }
}
PRIM(p_emit) { char c = (char)POP(); forth_type(f, &c, 1); }
PRIM(p_type) {
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = string_arg(f, a, n, "type", &len);

    if (len > 0) {
        forth_type(f, s, len);
    }
}
PRIM(p_cr) { forth_type(f, "\n", 1); }
PRIM(p_space) { forth_type(f, " ", 1); }
PRIM(p_spaces) { Cell n = POP(); while (n-- > 0) forth_type(f, " ", 1); }
PRIM(p_base) { PUSH(&task_sys(f, f->t)->base); }   /* (each task's own: another's `hex` changes none of its numbers) */
PRIM(p_state) { PUSH(&f->compiling); }

/* ---- the outer interpreter: parsing ---- */

static Source *source(Forth *f) {
    if (f->nsrc == 0) {
        forth_error(f, "no input");
    }
    return &f->src[f->nsrc - 1];
}

/* the next space-delimited word of the input (length 0 at the end) */
static const char *parse_word(Forth *f, size_t *len) {
    Source *s = source(f);
    size_t start;

    while (s->pos < s->len && isspace((unsigned char)s->text[s->pos])) {
        if (s->text[s->pos] == '\n') {
            s->line++;
        }
        s->pos++;
    }
    start = s->pos;
    while (s->pos < s->len && !isspace((unsigned char)s->text[s->pos])) {
        s->pos++;
    }
    *len = s->pos - start;
    return s->text + start;
}

/* the input up to (not including) the delimiter, which is skipped; one leading space skipped */
static const char *parse_until(Forth *f, char delim, size_t *len) {
    Source *s = source(f);
    size_t start;

    if (s->pos < s->len && s->text[s->pos] == ' ') {
        s->pos++;
    }
    start = s->pos;
    while (s->pos < s->len && s->text[s->pos] != delim) {
        if (s->text[s->pos] == '\n') {
            s->line++;
        }
        s->pos++;
    }
    *len = s->pos - start;
    if (s->pos < s->len) {
        s->pos++;
    }
    return s->text + start;
}

/* a name for a definition: it must be there */
static const char *parse_name(Forth *f, size_t *len, const char *who) {
    const char *name = parse_word(f, len);

    if (*len == 0) {
        forth_error(f, "%s: name expected", who);
    }
    return name;
}

static const char *where_else(Forth *f, const char *name, size_t len, char *buf, size_t size);
static Word *parse_find(Forth *f, const char *who) {
    size_t len;
    const char *name = parse_name(f, &len, who);
    Word *w = forth_find(f, name, len);

    if (w == NULL) {
        char hint[160];

        forth_error(f, "%s: unknown word %.*s%s", who, (int)len, name, where_else(f, name, len, hint, sizeof(hint)));
    }
    return w;
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;
}

/* an integer: optional '-', a base prefix ($ hex, # decimal, % binary), or 'c' for a character */
static int parse_int(Forth *f, const char *s, size_t n, Cell *out) {
    Cell base = number_base(f);
    UCell v = 0;
    int neg = 0, overflow = 0;
    size_t i = 0;

    if (n == 3 && s[0] == '\'' && s[2] == '\'') {
        *out = (unsigned char)s[1];
        return 1;
    }
    if (i < n && (s[i] == '$' || s[i] == '#' || s[i] == '%')) {
        base = s[i] == '$' ? 16 : s[i] == '#' ? 10 : 2;
        i++;
    }
    if (i < n && s[i] == '-') {
        neg = 1;
        i++;
    }
    if (i == n) {
        return 0;
    }
    for (; i < n; i++) {
        int d = digit_value(s[i]);

        if (d >= base) {
            return 0;
        }
        if (v > (UINTPTR_MAX - (UCell)d) / (UCell)base) {
            overflow = 1;
        }
        v = v * (UCell)base + (UCell)d;
    }
    if (overflow) {   /* (any cell's bits may be written: $ffffffffffffffff is -1) */
        forth_error(f, "%.*s is too large for a cell", (int)n, s);
    }
    *out = (Cell)(neg ? 0 - v : v);
    return 1;
}

/* a float: digits with a '.' or an exponent (1.5 -2. 3e2 1.5e-3) */
static int parse_float(const char *s, size_t n, Float *out) {
    char buf[64], *end;
    size_t i;
    int digit = 0, mark = 0;

    if (n == 0 || n >= sizeof(buf)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (isdigit((unsigned char)s[i])) {
            digit = 1;
        } else if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') {
            mark = 1;
        } else if (s[i] != '-' && s[i] != '+') {
            return 0;
        }
    }
    if (!digit || !mark) {
        return 0;
    }
    memcpy(buf, s, n);
    buf[n] = 0;
    if (buf[n - 1] == 'e' || buf[n - 1] == 'E') {   /* "2e" is 2.0 (the classic Forth float syntax) */
        buf[--n] = 0;
    }
    *out = strtod(buf, &end);
    return n > 0 && end == buf + n;
}

const char *forth_parse_name(Forth *f, size_t *len) {
    return parse_word(f, len);
}

void forth_compile_word(Forth *f, Word *w) {
    comma(f, (Cell)w);
}

void forth_compile_literal(Forth *f, Cell x) {
    comma(f, (Cell)f->w_lit);
    comma(f, x);
}

/* ---- the outer interpreter ---- */

/* a local's fetch (or store) laid down: one of this definition's, or - in a quotation - of the word (or quotation)
 * around it, k out, which makes the quotation (and those between) closures. 0 if the name isn't a local's. */
static int compile_local(Forth *f, const char *s, size_t n, int store) {
    struct ForthLocals *l = &f->locals;
    int k = local_index(f, s, n), level, j;

    if (k >= 0) {
        comma(f, (Cell)(l->is_float[k] ? (store ? f->w_flocal_store : f->w_flocal_fetch)
                                       : (store ? f->w_local_store : f->w_local_fetch)));
        comma(f, l->at[k]);
        return 1;
    }
    for (level = f->nlocals_saved - 1; level >= 0; level--) {
        struct ForthLocals *outer = &f->locals_saved[level];

        if ((k = local_in(outer, s, n)) >= 0) {
            l->captures = 1;   /* (this quotation, and each between it and that level, reach out) */
            for (j = level + 1; j < f->nlocals_saved; j++) {
                f->locals_saved[j].captures = 1;
            }
            comma(f, (Cell)(outer->is_float[k] ? (store ? f->w_fouter_store : f->w_fouter_fetch)
                                               : (store ? f->w_outer_store : f->w_outer_fetch)));
            comma(f, f->nlocals_saved - level);
            comma(f, outer->at[k]);
            return 1;
        }
    }
    return 0;
}

/* the code leaving a frame, before an exit: a quotation's (unqframe) - made an exit at `;]` if it has no frame - or
 * a word's (unlocals) once its frame is made */
static void leave_frame(Forth *f) {
    struct ForthLocals *l = &f->locals;

    if (l->quotation) {
        if (l->nexits == (int)(sizeof(l->exits) / sizeof(l->exits[0]))) {
            forth_error(f, "[: too many exits in one quotation");
        }
        align_here(f);
        l->exits[l->nexits++] = (Cell *)f->here;
        comma(f, (Cell)f->w_unqframe);
    } else if (l->frame_size != NULL) {
        comma(f, (Cell)f->w_unlocals);
    }
}

static int compile_inline(Forth *f, Word *w);

static void interpret(Forth *f) {
    for (;;) {
        size_t n;
        const char *s = parse_word(f, &n);
        Word *w;
        Cell x;
        Float fx;

        if (n == 0) {
            return;
        }
        if (f->compiling && compile_local(f, s, n, 0)) {   /* a local (its own, or its word's in a quotation) */
            continue;
        }
        if (f->compiling && n == 2 && strncasecmp(s, "to", 2) == 0) {   /* or `to` one */
            Source *src = source(f);
            size_t pos = src->pos, m;
            const char *t = parse_word(f, &m);

            if (compile_local(f, t, m, 1)) {
                continue;
            }
            src->pos = pos;   /* (a value's `to`) */
        }
        w = forth_find(f, s, n);
        if (w != NULL) {
            if (w == f->w_exit && f->compiling) {   /* (leaving: the frame first) */
                leave_frame(f);
            }
            if (w == f->w_jump && f->compiling) {   /* (it would leave for a local) */
                if (f->locals.frame_size != NULL && !f->locals.quotation) {
                    forth_error(f, "jump: not from a word with locals");
                }
                f->locals.jumps = 1;
            }
            if (f->compiling && !(w->flags & WORD_IMMEDIATE)) {
                Word *other = ambiguous(f, w, s, n);

                if (other != NULL && other->vocab != f->core) {
                    forth_warn(f, "%.*s is ambiguous here: %s:%.*s and %s:%.*s are both in sight (%s:%.*s taken, the "
                               "later in USING:) - write vocabulary:word for the one meant", (int)n, s, w->vocab->name,
                               (int)n, s, other->vocab->name, (int)n, s, w->vocab->name, (int)n, s);
                } else if (other != NULL) {
                    forth_warn(f, "%.*s here is %s:%.*s (%d in, %d out%s), not %s:%.*s (%d in, %d out%s) - two words of "
                               "that name in sight: write vocabulary:word for the one meant (or rename one)", (int)n, s,
                               w->vocab->name, (int)n, s, w->effect.in, w->effect.out,
                               w->effect.kind == EFFECT_HIGHER ? ", running a token" : "", other->vocab->name, (int)n, s,
                               other->effect.in, other->effect.out,
                               other->effect.kind == EFFECT_HIGHER ? ", running a token" : "");
                }
                if ((w->flags & WORD_COMPILED_ONLY) && w->operands > 0) {   /* (named, nothing lays its operands) */
                    forth_error(f, "%.*s: the compiler's word, laid down with its operands by the words that use it "
                                   "(`postpone` it, then `,` them)", (int)n, s);
                }
                if (!compile_inline(f, w)) {
                    comma(f, (Cell)w);
                }
            } else {
                if (w->flags & WORD_COMPILED_ONLY) {   /* (it would read the cells after it: there are none) */
                    forth_error(f, "%.*s: the compiler's word, only in compiled code", (int)n, s);
                }
                forth_execute(f, w);
            }
        } else if (parse_int(f, s, n, &x)) {
            if (f->compiling) {
                comma(f, (Cell)f->w_lit);
                comma(f, x);
            } else {
                PUSH(x);
            }
        } else if (parse_float(s, n, &fx)) {
            if (f->compiling) {
                comma(f, (Cell)f->w_flit);
                comma_float(f, fx);
            } else {
                FPUSH(fx);
            }
        } else {
            char hint[160];

            forth_error(f, "unknown word %.*s%s", (int)n, s, where_else(f, s, n, hint, sizeof(hint)));
        }
    }
}

static void push_source(Forth *f, const char *text, size_t len, const char *name, char *owned) {
    Source *s;

    if (f->nsrc >= SOURCE_DEPTH) {
        free(owned);
        forth_error(f, "includes nested too deep");
    }
    s = &f->src[f->nsrc++];
    s->text = text;
    s->len = len;
    s->pos = 0;
    s->name = name;
    s->line = 1;
    s->owned = owned;
}

static void pop_source(Forth *f) {
    Source *s = &f->src[--f->nsrc];

    free(s->owned);
    s->owned = NULL;
}

/* interpret text inside the current catch point (errors propagate) */
static void evaluate(Forth *f, const char *text, size_t len, const char *name, char *owned) {
    int level = f->nsrc;

    push_source(f, text, len, name, owned);
    interpret(f);
    forth_flush_warning(f);
    while (f->nsrc > level) {
        pop_source(f);
    }
}

/* ---- the library's own Forth: core/'s and basis/'s files built in (the build's forth_sources.c), found after the roots' files ---- */
typedef struct ForthSource {
    const char *name;   /* the vocabulary's ("prelude", "tester") */
    const char *text;
} ForthSource;
extern const ForthSource forth_builtin_sources[];   /* (ended by a NULL name) */
#define BUILTIN_DIR "(built in)"

static const ForthSource *builtin_source(const char *name, size_t len) {
    const ForthSource *s;

    for (s = forth_builtin_sources; s->name != NULL; s++) {
        if (strlen(s->name) == len && strncmp(s->name, name, len) == 0) {
            return s;
        }
    }
    return NULL;
}

/* a built-in source defines the name, outside <PRIVATE ... PRIVATE> (`: name`, `constant name` ...): its text read,
 * not loaded - so an unknown word can say which vocabulary to name in USING: */
static int defines_public(const char *text, const char *name, size_t len) {
    static const char *const before[] = {"\n: ", " constant ", " value ", " variable ", "\nmessage ", " defer "};
    size_t k;

    for (k = 0; k < sizeof(before) / sizeof(before[0]); k++) {
        size_t bn = strlen(before[k]);
        const char *at = text;

        while ((at = strstr(at, before[k])) != NULL) {
            const char *nm = at + bn, *open, *close, *p;
            char after = nm[len];

            at = nm;
            if (strncmp(nm, name, len) != 0 || !(after == ' ' || after == '\n' || after == '\t' || after == 0)) {
                continue;
            }
            open = NULL;   /* (the last <PRIVATE before it, and whether a PRIVATE> closes it before it) */
            for (p = text; (p = strstr(p, "<PRIVATE")) != NULL && p < nm; p += 8) {
                open = p;
            }
            close = open != NULL ? strstr(open, "PRIVATE>") : NULL;
            if (open == NULL || (close != NULL && close < nm)) {
                return 1;
            }
        }
    }
    return 0;
}
#define HINT_VOCABS_MAX 3
/* where else a name is a word: " (in vocabulary x, y: add it to USING:)" into buf, or "" - the vocabularies loaded,
 * and the library's not yet loaded (their text) */
static const char *where_else(Forth *f, const char *name, size_t len, char *buf, size_t size) {
    const ForthSource *src;
    Vocab *v;
    int found = 0;
    size_t used = 0;

    buf[0] = 0;
    if (len == 0 || len > WORD_NAME_MAX) {
        return buf;
    }
    for (v = f->vocabs; v != NULL && found < HINT_VOCABS_MAX; v = v->next) {
        if (v != f->core && v->parent == NULL && strcmp(v->name, "messages") != 0 && in_vocab(v, name, len) != NULL) {
            used += (size_t)snprintf(buf + used, size - used, "%s%s", found ? ", " : " (in vocabulary ", v->name);
            found++;
        }
    }
    for (src = forth_builtin_sources; src->name != NULL && found < HINT_VOCABS_MAX && used < size; src++) {
        if (vocab_find(f, src->name, strlen(src->name)) == NULL && defines_public(src->text, name, len)) {
            used += (size_t)snprintf(buf + used, size - used, "%s%s", found ? ", " : " (in vocabulary ", src->name);
            found++;
        }
    }
    if (found && used < size) {
        snprintf(buf + used, size - used, ": add it to USING:)");
    }
    return buf;
}

/* "(built in)/tester.fs": that source's text */
static char *builtin_text(const char *path, size_t *len) {
    const size_t prefix = strlen(BUILTIN_DIR "/");
    const char *dot = strrchr(path, '.');
    const ForthSource *s;
    char *copy;

    if (strncmp(path, BUILTIN_DIR "/", prefix) != 0 || dot == NULL ||
        (s = builtin_source(path + prefix, (size_t)(dot - path) - prefix)) == NULL) {
        return NULL;
    }
    *len = strlen(s->text);
    copy = malloc(*len + 1);
    if (copy != NULL) {
        memcpy(copy, s->text, *len + 1);
    }
    return copy;
}

int forth_prelude(Forth *f) {
    int r = forth_include(f, BUILTIN_DIR "/prelude.fs");

    /* (the combinators compiled inline after literal quotations: the prelude's, found by these names) */
    f->w_dip = in_vocab(f->core, "dip", 3);
    f->w_2dip = in_vocab(f->core, "2dip", 4);
    f->w_keep = in_vocab(f->core, "keep", 4);
    f->w_2keep = in_vocab(f->core, "2keep", 5);
    f->w_bi = in_vocab(f->core, "bi", 2);
    f->w_tri = in_vocab(f->core, "tri", 3);
    f->w_bi_at = in_vocab(f->core, "bi@", 3);
    f->w_when = in_vocab(f->core, "when", 4);
    f->w_unless = in_vocab(f->core, "unless", 6);
    f->w_each = in_vocab(f->core, "each", 4);
    f->w_map = in_vocab(f->core, "map", 3);
    f->w_filter = in_vocab(f->core, "filter", 6);
    f->w_reduce = in_vocab(f->core, "reduce", 6);
    f->w_find = in_vocab(f->core, "find", 4);
    f->w_with_scratch = in_vocab(f->core, "with-scratch", 12);
    f->w_atomically = in_vocab(f->core, "atomically", 10);
    f->w_to_store = in_vocab(f->core, "(to)", 4);
    {   /* (a value's code after does>: every value's - the checker knows a value's word by it) */
        Word *v = in_vocab(f->core, "(a-value)", 9);

        f->value_does = v != NULL ? v->does : NULL;
    }
    forth_native_bind(f);   /* (the prelude's words the native backend writes in line: within, true, 0>= ...) */
    if (f->w_with_scratch != NULL) {
        f->w_with_scratch->effect.kind = EFFECT_HIGHER;
        f->w_with_scratch->effect_tried = 1;
    }
    f->fence = f->here;   /* (the words the C keeps: the primitives' and these) */
    {   /* (CLANKFORTH_STRICT set, not 0: a program's warnings are errors - from here, the system's own words loaded) */
        const char *strict = getenv("CLANKFORTH_STRICT");

        f->strict = strict != NULL && strict[0] != 0 && strcmp(strict, "0") != 0;
    }
    return r;
}

static char *read_file(Forth *f, const char *path, size_t *len) {
    FILE *fp;
    char *buf;
    long n;

    if ((buf = builtin_text(path, len)) != NULL) {
        return buf;
    }
    if (f->reader != NULL && (buf = f->reader(f->reader_ctx, path, len)) != NULL) {
        return buf;
    }
    if ((fp = fopen(path, "rb")) == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = n >= 0 && n < LONG_MAX ? malloc((size_t)n + 1) : NULL;   /* (a folder's "length" may be neither) */
    /* (... or 0 - XFS's small folders: but reading one fails, so a byte more is tried for that error) */
    if (buf == NULL || fread(buf, 1, (size_t)n, fp) != (size_t)n || (getc(fp) == EOF && ferror(fp))) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/* ---- views: a source's manifest in force, the one it hid set aside until it ends (or a catch point unwinds to
 * where it was) - so a catch point keeps a depth, not a copy of the view ---- */

static void enter_view(Forth *f, const Manifest *m) {
    if (f->nviews == f->views_cap) {
        int cap = f->views_cap ? f->views_cap * 2 : SOURCE_DEPTH;
        Manifest *views = realloc(f->views, (size_t)cap * sizeof(Manifest));

        if (views == NULL) {
            forth_error(f, "include: out of memory");
        }
        f->views = views;
        f->views_cap = cap;
    }
    f->views[f->nviews++] = f->m;
    f->m = *m;
}

static void leave_view(Forth *f) { f->m = f->views[--f->nviews]; }

/* a catch point's view back: the one in force when it was made */
static void back_to_view(Forth *f, int depth) {
    if (f->nviews > depth) {
        f->m = f->views[depth];
        f->nviews = depth;
    }
}

/* include a file: names are kept for error messages for as long as the system lives */
struct FileName {
    struct FileName *next;
    char name[];
};

static void include_file(Forth *f, const char *path, size_t plen) {
    struct FileName *kept = malloc(sizeof(struct FileName) + plen + 1);
    char *name, *text;
    size_t len;
    Manifest fresh;

    if (kept == NULL) {
        forth_error(f, "include: out of memory");
    }
    kept->next = f->file_names;
    f->file_names = kept;
    name = kept->name;
    memcpy(name, path, plen);
    name[plen] = 0;
    text = read_file(f, name, &len);
    if (text == NULL) {
        forth_error(f, "include: can't read %s", name);
    }
    fresh = fresh_manifest(f, 0);
    enter_view(f, &fresh);
    evaluate(f, text, len, name, text);
    leave_view(f);   /* (on an error the catch point puts its own view back) */
}

/* a definition an error left unfinished: dropped, its space given back; back to interpreting */
static void drop_definition(Forth *f) {
    Word *d = f->defining;

    f->defining = NULL;
    f->compiling = 0;
    memset(&f->locals, 0, sizeof(f->locals));
    f->nlocals_saved = 0;
    if (d != NULL && f->latest == d && d->vocab->latest == d) {
        table_remove(d->vocab, d);
        d->vocab->latest = d->link;
        f->latest = f->latest_before;
        here_back(f, (uint8_t *)d);
    }
}

void forth_drop_definition(Forth *f) { drop_definition(f); }

/* a task's stacks emptied, as when it starts over: its locals' frame, the lists being gathered and
 * the floors too */
static void task_reset(Task *t) {
    if (t->sys != NULL) {
        t->sys->ncatch = 0;   /* (the catches it was inside: gone) */
    }
    t->sp = t->rp = t->fp = 0;
    t->sfloor = t->ffloor = 0;
    t->lp = 0;
    t->closure_lp = 0;
    t->nmarks = 0;
}

/* text interpreted from C runs at most the host's steps (forth_set_eval_budget), unless a limit is counting already
 * (a task's turn, an eval within an eval) */
static void limit_eval(Forth *f) {
    if (f->budget < 0) {
        f->budget = f->eval_budget;
    }
}

void forth_set_eval_budget(Forth *f, long steps) { f->eval_budget = steps; }

/* run text with a catch point; on error: back to interpreting, stacks emptied (ABORT) */
int forth_eval(Forth *f, const char *text, size_t len, const char *name) {
    jmp_buf jb, *prev = f->catch;
    int level = f->nsrc, depth = f->depth, views = f->nviews;
    Cell budget = f->budget;
    Cell *ip = IP;
    Task *t = f->t;
    int region = t->region;

    int self;

    if (!f->compiling) {   /* (a definition begun on an earlier line: what came since it began) */
        f->prompt_mark = f->here;
    }
    enter_view(f, &f->listener);   /* text from the prompt sees every loaded vocabulary */
    self = forth_actors_prompt_enter(f);   /* (and runs as the actor `attach`ed, if there is one) */
    if (setjmp(jb) != 0) {
        forth_actors_prompt_leave(f, self);
        forth_regions_unwind(f, t, region);
        /* (the prompt's view as its text left it: set aside by a file it included, else the one in force) */
        f->listener = f->nviews > views + 1 ? f->views[views + 1] : f->m;
        back_to_view(f, views);
        f->catch = prev;
        while (f->nsrc > level) {
            pop_source(f);
        }
        f->depth = depth;
        f->t = t;
        IP = ip;
        f->yielded = 0;
        f->budget = budget;
        drop_definition(f);
        task_reset(t);
        return -1;
    }
    f->catch = &jb;
    limit_eval(f);
    evaluate(f, text, len, name, NULL);
    if (!f->compiling) {   /* (a word defined again at the prompt: its callers get it - none half done) */
        forward_all_hidden(f, f->prompt_mark);
    }
    forth_actors_prompt_leave(f, self);
    f->budget = budget;
    f->catch = prev;
    f->listener = f->m;   /* (USING: and IN: at the prompt last) */
    leave_view(f);
    return 0;
}

int forth_include(Forth *f, const char *path) {
    jmp_buf jb, *prev = f->catch;
    int level = f->nsrc, depth = f->depth, views = f->nviews;
    Cell budget = f->budget;
    Task *t = f->t;
    int region = t->region;

    if (setjmp(jb) != 0) {
        forth_regions_unwind(f, t, region);
        back_to_view(f, views);
        f->catch = prev;
        while (f->nsrc > level) {
            pop_source(f);
        }
        f->depth = depth;
        f->budget = budget;
        drop_definition(f);
        task_reset(f->t);
        return -1;
    }
    f->catch = &jb;
    limit_eval(f);
    include_file(f, path, strlen(path));
    f->budget = budget;
    f->catch = prev;
    return 0;
}

int forth_call(Forth *f, Word *w) {
    jmp_buf jb, *prev = f->catch;
    int depth = f->depth, views = f->nviews, nsrc = f->nsrc;
    Cell *ip = IP;
    Task *t = f->t;
    int rp = t->rp, fp = t->fp, lp = t->lp, closure_lp = t->closure_lp;   /* (a primitive's caller's, as it was) */
    int nmarks = t->nmarks, region = t->region;
    Word *defining = f->defining;

    if (setjmp(jb) != 0) {
        forth_regions_unwind(f, t, region);
        back_to_view(f, views);
        f->catch = prev;
        f->depth = depth;
        f->t = t;
        t->rp = rp;   /* (the word that ran the primitive that called this goes on: its frame and locals as they were) */
        t->fp = fp;
        t->lp = lp;
        t->closure_lp = closure_lp;
        t->nmarks = nmarks;
        IP = ip;
        f->yielded = 0;
        while (f->nsrc > nsrc) {   /* (text it was evaluating: its own, gone with it) */
            pop_source(f);
        }
        if (f->defining != defining) {   /* (a definition begun inside it, unfinished) */
            drop_definition(f);
        }
        return -1;
    }
    f->catch = &jb;
    forth_execute(f, w);
    f->catch = prev;
    return 0;
}

/* ---- the compiler ---- */

PRIM(p_colon) {
    size_t len;
    const char *name = parse_name(f, &len, ":");
    Word *before = f->latest, *def = new_word(f, name, len, docol);

    def->flags = WORD_HIDDEN;   /* found again once complete */
    f->defining = def;
    f->latest_before = before;
    f->compiling = -1;
    f->declared.kind = EFFECT_UNKNOWN;
    f->declared_text[0] = 0;
    f->declared_given = 0;
    memset(&f->locals, 0, sizeof(f->locals));
    f->nlocals_saved = 0;
    f->nquoted = 0;
    f->top_sp = f->t->sp;
}
/* {: a b F: x | c F: y -- comment :} the definition's locals: a and b taken from the data stack
 * (b from the top), x from the float stack, c and y starting at 0; F: makes the names after it
 * floats (until |), what follows -- is a comment. A definition may have several: each adds to
 * the same frame (made at the first, which must be outside an if or a loop), and takes its values
 * where it stands - a later one inside a loop each time round. */
PRIM(p_locals_begin) {
    struct ForthLocals *l = &f->locals;
    int first = l->n, taken = -1, comment = 0, is_float = 0, i;

    if (!f->compiling) {
        forth_error(f, "{: only inside a definition");
    }
    if (l->frame_size == NULL && f->t->sp != f->top_sp) {   /* (the first makes the frame: it must be made) */
        forth_error(f, "{: the first in a definition inside a do loop or an if (it makes the frame): name them at the start - {: | name :} - and `to name` here");
    }
    for (;;) {
        size_t n;
        const char *s = parse_word(f, &n);

        if (n == 0) {
            forth_error(f, "{: without :}");
        }
        if (n == 2 && strncmp(s, ":}", 2) == 0) {
            break;
        }
        if (comment) {
            continue;
        }
        if (n == 2 && strncmp(s, "--", 2) == 0) {
            comment = 1;
            continue;
        }
        if (n == 1 && s[0] == '|') {
            taken = f->locals.n;
            is_float = 0;
            continue;
        }
        if (n == 2 && strncasecmp(s, "F:", 2) == 0) {
            is_float = 1;
            continue;
        }
        if (f->locals.n == FORTH_LOCALS || n > WORD_NAME_MAX) {
            forth_error(f, "{: too many locals, or a name too long");
        }
        if (n == 4 && strncasecmp(s, "exit", 4) == 0) {   /* (it would hide EXIT: `exit` would push it) */
            forth_error(f, "{: a local can't be called exit");
        }
        if (local_index(f, s, n) >= 0) {
            forth_error(f, "{: a local named twice");
        }
        {   /* (`to`, `if`, `loop` ... named by a local would stop meaning themselves) */
            Word *w = forth_find(f, s, n);

            if (w != NULL && (w->flags & WORD_IMMEDIATE)) {
                forth_error(f, "{: a local can't take an immediate word's name: %.*s", (int)n, s);
            }
        }
        l->is_float[l->n] = (char)is_float;
        l->at[l->n] = l->cells++;
        snprintf(l->names[l->n++], WORD_NAME_MAX + 1, "%.*s", (int)n, s);
    }
    if (taken < 0) {
        taken = l->n;
    }
    if (l->frame_size == NULL) {   /* the frame: its size patched as later {:s (and closures) add to it */
        comma(f, (Cell)f->w_locals);
        l->frame_size = (Cell *)f->here;
        comma(f, 0);
    }
    *l->frame_size = l->cells;
    for (i = taken - 1; i >= first; i--) {   /* (the last named is on top) */
        comma(f, (Cell)(l->is_float[i] ? f->w_flocal_store : f->w_local_store));
        comma(f, l->at[i]);
    }
}
PRIM(p_semicolon) {
    if (f->nlocals_saved > 0 || f->t->sp != f->top_sp) {   /* (a [:, an if, a loop: still waiting for its end) */
        forth_error(f, "; : %s not ended", f->nlocals_saved > 0 ? "a [:" : "an if, a begin or a do");
    }
    if (f->locals.frame_size != NULL) {
        comma(f, (Cell)f->w_unlocals);
    }
    memset(&f->locals, 0, sizeof(f->locals));
    f->nquoted = 0;
    comma(f, (Cell)f->w_exit);
    if (f->defining != NULL) {
        forth_check_definition(f, f->defining);
        if (f->defining->code == docol) {
            forth_native_compile(f, f->defining, (const Cell *)f->here);
        }
    }
    /* (the word `:` began; a host's own defining word may not say, then `latest`) */
    (f->defining != NULL ? f->defining : f->latest)->flags &= (uint8_t)~WORD_HIDDEN;
    f->defining = NULL;
    f->compiling = 0;
}
PRIM(p_noname) {   /* :noname ( -- xt ) a definition without a name */
    Word *before = f->latest, *def = new_word(f, "", 0, docol);

    def->flags = WORD_HIDDEN;
    f->defining = def;
    f->latest_before = before;
    f->declared.kind = EFFECT_UNKNOWN;
    f->declared_text[0] = 0;
    f->declared_given = 0;
    PUSH(def);
    f->compiling = -1;
    memset(&f->locals, 0, sizeof(f->locals));
    f->nlocals_saved = 0;
    f->nquoted = 0;
    f->top_sp = f->t->sp;
}
PRIM(p_immediate) {   /* (its comment is the code it lays down's: not checked, nor kept as its effect) */
    f->latest->flags |= WORD_IMMEDIATE;
    f->latest->effect.kind = EFFECT_UNKNOWN;
    if (f->held_for == f->latest) {
        f->held_warning[0] = 0;
    }
}
PRIM(p_lbracket) { f->compiling = 0; }
PRIM(p_rbracket) { f->compiling = -1; }
/* a control word outside a definition: an error, not a branch patched into whatever is on the stack */
PRIM(p_inside_definition) {
    if (!f->compiling) {
        forth_error(f, "only inside a definition");
    }
}
PRIM(p_tick) { PUSH(parse_find(f, "'")); }
PRIM(p_brackettick) {
    comma(f, (Cell)f->w_lit);
    comma(f, (Cell)parse_find(f, "[']"));
}
PRIM(p_literal) {
    comma(f, (Cell)f->w_lit);
    comma(f, POP());
}
PRIM(p_fliteral) {
    comma(f, (Cell)f->w_flit);
    comma_float(f, FPOP());
}
PRIM(p_postpone) {   /* compile the next word's compile-time behaviour */
    Word *x = parse_find(f, "postpone");

    if (x->flags & WORD_IMMEDIATE) {
        comma(f, (Cell)x);
    } else {
        comma(f, (Cell)f->w_lit);
        comma(f, (Cell)x);
        comma(f, (Cell)f->w_comma);
    }
}
PRIM(p_recurse) { comma(f, (Cell)f->latest); }
/* defer name: a word that runs another, chosen later (`' word is name`); body[0] its xt. It runs it as
 * `execute` does, not nested, so a call through it costs no frame (and a `jump` through it is a tail call). */
#define DEFER_HOPS_MAX 64   /* deferred words set to deferred words, at most (more: set to each other, round and round) */
static void dodefer(Forth *f, Word *w) {
    Word *x = w;
    int hops;

    for (hops = 0; x->code == dodefer; hops++) {   /* (followed here, not by calls: a loop of them is an error) */
        if (hops == DEFER_HOPS_MAX) {
            forth_error(f, "%s: deferred words set to each other in a loop", w->name);
        }
        if (x->body[0] == 0) {
            forth_error(f, "%s: deferred, and nothing set to it", x->name);
        }
        x = xt_arg(f, x->body[0], x->name);   /* (`is` stores whatever it is given) */
    }
    x->code(f, x);
}
PRIM(p_defer) {
    size_t len;
    const char *name = parse_name(f, &len, "defer");

    new_word(f, name, len, dodefer);
    comma(f, (Cell)forth_find(f, "noop", 4));
    f->deferred = f->latest;
    f->deferred_in = source(f);
    f->deferred_at = source(f)->pos;
    f->deferred_text[0] = 0;
}
PRIM(p_create) {
    size_t len;
    const char *name = parse_name(f, &len, "create");

    new_word(f, name, len, dovar);
}
/* enum: mood  calm wary tense angry ;   names for the values of something: constants from 0 up
 * (a number in the list sets the next one: `enum: act  $36 hide  $52 lie-down ;`), and
 * `mood>name ( n -- addr len )` naming a value ("?" for one not in the list). Comments - `\` to
 * the line's end, `( )` - may come between the names. */
static void doenum_name(Forth *f, Word *w) {
    Cell n = w->body[0], i, v = POP();

    for (i = 0; i < n; i++) {
        if (w->body[1 + 2 * i] == v) {
            Word *c = (Word *)w->body[2 + 2 * i];

            PUSH(c->name);
            PUSH(c->len);
            return;
        }
    }
    PUSH("?");
    PUSH(1);
}
PRIM(p_enum) {
    size_t len, m;
    const char *name = parse_name(f, &len, "enum:");
    char tname[WORD_NAME_MAX + 1];
    Word *consts[256];
    Cell values[256], next = 0, x;
    int n = 0, i;

    if (len + 5 > WORD_NAME_MAX) {
        forth_error(f, "enum: name too long: %.*s", (int)len, name);
    }
    snprintf(tname, sizeof tname, "%.*s>name", (int)len, name);
    for (;;) {
        const char *s = parse_word(f, &m);

        if (m == 0) {
            forth_error(f, "enum: %s without ;", tname);
        }
        if (m == 1 && s[0] == ';') {
            break;
        }
        if (m == 1 && s[0] == '\\') {   /* a comment: the rest of the line */
            (void)parse_until(f, '\n', &m);
            source(f)->line++;
            continue;
        }
        if (m == 1 && s[0] == '(') {
            (void)parse_until(f, ')', &m);
            continue;
        }
        if (parse_int(f, s, m, &x)) {
            next = x;
            continue;
        }
        if (n == 256) {
            forth_error(f, "enum: %s has too many names", tname);
        }
        consts[n] = new_word(f, s, m, docon);
        comma(f, next);
        values[n++] = next;
        next = (Cell)((UCell)next + 1);   /* (wraps, as sums do) */
    }
    new_word(f, tname, strlen(tname), doenum_name);
    comma(f, n);
    for (i = 0; i < n; i++) {
        comma(f, values[i]);
        comma(f, (Cell)consts[i]);
    }
}
/* strings: files  intro.sfd  menu.sfd  8 ending.sfd ;   a list of words as strings: `files ( i --
 * addr len )` gives string i ("" for one not in the list). A number sets the next index (those
 * skipped are ""); comments - `\` to the line's end, `( )` - may come between them. The body: the
 * count, then each string's address and length, then their bytes. */
static void dostrings(Forth *f, Word *w) {
    Cell n = w->body[0], i = POP();

    if (i < 0 || i >= n || w->body[1 + 2 * i] == 0) {
        PUSH("");
        PUSH(0);
        return;
    }
    PUSH(w->body[1 + 2 * i]);
    PUSH(w->body[2 + 2 * i]);
}
PRIM(p_strings) {
    enum { MAX = 1024 };
    static _Thread_local const char *ss[MAX];
    static _Thread_local size_t lens[MAX];
    size_t len, m;
    const char *name = parse_name(f, &len, "strings:");
    Cell next = 0, n = 0, x, i;
    Cell *body;

    for (i = 0; i < MAX; i++) {
        ss[i] = NULL;
        lens[i] = 0;
    }
    for (;;) {
        const char *s = parse_word(f, &m);

        if (m == 0) {
            forth_error(f, "strings: %.*s without ;", (int)len, name);
        }
        if (m == 1 && s[0] == ';') {
            break;
        }
        if (m == 1 && s[0] == '\\') {
            (void)parse_until(f, '\n', &m);
            source(f)->line++;
            continue;
        }
        if (m == 1 && s[0] == '(') {
            (void)parse_until(f, ')', &m);
            continue;
        }
        if (parse_int(f, s, m, &x)) {
            next = x;
            continue;
        }
        if (next < 0 || next >= MAX) {
            forth_error(f, "strings: %.*s has too many", (int)len, name);
        }
        ss[next] = s;
        lens[next] = m;
        if (++next > n) {
            n = next;
        }
    }
    (void)new_word(f, name, len, dostrings);
    comma(f, n);
    body = (Cell *)allot(f, (size_t)n * 2 * sizeof(Cell));
    for (i = 0; i < n; i++) {
        char *dst = ss[i] ? (char *)allot(f, lens[i] + 1) : NULL;

        if (dst) {
            memcpy(dst, ss[i], lens[i]);
            dst[lens[i]] = 0;
        }
        body[2 * i] = (Cell)dst;
        body[2 * i + 1] = (Cell)lens[i];
    }
}
PRIM(p_docompile) { comma(f, (Cell)f->w_does); }   /* does> */
/* >body ( xt -- addr ) a word's data: a created word's (a variable's, a value's, a deferred word's ...) - not a
 * primitive's, which has none (its address would be the next word's header), nor a colon definition's code */
PRIM(p_tobody) {
    Word *x = forth_xt_arg(f, POP(), ">body");

    if (is_colon(x) || x->code == doforward || body_end(f, x) == x->body) {
        forth_error(f, ">body: %s has no data (a primitive, or a colon definition): `to` and `is` take a value's, a "
                    "deferred word's", x->name);
    }
    PUSH(x->body);
}
PRIM(p_latest) { PUSH(f->latest); }
PRIM(p_find) {   /* ( addr len -- xt | 0 ) */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = string_arg(f, a, n, "find", &len);

    PUSH(forth_find(f, s, len));
}
PRIM(p_parse_line) {   /* ( "rest of the line" -- addr len ) */
    size_t len;
    const char *s = parse_until(f, '\n', &len);

    source(f)->line++;
    PUSH(s);
    PUSH(len);
}
PRIM(p_parse_name) {   /* ( "name" -- addr len ) */
    size_t len;
    const char *s = parse_word(f, &len);

    PUSH(s);
    PUSH(len);
}

/* strings and comments */
static void compile_string(Forth *f, const char *s, size_t len) {
    comma(f, (Cell)f->w_litstring);
    comma(f, (Cell)len);
    memcpy(allot(f, len), s, len);
    align_here(f);
}
/* a string at the prompt: in the next of the rotating buffers (one too long for them an error, not cut short) */
static void prompt_string(Forth *f, const char *word, const char *s, size_t len) {
    char *buf = f->sbuf[f->nsbuf++ % FORTH_SBUFS];

    if (len >= sizeof(f->sbuf[0])) {
        forth_error(f, "%s: longer than %d bytes at the prompt (inside a definition it may be longer)", word,
                    (int)sizeof(f->sbuf[0]) - 1);
    }
    memcpy(buf, s, len);
    buf[len] = 0;
    PUSH(buf);
    PUSH(len);
}
PRIM(p_squote) {   /* s" text" ( -- addr len ) */
    size_t len;
    const char *s = parse_until(f, '"', &len);

    if (f->compiling) {
        compile_string(f, s, len);
    } else {
        prompt_string(f, "s\"", s, len);
    }
}
/* s\" text" ( -- addr len ) as s", with escapes: \n \r \t \0 \e \" \\ and \xHH (two hex digits, a byte) */
#define ESCAPED_MAX 4096   /* an escaped string's bytes, at most */
#define ESCAPE_BYTE 27     /* \e: the terminal's escape */
static int hex_digit(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
/* the text up to the next " (one space after the word skipped), its escapes made what they say, into buf */
static size_t parse_escaped(Forth *f, char *buf, size_t size, const char *who) {
    Source *src = source(f);
    size_t len = 0;

    if (src->pos < src->len && src->text[src->pos] == ' ') {
        src->pos++;
    }
    while (src->pos < src->len && src->text[src->pos] != '"') {
        char c = src->text[src->pos++];

        if (c == '\\' && src->pos < src->len) {
            char e = src->text[src->pos++];

            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '0': c = 0; break;
            case 'e': c = ESCAPE_BYTE; break;
            case 'x': {
                int hi = src->pos < src->len ? hex_digit((unsigned char)src->text[src->pos]) : -1;
                int lo = src->pos + 1 < src->len ? hex_digit((unsigned char)src->text[src->pos + 1]) : -1;

                if (hi < 0 || lo < 0) {
                    forth_error(f, "%s: \\x wants two hex digits", who);
                }
                src->pos += 2;
                c = (char)(hi * 16 + lo);
                break;
            }
            default: c = e; break;   /* (\" \\ - and any other: itself) */
            }
        } else if (c == '\n') {
            src->line++;
        }
        if (len == size) {
            forth_error(f, "%s: longer than %zu bytes", who, size);
        }
        buf[len++] = c;
    }
    if (src->pos < src->len) {
        src->pos++;
    }
    return len;
}
PRIM(p_sbackslash) {
    char buf[ESCAPED_MAX];
    size_t len = parse_escaped(f, buf, sizeof(buf), "s\\\"");

    if (f->compiling) {
        compile_string(f, buf, len);
    } else {
        prompt_string(f, "s\\\"", buf, len);
    }
}
/* " text" ( -- s ) a string object (a sequence: map, append, % ...), with s\"'s escapes. In a definition, a new one
 * each time the word runs - made where its objects are made, its to keep or let go as any other string's - copied
 * from the text compiled once (kept by the system, never handed out) */
PRIM(p_quote) {
    char buf[ESCAPED_MAX];
    size_t len = parse_escaped(f, buf, sizeof(buf), "\"");

    if (f->compiling) {
        comma(f, (Cell)f->w_lit);
        comma(f, (Cell)forth_new_string_kept(f, buf, len));
        comma(f, (Cell)f->w_string_copy);
    } else {
        PUSH(forth_new_string(f, buf, len));
    }
}
PRIM(p_dotquote) {   /* ." text" */
    size_t len;
    const char *s = parse_until(f, '"', &len);

    if (f->compiling) {
        compile_string(f, s, len);
        comma(f, (Cell)f->w_type);
    } else {
        forth_type(f, s, len);
    }
}
PRIM(p_dotparen) {   /* .( text) prints now */
    size_t len;
    const char *s = parse_until(f, ')', &len);

    forth_type(f, s, len);
}
/* the comment just begun follows `defer name` with nothing between: the deferred word's stack comment */
static int after_defer(Forth *f, size_t comment_at) {
    const Source *src = source(f);
    size_t i;

    if (f->compiling || f->deferred == NULL || f->deferred != f->latest || f->deferred_in != src ||
        f->deferred_at > comment_at) {
        return 0;
    }
    for (i = f->deferred_at; i < comment_at; i++) {
        if (!isspace((unsigned char)src->text[i]) && src->text[i] != '(') {
            return 0;
        }
    }
    return 1;
}
PRIM(p_paren) {   /* ( comment ) - at a definition's start, its stack comment: `;` checks its code against it; right
                   * after `defer name`, the deferred word's (its callers are checked with it) */
    size_t start = source(f)->pos, len, k;
    const char *s = parse_until(f, ')', &len);
    ForthEffect e;

    if (after_defer(f, start) && forth_parse_effect(s, len, &e)) {   /* (a second comment - its floats - after it) */
        k = strlen(f->deferred_text);
        snprintf(f->deferred_text + k, sizeof(f->deferred_text) - k, "%s( %.*s )", k > 0 ? " " : "", (int)len, s);
        if (forth_parse_effect(f->deferred_text, strlen(f->deferred_text), &e)) {
            f->deferred->effect = e;
            f->deferred->effect_tried = 1;
        }
        f->deferred_at = source(f)->pos;   /* (the next comment, right after this one, is its too) */
        return;
    }

    if (f->compiling && f->defining != NULL && f->here == (uint8_t *)f->defining->body && f->nlocals_saved == 0 &&
        forth_parse_effect(s, len, &e)) {
        while (len > 0 && isspace((unsigned char)s[len - 1])) {
            len--;
        }
        while (len > 0 && isspace((unsigned char)*s)) {
            s++;
            len--;
        }
        if (f->declared_given) {   /* (a defining word said its effect: this must agree) */
            f->declared_given = 0;
            if (e.in != f->declared.in || e.out != f->declared.out || e.fin != f->declared.fin ||
                e.fout != f->declared.fout || e.either != f->declared.either || e.kind != f->declared.kind) {
                forth_warn(f, "%s ( %.*s ): %s", f->defining->len > 0 || f->defining->name[0] ? f->defining->name
                                                                                              : ":noname",
                           (int)len, s, f->declared_why);
                f->declared.kind = EFFECT_VARIES;   /* (one warning: its code isn't checked too) */
                return;
            }
            f->declared_text[0] = 0;
        }
        k = strlen(f->declared_text);
        snprintf(f->declared_text + k, sizeof(f->declared_text) - k, "%s( %.*s )", k > 0 ? " " : "", (int)len, s);
        forth_parse_effect(f->declared_text, strlen(f->declared_text), &f->declared);
    }
}
PRIM(p_backslash) { size_t len; (void)parse_until(f, '\n', &len); source(f)->line++; }
/* the rest of the source being read (the file, or the string `evaluate`d) skipped */
PRIM(p_skip_file) { source(f)->pos = source(f)->len; }
/* source-dir ( -- s len ) the folder of the file being read (the innermost: text it evaluates aside), a new string;
 * "." for none - a script's own files found beside it, wherever it was run from */
PRIM(p_source_dir) {
    int i;

    for (i = f->nsrc - 1; i >= 0; i--) {
        const char *name = f->src[i].name;
        size_t len = name != NULL ? strlen(name) : 0;

        if (len > 3 && strcmp(name + len - 3, ".fs") == 0) {
            const char *slash = strrchr(name, '/'), *back = strrchr(name, '\\');

            if (back != NULL && (slash == NULL || back > slash)) {
                slash = back;   /* (a Windows path) */
            }
            if (slash == NULL) {
                break;
            }
            PUSH(forth_new_string(f, name, (size_t)(slash - name)));
            PUSH(slash - name);
            return;
        }
    }
    PUSH(forth_new_string(f, ".", 1));
    PUSH(1);
}
PRIM(p_char) {
    size_t len;
    const char *s = parse_name(f, &len, "char");

    PUSH((unsigned char)s[0]);
}
PRIM(p_bracketchar) {
    size_t len;
    const char *s = parse_name(f, &len, "[char]");

    comma(f, (Cell)f->w_lit);
    comma(f, (unsigned char)s[0]);
}
PRIM(p_include) {
    size_t len;
    const char *s = parse_name(f, &len, "include");

    include_file(f, s, len);
}
PRIM(p_included) {
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = string_arg(f, a, n, "included", &len);

    include_file(f, s, len);
}
PRIM(p_evaluate) {
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = string_arg(f, a, n, "evaluate", &len);

    evaluate(f, s, len, "evaluate", NULL);
}

/* ---- inspecting the system ---- */

static void list_words(Forth *f, Vocab *v) {
    Word *x;
    int col = (int)strlen(v->name) + 2;

    forth_printf(f, "%s: ", v->name);
    for (x = v->latest; x != NULL; x = x->link) {
        if (x->flags & WORD_HIDDEN || x->len == 0) {
            continue;
        }
        if (col + x->len + 1 > 78) {
            forth_type(f, "\n  ", 3);
            col = 2;
        }
        forth_printf(f, "%s ", x->name);
        col += x->len + 1;
    }
    forth_type(f, "\n", 1);
}
PRIM(p_words) {   /* the words this source sees, by vocabulary */
    int k;

    for (k = 0; k < f->m.norder; k++) {
        if (f->m.order[k] != f->core) {
            list_words(f, f->m.order[k]);
        }
    }
    list_words(f, f->core);
}
PRIM(p_vocab_words) {   /* vocab-words name */
    size_t len;
    const char *name = parse_name(f, &len, "vocab-words");
    Vocab *v = vocab_find(f, name, len);

    if (v == NULL) {
        forth_error(f, "no vocabulary %.*s", (int)len, name);
    }
    list_words(f, v);
}
PRIM(p_vocabs) {   /* every vocabulary */
    Vocab *v;

    for (v = f->vocabs; v != NULL; v = v->next) {
        forth_printf(f, "%s%s ", v->name, v->state == VOCAB_LOADING ? "(loading)" : "");
    }
    forth_type(f, "\n", 1);
}

/* IN: name - definitions from here go into the vocabulary `name` */
PRIM(p_in) {
    size_t len;
    const char *name = parse_name(f, &len, "IN:");
    char buf[VOCAB_NAME_MAX + 1];
    Vocab *v;

    snprintf(buf, sizeof(buf), "%.*s", (int)len, name);
    v = forth_vocab(f, buf);
    if (v->state == VOCAB_NEW) {
        v->state = VOCAB_LOADED;   /* (a file included directly, not through USING:) */
    }
    forth_set_current(f, v);
}

/* a vocabulary's name: names between dots (a, a.b), none empty, no / or \ - its file stays in a root */
static int vocab_name_ok(const char *name, size_t len) {
    size_t i;

    if (len == 0 || name[0] == '.' || name[len - 1] == '.') {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == '\\' || (name[i] == '.' && name[i + 1] == '.')) {
            return 0;
        }
    }
    return 1;
}

static int file_there(Forth *f, const char *path) {   /* the host's reader has it, or the disk */
    FILE *fp;
    char *text;
    size_t tlen;

    if (f->reader != NULL && (text = f->reader(f->reader_ctx, path, &tlen)) != NULL) {
        free(text);
        return 1;
    }
    if ((fp = fopen(path, "rb")) != NULL) {
        fclose(fp);
        return 1;
    }
    return 0;
}

/* the file for vocabulary a.b: <root>/a/b.fs, or (laid out as the library's basis/) <root>/a/b/b.fs */
static int vocab_file(Forth *f, const char *name, size_t len, char *path, size_t n) {
    const char *last = name;   /* (its last part: b) */
    int r;
    size_t i;

    for (i = 0; i < len; i++) {
        if (name[i] == '.') {
            last = name + i + 1;
        }
    }

    for (r = 0; r < f->nroots; r++) {
        int k = snprintf(path, n, "%s/", f->roots[r]);

        for (i = 0; i < len && (size_t)k + 4 < n; i++) {
            path[k++] = name[i] == '.' ? '/' : name[i];
        }
        snprintf(path + k, n - (size_t)k, ".fs");
        if (file_there(f, path)) {
            return 1;
        }
        snprintf(path + k, n - (size_t)k, "/%.*s.fs", (int)(len - (size_t)(last - name)), last);
        if (file_there(f, path)) {
            return 1;
        }
    }
    if (builtin_source(name, len) != NULL) {
        snprintf(path, n, BUILTIN_DIR "/%.*s.fs", (int)len, name);
        return 1;
    }
    return 0;
}

/* a vocabulary, loaded from its file if it isn't yet */
static Vocab *require(Forth *f, const char *name, size_t len) {
    Vocab *volatile v = vocab_find(f, name, len);   /* (volatile: read after the longjmp) */
    char path[1024], buf[VOCAB_NAME_MAX + 1];
    jmp_buf jb, *volatile prev = f->catch;

    if (v != NULL && v->state == VOCAB_LOADED) {
        return v;
    }
    if (v != NULL && v->state == VOCAB_LOADING) {
        forth_error(f, "USING: %.*s - it is still loading (vocabularies using each other)", (int)len, name);
    }
    if (!vocab_name_ok(name, len)) {
        forth_error(f, "USING: %.*s isn't a vocabulary's name (a or a.b: no / or \\, nothing empty between dots)",
                    (int)len, name);
    }
    if (len > VOCAB_NAME_MAX || !vocab_file(f, name, len, path, sizeof(path))) {
        forth_error(f, "USING: no vocabulary %.*s (no %.*s.fs in the scripts)", (int)len, name, (int)len, name);
    }
    snprintf(buf, sizeof(buf), "%.*s", (int)len, name);
    v = forth_vocab(f, buf);
    v->state = VOCAB_LOADING;
    if (setjmp(jb) != 0) {   /* the file failed: not loaded after all; pass the error on */
        f->catch = prev;
        v->state = VOCAB_NEW;
        longjmp(*prev, 1);
    }
    f->catch = &jb;
    include_file(f, path, strlen(path));
    f->catch = prev;
    v->state = VOCAB_LOADED;
    return v;
}

/* a vocabulary's file read again (loaded the first time): what it defines anew, every caller of its old words gets
 * (forward_hidden - its <PRIVATE words' too); what the file no longer defines stays as it was. Top-level code runs
 * again (`reloading?` true meanwhile: what must run once - an actor spawned - can ask) */
static Vocab *reload(Forth *f, const char *name, size_t len) {
    Vocab *volatile v = vocab_find(f, name, len);
    char path[1024], private_name[VOCAB_NAME_MAX + 1];
    const uint8_t *volatile mark = f->here;
    jmp_buf jb, *volatile prev = f->catch;
    Vocab *private_vocab;

    if (v == NULL || v->state != VOCAB_LOADED) {
        return require(f, name, len);
    }
    if (v == f->core || !vocab_file(f, name, len, path, sizeof(path))) {
        forth_error(f, "reload: no file for vocabulary %.*s", (int)len, name);
    }
    v->state = VOCAB_LOADING;
    if (f->reloading++ == 0) {
        f->reload_mark = mark;
    }
    if (setjmp(jb) != 0) {   /* the file failed: what it defined before the error stays, nothing forwarded */
        f->catch = prev;
        f->reloading--;
        v->state = VOCAB_LOADED;
        longjmp(*prev, 1);
    }
    f->catch = &jb;
    include_file(f, path, strlen(path));
    f->catch = prev;
    f->reloading--;
    v->state = VOCAB_LOADED;
    forward_hidden(f, v, mark);
    snprintf(private_name, sizeof(private_name), "%.55s.private", v->name);
    if ((private_vocab = vocab_find(f, private_name, strlen(private_name))) != NULL) {
        forward_hidden(f, private_vocab, mark);
    }
    return v;
}
PRIM(p_reload) {   /* reload ( "name" -- ) */
    size_t len;
    const char *name = parse_name(f, &len, "reload");

    reload(f, name, len);
}
PRIM(p_reloading_q) { PUSH(f->reloading > 0 ? -1 : 0); }   /* reloading? ( -- flag ) */

int forth_reload(Forth *f, const char *name) {   /* (as forth_include, its file found as USING: finds it) */
    jmp_buf jb, *prev = f->catch;
    int level = f->nsrc, depth = f->depth, views = f->nviews;
    Cell budget = f->budget;
    Task *t = f->t;
    int region = t->region;

    if (setjmp(jb) != 0) {
        forth_regions_unwind(f, t, region);
        back_to_view(f, views);
        f->catch = prev;
        while (f->nsrc > level) {
            pop_source(f);
        }
        f->depth = depth;
        f->budget = budget;
        drop_definition(f);
        task_reset(f->t);
        return -1;
    }
    f->catch = &jb;
    limit_eval(f);
    reload(f, name, strlen(name));
    f->budget = budget;
    f->catch = prev;
    return 0;
}

/* USING: a b c ; - this source uses these vocabularies (loading them if needed) */
PRIM(p_using) {
    for (;;) {
        size_t len;
        const char *name = parse_name(f, &len, "USING:");

        if (len == 1 && name[0] == ';') {
            return;
        }
        order_add(&f->m, require(f, name, len));
    }
}
PRIM(p_use) {   /* USE: name - one vocabulary */
    size_t len;
    const char *name = parse_name(f, &len, "USE:");

    order_add(&f->m, require(f, name, len));
}

/* <PRIVATE ... PRIVATE> - helpers in name.private: visible here, not to other users */
PRIM(p_private_begin) {
    char buf[VOCAB_NAME_MAX + 1];
    Vocab *v;

    snprintf(buf, sizeof(buf), "%.55s.private", f->m.current->name);
    v = forth_vocab(f, buf);
    v->parent = f->m.current;
    v->state = VOCAB_LOADED;
    forth_set_current(f, v);
}
PRIM(p_private_end) {
    if (f->m.current->parent == NULL) {
        forth_error(f, "PRIVATE> without <PRIVATE");
    }
    f->m.current = f->m.current->parent;
}

static const char *word_kind(Word *x) {
    if (is_colon(x)) return "colon";
    if (x->code == dovar) return "variable";
    if (x->code == docon) return "constant";
    if (x->code == dofcon) return "fconstant";
    if (x->code == dodoes) return "does>";
    if (x->code == dodefer) return "defer";
    if (x->code == dotaskvar) return "task-variable";
    if (x->code == doforward) return "word defined again";
    return "primitive";
}

/* the word at x, if x is one (a literal that is an execution token) */
static Word *as_word(Forth *f, Cell x) {
    Vocab *v;
    Word *w;

    for (v = f->vocabs; v != NULL; v = v->next) {
        for (w = v->latest; w != NULL; w = w->link) {
            if ((Cell)w == x) {
                return w;
            }
        }
    }
    return NULL;
}

/* a combinator compiled inline at p, after the n quotations qs: its name, and the cells it takes (0: none there) */
static const char *see_inline(Forth *f, Cell *p, Cell *end, Word **qs, int n, size_t *cells) {
    static const struct {
        const char *name, *code;   /* its code: words, Q for each quotation called (in order: 0 1 2, or 0 0), @ a
                                    * branch's target that is the code's end, ^ one that is within it */
        int quotations;
    } inlined[] = {
        {"dip", ">r Q0 r>", 1}, {"2dip", ">r >r Q0 r> r>", 1}, {"keep", "dup >r Q0 r>", 1},
        {"2keep", "2dup >r >r Q0 r> r>", 1}, {"bi", "dup >r Q0 r> Q1", 2}, {"tri", "dup >r Q0 r@ Q1 r> Q2", 3},
        {"bi@", ">r Q0 r> Q0", 1}, {"when", "0branch @ Q0", 1}, {"unless", "0= 0branch @ Q0", 1},
        {"each", "(each-begin) (each-next) ^ Q0 branch ^ (each-end)", 1},
        {"map", "dup >r dup (like-acc) >r (each-begin) (each-next) ^ Q0 (each-keep) branch ^ (each-end) r> r> (like-done)", 1},
        {"filter", "dup >r dup (like-acc) >r (each-begin) (each-next) ^ Q0 0branch ^ (each-item) (each-keep) branch ^ (each-end) r> r> (like-done)", 1},
        {"reduce", "swap (each-begin) (each-next) ^ Q0 branch ^ (each-end)", 1},
        {"find", "(each-begin) (each-next) ^ Q0 0branch ^ (each-found)", 1},
    };
    size_t i;

    for (i = 0; i < sizeof(inlined) / sizeof(inlined[0]); i++) {
        const char *c = inlined[i].code;
        Cell *q = p, *patch = NULL;
        int ok = inlined[i].quotations <= n;

        while (ok && *c) {
            const char *sp = strchr(c, ' ');
            size_t len = sp != NULL ? (size_t)(sp - c) : strlen(c);

            if (q >= end) {
                ok = 0;
            } else if (c[0] == 'Q' && len == 2) {   /* a quotation's call (a closure's: (closure-call) q) */
                Word *want = qs[n - inlined[i].quotations + (c[1] - '0')];

                if ((Word *)*q == f->w_closure_call) {
                    q++;
                }
                ok = q < end && (Word *)*q == want;
                q++;
            } else if (c[0] == '@' && len == 1) {   /* a branch's target: patched to past the code */
                patch = q++;
            } else if (c[0] == '^' && len == 1) {   /* (one within the code: a walk's next turn, its end) */
                q++;
            } else {
                Word *x = (Word *)*q++;

                ok = is_word(f, (Cell)x) && x->len == len && memcmp(x->name, c, len) == 0;
            }
            c += len;
            c += *c == ' ';
        }
        if (ok && (patch == NULL || (Cell *)*patch == q)) {
            *cells = (size_t)(q - p);
            return inlined[i].name;
        }
    }
    return NULL;
}

/* a string object's literal as `" text"`, with the escapes that read it back */
static void see_string(Forth *f, const char *s, size_t n) {
    size_t i;

    forth_type(f, "\" ", 2);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '"' || c == '\\') {
            forth_printf(f, "\\%c", c);
        } else if (c == '\n') {
            forth_type(f, "\\n", 2);
        } else if (c == '\t') {
            forth_type(f, "\\t", 2);
        } else if (c == '\r') {
            forth_type(f, "\\r", 2);
        } else if (c < 32 || c == 127) {
            forth_printf(f, "\\x%02x", c);
        } else {
            forth_type(f, (const char *)&s[i], 1);
        }
    }
    forth_type(f, "\" ", 2);
}

/* show a word: a colon definition is decompiled */
static void see_code(Forth *f, Cell *p, Cell *end, int newline) {
    Word *qs[3];   /* the quotations shown last, a combinator compiled inline after them may call */
    int nq = 0;

    while (p < end) {
        Word *x = (Word *)*p++;

        if (!is_word(f, (Cell)x)) {   /* (a cell laid down by hand: shown as it was) */
            forth_printf(f, "[ %s , ] ", format_number(f, (Cell)x, 0));
            continue;
        }
        if (x == f->w_lit) {
            Word *lw = as_word(f, *p);
            size_t slen;
            const char *str = lw == NULL ? forth_object_string(f, *p, &slen) : NULL;

            if (lw != NULL) {
                forth_printf(f, "['] %s ", lw->name);
            } else if (str != NULL && p + 1 < end && (Word *)p[1] == f->w_string_copy) {   /* (`" text"`'s) */
                see_string(f, str, slen);
                p++;
            } else {
                forth_printf(f, "%s ", format_number(f, *p, 0));
            }
            p++;
        } else if (x == f->w_flit) {
            Float v;

            memcpy(&v, p, sizeof(v));
            p += FCELLS;
            forth_printf(f, "%g ", v);
        } else if (x == f->w_litstring) {
            Cell n = *p++;

            forth_printf(f, "s\" %.*s\" ", (int)n, (const char *)p);
            p += CELLS_FOR((size_t)n);
        } else if (x == f->w_exit && p == end) {
            if (newline) {
                forth_type(f, ";", 1);
            }
        } else if (x->operands == 1 && x->len == 6 && memcmp(x->name, "branch", 6) == 0 && (Cell *)*p > p &&
                   is_colon((Word *)(p + 1)) && ((Word *)(p + 1))->len == 0) {
            /* a quotation: a branch over a headless word, then the word as a literal (a closure's: 3 cells) - or,
             * a combinator compiled inline, its call where it stands */
            Word *q = (Word *)(p + 1);
            Cell *after = (Cell *)*p;
            const char *inl;
            size_t cells;

            forth_type(f, "[: ", 3);
            see_code(f, q->body + 2, after - 1, 0);   /* (its body after its frame, up to its exit) */
            forth_type(f, ";] ", 3);
            p = after;
            if ((Word *)*p == f->w_lit && (Word *)p[1] == q) {
                p += 2;
                nq = 0;
            } else if ((Word *)*p == f->w_closure && (Word *)p[2] == q) {
                p += 3;
                nq = 0;
            } else {   /* (no literal: a combinator compiled inline after it, or another quotation for it) */
                if (nq == 3) {
                    memmove(qs, qs + 1, 2 * sizeof(qs[0]));
                    nq = 2;
                }
                qs[nq++] = q;
                if ((inl = see_inline(f, p, end, qs, nq, &cells)) != NULL) {
                    forth_printf(f, "%s ", inl);
                    p += cells;
                    nq = 0;
                }
            }
            continue;
        } else if (x == f->w_unqframe && p < end && (Word *)*p == f->w_exit && p + 1 == end) {
            p++;   /* (a quotation's last exit) */
        } else if (x == f->w_unqframe || x == f->w_unlocals) {
            /* (a frame left as it exits: the exit says) */
        } else if (x == f->w_locals || x == f->w_qframe || x == f->w_nop1) {
            p++;   /* (a frame made) */
        } else if (x == f->w_local_fetch || x == f->w_flocal_fetch || x == f->w_local_store || x == f->w_flocal_store) {
            forth_printf(f, "%slocal%ld ", x == f->w_local_store || x == f->w_flocal_store ? "to " : "", (long)*p);
            p++;   /* (its name is gone: its place in the frame) */
        } else if (x == f->w_outer_fetch || x == f->w_fouter_fetch || x == f->w_outer_store || x == f->w_fouter_store) {
            forth_printf(f, "%slocal%ld^%ld ", x == f->w_outer_store || x == f->w_fouter_store ? "to " : "", (long)p[1],
                         (long)p[0]);   /* (its word's: so many frames out) */
            p += 2;
        } else if (x->operands == 1) {
            forth_printf(f, "%s(%+ld) ", x->name, (long)(((Cell *)*p - p) * (Cell)sizeof(Cell)));
            p++;
        } else {
            forth_printf(f, "%s ", x->len > 0 ? x->name : "?");
        }
    }
    if (newline) {
        forth_type(f, "\n", 1);
    }
}
/* a word as `see` shows it: a colon definition's code (a nameless one - a quotation, a handler - by the name the
 * call chain gives it), a does> word's, a word defined again where it forwards, else its kind */
void forth_see(Forth *f, Word *x) {
    Word *next = word_after(f, x);
    Cell *end = next != NULL ? (Cell *)next : (Cell *)f->here;

    if (body_end(f, x) < end) {   /* (a headless word after it - a quotation typed at the prompt - is in no list) */
        end = (Cell *)body_end(f, x);
    }
    if (is_colon(x)) {
        forth_printf(f, ": %s %s", x->name[0] ? x->name : "(nameless)", x->flags & WORD_IMMEDIATE ? "( immediate ) " : "");
        see_code(f, x->body, end, 1);
    } else if (x->code == dodoes) {
        forth_printf(f, "%s (made by a defining word); does> ", x->name);
        see_code(f, x->does, (Cell *)x, 1);
    } else if (x->code == doforward) {
        Word *to = (Word *)x->does;

        forth_printf(f, "%s was defined again: it goes on to %s:%s\n", x->name, to->vocab->name, to->name);
    } else {
        forth_printf(f, "%s is a %s\n", x->name, word_kind(x));
    }
}
PRIM(p_see) { forth_see(f, parse_find(f, "see")); }
PRIM(p_see_xt) { forth_see(f, forth_xt_arg(f, POP(), "see-xt")); }   /* see-xt ( xt -- ) */

/* with-output ( xt -- s ) xt run, what it prints (type, ., the errors said) gathered: a new string. An error in it:
 * what it printed goes out as it would have, and the error goes on */
typedef struct Gathered {
    char *p;
    size_t n, cap;
    int short_of_memory;
} Gathered;

static void gather(void *ctx, const char *s, size_t n) {
    Gathered *g = ctx;

    if (g->n + n > g->cap) {
        size_t cap = g->cap ? g->cap : 256;
        char *more;

        while (cap < g->n + n) {
            cap *= 2;
        }
        if ((more = realloc(g->p, cap)) == NULL) {
            g->short_of_memory = 1;
            return;
        }
        g->p = more;
        g->cap = cap;
    }
    memcpy(g->p + g->n, s, n);
    g->n += n;
}
PRIM(p_with_output) {
    Word *xt = forth_xt_arg(f, POP(), "with-output");
    Gathered g = {NULL, 0, 0, 0};
    OutputFn out = f->out;
    void *ctx = f->out_ctx;
    int failed;
    char *s;

    f->out = gather;
    f->out_ctx = &g;
    failed = forth_call(f, xt) != 0;
    f->out = out;
    f->out_ctx = ctx;
    if (failed) {
        char error[sizeof(f->error)];

        forth_type(f, g.p != NULL ? g.p : "", g.n);
        free(g.p);
        snprintf(error, sizeof(error), "%s", f->error);
        forth_error(f, "%s", error);   /* (the same again: not said twice) */
    }
    if (g.short_of_memory) {
        free(g.p);
        forth_error(f, "with-output: out of memory for what was printed");
    }
    s = forth_new_string(f, g.p != NULL ? g.p : "", g.n);
    free(g.p);
    PUSH(s);
}
PRIM(p_bye) { f->bye = 1; }

/* ---- quotations: [: ... ;] leaves the code between as an execution token ---- */

/* the code goes inline: a branch over a headless word, then the word as a literal. At the
 * prompt it is compiled the same way, then the token left on the stack */
PRIM(p_quote_begin) {
    Cell *slot = NULL;
    Word *q;

    if (f->compiling) {
        comma(f, (Cell)forth_find(f, "branch", 6));
        comma(f, 0);
        slot = (Cell *)(f->here - sizeof(Cell));
    }
    align_here(f);
    q = allot(f, sizeof(Word));   /* not linked into a vocabulary: it has no name */
    memset(q, 0, sizeof(Word));
    mark_word(f, q);
    q->code = docol;
    q->vocab = f->m.current;
    PUSH(slot);
    PUSH(q);
    f->compiling = -1;
    if (f->nlocals_saved == 4) {
        forth_error(f, "[: nested too deep");
    }
    f->quote_saved[f->nlocals_saved][0] = (Cell)slot;
    f->quote_saved[f->nlocals_saved][1] = (Cell)q;
    f->top_sp_saved[f->nlocals_saved] = f->top_sp;
    f->locals_saved[f->nlocals_saved++] = f->locals;   /* (the word's locals aren't the quotation's) */
    memset(&f->locals, 0, sizeof(f->locals));
    f->locals.quotation = 1;
    f->nquoted = 0;
    f->top_sp = f->t->sp;
    comma(f, (Cell)f->w_qframe);   /* (its frame: (nop1) at `;]` if it needs none) */
    f->locals.frame_size = (Cell *)f->here;
    comma(f, 0);
}
/* a quotation laid down as a literal at `at`: kept, for a combinator after it to take inline */
static void quoted(Forth *f, Cell *at, Word *q, int closure) {
    if (f->nquoted > 0) {
        Cell *end = f->quoted[f->nquoted - 1].at + (f->quoted[f->nquoted - 1].closure ? 3 : 2);

        if (end != at) {   /* (not straight after the last: a run of its own) */
            f->nquoted = 0;
        }
    }
    if (f->nquoted == 3) {
        memmove(&f->quoted[0], &f->quoted[1], 2 * sizeof(f->quoted[0]));
        f->nquoted = 2;
    }
    f->quoted[f->nquoted].at = at;
    f->quoted[f->nquoted].q = q;
    f->quoted[f->nquoted++].closure = closure;
}
PRIM(p_quote_end) {
    Word *q;
    Cell *slot;
    struct ForthLocals *l = &f->locals;
    int i, closure = l->captures;

    if (!l->quotation || f->nlocals_saved == 0) {   /* (what it would take off the stack isn't a quotation's) */
        forth_error(f, ";]: no [: to end");
    }
    if (f->t->sp != f->top_sp || f->t->sp < 2 || f->t->ds[f->t->sp - 1] != f->quote_saved[f->nlocals_saved - 1][1] ||
        f->t->ds[f->t->sp - 2] != f->quote_saved[f->nlocals_saved - 1][0]) {
        forth_error(f, ";]: an if or a loop inside [: not ended, or the stack changed (as it compiled)");
    }
    q = (Word *)POP();
    slot = (Cell *)POP();

    leave_frame(f);
    comma(f, (Cell)f->w_exit);
    if (l->n == 0 && !l->captures) {   /* no frame needed: none made, its exits plain */
        l->frame_size[-1] = (Cell)f->w_nop1;
        for (i = 0; i < l->nexits; i++) {
            *l->exits[i] = (Cell)f->w_exit;
        }
    } else if (l->jumps) {
        forth_error(f, "jump: not from a quotation with locals (its own, or its word's)");
    }
    forth_check_quotation(f, q);
    forth_native_compile(f, q, (const Cell *)f->here);
    if (f->nlocals_saved > 0) {
        f->locals = f->locals_saved[--f->nlocals_saved];
        f->top_sp = f->top_sp_saved[f->nlocals_saved];
    }
    if (slot == NULL) {   /* at the prompt: the token */
        f->compiling = 0;
        PUSH(q);
        return;
    }
    *slot = (Cell)f->here;   /* inside a definition: jump here, then the token as a literal */
    align_here(f);
    if (closure) {   /* (made in the frame around it as it is reached) */
        struct ForthLocals *o = &f->locals;
        int at = o->cells;

        if (o->frame_size == NULL) {
            forth_error(f, "[: a quotation using locals, in code with no frame for it");
        }
        o->cells += CLOSURE_CELLS;
        *o->frame_size = o->cells;
        quoted(f, (Cell *)f->here, q, 1);
        comma(f, (Cell)f->w_closure);
        comma(f, at);
        comma(f, (Cell)q);
    } else {
        quoted(f, (Cell *)f->here, q, 0);
        comma(f, (Cell)f->w_lit);
        comma(f, (Cell)q);
    }
}

/* ---- combinators compiled inline: after literal quotations (`[: ... ;] dip`) the quotations are called where they
 * stand - no execute, no call of the combinator - so they cost nothing more than the code, and are checked ---- */

static void call_quoted(Forth *f, int k) {
    if (f->quoted[k].closure) {
        comma(f, (Cell)f->w_closure_call);
    }
    comma(f, (Cell)f->quoted[k].q);
}

/* (the core's word of that name: not one of the source's own that hides it) */
static void lay(Forth *f, const char *name) { comma(f, (Cell)in_vocab(f->core, name, strlen(name))); }

/* a list's walk laid down in place of `[: q ;] each` (map, filter, reduce, find): the list, its length and the next
 * item's index kept on the return stack ((each-begin) ... (each-end)), so q runs on the stack as the word left it -
 *   each:    (each-begin)  again: (each-next) out  q  branch again  out: (each-end)
 *   map:     dup >r dup (like-acc) >r  ...  q (each-keep)  ...  r> r> (like-done)   (each result gathered under the walk,
 *            as a sequence of the walked one's kind)
 *   filter:  dup >r dup (like-acc) >r  ...  q 0branch again  (each-item) (each-keep)  ...  r> r> (like-done)
 *   reduce:  swap  as each                                    (the list from under what q adds each item to)
 *   find:    ...  q 0branch again  out: (each-found)          ( -- x true | false ) */
static void lay_walk(Forth *f, Word *w, int q) {
    int gathers = w == f->w_map || w == f->w_filter;
    Cell *again, *out;

    if (gathers) {   /* (the sequence kept, and what is gathered - of its kind: like-acc, like-done) */
        lay(f, "dup"); lay(f, ">r"); lay(f, "dup"); lay(f, "(like-acc)"); lay(f, ">r");
    }
    if (w == f->w_reduce) {
        lay(f, "swap");
    }
    comma(f, (Cell)f->w_each_begin);
    again = (Cell *)f->here;
    comma(f, (Cell)f->w_each_next);
    out = (Cell *)f->here;
    comma(f, 0);
    call_quoted(f, q);
    if (w == f->w_map) {
        comma(f, (Cell)f->w_each_keep);
    }
    if (w == f->w_filter || w == f->w_find) {
        comma(f, (Cell)f->w_0branch);
        comma(f, (Cell)again);
    }
    if (w == f->w_filter) {
        comma(f, (Cell)f->w_each_item);
        comma(f, (Cell)f->w_each_keep);
    }
    if (w != f->w_find) {
        comma(f, (Cell)f->w_branch);
        comma(f, (Cell)again);
    }
    *out = (Cell)f->here;
    comma(f, (Cell)(w == f->w_find ? f->w_each_found : f->w_each_end));
    if (gathers) {
        lay(f, "r>"); lay(f, "r>"); lay(f, "(like-done)");
    }
}

static int compile_inline(Forth *f, Word *w) {
    int need = w == f->w_tri ? 3 : w == f->w_bi ? 2 : 1, first, k;
    int walk = w == f->w_each || w == f->w_map || w == f->w_filter || w == f->w_reduce || w == f->w_find;
    Cell *end, *patch;

    if (w == NULL || f->nquoted < need ||
        !(w == f->w_dip || w == f->w_2dip || w == f->w_keep || w == f->w_2keep || w == f->w_bi || w == f->w_tri ||
          w == f->w_bi_at || w == f->w_when || w == f->w_unless || walk)) {
        return 0;
    }
    first = f->nquoted - need;
    end = f->quoted[f->nquoted - 1].at + (f->quoted[f->nquoted - 1].closure ? 3 : 2);
    if ((uint8_t *)end != f->here) {   /* (something laid down since: not straight after its quotations) */
        return 0;
    }
    for (k = f->nquoted - 1; k >= first; k--) {   /* the literals taken back (the last closures' cells too) */
        if (f->quoted[k].closure && f->locals.frame_size != NULL &&
            f->quoted[k].at[1] == f->locals.cells - CLOSURE_CELLS) {
            f->locals.cells -= CLOSURE_CELLS;
            *f->locals.frame_size = f->locals.cells;
        }
    }
    here_back(f, (uint8_t *)f->quoted[first].at);
    if (walk) {   /* l [: q ;] each */
        lay_walk(f, w, first);
    } else if (w == f->w_dip) {   /* x [: q ;] dip */
        lay(f, ">r"); call_quoted(f, first); lay(f, "r>");
    } else if (w == f->w_2dip) {
        lay(f, ">r"); lay(f, ">r"); call_quoted(f, first); lay(f, "r>"); lay(f, "r>");
    } else if (w == f->w_keep) {
        lay(f, "dup"); lay(f, ">r"); call_quoted(f, first); lay(f, "r>");
    } else if (w == f->w_2keep) {
        lay(f, "2dup"); lay(f, ">r"); lay(f, ">r"); call_quoted(f, first); lay(f, "r>"); lay(f, "r>");
    } else if (w == f->w_bi) {   /* x [: p ;] [: q ;] bi */
        lay(f, "dup"); lay(f, ">r"); call_quoted(f, first); lay(f, "r>"); call_quoted(f, first + 1);
    } else if (w == f->w_tri) {
        lay(f, "dup"); lay(f, ">r"); call_quoted(f, first); lay(f, "r@"); call_quoted(f, first + 1);
        lay(f, "r>"); call_quoted(f, first + 2);
    } else if (w == f->w_bi_at) {   /* x y [: q ;] bi@ */
        lay(f, ">r"); call_quoted(f, first); lay(f, "r>"); call_quoted(f, first);
    } else {   /* flag [: q ;] when / unless */
        if (w == f->w_unless) {
            lay(f, "0=");
        }
        comma(f, (Cell)f->w_0branch);
        patch = (Cell *)f->here;
        comma(f, 0);
        call_quoted(f, first);
        *patch = (Cell)f->here;
    }
    f->nquoted = 0;
    return 1;
}

/* ---- warnings ---- */

static void warning_text(Forth *f, char *buf, size_t n, const char *fmt, va_list ap) {
    char msg[384];

    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (f->nsrc > 0) {
        snprintf(buf, n, "warning: %s (%s:%d)\n", msg, f->src[f->nsrc - 1].name, f->src[f->nsrc - 1].line);
    } else {
        snprintf(buf, n, "warning: %s\n", msg);
    }
}

/* a warning in strict mode: an error - its text without "warning: " and its line end (its place kept in it) */
_Noreturn static void strict_error(Forth *f, const char *line) {
    const char *msg = strncmp(line, "warning: ", 9) == 0 ? line + 9 : line;
    size_t n = strlen(msg);

    while (n > 0 && msg[n - 1] == '\n') {
        n--;
    }
    forth_error(f, "%.*s - a warning, an error in strict mode (strict!, CLANKFORTH_STRICT)", (int)n, msg);
}

void forth_warn(Forth *f, const char *fmt, ...) {
    char line[512];
    va_list ap;

    forth_flush_warning(f);
    va_start(ap, fmt);
    warning_text(f, line, sizeof(line), fmt, ap);
    va_end(ap);
    f->warnings++;
    if (f->strict) {
        strict_error(f, line);
    }
    forth_type(f, line, strlen(line));
}

/* a definition's warning held until the next begins: `immediate` may come, and then its comment is the code it lays
 * down's, not its own */
void forth_hold_warning(Forth *f, const char *fmt, ...) {
    va_list ap;

    forth_flush_warning(f);
    va_start(ap, fmt);
    warning_text(f, f->held_warning, sizeof(f->held_warning), fmt, ap);
    va_end(ap);
    f->held_for = f->latest;
}

static void flush_held(Forth *f, int may_raise) {
    char line[sizeof(f->held_warning)];

    if (f->held_warning[0]) {
        f->warnings++;
        snprintf(line, sizeof(line), "%s", f->held_warning);
        f->held_warning[0] = 0;
        if (f->strict && may_raise) {
            strict_error(f, line);
        }
        forth_type(f, line, strlen(line));
    }
}
void forth_flush_warning(Forth *f) { flush_held(f, 1); }

PRIM(p_warnings) { PUSH(f->warnings); }   /* ( -- n ) warnings given so far */
PRIM(p_strict_store) { f->strict = POP() != 0; }   /* strict! ( flag -- ) a warning an error from now (true) */
PRIM(p_strict_q) { PUSH(f->strict ? -1 : 0); }     /* strict? ( -- flag ) */

static CatchPlace place_now(const Forth *f) {
    CatchPlace p;

    p.nsrc = f->nsrc - f->turn_nsrc;
    p.views = f->nviews - f->turn_views;
    p.def_turns = f->defining == f->turn_defining;
    p.compiling_turns = f->compiling == f->turn_compiling;
    p.defining = f->defining;
    p.compiling = f->compiling;
    return p;
}
/* the interpreter's state put back as the catch began, from where this turn began: sources and views opened inside
 * it gone, a definition begun inside it dropped */
static void place_back(Forth *f, const CatchPlace *p) {
    Word *defining = p->def_turns ? f->turn_defining : p->defining;

    back_to_view(f, f->turn_views + p->views);
    while (f->nsrc > f->turn_nsrc + p->nsrc) {
        pop_source(f);
    }
    if (f->defining != defining) {   /* (a definition begun inside it, unfinished) */
        drop_definition(f);
    }
    f->compiling = p->compiling_turns ? f->turn_compiling : p->compiling;
}

/* (threaded) a catch the task may wait in, begun: xt run next by the interpreter, then (catch-end); 0 if it is in
 * as many as it may be already, or has no room for them (a catch as C's, then: no waiting inside it) */
static int catch_frame_push(Forth *f, Task *t, Word *x) {
    struct ForthCatch *c;

    if (task_sys_or_null(t) == NULL) {
        return 0;
    }
    if (t->sys->ncatch == CATCH_FRAMES_MAX) {
        return 0;
    }
    c = &t->sys->catches[t->sys->ncatch++];
    *c = (struct ForthCatch){IP, t->sp, t->rp, t->fp, t->lp, t->nmarks, t->region, place_now(f),
                             {(Cell)x, (Cell)f->w_catch_end}};
    IP = c->code;
    return 1;
}

/* (catch-end) ( -- 0 ): xt done, the catch's 0 - only as the end of a catch's code */
PRIM(p_catch_end) {
    Task *t = f->t;
    struct ForthCatch *c = t->sys != NULL && t->sys->ncatch > 0 ? &t->sys->catches[t->sys->ncatch - 1] : NULL;

    if (c == NULL || IP != &c->code[2]) {
        forth_error(f, "(catch-end): only the end of a catch's code");
    }
    t->sys->ncatch--;
    IP = c->ip;
    PUSH(0);
}

/* (threaded) an error inside one of the task's catches: the stacks put back as they were, -1 (or what was thrown)
 * given, and the task going on after it at depth (its turn's), jb where an error unwinds to again; 0 if it is in
 * none */
static int catch_frame_caught(Forth *f, Task *t, int depth, jmp_buf *jb) {
    struct ForthCatch *c;

    if (t->sys == NULL || t->sys->ncatch == 0) {
        return 0;
    }
    c = &t->sys->catches[--t->sys->ncatch];
    f->t = t;
    f->catch = jb;
    forth_regions_unwind(f, t, c->region);
    t->sp = c->sp;
    t->rp = c->rp;
    t->fp = c->fp;
    t->lp = c->lp;
    t->closure_lp = 0;
    t->nmarks = c->nmarks;
    f->depth = f->task_depth = depth;
    place_back(f, &c->place);
    IP = c->ip;
    f->yielded = 0;
    PUSH(f->thrown);
    return 1;
}

/* catch ( xt -- 0 | -1 ): run xt; if it fails, put the stacks back as they were and give -1
 * (the message is in `error-message`). A task may wait inside it (`await`, `wait`): with a native stack its frames
 * wait with it, its depths kept from where its turn began (forth_native_bases); threaded, its frame is the task's
 * (a ForthCatch) rather than C's, run by the turn's own interpreter. */
PRIM(p_catch) {
    Word *x = xt_arg(f, POP(), "catch");
    jmp_buf jb, *prev = f->catch;
    Task *t = f->t;
    int waitable = t != f->main && f->depth == f->task_depth, base, silent_base;
    int sp = t->sp, rp = t->rp, fp = t->fp, lp = t->lp;
    int nmarks = t->nmarks, region = t->region;
    int depth, task_depth, silent;
    Cell *ip = IP;
    CatchPlace place = place_now(f);

    if (waitable && f->native == NULL) {
        if (catch_frame_push(f, t, x)) {
            return;   /* (threaded: the turn's interpreter runs xt, then (catch-end)) */
        }
        waitable = 0;   /* (no frame of the task's: C's, which a wait can't come back into) */
    }
    forth_native_bases(f, &base, &silent_base);
    depth = f->depth - base;
    task_depth = f->task_depth - base;
    silent = f->silent - silent_base;
    if (setjmp(jb) != 0) {
        forth_native_bases(f, &base, &silent_base);   /* (after a wait, its turn may begin at other depths) */
        forth_regions_unwind(f, t, region);   /* (the scratch regions opened inside it: ended) */
        f->catch = prev;
        f->t = t;
        t->sp = sp;
        t->rp = rp;
        t->fp = fp;
        t->lp = lp;
        t->closure_lp = 0;
        t->nmarks = nmarks;   /* (a `{` the error left open) */
        f->depth = base + depth;
        f->task_depth = base + task_depth;
        f->silent = silent_base + silent;
        place_back(f, &place);
        IP = ip;
        PUSH(f->thrown);
        return;
    }
    f->catch = &jb;
    f->silent++;
    if (waitable) {   /* (a native stack: xt's depth a task's, that may wait) */
        f->task_depth++;
    }
    forth_execute(f, x);
    if (waitable) {
        f->task_depth--;
    }
    f->silent--;
    f->catch = prev;
    PUSH(0);
}
PRIM(p_version) { PUSH(CLANKFORTH_VERSION); PUSH(strlen(CLANKFORTH_VERSION)); }

const char *forth_version(void) { return CLANKFORTH_VERSION; }

PRIM(p_error_message) {   /* (the running task's last: another's error since, between its catch and this, isn't it) */
    const char *e = f->t != NULL && f->t->sys != NULL ? f->t->sys->error : f->error;

    PUSH(e);
    PUSH(strlen(e));
}
/* a task-variable's word: the running task's cell of it (its number in its body) */
static void dotaskvar(Forth *f, Word *w) {
    Cell i = w->body[0];

    if (i < 0 || i >= TASK_VARS) {
        forth_error(f, "task-variable %ld: none such (0 to %d)", (long)i, TASK_VARS - 1);
    }
    PUSH(&task_sys(f, f->t)->vars[i]);
}
/* (task-var) ( i -- addr ) the running task's cell of task-variable i (task-variable's words) */
PRIM(p_task_var) {
    Cell i = POP();

    if (i < 0 || i >= TASK_VARS) {
        forth_error(f, "task-variable %ld: none such (0 to %d)", (long)i, TASK_VARS - 1);
    }
    PUSH(&task_sys(f, f->t)->vars[i]);
}
/* (task-var-new) ( -- i ) the number of the task-variable `create` just made (latest): one of that name in its
 * vocabulary made before - defined again, a file reloaded - has its number again */
PRIM(p_task_var_new) {
    Cell i;

    for (i = 0; i < f->task_vars_made; i++) {
        if (f->task_var_vocab[i] == f->latest->vocab && strcmp(f->task_var_name[i], f->latest->name) == 0) {
            PUSH(i);
            return;
        }
    }
    if (f->task_vars_made >= TASK_VARS) {
        forth_error(f, "task-variable: all %d made", TASK_VARS);
    }
    f->task_var_vocab[i] = f->latest->vocab;
    snprintf(f->task_var_name[i], sizeof(f->task_var_name[i]), "%s", f->latest->name);
    PUSH(f->task_vars_made++);
}
/* n>str ( n -- addr len ) a whole number's digits in decimal - in the task's own buffer, which its next n>str reuses */
PRIM(p_n_to_str) {
    Cell n = POP();
    char *end = task_sys(f, f->t)->num + NUM_TEXT_BYTES, *p = end;
    UCell u = n < 0 ? (UCell)0 - (UCell)n : (UCell)n;

    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (n < 0) {
        *--p = '-';
    }
    PUSH(p);
    PUSH(end - p);
}
PRIM(p_text_buf) { PUSH(task_sys(f, f->t)->text); }      /* ($) ( -- addr ) the task's own $ string */
PRIM(p_text_len) { PUSH(&task_sys(f, f->t)->text_n); }   /* ($len) ( -- addr ) its length */
/* atomically ( xt -- ) xt run with no other handler of the node's between its words: a short look at, or change
 * to, what the node's actors share (a cache, a counter read and written). Run from C - as map-each's code is - so
 * no slice ends inside it, and nothing in it may wait. */
PRIM(p_atomically) { forth_execute(f, xt_arg(f, POP(), "atomically")); }
/* throw ( x -- ) 0: nothing; else an error, and `catch` gives x (the system's own errors give -1). A string object
 * is the error's message; anything else - a number, a tuple saying what went wrong - is said as a number. */
PRIM(p_throw) {
    Cell x = POP();
    size_t len;
    const char *text;

    if (x == 0) {
        return;
    }
    text = forth_object_string(f, x, &len);
    f->throwing = x;
    if (text != NULL) {
        forth_error(f, "%.*s", (int)(len < INT_MAX ? len : INT_MAX), text);
    }
    forth_error(f, "thrown: %ld", (long)x);
}

/* ---- random numbers: xoshiro256** (Blackman and Vigna), each Forth its own; seeded from the clock as it is made,
 * or by seed-random (the same seed, the same numbers: a replay, a test) ---- */

#define RANDOM_FLOAT_SHIFT 11   /* a float's 53 bits of mantissa: the top bits of a random number, ... */
#define RANDOM_FLOAT_SCALE (1.0 / 9007199254740992.0)   /* ... times 2^-53: 0 up to (not) 1 */

static uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t random_next(Forth *f) {
    uint64_t *s = f->random, result = rotl64(s[1] * 5, 7) * 9, t = s[1] << 17;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

static void random_seed(Forth *f, uint64_t seed) {   /* (splitmix64 spreads one number over the four words) */
    int i;

    for (i = 0; i < 4; i++) {
        uint64_t z = (seed += 0x9E3779B97F4A7C15ull);

        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        f->random[i] = z ^ (z >> 31);
    }
}

PRIM(p_random) {   /* random ( n -- u ) 0 .. n-1, each as likely (n at least 1) */
    Cell n = POP();
    uint64_t limit, x;

    if (n < 1) {
        forth_error(f, "random: a count of at least 1, not %ld", (long)n);
    }
    limit = UINT64_MAX - UINT64_MAX % (uint64_t)n;   /* (past it, a number would make the low ones likelier) */
    do {
        x = random_next(f);
    } while (x >= limit);
    PUSH((Cell)(x % (uint64_t)n));
}
PRIM(p_random_cell) { PUSH((Cell)random_next(f)); }   /* ( -- x ) any cell */
PRIM(p_frandom) { FPUSH((Float)(random_next(f) >> RANDOM_FLOAT_SHIFT) * RANDOM_FLOAT_SCALE); }   /* ( F: -- r ) */
PRIM(p_seed_random) { random_seed(f, (uint64_t)POP()); }   /* ( x -- ) */

/* ---- tasks ---- */

Task *forth_spawn(Forth *f, Word *w) {
    Task *t = calloc(1, sizeof(Task)), **end;

    if (t == NULL) {
        forth_error(f, "start-task: out of memory");
    }
    t->boot[0] = (Cell)w;
    t->boot[1] = (Cell)f->w_halt;
    t->ip = t->boot;
    t->state = TASK_READY;
    t->id = ++f->next_task_id;
    snprintf(t->name, sizeof(t->name), "%s", w->len > 0 ? w->name : ":noname");
    for (end = &f->tasks; *end != NULL; end = &(*end)->next) {
    }
    *end = t;
    return t;
}

/* a task's turn: FORTH_TURN_DONE, _WAITING (it yielded) or _FAILED (the error said; the task done) */
static int turn(Forth *f, Task *t) {
    jmp_buf jb, *prev = f->catch;
    Task *saved = f->t;
    int depth = f->depth, task_depth = f->task_depth, views = f->nviews, nsrc = f->nsrc, r;
    int turn_nsrc = f->turn_nsrc, turn_views = f->turn_views;
    Cell budget = f->budget, turn_compiling = f->turn_compiling;
    long turn_slice = f->turn_slice, unsliced = f->unsliced, slice = f->next_slice;
    Word *defining = f->defining, *turn_defining = f->turn_defining;

    f->turn_nsrc = nsrc;   /* (where this turn begins: what its catches put back is reckoned from here) */
    f->turn_views = views;
    f->turn_defining = defining;
    f->turn_compiling = f->compiling;
    if (setjmp(jb) != 0) {
        if (!catch_frame_caught(f, t, depth + 1, &jb)) {   /* (threaded, inside a catch of its own: it goes on) */
            back_to_view(f, views);
            while (f->nsrc > nsrc) {   /* (text the task was evaluating: not the interpreter's to go on with) */
                pop_source(f);
            }
            if (f->defining != defining) {   /* (a definition it began, unfinished) */
                drop_definition(f);
            }
            forth_regions_unwind(f, t, 0);
            if (t->sys != NULL) {
                t->sys->ncatch = 0;
            }
            t->state = TASK_DONE;
            r = FORTH_TURN_FAILED;
            goto over;
        }
    } else {
        f->catch = &jb;
        if (f->depth < NESTING_MAX) {
            f->nesting[f->depth] = (Nesting){saved, saved->ip, saved->rp};
        }
        f->t = t;
        t->running = 1;
        f->yielded = 0;
        f->next_slice = 0;
        f->turn_slice = slice;
        if (t->sys != NULL) {
            t->sys->gave_way = 0;
        }
        f->unsliced = 0;
        f->budget = slice > 0 ? slice : TASK_BUDGET;
        f->depth++;
        f->task_depth = f->depth;
    }
    if (!forth_native_turn(f, t)) {   /* (a coroutine on its own stack; or threaded, here) */
        run(f);
    }
    if (!f->yielded) {
        t->state = TASK_DONE;
    }
    r = t->state == TASK_DONE ? FORTH_TURN_DONE : FORTH_TURN_WAITING;
over:
    t->running = 0;
    f->turn_nsrc = turn_nsrc;
    f->turn_views = turn_views;
    f->turn_defining = turn_defining;
    f->turn_compiling = turn_compiling;
    f->catch = prev;
    f->t = saved;
    f->depth = depth;
    f->task_depth = task_depth;
    f->yielded = 0;
    f->budget = budget;
    f->turn_slice = turn_slice;
    f->unsliced = unsliced;
    return r;
}

static void task_turn(Forth *f, Task *t) {
    if (turn(f, t) == FORTH_TURN_FAILED) {
        forth_printf(f, "task %d (%s) stopped by the error\n", t->id, t->name);
        if (f->task_failed != NULL) {
            f->task_failed(f, t);
        }
    }
}

Task *forth_task_new(Forth *f, const char *name) {
    Task *t = calloc(1, sizeof(Task));

    if (t == NULL) {
        forth_error(f, "%s: out of memory", name);
    }
    t->state = TASK_DONE;
    t->id = ++f->next_task_id;
    snprintf(t->name, sizeof(t->name), "%s", name);
    return t;
}

/* a task for another's use (an actor's handler: actors.c lends its tasks) - `spare` made as new, or with none a new
 * one: what a task has of its own (its base, its task-variables, its last error) as a new task's, the name its
 * user's. A spare's memory - its stacks, its machine stack - is kept. NULL if there is no memory for a new one. */
Task *forth_task_for(Forth *f, Task *spare, const char *name) {
    Task *t = spare != NULL ? spare : calloc(1, sizeof(Task));
    struct ForthTaskSys *sys;

    if (t == NULL) {
        return NULL;
    }
    if (spare == NULL) {
        t->state = TASK_DONE;
        t->id = ++f->next_task_id;
    } else if ((sys = t->sys) != NULL) {
        forth_job_let_go(sys->job);
        sys->job = NULL;
        sys->ncatch = 0;
        sys->base = BASE_DEFAULT;
        sys->error[0] = 0;
        memset(sys->vars, 0, (size_t)f->task_vars_made * sizeof(Cell));
        sys->text_n = 0;
        sys->wake_ms = 0;
        sys->gave_way = 0;
    }
    snprintf(t->name, sizeof(t->name), "%s", name);
    return t;
}

void forth_task_start(Forth *f, Task *t, Word *w) {
    forth_regions_unwind(f, t, 0);   /* (a task started over: what it was in the middle of let go) */
    task_reset(t);
    forth_native_task_reset(f, t);
    t->boot[0] = (Cell)w;
    t->boot[1] = (Cell)f->w_halt;
    t->ip = t->boot;
    t->state = TASK_READY;
}

int forth_task_turn(Forth *f, Task *t) {
    if (t->state == TASK_WAITING && f->frame >= t->wake) {
        t->state = TASK_READY;
    }
    if (t->state == TASK_DONE) {
        return FORTH_TURN_DONE;
    }
    return t->running ? FORTH_TURN_WAITING : turn(f, t);   /* (running further out: it goes on there, not twice) */
}

int forth_task_ready(Forth *f, const Task *t) {
    if (t->sys != NULL && t->sys->job != NULL && forth_job_pending(t->sys->job)) {   /* (its job not done yet) */
        return 0;
    }
    if (t->sys != NULL && t->sys->wake_ms != 0 && monotonic_ms() < t->sys->wake_ms) {   /* (its sleep not over) */
        return 0;
    }
    return t->state == TASK_READY || (t->state == TASK_WAITING && f->frame >= t->wake);
}

void forth_task_free(Forth *f, Task *t) {
    forth_regions_unwind(f, t, 0);
    forth_native_task_free(f, t);
    if (t->sys != NULL) {
        forth_job_let_go(t->sys->job);   /* (a job it waited for: let go - the worker frees it as it ends) */
    }
    free(t->sys);
    free(t);
}

/* each task that can go on given a turn (those done let go) */
static void tasks_turns(Forth *f) {
    Task **pp = &f->tasks;

    while (*pp != NULL) {
        Task *t = *pp;

        if (t->state == TASK_WAITING && f->frame >= t->wake) {
            t->state = TASK_READY;
        }
        if (t->running) {   /* (the scheduler run from inside a task's turn: that one is left as it is) */
            pp = &t->next;
            continue;
        }
        if (t->state == TASK_READY && !t->held) {
            task_turn(f, t);
        }
        if (t->state == TASK_DONE) {
            *pp = t->next;
            forth_native_task_free(f, t);
            if (t->sys != NULL) {
        forth_job_let_go(t->sys->job);   /* (a job it waited for: let go - the worker frees it as it ends) */
    }
    free(t->sys);
            free(t);
        } else {
            pp = &t->next;
        }
    }
}

/* a frame of the tasks: a host whose loop counts frames calls it each one (the `frames` vocabulary's words - wait,
 * frame - count them) */
void forth_run_tasks(Forth *f) {
    if (f->tasks_paused) {
        return;
    }
    f->frame++;
    tasks_turns(f);
}
/* ... and where nothing counts frames (forth_round): the tasks that can go on, a turn each. One waiting frames -
 * its `wait` said where a host's frames ran, or by a turn of `resume`'s - would wait for ever: stopped, and said. */
void forth_tasks_go_on(Forth *f) {
    Task *t;

    if (f->tasks_paused) {
        return;
    }
    for (t = f->tasks; t != NULL; t = t->next) {
        if (forth_task_waits_frames(t) && !t->held && !t->running) {
            forth_printf(f, "\nerror: wait: " FORTH_NO_FRAMES "\ntask %d (%s) stopped by the error\n", t->id, t->name);
            f->errors++;
            forth_regions_unwind(f, t, 0);
            t->state = TASK_DONE;   /* (let go with the rest that are done, below) */
        }
    }
    tasks_turns(f);
}
/* a task can go on now (0: none - *wake then the soonest a sleeping one's time is up, monotonic ms, if sooner than
 * it was) */
int forth_tasks_ready(Forth *f, long long *wake) {
    const Task *t;

    for (t = f->tasks_paused ? NULL : f->tasks; t != NULL; t = t->next) {
        if (t->held || t->running || t->state == TASK_DONE) {
            continue;
        }
        if (forth_task_ready(f, t)) {
            return 1;
        }
        if (t->sys != NULL && t->sys->wake_ms != 0 && t->sys->wake_ms < *wake) {
            *wake = t->sys->wake_ms;
        }
    }
    return 0;
}

/* the frame n frames from now (one at least); a count past the last frame there can be is the last */
Cell forth_frame_after(const Forth *f, Cell n) {
    if (n < 1) {
        n = 1;
    }
    return n > INTPTR_MAX - f->frame ? INTPTR_MAX : f->frame + n;
}

static void yield_check(Forth *f, const char *who) {
    if (f->t == f->main || f->depth != f->task_depth) {
        forth_error(f, "%s: only a task can wait (and not from inside a C callback)", who);
    }
}
PRIM(p_yield) {
    yield_check(f, "yield");
    f->yielded = 1;
    forth_native_suspend(f);
}
PRIM(p_wait) {   /* ( frames -- ) */
    Cell n = POP();

    yield_check(f, "wait");
    if (f->frameless) {   /* (it would never go on: said, not left hanging) */
        forth_error(f, "wait: " FORTH_NO_FRAMES);
    }
    if (f->t->state != TASK_DONE) {   /* (stopped meanwhile, by a task it resumed: it stays stopped) */
        f->t->state = TASK_WAITING;
        f->t->wake = forth_frame_after(f, n);
    }
    f->yielded = 1;
    forth_native_suspend(f);
}
/* a token for another task: not a closure (it lasts only while its word runs, in this task) */
static Word *task_xt(Forth *f, Cell x, const char *word) {
    Word *w = xt_arg(f, x, word);

    if (w->code == doclosure) {
        forth_error(f, "%s: a quotation using its word's locals can't be another task's (it lasts as its word runs)", word);
    }
    return w;
}
PRIM(p_spawn) { PUSH(forth_spawn(f, task_xt(f, POP(), "start-task"))->id); }   /* start-task ( xt -- id ) */
PRIM(p_tasks_paused) { f->tasks_paused = POP() != 0; }   /* ( flag -- ) every task stands still (their waits too) */
PRIM(p_spawn_held) {   /* start-held-task ( xt -- id ) a task only `resume` runs */
    Task *t = forth_spawn(f, task_xt(f, POP(), "start-held-task"));

    t->held = 1;
    PUSH(t->id);
}
static Task *task_by_id(Forth *f, Cell id) {
    Task *t;

    for (t = f->tasks; t != NULL && t->id != id; t = t->next) {
    }
    return t;
}
PRIM(p_resume) {   /* ( id -- flag ) give task `id` its turn now (it may be waiting); is it still alive? */
    Task *t = task_by_id(f, POP());

    if (t != NULL && !t->running && t->state != TASK_DONE) {   /* (not one whose turn is under way: this one's, or
                                                                  * one further out that resumed it) */
        if (t->state == TASK_WAITING && f->frame >= t->wake) {
            t->state = TASK_READY;
        }
        if (t->state == TASK_READY) {
            task_turn(f, t);
        }
    }
    PUSH(t != NULL && t->state != TASK_DONE ? -1 : 0);
    if (f->t != f->main && f->t->state == TASK_DONE) {   /* (the one it resumed stopped this one: it stops now) */
        f->yielded = 1;
        forth_native_suspend(f);
    }
}
PRIM(p_restart) {   /* ( xt -- ) the running task starts over in xt: stacks emptied, it goes on at once */
    Word *to = task_xt(f, POP(), "restart");

    yield_check(f, "restart");
    f->t->boot[0] = (Cell)to;
    f->t->boot[1] = (Cell)f->w_halt;
    forth_regions_unwind(f, f->t, 0);
    task_reset(f->t);
    IP = f->t->boot;
    forth_native_restart(f);   /* (a coroutine: its native frames dropped too) */
}
PRIM(p_kill) {   /* stop-task ( id -- ) */
    Cell id = POP();
    Task *t;

    if (f->t != f->main && f->t->id == id) {   /* (stopping itself: as a wait, not from inside catch) */
        yield_check(f, "stop-task");
    }
    for (t = f->tasks; t != NULL; t = t->next) {
        if (t->id == id) {
            t->state = TASK_DONE;
            forth_regions_unwind(f, t, 0);   /* (what it was in the middle of: let go) */
            if (t == f->t) {   /* killing itself: stop now */
                f->yielded = 1;
                forth_native_suspend(f);   /* (a coroutine: never to go on) */
            }
        }
    }
}
PRIM(p_tick_tasks) {   /* ( frames -- ) run the scheduler (from the interpreter: tests, the console) */
    Cell n = POP();

    if (f->t != f->main) {
        forth_error(f, "tick-tasks: only from the interpreter");
    }
    while (n-- > 0) {
        forth_run_tasks(f);
    }
}
PRIM(p_me) { PUSH(f->t == f->main ? 0 : f->t->id); }
/* round ( -- ms ) what can run, runs - the tasks' turns, the actors' messages and handlers - and how long the loop
 * may wait for what comes (0: something can still run): a loop of the program's own is `begin round net-poll drop
 * again`, as serve's is. From the interpreter. */
PRIM(p_round) {
    if (f->t != f->main) {
        forth_error(f, "round: only from the interpreter (it runs the tasks and the actors itself)");
    }
    PUSH(forth_round(f));
}
PRIM(p_rounds) { PUSH((Cell)f->rounds); }   /* rounds ( -- n ) the rounds run so far: a loop asleep runs none */
PRIM(p_frame) { PUSH(f->frame); }
PRIM(p_tasks) {
    Task *t;

    for (t = f->tasks; t != NULL; t = t->next) {
        forth_printf(f, "%3d %-20s %s\n", t->id, t->name,
                     t->state == TASK_WAITING ? "waiting" : t->state == TASK_DONE ? "done" : "ready");
    }
}

/* ---- forth.c's, for the system's other files (forth_internal.h says what each is) ---- */

/* Frames are a host's: one whose loop is a frame at a time (a game's) calls forth_run_tasks, and forth_actors_frame,
 * each one, and what counts them is the `frames` vocabulary - a script that times things so says USING: frames ;
 * The system's own loops (serve, a node's, forth_round) count none: what runs, runs; what waits, waits by the clock. */
void forth_frames_words(Forth *f, const ForthPrim *prims, size_t n) {
    Vocab *saved = f->m.current, *v = forth_vocab(f, "frames");

    forth_set_current(f, v);
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, n);
    forth_set_current(f, saved);
}

void forth_define_prims(Forth *f, const ForthPrim *prims, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        Word *w = forth_prim(f, prims[i].name, prims[i].code);

        if (prims[i].effect != NULL) {
            forth_set_effect(f, w, prims[i].effect);
        }
    }
}
Word *forth_prim_word(Forth *f, const char *name, Code code, int flags, int operands) {
    return prim_word(f, name, code, flags, operands);
}
Word *forth_new_word(Forth *f, const char *name, size_t len, Code code) { return new_word(f, name, len, code); }
void forth_comma(Forth *f, Cell x) { comma(f, x); }
Word *forth_xt_arg(Forth *f, Cell x, const char *word) { return xt_arg(f, x, word); }
void *forth_addr_n(Forth *f, Cell a, size_t size) { return addr_n(f, a, size); }
int forth_is_word(const Forth *f, Cell x) { return is_word(f, x); }
Word *forth_in_vocab(Vocab *v, const char *name, size_t len) { return in_vocab(v, name, len); }
Source *forth_source(Forth *f) { return source(f); }
const char *forth_parse_until(Forth *f, char delim, size_t *len) { return parse_until(f, delim, len); }
const char *forth_parse_required(Forth *f, size_t *len, const char *who) { return parse_name(f, len, who); }
Word *forth_parse_find(Forth *f, const char *who) { return parse_find(f, who); }
int forth_parse_int(Forth *f, const char *s, size_t n, Cell *out) { return parse_int(f, s, n, out); }
int forth_parse_float(const char *s, size_t n, Float *out) { return parse_float(s, n, out); }
const char *forth_format_number(Forth *f, Cell x, int is_unsigned) { return format_number(f, x, is_unsigned); }
int forth_is_variable(const Word *w) { return w->code == dovar; }
long forth_task_variable(const Word *w, size_t *cells_at) {
    *cells_at = offsetof(struct ForthTaskSys, vars);
    return w->code == dotaskvar && w->body[0] >= 0 && w->body[0] < TASK_VARS ? (long)w->body[0] : -1;
}

int forth_code_of(const Word *w) {
    return is_colon(w) ? CODE_COLON : w->code == dovar || w->code == dotaskvar ? CODE_VARIABLE :   /* (an address) */
           w->code == docon ? CODE_CONSTANT :
           w->code == dofcon ? CODE_FCONSTANT : w->code == dodoes ? CODE_DOES : w->code == dodefer ? CODE_DEFER :
           w->code == doenum_name ? CODE_ENUM_NAME : w->code == dostrings ? CODE_STRINGS : CODE_OTHER;
}

/* ---- setting up ---- */

static void define_core(Forth *f) {
    /* each with its stack effect: what callers' code is checked with (NULL: not a fixed one) */
    static const ForthPrim frame_prims[] = {
        {"wait", p_wait, "( frames -- )"}, {"frame", p_frame, "( -- n )"}, {"tick-tasks", p_tick_tasks, "( frames -- )"},
        {"tasks-paused!", p_tasks_paused, "( flag -- )"},
    };
    static const ForthPrim prims[] = {
        {"dup", p_dup, "( x -- x x )"}, {"drop", p_drop, "( x -- )"}, {"swap", p_swap, "( a b -- b a )"},
        {"over", p_over, "( a b -- a b a )"}, {"rot", p_rot, "( a b c -- b c a )"},
        {"-rot", p_mrot, "( a b c -- c a b )"}, {"nip", p_nip, "( a b -- b )"}, {"tuck", p_tuck, "( a b -- b a b )"},
        {"2dup", p_2dup, "( a b -- a b a b )"}, {"2drop", p_2drop, "( a b -- )"},
        {"2swap", p_2swap, "( a b c d -- c d a b )"}, {"2over", p_2over, "( a b c d -- a b c d a b )"},
        {"?dup", p_qdup, "( x -- 0 | x x )"}, {"pick", p_pick, "( n -- x )"}, {"depth", p_depth, "( -- n )"},
        {">r", p_tor, "( x -- )"}, {"r>", p_rfrom, "( -- x )"}, {"r@", p_rfetch, "( -- x )"},
        {"rdrop", p_rdrop, "( -- )"}, {"+", p_add, "( a b -- c )"}, {"-", p_sub, "( a b -- c )"},
        {"*", p_mul, "( a b -- c )"}, {"/", p_div, "( a b -- c )"}, {"mod", p_mod, "( a b -- c )"},
        {"/mod", p_divmod, "( a b -- r q )"}, {"*/", p_muldiv, "( a b c -- d )"},
        {"u/", p_udiv, "( u1 u2 -- q )"}, {"umod", p_umod, "( u1 u2 -- r )"}, {"u/mod", p_udivmod, "( u1 u2 -- r q )"},
        {"um*", p_umstar, "( u1 u2 -- lo hi )"}, {"m*", p_mstar, "( n1 n2 -- lo hi )"},
        {"um/mod", p_umdivmod, "( lo hi u -- r q )"}, {"negate", p_negate, "( x -- y )"},
        {"abs", p_abs, "( x -- y )"}, {"min", p_min, "( a b -- c )"}, {"max", p_max, "( a b -- c )"},
        {"1+", p_1add, "( x -- y )"}, {"1-", p_1sub, "( x -- y )"}, {"and", p_and, "( a b -- c )"},
        {"or", p_or, "( a b -- c )"}, {"xor", p_xor, "( a b -- c )"}, {"invert", p_invert, "( x -- y )"},
        {"lshift", p_lshift, "( x n -- y )"}, {"rshift", p_rshift, "( x n -- y )"},
        {"arshift", p_arshift, "( x n -- y )"}, {"=", p_eq, "( a b -- flag )"}, {"<>", p_ne, "( a b -- flag )"},
        {"<", p_lt, "( a b -- flag )"}, {">", p_gt, "( a b -- flag )"}, {"<=", p_le, "( a b -- flag )"},
        {">=", p_ge, "( a b -- flag )"}, {"u<", p_ult, "( a b -- flag )"}, {"u>", p_ugt, "( a b -- flag )"},
        {"0=", p_0eq, "( x -- flag )"}, {"0<>", p_0ne, "( x -- flag )"}, {"0<", p_0lt, "( x -- flag )"},
        {"0>", p_0gt, "( x -- flag )"}, {"@", p_fetch, "( addr -- x )"}, {"!", p_store, "( x addr -- )"},
        {"+!", p_pstore, "( n addr -- )"}, {"c@", p_cfetch, "( addr -- c )"}, {"c!", p_cstore, "( c addr -- )"},
        {"w@", p_wfetch, "( addr -- x )"}, {"sw@", p_swfetch, "( addr -- x )"}, {"w!", p_wstore, "( x addr -- )"},
        {"l@", p_lfetch, "( addr -- x )"}, {"sl@", p_slfetch, "( addr -- x )"}, {"l!", p_lstore, "( x addr -- )"},
        {"move", p_move, "( src dst n -- )"}, {"fill", p_fill, "( addr n c -- )"}, {"cells", p_cells, "( n -- n' )"},
        {"floats", p_floats, "( n -- n' )"}, {"here", p_here, "( -- addr )"}, {"allot", p_allot, "( n -- )"},
        {",", p_comma, "( x -- )"}, {"c,", p_ccomma, "( c -- )"}, {"f,", p_fcomma, "( F: x -- )"},
        {"align", p_align, "( -- )"}, {"compare", p_compare, "( a1 n1 a2 n2 -- n )"},
        {"unused", p_unused, "( -- n )"}, {"f+", p_fadd, "( F: a b -- F: c )"}, {"f-", p_fsub, "( F: a b -- F: c )"},
        {"f*", p_fmul, "( F: a b -- F: c )"}, {"f/", p_fdiv, "( F: a b -- F: c )"},
        {"fmin", p_fmin, "( F: a b -- F: c )"}, {"fmax", p_fmax, "( F: a b -- F: c )"},
        {"fatan2", p_fatan2, "( F: y x -- F: a )"}, {"f**", p_fpow, "( F: a b -- F: c )"},
        {"fnegate", p_fnegate, "( F: x -- F: y )"}, {"fabs", p_fabs, "( F: x -- F: y )"},
        {"fsqrt", p_fsqrt, "( F: x -- F: y )"}, {"fsin", p_fsin, "( F: x -- F: y )"},
        {"fcos", p_fcos, "( F: x -- F: y )"}, {"ftan", p_ftan, "( F: x -- F: y )"},
        {"fasin", p_fasin, "( F: x -- F: y )"}, {"facos", p_facos, "( F: x -- F: y )"},
        {"fexp", p_fexp, "( F: x -- F: y )"}, {"fln", p_fln, "( F: x -- F: y )"},
        {"ffloor", p_ffloor, "( F: x -- F: y )"}, {"fround", p_fround, "( F: x -- F: y )"},
        {"f<", p_flt, "( -- flag ) ( F: a b -- )"}, {"f>", p_fgt, "( -- flag ) ( F: a b -- )"},
        {"f<=", p_fle, "( -- flag ) ( F: a b -- )"}, {"f>=", p_fge, "( -- flag ) ( F: a b -- )"},
        {"f=", p_feq, "( -- flag ) ( F: a b -- )"}, {"f0=", p_f0eq, "( -- flag ) ( F: x -- )"},
        {"f0<", p_f0lt, "( -- flag ) ( F: x -- )"}, {"s>f", p_stof, "( n -- ) ( F: -- x )"},
        {"3s>f", p_3stof, "( x y z -- ) ( F: -- x y z )"}, {"3f>s", p_3ftos, "( -- x y z ) ( F: x y z -- )"},
        {"f>s", p_ftos, "( -- n ) ( F: x -- )"}, {"fdup", p_fdup, "( F: x -- F: x x )"},
        {"fdrop", p_fdrop, "( F: x -- )"}, {"fswap", p_fswap, "( F: a b -- F: b a )"},
        {"fover", p_fover, "( F: a b -- F: a b a )"}, {"frot", p_frot, "( F: a b c -- F: b c a )"},
        {"fdepth", p_fdepth, "( -- n )"}, {"f@", p_ffetch, "( addr -- ) ( F: -- x )"},
        {"f!", p_fstore, "( addr -- ) ( F: x -- )"}, {"sf@", p_sffetch, "( addr -- ) ( F: -- x )"},
        {"sf!", p_sfstore, "( addr -- ) ( F: x -- )"}, {".", p_dot, "( n -- )"}, {"u.", p_udot, "( u -- )"},
        {"f.", p_fdot, "( F: x -- )"}, {".s", p_dots, "( -- )"}, {"emit", p_emit, "( c -- )"},
        {"type", p_type, "( addr len -- )"}, {"cr", p_cr, "( -- )"}, {"space", p_space, "( -- )"},
        {"spaces", p_spaces, "( n -- )"}, {"base", p_base, "( -- addr )"}, {"state", p_state, "( -- addr )"},
        {":", p_colon, "( \"name\" -- )"}, {":noname", p_noname, "( -- xt )"}, {"immediate", p_immediate, "( -- )"},
        {"]", p_rbracket, "( -- )"}, {"inside-definition", p_inside_definition, "( -- )"},
        {"'", p_tick, "( \"name\" -- xt )"}, {"create", p_create, "( \"name\" -- )"},
        {">body", p_tobody, "( xt -- addr )"}, {"latest", p_latest, "( -- xt )"},
        {"find", p_find, "( addr len -- xt|0 )"}, {"parse-name", p_parse_name, "( \"name\" -- addr len )"},
        {"parse-line", p_parse_line, "( -- addr len )"}, {"char", p_char, "( \"c\" -- c )"},
        {"include", p_include, NULL}, {"included", p_included, NULL}, {"evaluate", p_evaluate, NULL},
        {"skip-file", p_skip_file, "( -- )"}, {"source-dir", p_source_dir, "( -- s len )"},
        {"execute", p_execute, NULL}, {"jump", p_jump, NULL},
        {"defer", p_defer, "( \"name\" -- )"}, {"(abort\")", p_abort_q, "( flag addr len -- )"},
        {"words", p_words, "( -- )"}, {"backtrace", p_backtrace, "( -- addr len )"},
        {"see", p_see, "( \"name\" -- )"}, {"see-xt", p_see_xt, "( xt -- )"},
        {"with-output", p_with_output, "( xt -- s )"}, {"enum:", p_enum, "( -- )"}, {"strings:", p_strings, "( -- )"},
        {"vocabs", p_vocabs, "( -- )"}, {"vocab-words", p_vocab_words, "( \"name\" -- )"}, {"IN:", p_in, "( -- )"},
        {"USING:", p_using, "( -- )"}, {"reload", p_reload, "( \"name\" -- )"},
        {"reloading?", p_reloading_q, "( -- flag )"}, {"USE:", p_use, "( -- )"}, {"<PRIVATE", p_private_begin, "( -- )"},
        {"PRIVATE>", p_private_end, "( -- )"}, {"bye", p_bye, "( -- )"}, {"catch", p_catch, NULL},
        {"error-message", p_error_message, "( -- addr len )"}, {"(task-var)", p_task_var, "( i -- addr )"},
        {"(task-var-new)", p_task_var_new, "( -- i )"}, {"atomically", p_atomically, "( xt -- )"},
        {"n>str", p_n_to_str, "( n -- addr len )"}, {"($)", p_text_buf, "( -- addr )"}, {"($len)", p_text_len, "( -- addr )"},
        {"clankforth-version", p_version, "( -- addr len )"}, {"throw", p_throw, "( x -- )"},
        {"random", p_random, "( n -- u )"}, {"random-cell", p_random_cell, "( -- x )"},
        {"frandom", p_frandom, "( -- ) ( F: -- F: r )"}, {"seed-random", p_seed_random, "( x -- )"}, {"i", p_i, "( -- n )"}, {"j", p_j, "( -- n )"},
        {"leave", p_leave, NULL}, {"unloop", p_unloop, NULL}, {"yield", p_yield, "( -- )"},
        {"start-task", p_spawn, "( xt -- id )"}, {"start-held-task", p_spawn_held, "( xt -- id )"},
        {"resume", p_resume, "( id -- flag )"}, {"restart", p_restart, NULL}, {"stop-task", p_kill, "( id -- )"},
        {"me", p_me, "( -- id )"}, {"round", p_round, "( -- ms )"}, {"rounds", p_rounds, "( -- n )"},
        {".tasks", p_tasks, "( -- )"}, {"warnings", p_warnings, "( -- n )"}, {"strict!", p_strict_store, "( flag -- )"}, {"strict?", p_strict_q, "( -- flag )"},
    };
    static const struct {
        const char *name;
        Code code;
    } immediates[] = {
        {";", p_semicolon}, {"[", p_lbracket}, {"[']", p_brackettick}, {"literal", p_literal},
        {"fliteral", p_fliteral}, {"postpone", p_postpone}, {"recurse", p_recurse},
        {"does>", p_docompile}, {"s\"", p_squote}, {"s\\\"", p_sbackslash}, {"\"", p_quote}, {".\"", p_dotquote}, {".(", p_dotparen},
        {"(", p_paren}, {"\\", p_backslash}, {"[char]", p_bracketchar}, {"[:", p_quote_begin}, {"{:", p_locals_begin},
        {";]", p_quote_end},
    };
    size_t i;

    /* the words the compiler lays down (names in parentheses: not for direct use) */
    f->w_lit = prim_word(f, "(lit)", p_lit, WORD_COMPILED_ONLY, 0);
    f->w_flit = prim_word(f, "(flit)", p_flit, WORD_COMPILED_ONLY, 0);
    f->w_litstring = prim_word(f, "(litstring)", p_litstring, WORD_COMPILED_ONLY, 0xFF);
    f->w_exit = prim_word(f, "exit", p_exit, 0, 0);
    f->w_locals = prim_word(f, "(locals)", p_locals, WORD_COMPILED_ONLY, 1);
    f->w_unlocals = prim_word(f, "(unlocals)", p_unlocals, WORD_COMPILED_ONLY, 0);
    f->w_local_fetch = prim_word(f, "(local@)", p_local_fetch, WORD_COMPILED_ONLY, 1);
    f->w_local_store = prim_word(f, "(local!)", p_local_store, WORD_COMPILED_ONLY, 1);
    f->w_flocal_fetch = prim_word(f, "(flocal@)", p_flocal_fetch, WORD_COMPILED_ONLY, 1);
    f->w_flocal_store = prim_word(f, "(flocal!)", p_flocal_store, WORD_COMPILED_ONLY, 1);
    f->w_halt = prim_word(f, "(halt)", p_halt, 0, 0);
    f->w_catch_end = prim_word(f, "(catch-end)", p_catch_end, 0, 0);
    f->w_qframe = prim_word(f, "(qframe)", p_qframe, WORD_COMPILED_ONLY, 1);
    f->w_unqframe = prim_word(f, "(unqframe)", p_unqframe, WORD_COMPILED_ONLY, 0);
    f->w_nop1 = prim_word(f, "(nop1)", p_nop1, WORD_COMPILED_ONLY, 1);
    f->w_outer_fetch = prim_word(f, "(outer@)", p_outer_fetch, WORD_COMPILED_ONLY, 2);
    f->w_outer_store = prim_word(f, "(outer!)", p_outer_store, WORD_COMPILED_ONLY, 2);
    f->w_fouter_fetch = prim_word(f, "(fouter@)", p_fouter_fetch, WORD_COMPILED_ONLY, 2);
    f->w_fouter_store = prim_word(f, "(fouter!)", p_fouter_store, WORD_COMPILED_ONLY, 2);
    f->w_closure = prim_word(f, "(closure)", p_closure, WORD_COMPILED_ONLY, 2);
    f->w_closure_call = prim_word(f, "(closure-call)", p_closure_call, WORD_COMPILED_ONLY, 1);
    f->w_does = prim_word(f, "(does>)", p_does, WORD_COMPILED_ONLY, 0);
    f->w_branch = prim_word(f, "branch", p_branch, WORD_COMPILED_ONLY, 1);
    f->w_0branch = prim_word(f, "0branch", p_0branch, WORD_COMPILED_ONLY, 1);
    f->w_do = prim_word(f, "(do)", p_do, WORD_COMPILED_ONLY, 1);
    f->w_qdo = prim_word(f, "(?do)", p_qdo, WORD_COMPILED_ONLY, 1);
    f->w_loop = prim_word(f, "(loop)", p_loop, WORD_COMPILED_ONLY, 1);
    f->w_ploop = prim_word(f, "(+loop)", p_ploop, WORD_COMPILED_ONLY, 1);
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_frames_words(f, frame_prims, sizeof(frame_prims) / sizeof(frame_prims[0]));
    forth_objects_words(f);   /* (objects.c: lists, tuples, strings) */
    forth_effects_words(f);   /* (effects.c: the stack checker's) */
    forth_native_words(f);    /* (native.c: the native backend's) */
    forth_crypto_words(f);    /* (crypto.c: SHA-256, HMAC, PBKDF2, secure random bytes) */
    forth_compress_words(f);  /* (compress.c: DEFLATE both ways, gzip) */
    for (i = 0; i < sizeof(immediates) / sizeof(immediates[0]); i++) {
        prim_word(f, immediates[i].name, immediates[i].code, WORD_IMMEDIATE, 0);
    }
    f->w_comma = forth_find(f, ",", 1);
    f->w_type = forth_find(f, "type", 4);
    f->w_jump = forth_find(f, "jump", 4);
    f->w_leave = forth_find(f, "leave", 5);
    f->w_unloop = forth_find(f, "unloop", 6);
    f->w_execute = forth_find(f, "execute", 7);
    f->w_catch = forth_find(f, "catch", 5);
    f->w_restart = forth_find(f, "restart", 7);
    f->w_pick = forth_find(f, "pick", 4);
    f->w_tor = forth_find(f, ">r", 2);
    f->w_rfrom = forth_find(f, "r>", 2);
    f->w_rdrop = forth_find(f, "rdrop", 5);
    f->w_fetch = forth_find(f, "@", 1);
    f->w_cfetch = forth_find(f, "c@", 2);
    f->w_cstore = forth_find(f, "c!", 2);
    f->w_task_var = forth_find(f, "(task-var)", 10);
    f->w_store = forth_find(f, "!", 1);
    f->w_dup = forth_find(f, "dup", 3);
    f->w_qdup = forth_find(f, "?dup", 4);
    f->w_swap = forth_find(f, "swap", 4);
    f->w_over = forth_find(f, "over", 4);
    f->w_restart->effect.kind = EFFECT_ENDS;
    f->w_restart->effect_tried = 1;
    f->w_execute->effect.kind = f->w_catch->effect.kind = f->w_jump->effect.kind = EFFECT_HIGHER;
    f->w_execute->effect_tried = f->w_catch->effect_tried = f->w_jump->effect_tried = 1;
    forth_constant(f, "cell", (Cell)sizeof(Cell));
#ifndef NDEBUG
    forth_constant(f, "memory-checked?", -1);   /* (a debug build: lent memory's ends checked - forth_guard) */
#else
    forth_constant(f, "memory-checked?", 0);
#endif
}

Forth *forth_new(size_t dict_bytes) {
    Forth *f = calloc(1, sizeof(Forth));

    if (f == NULL) {
        return NULL;
    }
    dict_bytes &= ~(sizeof(Cell) - 1);   /* (whole cells: an aligned `here` never passes the end) */
    f->mem = malloc(dict_bytes);
    f->word_map = calloc(1, word_map_bytes(dict_bytes));
    f->names_beyond_core = calloc(1, NAME_BITS_BYTES);
    f->main = calloc(1, sizeof(Task));
    if (f->mem == NULL || f->word_map == NULL || f->names_beyond_core == NULL || f->main == NULL) {
        free(f->mem);
        free(f->word_map);
        free(f->names_beyond_core);
        free(f->main);
        free(f);
        return NULL;
    }
    forth_set_output(f, NULL, NULL);   /* (first: an error while the core is defined is said) */
    f->budget = -1;   /* (the interpreter has no limit; a task gets TASK_BUDGET each turn) */
    f->eval_budget = -1;   /* (nor text the host has interpreted, until it sets one: forth_set_eval_budget) */
    f->here = f->mem;
    f->end = f->mem + dict_bytes;
    f->core = forth_vocab(f, "forth");
    f->core->state = VOCAB_LOADED;
    f->m.current = f->core;
    f->m.order[f->m.norder++] = f->core;
    snprintf(f->main->name, sizeof(f->main->name), "interpreter");
    f->t = f->main;
    random_seed(f, (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)f);   /* (a run unlike the last: seed-random for alike) */
    forth_native_init(f);
    define_core(f);
    f->listener = fresh_manifest(f, 1);
    return f;
}

void forth_free(Forth *f) {
    if (sCrashForth == f) {
        sCrashForth = NULL;
    }
    forth_actors_free(f);   /* (first: those on other nodes watching its actors told they are gone) */
    forth_net_free(f);      /* (before the node: no other thread wakes its loop after) */
    forth_system_free(f);
    forth_foreign_free(f);
    forth_node_leave(f);
    forth_objects_free(f);
    free(f->guards);
    free(f->tokens);
    while (f->nshared > 0) {
        f->nshared--;
        free(f->shared[f->nshared].stored);
        free(f->shared[f->nshared].fetched);
    }
    free(f->shared);
    free(f->shared_index);
    free(f->meant_shared);
    free(f->views);
    while (f->file_names != NULL) {
        struct FileName *n = f->file_names;

        f->file_names = n->next;
        free(n);
    }
    while (f->tasks != NULL) {
        Task *t = f->tasks;

        f->tasks = t->next;
        forth_native_task_free(f, t);
        if (t->sys != NULL) {
        forth_job_let_go(t->sys->job);   /* (a job it waited for: let go - the worker frees it as it ends) */
    }
    free(t->sys);
        free(t);
    }
    forth_native_free(f);
    while (f->nsrc > 0) {
        pop_source(f);
    }
    while (f->vocabs != NULL) {
        Vocab *v = f->vocabs;

        f->vocabs = v->next;
        free(v->table);
        free(v);
    }
    if (f->main->sys != NULL) {
        forth_job_let_go(f->main->sys->job);
    }
    free(f->main->sys);   /* (the interpreter's own: its base, its last error ...) */
    free(f->main);
    free(f->word_map);
    free(f->names_beyond_core);
    free(f->mem);
    free(f);
}

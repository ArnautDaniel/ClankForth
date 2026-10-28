/* The Forth system: the inner interpreter, the outer interpreter (text -> actions), the
 * compiler and the core primitives. See forth.h for the model and docs/forth.md for the dialect. */
#include "forth.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cells a Float takes in threaded code (one on 64-bit, two on 32-bit) */
#define FCELLS ((sizeof(Float) + sizeof(Cell) - 1) / sizeof(Cell))
#define CELLS_FOR(bytes) (((bytes) + sizeof(Cell) - 1) / sizeof(Cell))
#define CELL_BITS (sizeof(Cell) * CHAR_BIT)
/* nothing is mapped below this, nor at a "negative" address: a number there is a mistake */
#define LOWEST_ADDRESS 0x10000

#define PRIM(name) static void name(Forth *f, Word *w __attribute__((unused)))
#define PUSH(x) forth_push(f, (Cell)(x))
#define POP() forth_pop(f)
#define FPUSH(x) forth_fpush(f, (Float)(x))
#define FPOP() forth_fpop(f)
#define IP (f->t->ip)

/* ---- stacks ---- */

void forth_push(Forth *f, Cell x) {
    Task *t = f->t;

    if (t->sp >= STACK_CELLS) {
        forth_error(f, "stack overflow");
    }
    t->ds[t->sp++] = x;
}

Cell forth_pop(Forth *f) {
    Task *t = f->t;

    if (t->sp <= t->sfloor) {
        forth_error(f, "stack underflow");
    }
    return t->ds[--t->sp];
}

static void rpush(Forth *f, Cell x) {
    Task *t = f->t;

    if (t->rp >= STACK_CELLS) {
        forth_error(f, "return stack overflow");
    }
    t->rs[t->rp++] = x;
}

static Cell rpop(Forth *f) {
    Task *t = f->t;

    if (t->rp <= 0) {
        forth_error(f, "return stack underflow");
    }
    return t->rs[--t->rp];
}

void forth_fpush(Forth *f, Float x) {
    Task *t = f->t;

    if (t->fp >= FSTACK_CELLS) {
        forth_error(f, "float stack overflow");
    }
    t->fs[t->fp++] = x;
}

Float forth_fpop(Forth *f) {
    Task *t = f->t;

    if (t->fp <= t->ffloor) {
        forth_error(f, "float stack underflow");
    }
    return t->fs[--t->fp];
}

/* the top items, checked (n = how many must be there) */
static Cell *top(Forth *f, int n) {
    if (f->t->sp - f->t->sfloor < n) {
        forth_error(f, "stack underflow");
    }
    return &f->t->ds[f->t->sp - 1];
}

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

#define ERROR_REPEATS_SAID 300   /* (a repeated error's count: each 300, ten seconds of frames) */
#define BACKTRACE_BYTES 1024

_Noreturn void forth_error(Forth *f, const char *fmt, ...) {
    va_list ap;

    forth_flush_warning(f);
    va_start(ap, fmt);
    vsnprintf(f->error, sizeof(f->error), fmt, ap);
    va_end(ap);
    f->quiet = 0;
    f->error_chain[0] = 0;
    if (f->silent <= 0) {
        forth_backtrace(f, f->error_chain, sizeof(f->error_chain));
    }
    if (f->silent > 0) {
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

static Word *in_vocab(Vocab *v, const char *name, size_t len);
static Word *in_vocab_hashed(Vocab *v, const char *name, size_t len, uint32_t h);
static uint32_t name_hash(const char *name, size_t len);
static void table_add(Forth *f, Vocab *v, Word *w);

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
    if (!f->m.listener && len > 0) {   /* a file's word hiding one of a vocabulary it uses: likely a mistake - say
                                          * so (not the messages': `send` finds those by message) */
        uint32_t h = name_hash(name, len);
        int k;
        for (k = f->m.norder - 1; k >= 0; k--) {
            Vocab *v = f->m.order[k];
            if (v != f->core && v != f->m.current && in_vocab_hashed(v, name, len, h) != NULL &&
                strcmp(v->name, "messages") != 0) {
                forth_warn(f, "%.*s hides %s:%.*s (%s)", (int)len, name, v->name, (int)len, name, f->m.current->name);
                break;
            }
        }
        if (f->m.current != f->core && in_vocab(f->m.current, name, len) != NULL) {   /* (its own: the later wins on) */
            forth_warn(f, "%.*s is defined again in %s", (int)len, name, f->m.current->name);
        }
    }
    align_here(f);
    w = allot(f, sizeof(Word));
    memset(w, 0, sizeof(Word));
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
        for (i = top - 1; i >= bottom && i < STACK_CELLS; i--) {
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
    static char chain[BACKTRACE_BYTES];
    size_t n = forth_backtrace(f, chain, sizeof(chain));

    PUSH(chain);
    PUSH(n);
}

static Forth *sCrashForth;

static void crash_report(const char *what) {
    char chain[BACKTRACE_BYTES];

    if (sCrashForth != NULL && forth_backtrace(sCrashForth, chain, sizeof(chain)) > 0) {
        fprintf(stderr, "%s, in the words %s\n", what, chain);
    }
}

#if defined(__SANITIZE_ADDRESS__)
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

void forth_report_crashes(Forth *f) {
    sCrashForth = f;
#if !defined(__SANITIZE_ADDRESS__)
    signal(SIGSEGV, on_crash);
    signal(SIGFPE, on_crash);
    signal(SIGILL, on_crash);
#ifdef SIGBUS
    signal(SIGBUS, on_crash);
#endif
#endif
}

/* ---- the inner interpreter ---- */

/* run threaded code from IP until it stops (IP = NULL: `halt`) or the task yields */
static void run(Forth *f) {
    while (IP != NULL && !f->yielded) {
        Word *w = (Word *)*IP++;

        if (f->budget >= 0 && --f->budget < 0) {
            forth_error(f, "task %s ran too long without yielding", f->t->name);
        }
        w->code(f, w);
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
PRIM(p_does) {   /* (does>): the latest word runs the rest of this definition; return now */
    f->latest->code = dodoes;
    f->latest->does = IP;
    IP = (Cell *)rpop(f);
}
static int live_closure(Forth *f, const Word *w);
/* an execution token from the stack: a number there (a value fetched again) is an error, not a crash. Every word is
 * in the dictionary, but for a closure (in its word's frame): one of those must be still there. */
static Word *xt_arg(Forth *f, Cell x, const char *word) {
    if (x == 0) {
        forth_error(f, "%s: null", word);
    }
    if ((UCell)x < LOWEST_ADDRESS || x < 0 || (UCell)x % sizeof(Cell) != 0) {   /* (every word is cell-aligned) */
        forth_error(f, "%s: not a word: %ld", word, (long)x);
    }
    if (((const uint8_t *)x < f->mem || (const uint8_t *)x >= f->end) && !live_closure(f, (const Word *)x)) {
        forth_error(f, "%s: not a word (a quotation using its word's locals, after that word returned?)", word);
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
    Cell len = POP();
    const char *msg = (const char *)POP();

    if (POP()) {
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
    if ((d ^ nd) < 0) {   /* crossed the line between limit - 1 and limit */
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
PRIM(p_cells) { *top(f, 1) *= (Cell)sizeof(Cell); }
PRIM(p_floats) { *top(f, 1) *= (Cell)sizeof(Float); }
PRIM(p_here) { align_here(f); PUSH(f->here); }
PRIM(p_allot) {   /* ( n -- ) a negative n gives space back, down to the dictionary's start */
    Cell n = POP();

    if (n >= 0) {
        allot(f, (size_t)n);
    } else if ((UCell)0 - (UCell)n > (size_t)(f->here - f->mem)) {
        forth_error(f, "allot: %ld is past the dictionary's start", (long)n);
    } else {
        f->here -= (size_t)((UCell)0 - (UCell)n);
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
PRIM(p_3ftos) {   /* ( F: x y z -- ) ( -- x y z ) and back, as whole numbers */
    Float z = forth_fpop(f), y = forth_fpop(f), x = forth_fpop(f);
    PUSH((Cell)x);
    PUSH((Cell)y);
    PUSH((Cell)z);
}
PRIM(p_ftos) { PUSH((Cell)FPOP()); }   /* truncates toward zero */
PRIM(p_fdup) { Float a = FPOP(); FPUSH(a); FPUSH(a); }
PRIM(p_fdrop) { (void)FPOP(); }
PRIM(p_fswap) { Float b = FPOP(), a = FPOP(); FPUSH(b); FPUSH(a); }
PRIM(p_fover) { Float b = FPOP(), a = FPOP(); FPUSH(a); FPUSH(b); FPUSH(a); }
PRIM(p_frot) { Float c = FPOP(), b = FPOP(), a = FPOP(); FPUSH(b); FPUSH(c); FPUSH(a); }
PRIM(p_fdepth) { PUSH(f->t->fp); }
PRIM(p_ffetch) { Float x; memcpy(&x, addr_n(f, POP(), sizeof(x)), sizeof(x)); FPUSH(x); }
PRIM(p_fstore) { Float x = FPOP(); memcpy(addr_n(f, POP(), sizeof(x)), &x, sizeof(x)); }
PRIM(p_sffetch) { float x; memcpy(&x, addr_n(f, POP(), 4), 4); FPUSH(x); }
PRIM(p_sfstore) { float x = (float)FPOP(); memcpy(addr_n(f, POP(), 4), &x, 4); }

/* ---- output ---- */

/* x in the current base */
static const char *format_number(Forth *f, Cell x, int is_unsigned) {
    char *p = f->numbuf + sizeof(f->numbuf) - 1;
    UCell u = is_unsigned || x >= 0 ? (UCell)x : (UCell)0 - (UCell)x;
    UCell base = f->base >= 2 && f->base <= 36 ? (UCell)f->base : 10;

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
PRIM(p_base) { PUSH(&f->base); }
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

static Word *parse_find(Forth *f, const char *who) {
    size_t len;
    const char *name = parse_name(f, &len, who);
    Word *w = forth_find(f, name, len);

    if (w == NULL) {
        forth_error(f, "%s: unknown word %.*s", who, (int)len, name);
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
    Cell base = f->base;
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
                if (!compile_inline(f, w)) {
                    comma(f, (Cell)w);
                }
            } else {
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
            forth_error(f, "unknown word %.*s", (int)n, s);
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

/* ---- the library's own Forth: lib/'s files built in (the build's forth_sources.c), found after the roots' files ---- */
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
    buf = malloc((size_t)n + 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
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
        f->here = (uint8_t *)d;
    }
}

/* a task's stacks emptied, as when it starts over: its locals' frame, the lists being gathered and
 * the floors too */
static void task_reset(Task *t) {
    t->sp = t->rp = t->fp = 0;
    t->sfloor = t->ffloor = 0;
    t->lp = 0;
    t->closure_lp = 0;
    t->nmarks = 0;
}

/* run text with a catch point; on error: back to interpreting, stacks emptied (ABORT) */
int forth_eval(Forth *f, const char *text, size_t len, const char *name) {
    jmp_buf jb, *prev = f->catch;
    int level = f->nsrc, depth = f->depth, views = f->nviews;
    Cell *ip = IP;
    Task *t = f->t;

    enter_view(f, &f->listener);   /* text from the prompt sees every loaded vocabulary */
    if (setjmp(jb) != 0) {
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
        drop_definition(f);
        task_reset(t);
        return -1;
    }
    f->catch = &jb;
    evaluate(f, text, len, name, NULL);
    f->catch = prev;
    f->listener = f->m;   /* (USING: and IN: at the prompt last) */
    leave_view(f);
    return 0;
}

int forth_include(Forth *f, const char *path) {
    jmp_buf jb, *prev = f->catch;
    int level = f->nsrc, depth = f->depth, views = f->nviews;

    if (setjmp(jb) != 0) {
        back_to_view(f, views);
        f->catch = prev;
        while (f->nsrc > level) {
            pop_source(f);
        }
        f->depth = depth;
        drop_definition(f);
        task_reset(f->t);
        return -1;
    }
    f->catch = &jb;
    include_file(f, path, strlen(path));
    f->catch = prev;
    return 0;
}

int forth_call(Forth *f, Word *w) {
    jmp_buf jb, *prev = f->catch;
    int depth = f->depth, views = f->nviews;
    Cell *ip = IP;
    Task *t = f->t;
    Word *defining = f->defining;

    if (setjmp(jb) != 0) {
        back_to_view(f, views);
        f->catch = prev;
        f->depth = depth;
        f->t = t;
        IP = ip;
        f->yielded = 0;
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
static void check_definition(Forth *f, Word *w);
PRIM(p_semicolon) {
    if (f->locals.frame_size != NULL) {
        comma(f, (Cell)f->w_unlocals);
    }
    memset(&f->locals, 0, sizeof(f->locals));
    f->nquoted = 0;
    comma(f, (Cell)f->w_exit);
    if (f->defining != NULL) {
        check_definition(f, f->defining);
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
static void dodefer(Forth *f, Word *w) {
    Word *x = (Word *)w->body[0];

    if (x == NULL) {
        forth_error(f, "%s: deferred, and nothing set to it", w->name);
    }
    x->code(f, x);
}
PRIM(p_defer) {
    size_t len;
    const char *name = parse_name(f, &len, "defer");

    new_word(f, name, len, dodefer);
    comma(f, (Cell)forth_find(f, "noop", 4));
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
        values[n++] = next++;
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
    static const char *ss[MAX];
    static size_t lens[MAX];
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
PRIM(p_tobody) { Word *x = (Word *)POP(); PUSH(x->body); }
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
PRIM(p_squote) {   /* s" text" ( -- addr len ) */
    size_t len;
    const char *s = parse_until(f, '"', &len);

    if (f->compiling) {
        compile_string(f, s, len);
    } else {
        char *buf = f->sbuf[f->nsbuf++ % 16];

        if (len >= sizeof(f->sbuf[0])) {
            len = sizeof(f->sbuf[0]) - 1;
        }
        memcpy(buf, s, len);
        buf[len] = 0;
        PUSH(buf);
        PUSH(len);
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
static int parse_effect(const char *s, size_t n, ForthEffect *e);
PRIM(p_paren) {   /* ( comment ) - at a definition's start, its stack comment: `;` checks its code against it */
    size_t len, k;
    const char *s = parse_until(f, ')', &len);
    ForthEffect e;

    if (f->compiling && f->defining != NULL && f->here == (uint8_t *)f->defining->body && f->nlocals_saved == 0 &&
        parse_effect(s, len, &e)) {
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
        parse_effect(f->declared_text, strlen(f->declared_text), &f->declared);
    }
}
PRIM(p_backslash) { size_t len; (void)parse_until(f, '\n', &len); source(f)->line++; }
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

/* the file for vocabulary a.b: <root>/a/b.fs */
static int vocab_file(Forth *f, const char *name, size_t len, char *path, size_t n) {
    int r;
    size_t i;

    for (r = 0; r < f->nroots; r++) {
        FILE *fp;
        char *text;
        size_t tlen;
        int k = snprintf(path, n, "%s/", f->roots[r]);

        for (i = 0; i < len && (size_t)k + 4 < n; i++) {
            path[k++] = name[i] == '.' ? '/' : name[i];
        }
        snprintf(path + k, n - (size_t)k, ".fs");
        if (f->reader != NULL && (text = f->reader(f->reader_ctx, path, &tlen)) != NULL) {
            free(text);
            return 1;
        }
        if ((fp = fopen(path, "rb")) != NULL) {
            fclose(fp);
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
    Vocab *v = vocab_find(f, name, len);
    char path[1024], buf[VOCAB_NAME_MAX + 1];
    jmp_buf jb, *prev = f->catch;

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
    if (x->code == docol) return "colon";
    if (x->code == dovar) return "variable";
    if (x->code == docon) return "constant";
    if (x->code == dofcon) return "fconstant";
    if (x->code == dodoes) return "does>";
    if (x->code == dodefer) return "defer";
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
        const char *name, *code;   /* its code: words, and Q for each quotation called (in order: 0 1 2, or 0 0) */
        int quotations;
    } inlined[] = {
        {"dip", ">r Q0 r>", 1}, {"2dip", ">r >r Q0 r> r>", 1}, {"keep", "dup >r Q0 r>", 1},
        {"2keep", "2dup >r >r Q0 r> r>", 1}, {"bi", "dup >r Q0 r> Q1", 2}, {"tri", "dup >r Q0 r@ Q1 r> Q2", 3},
        {"bi@", ">r Q0 r> Q0", 1}, {"when", "0branch @ Q0", 1}, {"unless", "0= 0branch @ Q0", 1},
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
            } else {
                Word *x = (Word *)*q++;

                ok = x->len == len && memcmp(x->name, c, len) == 0;
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

/* show a word: a colon definition is decompiled */
static void see_code(Forth *f, Cell *p, Cell *end, int newline) {
    Word *qs[3];   /* the quotations shown last, a combinator compiled inline after them may call */
    int nq = 0;

    while (p < end) {
        Word *x = (Word *)*p++;

        if (x == f->w_lit) {
            Word *lw = as_word(f, *p);

            if (lw != NULL) {
                forth_printf(f, "['] %s ", lw->name);
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
                   ((Word *)(p + 1))->code == docol && ((Word *)(p + 1))->len == 0) {
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
PRIM(p_see) {
    Word *x = parse_find(f, "see"), *next = word_after(f, x);
    Cell *end = next != NULL ? (Cell *)next : (Cell *)f->here;

    if (x->code == docol) {
        forth_printf(f, ": %s %s", x->name, x->flags & WORD_IMMEDIATE ? "( immediate ) " : "");
        see_code(f, x->body, end, 1);
    } else if (x->code == dodoes) {
        forth_printf(f, "%s (made by a defining word); does> ", x->name);
        see_code(f, x->does, (Cell *)x, 1);
    } else {
        forth_printf(f, "%s is a %s\n", x->name, word_kind(x));
    }
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
    q->code = docol;
    q->vocab = f->m.current;
    PUSH(slot);
    PUSH(q);
    f->compiling = -1;
    if (f->nlocals_saved == 4) {
        forth_error(f, "[: nested too deep");
    }
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
    Word *q = (Word *)POP();
    Cell *slot = (Cell *)POP();
    struct ForthLocals *l = &f->locals;
    int i, closure = l->captures;

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

static void lay(Forth *f, const char *name) { comma(f, (Cell)forth_find(f, name, strlen(name))); }

static int compile_inline(Forth *f, Word *w) {
    int need = w == f->w_tri ? 3 : w == f->w_bi ? 2 : 1, first, k;
    Cell *end, *patch;

    if (w == NULL || f->nquoted < need ||
        !(w == f->w_dip || w == f->w_2dip || w == f->w_keep || w == f->w_2keep || w == f->w_bi || w == f->w_tri ||
          w == f->w_bi_at || w == f->w_when || w == f->w_unless)) {
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
    f->here = (uint8_t *)f->quoted[first].at;
    if (w == f->w_dip) {   /* x [: q ;] dip */
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
    Word *x = xt_arg(f, POP(), "with-scratch");

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
    Word *x = xt_arg(f, POP(), w->name);
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
    w = new_word(f, buf, strlen(buf), code);
    comma(f, slot);
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
    const char *name = parse_name(f, &len, "TUPLE:");
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
        const char *s = parse_word(f, &m);

        if (m == 0) {
            forth_error(f, "TUPLE: %s without ;", c->name);
        }
        if (m == 1 && s[0] == ';') {
            break;
        }
        if (m == 1 && s[0] == '\\') {   /* comments, as enum: has them */
            (void)parse_until(f, '\n', &m);
            source(f)->line++;
            continue;
        }
        if (m == 1 && s[0] == '(') {
            (void)parse_until(f, ')', &m);
            continue;
        }
        if (m == 1 && s[0] == '<' && c->nslots == 0 && c->parent == &t->object) {   /* < parent: its slots first */
            Word *pw = parse_find(f, "TUPLE: <");

            if (pw->code != docon) {
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
    made = new_word(f, buf, strlen(buf), doconstructor);
    comma(f, (Cell)c);
    {
        char full[512];

        snprintf(full, sizeof(full), "(%s -- obj )%s%s%s", effect, fin > 0 ? " ( F:" : "", feffect, fin > 0 ? " -- )" : "");
        forth_set_effect(f, made, full);
    }
    (void)din;
    snprintf(buf, sizeof(buf), "%s?", c->name);
    made = new_word(f, buf, strlen(buf), dopredicate);
    comma(f, (Cell)c);
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
    const char *name = parse_name(f, &len, "GENERIC:"), *s;
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
    made = new_word(f, name, len, dogeneric);
    comma(f, (Cell)g);
    {   /* its stack comment, if one follows: its effect, and its methods' */
        Source *src = source(f);
        size_t pos = src->pos;

        s = parse_word(f, &m);
        if (m == 1 && s[0] == '(') {
            s = parse_until(f, ')', &m);
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
    Word *cw = parse_find(f, "M:"), *gw = parse_find(f, "M:"), *def;
    TupleClass *c;
    Generic *g;
    Method *m;
    char name[2 * WORD_NAME_MAX + 4], why[200];

    if (cw->code != docon) {
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
            forth_printf(f, " %s: %s", sn->name, format_number(f, t[i + 1], 0));
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
    const char *s = string_arg(f, a, n, ">string", &len);

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
    const char *s1 = string_arg(f, a1, n1, "s+", &l1), *s2 = string_arg(f, a2, n2, "s+", &l2);
    char *p = string_alloc(f, l1 + l2);

    memcpy(p, s1, l1);
    memcpy(p + l1, s2, l2);
    PUSH(p);
    PUSH(l1 + l2);
}
PRIM(p_n_to_s) {   /* n>s ( n -- a n ) its digits in the base, a new string */
    const char *d = format_number(f, POP(), 0);

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
    const char *s = string_arg(f, a, n, "string>number", &len);
    int ok = len > 0 && parse_int(f, s, len, &x);

    PUSH(ok ? x : 0);
    PUSH(ok ? -1 : 0);
}
PRIM(p_string_to_float) {   /* string>float ( a n -- flag ) ( F: -- x ) a float (1.5 2e): 0e false if not */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = string_arg(f, a, n, "string>float", &len);
    Float x = 0;
    int ok = len > 0 && parse_float(s, len, &x);

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
    const char *s1 = string_arg(f, a1, n1, "s-find", &l1), *s2 = string_arg(f, a2, n2, "s-find", &l2);

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
    PUSH(a + i);
    PUSH(k);
}
PRIM(p_s_trim) {   /* s-trim ( a n -- a' n' ) without the spaces at its ends: no copy */
    Cell n = POP(), a = POP();
    size_t len;
    const char *s = string_arg(f, a, n, "s-trim", &len);

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
    const char *s1 = string_arg(f, a1, n1, word, &l1), *s2 = string_arg(f, a2, n2, word, &l2);

    return l2 <= l1 && memcmp(at_start ? s1 : s1 + l1 - l2, s2, l2) == 0;
}
PRIM(p_s_starts) { PUSH(ends_with(f, 1, "s-starts?") ? -1 : 0); }
PRIM(p_s_ends) { PUSH(ends_with(f, 0, "s-ends?") ? -1 : 0); }
static void cased(Forth *f, int upper, const char *word) {   /* s-upper s-lower ( a n -- a' n ) a new string */
    Cell n = POP(), a = POP();
    size_t len, i;
    const char *s = string_arg(f, a, n, word, &len);
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
    const char *s1 = string_arg(f, a1, n1, "s-split", &l1), *s2 = string_arg(f, a2, n2, "s-split", &l2);
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
    const char *s = string_arg(f, a, n, "s-join", &sep);
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

/* catch ( xt -- 0 | -1 ): run xt; if it fails, put the stacks back as they were and give -1
 * (the message is in `error-message`) */
PRIM(p_catch) {
    Word *x = xt_arg(f, POP(), "catch");
    jmp_buf jb, *prev = f->catch;
    Task *t = f->t;
    int sp = t->sp, rp = t->rp, fp = t->fp, lp = t->lp, depth = f->depth, nsrc = f->nsrc, silent = f->silent;
    int views = f->nviews;
    Cell *ip = IP, compiling = f->compiling;
    Word *defining = f->defining;

    if (setjmp(jb) != 0) {
        back_to_view(f, views);
        f->catch = prev;
        f->t = t;
        t->sp = sp;
        t->rp = rp;
        t->fp = fp;
        t->lp = lp;
        t->closure_lp = 0;
        f->depth = depth;
        f->silent = silent;
        while (f->nsrc > nsrc) {
            pop_source(f);
        }
        IP = ip;
        if (f->defining != defining) {   /* (a definition begun inside it, unfinished) */
            drop_definition(f);
        }
        f->compiling = compiling;
        PUSH(-1);
        return;
    }
    f->catch = &jb;
    f->silent++;
    forth_execute(f, x);
    f->silent--;
    f->catch = prev;
    PUSH(0);
}
PRIM(p_error_message) { PUSH(f->error); PUSH(strlen(f->error)); }

/* ---- tasks ---- */

Task *forth_spawn(Forth *f, Word *w) {
    Task *t = calloc(1, sizeof(Task)), **end;

    if (t == NULL) {
        forth_error(f, "spawn: out of memory");
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

#define TASK_BUDGET 20000000L

/* a task's turn: FORTH_TURN_DONE, _WAITING (it yielded) or _FAILED (the error said; the task done) */
static int turn(Forth *f, Task *t) {
    jmp_buf jb, *prev = f->catch;
    Task *saved = f->t;
    int depth = f->depth, task_depth = f->task_depth, views = f->nviews, r;
    long budget = f->budget;
    Word *defining = f->defining;

    if (setjmp(jb) != 0) {
        back_to_view(f, views);
        if (f->defining != defining) {   /* (a definition it began, unfinished) */
            drop_definition(f);
        }
        t->state = TASK_DONE;
        r = FORTH_TURN_FAILED;
    } else {
        f->catch = &jb;
        if (f->depth < NESTING_MAX) {
            f->nesting[f->depth] = (Nesting){saved, saved->ip, saved->rp};
        }
        f->t = t;
        f->yielded = 0;
        f->budget = TASK_BUDGET;
        f->depth++;
        f->task_depth = f->depth;
        run(f);
        if (!f->yielded) {
            t->state = TASK_DONE;
        }
        r = t->state == TASK_DONE ? FORTH_TURN_DONE : FORTH_TURN_WAITING;
    }
    f->catch = prev;
    f->t = saved;
    f->depth = depth;
    f->task_depth = task_depth;
    f->yielded = 0;
    f->budget = budget;
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

void forth_task_start(Forth *f, Task *t, Word *w) {
    task_reset(t);
    t->boot[0] = (Cell)w;
    t->boot[1] = (Cell)f->w_halt;
    t->ip = t->boot;
    t->state = TASK_READY;
}

int forth_task_turn(Forth *f, Task *t) {
    if (t->state == TASK_WAITING && f->frame >= t->wake) {
        t->state = TASK_READY;
    }
    return t->state == TASK_DONE ? FORTH_TURN_DONE : turn(f, t);
}

int forth_task_ready(Forth *f, const Task *t) {
    return t->state == TASK_READY || (t->state == TASK_WAITING && f->frame >= t->wake);
}

void forth_task_free(Forth *f, Task *t) {
    (void)f;
    free(t);
}

void forth_run_tasks(Forth *f) {
    Task **pp = &f->tasks;

    if (f->tasks_paused) {
        return;
    }
    f->frame++;
    while (*pp != NULL) {
        Task *t = *pp;

        if (t->state == TASK_WAITING && f->frame >= t->wake) {
            t->state = TASK_READY;
        }
        if (t->state == TASK_READY && !t->held) {
            task_turn(f, t);
        }
        if (t->state == TASK_DONE) {
            *pp = t->next;
            free(t);
        } else {
            pp = &t->next;
        }
    }
}

static void yield_check(Forth *f, const char *who) {
    if (f->t == f->main || f->depth != f->task_depth) {
        forth_error(f, "%s: only a task can wait (and not from inside a C callback)", who);
    }
}
PRIM(p_yield) {
    yield_check(f, "yield");
    f->yielded = 1;
}
PRIM(p_wait) {   /* ( frames -- ) */
    Cell n = POP();

    yield_check(f, "wait");
    f->t->state = TASK_WAITING;
    f->t->wake = f->frame + (n > 0 ? n : 1);
    f->yielded = 1;
}
/* a token for another task: not a closure (it lasts only while its word runs, in this task) */
static Word *task_xt(Forth *f, Cell x, const char *word) {
    Word *w = xt_arg(f, x, word);

    if (w->code == doclosure) {
        forth_error(f, "%s: a quotation using its word's locals can't be another task's (it lasts as its word runs)", word);
    }
    return w;
}
PRIM(p_spawn) { PUSH(forth_spawn(f, task_xt(f, POP(), "spawn"))->id); }   /* ( xt -- id ) */
PRIM(p_tasks_paused) { f->tasks_paused = POP() != 0; }   /* ( flag -- ) every task stands still (their waits too) */
PRIM(p_spawn_held) {   /* ( xt -- id ) a task only `resume` runs */
    Task *t = forth_spawn(f, task_xt(f, POP(), "spawn-held"));

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

    if (t != NULL && t != f->t && t->state != TASK_DONE) {
        if (t->state == TASK_WAITING && f->frame >= t->wake) {
            t->state = TASK_READY;
        }
        if (t->state == TASK_READY) {
            task_turn(f, t);
        }
    }
    PUSH(t != NULL && t->state != TASK_DONE ? -1 : 0);
}
PRIM(p_restart) {   /* ( xt -- ) the running task starts over in xt: stacks emptied, it goes on at once */
    Word *to = task_xt(f, POP(), "restart");

    yield_check(f, "restart");
    f->t->boot[0] = (Cell)to;
    f->t->boot[1] = (Cell)f->w_halt;
    task_reset(f->t);
    IP = f->t->boot;
}
PRIM(p_kill) {   /* ( id -- ) */
    Cell id = POP();
    Task *t;

    for (t = f->tasks; t != NULL; t = t->next) {
        if (t->id == id) {
            t->state = TASK_DONE;
            if (t == f->t) {   /* killing itself: stop now */
                f->yielded = 1;
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
PRIM(p_frame) { PUSH(f->frame); }
PRIM(p_tasks) {
    Task *t;

    for (t = f->tasks; t != NULL; t = t->next) {
        forth_printf(f, "%3d %-20s %s\n", t->id, t->name,
                     t->state == TASK_WAITING ? "waiting" : t->state == TASK_DONE ? "done" : "ready");
    }
}

/* ---- stack effects: a stack comment read as ( in -- out ), and a definition's code checked against it ---- */

void forth_declare_effect(Forth *f, const char *comment, const char *why) {
    if (parse_effect(comment, strlen(comment), &f->declared)) {
        snprintf(f->declared_text, sizeof(f->declared_text), "%s", comment);
        snprintf(f->declared_why, sizeof(f->declared_why), "%s", why);
        f->declared_given = 1;
    }
}

static void warning_text(Forth *f, char *buf, size_t n, const char *fmt, va_list ap) {
    char msg[384];

    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (f->nsrc > 0) {
        snprintf(buf, n, "warning: %s (%s:%d)\n", msg, f->src[f->nsrc - 1].name, f->src[f->nsrc - 1].line);
    } else {
        snprintf(buf, n, "warning: %s\n", msg);
    }
}

void forth_warn(Forth *f, const char *fmt, ...) {
    char line[512];
    va_list ap;

    forth_flush_warning(f);
    va_start(ap, fmt);
    warning_text(f, line, sizeof(line), fmt, ap);
    va_end(ap);
    f->warnings++;
    forth_type(f, line, strlen(line));
}

/* a definition's warning held until the next begins: `immediate` may come, and then its comment is the code it lays
 * down's, not its own */
static void hold_warning(Forth *f, const char *fmt, ...) {
    va_list ap;

    forth_flush_warning(f);
    va_start(ap, fmt);
    warning_text(f, f->held_warning, sizeof(f->held_warning), fmt, ap);
    va_end(ap);
    f->held_for = f->latest;
}

void forth_flush_warning(Forth *f) {
    if (f->held_warning[0]) {
        f->warnings++;
        forth_type(f, f->held_warning, strlen(f->held_warning));
        f->held_warning[0] = 0;
    }
}

/* one comment's counts: each stack's inputs, and its outputs - two sets where `|` gives alternatives */
typedef struct Counts {
    int in[2], out[2][2];   /* [stack: 0 data, 1 float], out[stack][alternative] */
    int alts[2];            /* alternatives each stack's outputs have (1, or 2 with a `|`) */
    int either;             /* outputs after an F: begun among the inputs: on either stack */
    int false_alt[2];       /* the alternative ending in 0 / false, -1 if none does */
    int dashes, varies, seen[2];
} Counts;

static int is_false_token(const char *s, size_t n) {
    return (n == 1 && s[0] == '0') || (n == 5 && strncasecmp(s, "false", 5) == 0);
}

/* "..", as in x1..xn (memmem's, which not every C library has: MinGW's hasn't) */
static int has_dots(const char *s, size_t n) {
    size_t i;

    for (i = 0; i + 1 < n; i++) {
        if (s[i] == '.' && s[i + 1] == '.') {
            return 1;
        }
    }
    return 0;
}

/* the tokens of one comment's text (between its parentheses) counted into c */
static void count_comment(const char *s, size_t n, Counts *c) {
    enum { DATA, FLOAT, OTHER } stack = DATA;   /* (R: the return stack's, C: the compiler's: not counted) */
    size_t i = 0;
    int out = 0, carried = 0, floats_in = 0;   /* (carried: the outputs are in an F: the inputs began, with inputs) */

    while (i < n) {
        size_t start, len;

        while (i < n && isspace((unsigned char)s[i])) {
            i++;
        }
        start = i;
        while (i < n && !isspace((unsigned char)s[i])) {
            i++;
        }
        len = i - start;
        if (len == 0) {
            break;
        }
        if (s[start] == '"') {   /* a word parsed from the input ("name", "rest of line"): not on a stack */
            if (len == 1 || s[i - 1] != '"') {
                while (i < n && s[i] != '"') {
                    i++;
                }
                i++;
            }
            continue;
        }
        if (len == 2 && s[start] == '-' && s[start + 1] == '-') {
            if (out) {
                c->varies = 1;
            }
            out = 1;
            c->dashes = 1;
            carried = stack == FLOAT && floats_in > 0;
            continue;
        }
        if (len == 2 && s[start + 1] == ':' && isalpha((unsigned char)s[start])) {   /* F: R: C: ... */
            stack = toupper((unsigned char)s[start]) == 'F' ? FLOAT : toupper((unsigned char)s[start]) == 'S' ? DATA
                                                                                                            : OTHER;
            carried = 0;
            floats_in = 0;
            continue;
        }
        if (stack == OTHER) {
            continue;
        }
        if (len == 1 && s[start] == '|') {
            if (!out || c->alts[stack] == 2) {
                c->varies = 1;
            }
            c->alts[stack] = 2;
            continue;
        }
        if (has_dots(s + start, len) || (len == 3 && memcmp(s + start, "\xe2\x80\xa6", 3) == 0)) {
            c->varies = 1;   /* (x1 .. xn: a count not fixed) */
            continue;
        }
        c->seen[stack] = 1;
        if (!out) {
            c->in[stack]++;
            floats_in += stack == FLOAT;
        } else if (carried) {
            if (c->alts[stack] == 2) {
                c->varies = 1;
            }
            c->either++;
        } else {
            int alt = c->alts[stack] - 1;

            c->out[stack][alt]++;
            c->false_alt[stack] = is_false_token(s + start, len) ? alt : c->false_alt[stack] == alt ? -1
                                                                                                     : c->false_alt[stack];
        }
    }
}

/* the effect a stack comment's text gives (several comments together: "( addr -- ) ( F: -- x )"); 0 if it isn't one */
static int parse_effect(const char *s, size_t n, ForthEffect *e) {
    Counts c;
    size_t i = 0;
    int k;

    memset(&c, 0, sizeof(c));
    c.alts[0] = c.alts[1] = 1;
    c.false_alt[0] = c.false_alt[1] = -1;
    while (i < n) {   /* each parenthesized comment; text with no parentheses is one */
        const char *open = memchr(s + i, '(', n - i), *close;

        if (open == NULL) {
            if (i == 0) {
                count_comment(s, n, &c);
            }
            break;
        }
        close = memchr(open, ')', (size_t)(s + n - open));
        if (close == NULL) {
            close = s + n;
        }
        count_comment(open + 1, (size_t)(close - open - 1), &c);
        i = (size_t)(close - s) + 1;
    }
    if (!c.dashes) {
        return 0;
    }
    memset(e, 0, sizeof(*e));
    e->in = (int8_t)c.in[0];
    e->fin = (int8_t)c.in[1];
    e->out = (int8_t)c.out[0][0];
    e->fout = (int8_t)c.out[1][0];
    e->either = (int8_t)c.either;
    e->kind = EFFECT_FIXED;
    if (c.varies || (c.alts[1] == 2 && c.out[1][0] != c.out[1][1])) {
        e->kind = EFFECT_VARIES;
    } else if (c.alts[0] == 2 && c.out[0][0] != c.out[0][1]) {
        k = c.false_alt[0];
        if (k < 0) {
            e->kind = EFFECT_VARIES;
        } else {   /* ( -- x true | false ): true with one, false with the other */
            e->kind = EFFECT_FLAGGED;
            e->out = (int8_t)c.out[0][1 - k];
            e->out_false = (int8_t)c.out[0][k];
        }
    }
    return 1;
}

int forth_set_effect(Forth *f, Word *w, const char *comment) {
    ForthEffect e;

    (void)f;
    if (!parse_effect(comment, strlen(comment), &e)) {
        return -1;
    }
    w->effect = e;
    w->effect_tried = 1;
    return 0;
}

static void format_effect(const ForthEffect *e, char *buf, size_t n) {
    int k = snprintf(buf, n, "( %d -- %d", e->in, e->out);

    if (e->fin != 0 || e->fout != 0) {
        snprintf(buf + k, n - (size_t)k, " F: %d -- %d )", e->fin, e->fout);
    } else {
        snprintf(buf + k, n - (size_t)k, " )");
    }
}

/* ---- working out the effect of threaded code: every way through it followed, the depths kept (relative to its
 * start: below 0 is what it takes), and where ways meet they must agree ---- */

#define EFFECT_WORKED_OUT 2   /* (effect_tried: its code was followed through - checked, if it has a comment) */

/* a fixed effect ( in -- out ) ( F: fin -- fout ) */
#define EFFECT_OF(in_, out_, fin_, fout_) \
    ((ForthEffect){.in = (int8_t)(in_), .out = (int8_t)(out_), .fin = (int8_t)(fin_), .fout = (int8_t)(fout_), .kind = EFFECT_FIXED})

#define FLOW_STATES 65536  /* places in one word's code its walk keeps (a longer word goes unchecked) */
#define FLOW_LOOPS 8       /* do loops open at once */
#define FLOW_NESTED 8      /* quotations within quotations worked out for a call */

typedef struct FlowState {
    Cell *ip;
    int d, fd;                  /* the depths here */
    int nloops;
    Cell *loops[FLOW_LOOPS];    /* each open loop's exit */
    int other;                  /* (a flagged word's) the place's second depths: their record's index + 1 */
    int back;                   /* come to by a jump back (a loop's next turn) */
} FlowState;

enum { FLOW_OK, FLOW_UNKNOWN, FLOW_HIGHER, FLOW_UNBALANCED };

typedef struct Flow {
    Forth *f;
    Word *self;                 /* the word being compiled (`recurse` calls it): its declared effect */
    FlowState *seen;            /* each place reached, and the depths there */
    int nseen, cap;
    int *index, slots;          /* seen by address: its index + 1, 0 for none (open addressing) */
    FlowState *todo;            /* the ways still to follow (an if's other, a loop's next turn) */
    int ntodo, todo_cap;
    int low, flow;              /* the deepest each stack is reached */
    int ends, end_d[2], end_fd; /* the depths it returns with (two for a flagged word's two exits) */
    int nested;
    int two_ways;               /* the word is declared ( -- x true | false ): its ways may meet two deep */
} Flow;

static int infer_code(Forth *f, Word *self, Cell *start, int d0, ForthEffect *out, int nested);

/* a lit's value as a word: an execution token laid down ([']) or a quotation's */
static Word *literal_word(Forth *f, Cell x) {
    Word *w = (Word *)x;
    Vocab *v;

    if ((const uint8_t *)w < f->mem || (const uint8_t *)w + sizeof(Word) > f->here || x % (Cell)sizeof(Cell) != 0) {
        return NULL;
    }
    if (w->len == 0) {   /* a quotation: headless colon code */
        return w->code == docol ? w : NULL;
    }
    for (v = f->vocabs; v != NULL && v != w->vocab; v = v->next) {
    }
    return v != NULL && in_vocab(v, w->name, w->len) == w ? w : NULL;
}

/* what calling w does; EFFECT_UNKNOWN if it can't be said */
static ForthEffect effect_of(Flow *fl, Word *w) {
    Forth *f = fl->f;
    ForthEffect e;

    memset(&e, 0, sizeof(e));
    if (w == fl->self) {
        e = f->declared;
        if (e.either > 0) {   /* (its own effect, its results' stacks not known yet) */
            e.kind = EFFECT_UNKNOWN;
        }
        return e;
    }
    if (w->effect_tried) {
        e = w->effect;
        if (e.either > 0) {
            e.kind = EFFECT_UNKNOWN;
        }
        return e;
    }
    e.kind = EFFECT_FIXED;
    if (w->code == docon || w->code == dovar) {
        e.out = 1;
    } else if (w->code == dofcon) {
        e.fout = 1;
    } else if (w->code == doenum_name || w->code == dostrings) {
        e.in = 1;
        e.out = 2;
    } else if (w->code == docol && fl->nested < FLOW_NESTED && w->len == 0) {   /* a quotation, a handler */
        w->effect_tried = 1;   /* (first: a quotation calling itself isn't worked out forever) */
        if (infer_code(f, NULL, w->body, 0, &e, fl->nested + 1) != FLOW_OK) {
            e.kind = EFFECT_UNKNOWN;
        }
        w->effect = e;
    } else if (w->code == dodoes && fl->nested < FLOW_NESTED) {   /* its body's address, then its DOES> code */
        w->effect_tried = 1;
        if (infer_code(f, NULL, w->does, 1, &e, fl->nested + 1) != FLOW_OK) {
            e.kind = EFFECT_UNKNOWN;
        }
        w->effect = e;
    } else {
        e.kind = EFFECT_UNKNOWN;
    }
    return e;
}

static int flow_go(Flow *fl, const FlowState *s) {
    if (fl->ntodo == fl->todo_cap) {
        int cap = fl->todo_cap ? fl->todo_cap * 2 : 16;
        FlowState *todo = cap <= FLOW_STATES ? realloc(fl->todo, (size_t)cap * sizeof(FlowState)) : NULL;

        if (todo == NULL) {
            return FLOW_UNKNOWN;
        }
        fl->todo = todo;
        fl->todo_cap = cap;
    }
    fl->todo[fl->ntodo++] = *s;
    return FLOW_OK;
}

/* the stacks taking e at s */
static void flow_apply(Flow *fl, FlowState *s, const ForthEffect *e, int out) {
    s->d -= e->in;
    s->fd -= e->fin;
    if (s->d < fl->low) {
        fl->low = s->d;
    }
    if (s->fd < fl->flow) {
        fl->flow = s->fd;
    }
    s->d += out;
    s->fd += e->fout;
}

/* a way out of the word: its depths kept, two at most (a flagged word's) */
static int flow_end(Flow *fl, const FlowState *s) {
    int i;

    if (s->nloops > 0) {   /* (an exit with a loop open: unloop forgotten - left to the run to say) */
        return FLOW_UNKNOWN;
    }
    for (i = 0; i < fl->ends; i++) {
        if (fl->end_d[i] == s->d && fl->end_fd == s->fd) {
            return FLOW_OK;
        }
    }
    if (fl->ends == 2 || (fl->ends == 1 && fl->end_fd != s->fd)) {
        return FLOW_UNBALANCED;
    }
    fl->end_d[fl->ends++] = s->d;
    fl->end_fd = s->fd;
    return FLOW_OK;
}

static unsigned flow_slot(const Flow *fl, const Cell *ip) {
    return (unsigned)(((uintptr_t)ip / sizeof(Cell)) * 2654435761u) & (unsigned)(fl->slots - 1);
}

static int flow_keep(Flow *fl, const FlowState *s) {
    if (fl->nseen == fl->cap) {
        int cap = fl->cap ? fl->cap * 2 : 128;
        FlowState *seen = realloc(fl->seen, (size_t)cap * sizeof(FlowState));

        if (seen == NULL) {
            return -1;
        }
        fl->seen = seen;
        fl->cap = cap;
    }
    fl->seen[fl->nseen] = *s;
    fl->seen[fl->nseen].other = 0;
    return fl->nseen++;
}

static int same_depths(const FlowState *a, const FlowState *b) {
    return a->d == b->d && a->fd == b->fd && a->nloops == b->nloops;
}

/* the place s is at seen before with other depths (its record), or NULL - seen with the same depths, or kept as seen
 * now (*fresh then); *full when there is no more room */
static FlowState *flow_seen(Flow *fl, const FlowState *s, int *full, int *fresh) {
    unsigned k;
    int i;

    *full = 0;
    *fresh = 0;
    if (fl->nseen * 2 >= fl->slots) {   /* (the index twice as big: every place put back) */
        int slots = fl->slots ? fl->slots * 2 : 256, *index;

        if (slots > 2 * FLOW_STATES || (index = calloc((size_t)slots, sizeof(int))) == NULL) {
            *full = 1;
            return NULL;
        }
        free(fl->index);
        fl->index = index;
        fl->slots = slots;
        for (i = 0; i < fl->nseen; i++) {
            for (k = flow_slot(fl, fl->seen[i].ip); fl->index[k] != 0; k = (k + 1) & (unsigned)(slots - 1)) {
            }
            fl->index[k] = i + 1;
        }
    }
    for (k = flow_slot(fl, s->ip); fl->index[k] != 0; k = (k + 1) & (unsigned)(fl->slots - 1)) {
        FlowState *t = &fl->seen[fl->index[k] - 1];

        if (t->ip == s->ip) {
            if (same_depths(t, s) || (t->other != 0 && same_depths(&fl->seen[t->other - 1], s))) {
                return NULL;
            }
            if (!fl->two_ways || t->other != 0) {
                return t;
            }
            if ((i = flow_keep(fl, s)) < 0) {   /* (a flagged word's second depths here: walked on) */
                *full = 1;
                return NULL;
            }
            fl->seen[fl->index[k] - 1].other = i + 1;
            *fresh = 1;
            return NULL;
        }
    }
    if ((i = flow_keep(fl, s)) < 0) {
        *full = 1;
        return NULL;
    }
    fl->index[k] = i + 1;
    *fresh = 1;
    return NULL;
}

/* one way through the code from s, to its end or a place already seen */
static int flow_walk(Flow *fl, FlowState s) {
    Forth *f = fl->f;
    Cell last_lit = 0;
    int have_lit = 0;

    for (;;) {
        Word *w;
        int r, full, fresh;

        if ((const uint8_t *)s.ip < f->mem || (const uint8_t *)s.ip >= f->here) {
            return FLOW_UNKNOWN;
        }
        if (flow_seen(fl, &s, &full, &fresh) != NULL) {   /* (a place seen again: the depths must be the same) */
            /* (unless it is a loop's start: turns taking or leaving an item each - n stores in an n-turn loop - are
             * more than this can count) */
            return s.back ? FLOW_UNKNOWN : FLOW_UNBALANCED;
        }
        if (full) {
            return FLOW_UNKNOWN;
        }
        if (!fresh) {
            return FLOW_OK;
        }
        s.back = 0;
        w = (Word *)*s.ip++;
        if (w == f->w_lit) {
            ForthEffect push = EFFECT_OF(0, 1, 0, 0);

            last_lit = *s.ip++;
            flow_apply(fl, &s, &push, 1);
            have_lit = 1;
            continue;
        }
        if (w == f->w_flit) {
            ForthEffect push = EFFECT_OF(0, 0, 0, 1);

            s.ip += FCELLS;
            flow_apply(fl, &s, &push, 0);
        } else if (w == f->w_litstring) {
            ForthEffect push = EFFECT_OF(0, 2, 0, 0);
            Cell n = *s.ip++;

            s.ip += CELLS_FOR((size_t)n);
            flow_apply(fl, &s, &push, 2);
        } else if (w == f->w_exit || w == f->w_does) {
            return flow_end(fl, &s);
        } else if (w == f->w_halt) {
            return FLOW_OK;
        } else if (w == f->w_branch) {
            s.back = (Cell *)*s.ip <= s.ip;
            s.ip = (Cell *)*s.ip;
            have_lit = 0;
            continue;
        } else if (w == f->w_0branch) {
            FlowState taken;
            ForthEffect pop = EFFECT_OF(1, 0, 0, 0);

            flow_apply(fl, &s, &pop, 0);
            taken = s;
            taken.ip = (Cell *)*s.ip;
            taken.back = taken.ip <= s.ip;
            s.ip++;
            if (taken.back) {   /* (a loop's next turn first: one taking or leaving an item each is found out before
                                 * the code after it is counted with one turn's) */
                FlowState after = s;

                s = taken;
                taken = after;
            }
            if ((r = flow_go(fl, &taken)) != FLOW_OK) {
                return r;
            }
        } else if (w == f->w_do || w == f->w_qdo) {
            ForthEffect pop = EFFECT_OF(2, 0, 0, 0);
            Cell *exit = (Cell *)*s.ip++;

            flow_apply(fl, &s, &pop, 0);
            if (s.nloops == FLOW_LOOPS) {
                return FLOW_UNKNOWN;
            }
            if (w == f->w_qdo) {   /* (skipped: straight to the exit) */
                FlowState skip = s;

                skip.ip = exit;
                if ((r = flow_go(fl, &skip)) != FLOW_OK) {
                    return r;
                }
            }
            s.loops[s.nloops++] = exit;
        } else if (w == f->w_loop || w == f->w_ploop) {
            FlowState back;

            if (w == f->w_ploop) {
                ForthEffect pop = EFFECT_OF(1, 0, 0, 0);

                flow_apply(fl, &s, &pop, 0);
            }
            if (s.nloops == 0) {
                return FLOW_UNKNOWN;
            }
            back = s;   /* (its next turn first, as for 0branch) */
            back.ip = (Cell *)*s.ip;
            back.back = 1;
            s.ip++;
            s.nloops--;
            if ((r = flow_go(fl, &s)) != FLOW_OK) {
                return r;
            }
            s = back;
        } else if (w == f->w_leave) {
            if (s.nloops == 0) {
                return FLOW_UNKNOWN;
            }
            s.ip = s.loops[--s.nloops];
        } else if (w == f->w_unloop) {
            if (s.nloops == 0) {
                return FLOW_UNKNOWN;
            }
            s.nloops--;
        } else if (w == f->w_locals || w == f->w_unlocals || w == f->w_qframe || w == f->w_unqframe ||
                   w == f->w_nop1) {
            s.ip += w == f->w_locals || w == f->w_qframe || w == f->w_nop1;
        } else if (w == f->w_outer_fetch || w == f->w_outer_store || w == f->w_fouter_fetch ||
                   w == f->w_fouter_store) {
            ForthEffect e = EFFECT_OF(w == f->w_outer_store, w == f->w_outer_fetch, w == f->w_fouter_store,
                                      w == f->w_fouter_fetch);

            s.ip += 2;
            flow_apply(fl, &s, &e, e.out);
        } else if (w == f->w_closure) {   /* (a literal: a quotation's token) */
            ForthEffect push = EFFECT_OF(0, 1, 0, 0);

            last_lit = s.ip[1];
            s.ip += 2;
            flow_apply(fl, &s, &push, 1);
            have_lit = 1;
            continue;
        } else if (w == f->w_closure_call) {
            ForthEffect e = effect_of(fl, (Word *)*s.ip++);

            if (e.kind != EFFECT_FIXED) {
                return e.kind == EFFECT_HIGHER ? FLOW_HIGHER : FLOW_UNKNOWN;
            }
            flow_apply(fl, &s, &e, e.out);
        } else if (w == f->w_local_fetch || w == f->w_local_store || w == f->w_flocal_fetch ||
                   w == f->w_flocal_store) {
            ForthEffect e = EFFECT_OF(w == f->w_local_store, w == f->w_local_fetch, w == f->w_flocal_store,
                                      w == f->w_flocal_fetch);

            s.ip++;
            flow_apply(fl, &s, &e, e.out);
        } else if ((w->flags & WORD_EFFECT_OF_LITERAL) && have_lit && f->literal_effect != NULL) {
            ForthEffect e = f->literal_effect(f, w, last_lit);

            if (e.kind == EFFECT_FLAGGED && (Word *)*s.ip == f->w_0branch) {   /* ( -- x true | false ) if */
                FlowState no = s;
                ForthEffect flag = EFFECT_OF(1, 0, 0, 0);

                flow_apply(fl, &no, &e, e.out_false);
                flow_apply(fl, &no, &flag, 0);
                no.ip = (Cell *)s.ip[1];
                flow_apply(fl, &s, &e, e.out);
                flow_apply(fl, &s, &flag, 0);
                s.ip += 2;
                if ((r = flow_go(fl, &no)) != FLOW_OK) {
                    return r;
                }
            } else if (e.kind != EFFECT_FIXED) {
                return FLOW_UNKNOWN;
            } else {
                flow_apply(fl, &s, &e, e.out);
            }
        } else if (w == f->w_pick && have_lit && last_lit >= 0 && last_lit < STACK_CELLS) {
            ForthEffect reach = EFFECT_OF((int8_t)(last_lit + 2), (int8_t)(last_lit + 2), 0, 0);
            ForthEffect pick = EFFECT_OF(1, 1, 0, 0);
            FlowState probe = s;

            flow_apply(fl, &probe, &reach, reach.out);   /* (how deep it reaches: n below the n) */
            flow_apply(fl, &s, &pick, 1);
        } else if ((w == f->w_execute || w == f->w_catch || w == f->w_jump || w == f->w_restart ||
                    w == f->w_with_scratch) && have_lit &&
                   literal_word(f, last_lit) != NULL) {
            /* a known word run: its effect (the token taken first); catch's flag after it */
            ForthEffect token = EFFECT_OF(1, 0, 0, 0);
            ForthEffect e = effect_of(fl, literal_word(f, last_lit));

            flow_apply(fl, &s, &token, 0);
            if (w == f->w_restart) {
                return FLOW_OK;   /* (it starts over: nothing after it runs) */
            }
            if (e.kind == EFFECT_HIGHER) {
                return FLOW_HIGHER;
            }
            if (e.kind == EFFECT_ENDS) {
                return FLOW_OK;
            }
            if (e.kind != EFFECT_FIXED) {
                return FLOW_UNKNOWN;
            }
            flow_apply(fl, &s, &e, e.out);
            if (w == f->w_catch) {
                ForthEffect flag = EFFECT_OF(0, 1, 0, 0);

                flow_apply(fl, &s, &flag, 1);
            }
            if (w == f->w_jump) {
                return flow_end(fl, &s);
            }
        } else {
            ForthEffect e = effect_of(fl, w);

            if (e.kind == EFFECT_HIGHER) {
                return FLOW_HIGHER;
            }
            if (e.kind == EFFECT_ENDS) {
                return FLOW_OK;
            }
            if (e.kind == EFFECT_FLAGGED && (Word *)*s.ip == f->w_0branch) {   /* ( -- x true | false ) if */
                FlowState no = s;
                ForthEffect flag = EFFECT_OF(1, 0, 0, 0);

                flow_apply(fl, &no, &e, e.out_false);
                flow_apply(fl, &no, &flag, 0);
                no.ip = (Cell *)s.ip[1];
                flow_apply(fl, &s, &e, e.out);
                flow_apply(fl, &s, &flag, 0);
                s.ip += 2;
                if ((r = flow_go(fl, &no)) != FLOW_OK) {
                    return r;
                }
            } else if (e.kind != EFFECT_FIXED) {
                return FLOW_UNKNOWN;
            } else {
                flow_apply(fl, &s, &e, e.out);
            }
        }
        have_lit = 0;
    }
}

/* the effect of the code at start (d0 cells already pushed: a DOES> word's body address) */
static int infer_code(Forth *f, Word *self, Cell *start, int d0, ForthEffect *out, int nested) {
    Flow flow, *fl = &flow;
    FlowState s;
    int r = FLOW_OK;

    memset(fl, 0, sizeof(*fl));
    fl->f = f;
    fl->self = self;
    fl->nested = nested;
    fl->two_ways = self != NULL && f->declared.kind == EFFECT_FLAGGED;
    memset(&s, 0, sizeof(s));
    s.ip = start;
    s.d = d0;
    if (flow_go(fl, &s) != FLOW_OK) {
        r = FLOW_UNKNOWN;
    }
    while (r == FLOW_OK && fl->ntodo > 0) {
        r = flow_walk(fl, fl->todo[--fl->ntodo]);
    }
    memset(out, 0, sizeof(*out));
    if (r == FLOW_OK) {
        out->in = (int8_t)-fl->low;
        out->fin = (int8_t)-fl->flow;
        if (fl->ends == 0) {   /* (every way ends in `restart`, an error ...) */
            out->kind = EFFECT_ENDS;
        } else {
            out->out = (int8_t)(fl->end_d[0] - fl->low);
            out->fout = (int8_t)(fl->end_fd - fl->flow);
            out->kind = EFFECT_FIXED;
            if (fl->ends == 2) {
                out->out_false = (int8_t)(fl->end_d[1] - fl->low);
                out->kind = EFFECT_FLAGGED;
            }
        }
    }
    free(fl->seen);
    free(fl->index);
    free(fl->todo);
    return r;
}

/* does the code's effect fit what its comment declares? *kept: the effect its callers see (an input it leaves alone
 * counted as an output; results on either stack where they are) */
static int effect_fits(const ForthEffect *code, const ForthEffect *decl, ForthEffect *kept) {
    int unused = decl->in - code->in, funused = decl->fin - code->fin;
    int out = code->out + unused, fout = code->fout + funused;
    int extra = out - decl->out, fextra = fout - decl->fout;

    *kept = *decl;
    if (unused < 0 || funused < 0) {   /* (it takes more than it says) */
        return 0;
    }
    if (decl->kind == EFFECT_FLAGGED) {   /* its two exits: one each (or both the same) */
        int a = out, b = code->kind == EFFECT_FLAGGED ? code->out_false + unused : out;

        return fextra == 0 && (a == decl->out || a == decl->out_false) && (b == decl->out || b == decl->out_false);
    }
    if (code->kind != EFFECT_FIXED || extra < 0 || fextra < 0 || extra + fextra != decl->either) {
        return 0;
    }
    kept->out = (int8_t)out;
    kept->fout = (int8_t)fout;
    kept->either = 0;
    return 1;
}

/* `;`: the definition's code worked out, checked against its comment; its effect kept for its callers */
static void check_definition(Forth *f, Word *w) {
    ForthEffect code, kept;
    char name[WORD_NAME_MAX + 8];
    int r;

    snprintf(name, sizeof(name), "%s", w->len > 0 || w->name[0] ? w->name : ":noname");   /* (a handler: named) */
    r = infer_code(f, w, w->body, 0, &code, 0);
    w->effect_tried = r == FLOW_OK ? EFFECT_WORKED_OUT : 1;
    if (f->declared.kind == EFFECT_UNKNOWN) {   /* no comment: what its code does, if that is known */
        w->effect = r == FLOW_OK ? code : (ForthEffect){.kind = r == FLOW_HIGHER ? EFFECT_HIGHER : EFFECT_UNKNOWN};
        return;
    }
    w->effect = f->declared;
    if (r == FLOW_HIGHER) {
        w->effect.kind = EFFECT_HIGHER;
        return;
    }
    if (f->declared.kind != EFFECT_FIXED && f->declared.kind != EFFECT_FLAGGED) {
        return;
    }
    if (f->declared.either > 0) {   /* (where its results are: as its code has them, if that is known) */
        w->effect.kind = EFFECT_UNKNOWN;
    }
    if (r == FLOW_OK && code.kind != EFFECT_ENDS && effect_fits(&code, &f->declared, &kept)) {
        w->effect = kept;
    } else if (r == FLOW_UNBALANCED) {
        hold_warning(f, "%s %s: its code leaves the stacks at other depths where its ways meet (an if's two ways, a "
                   "loop's turns)", name, f->declared_text);
    } else if (r == FLOW_OK && code.kind != EFFECT_ENDS) {
        char said[48];

        format_effect(&code, said, sizeof(said));
        hold_warning(f, "%s %s: its code is %s", name, f->declared_text, said);
    }
}

/* ( "name" -- ) the effect a word has as its callers see it: "( 2 -- 1 )", or why not */
PRIM(p_effect_dot) {
    Word *x = parse_find(f, "effect.");
    ForthEffect e;
    char buf[48];
    Flow fl;

    memset(&fl, 0, sizeof(fl));
    fl.f = f;
    e = effect_of(&fl, x);
    format_effect(&e, buf, sizeof(buf));
    switch (e.kind) {
    case EFFECT_FIXED: forth_printf(f, "%s\n", buf); break;
    case EFFECT_FLAGGED: forth_printf(f, "%s or %d with a false flag\n", buf, e.out_false); break;
    case EFFECT_VARIES: forth_printf(f, "varies\n"); break;
    case EFFECT_HIGHER: forth_printf(f, "that of the token it runs\n"); break;
    case EFFECT_ENDS: forth_printf(f, "it doesn't return\n"); break;
    default: forth_printf(f, "not known\n"); break;
    }
}
/* ( xt -- in out fin fout kind ) a word's effect, as numbers (kind: 0 unknown, 1 fixed, 2 flagged, 3 varies,
 * 4 runs a token, 5 doesn't return) */
PRIM(p_stack_effect) {
    Flow fl;
    ForthEffect e;

    memset(&fl, 0, sizeof(fl));
    fl.f = f;
    e = effect_of(&fl, xt_arg(f, POP(), "stack-effect"));
    PUSH(e.in);
    PUSH(e.out);
    PUSH(e.fin);
    PUSH(e.fout);
    PUSH(e.kind);
}
PRIM(p_warnings) { PUSH(f->warnings); }   /* ( -- n ) warnings given so far */
/* .checked ( -- ) how much of the colon definitions loaded has an effect known (checked, or worked out), and how
 * much not - by vocabulary, the most unknown first: where giving C words their effects would check more */
PRIM(p_dot_checked) {
    long kinds[6] = {0}, all = 0, worked = 0;
    Vocab *v;
    Word *x;

    for (v = f->vocabs; v != NULL; v = v->next) {
        long known = 0, unknown = 0;

        for (x = v->latest; x != NULL; x = x->link) {
            if (x->code == docol && x->len > 0 && !(x->flags & WORD_IMMEDIATE)) {
                int k = x->effect_tried ? x->effect.kind : EFFECT_UNKNOWN;

                kinds[k]++;
                all++;
                worked += x->effect_tried == EFFECT_WORKED_OUT;
                known += k == EFFECT_FIXED || k == EFFECT_FLAGGED;
                unknown += k == EFFECT_UNKNOWN;
            }
        }
        if (unknown > 0 && unknown * 4 > known) {
            forth_printf(f, "  %-24s %ld of %ld not known\n", v->name, unknown, known + unknown);
        }
    }
    forth_printf(f, "%ld colon definitions: %ld with their code followed through (checked), %ld more known by their "
                 "comment alone, %ld not known, %ld run a token, %ld vary\n", all, worked,
                 kinds[EFFECT_FIXED] + kinds[EFFECT_FLAGGED] - worked, kinds[EFFECT_UNKNOWN], kinds[EFFECT_HIGHER],
                 kinds[EFFECT_VARIES]);
}

/* ---- setting up ---- */

static void define_core(Forth *f) {
    /* each with its stack effect: what callers' code is checked with (NULL: not a fixed one) */
    static const struct {
        const char *name;
        Code code;
        const char *effect;
    } prims[] = {
        {"dup", p_dup, "( x -- x x )"}, {"drop", p_drop, "( x -- )"}, {"swap", p_swap, "( a b -- b a )"},
        {"over", p_over, "( a b -- a b a )"}, {"rot", p_rot, "( a b c -- b c a )"}, {"-rot", p_mrot, "( a b c -- c a b )"},
        {"nip", p_nip, "( a b -- b )"}, {"tuck", p_tuck, "( a b -- b a b )"}, {"2dup", p_2dup, "( a b -- a b a b )"},
        {"2drop", p_2drop, "( a b -- )"}, {"2swap", p_2swap, "( a b c d -- c d a b )"},
        {"2over", p_2over, "( a b c d -- a b c d a b )"}, {"?dup", p_qdup, "( x -- 0 | x x )"},
        {"pick", p_pick, "( n -- x )"}, {"depth", p_depth, "( -- n )"},
        {">r", p_tor, "( x -- )"}, {"r>", p_rfrom, "( -- x )"}, {"r@", p_rfetch, "( -- x )"}, {"rdrop", p_rdrop, "( -- )"},
        {"+", p_add, "( a b -- c )"}, {"-", p_sub, "( a b -- c )"}, {"*", p_mul, "( a b -- c )"}, {"/", p_div, "( a b -- c )"},
        {"mod", p_mod, "( a b -- c )"}, {"/mod", p_divmod, "( a b -- r q )"}, {"*/", p_muldiv, "( a b c -- d )"},
        {"negate", p_negate, "( x -- y )"}, {"abs", p_abs, "( x -- y )"}, {"min", p_min, "( a b -- c )"},
        {"max", p_max, "( a b -- c )"}, {"1+", p_1add, "( x -- y )"}, {"1-", p_1sub, "( x -- y )"},
        {"and", p_and, "( a b -- c )"}, {"or", p_or, "( a b -- c )"}, {"xor", p_xor, "( a b -- c )"},
        {"invert", p_invert, "( x -- y )"}, {"lshift", p_lshift, "( x n -- y )"}, {"rshift", p_rshift, "( x n -- y )"},
        {"arshift", p_arshift, "( x n -- y )"},
        {"=", p_eq, "( a b -- flag )"}, {"<>", p_ne, "( a b -- flag )"}, {"<", p_lt, "( a b -- flag )"},
        {">", p_gt, "( a b -- flag )"}, {"<=", p_le, "( a b -- flag )"}, {">=", p_ge, "( a b -- flag )"},
        {"u<", p_ult, "( a b -- flag )"}, {"u>", p_ugt, "( a b -- flag )"}, {"0=", p_0eq, "( x -- flag )"},
        {"0<>", p_0ne, "( x -- flag )"}, {"0<", p_0lt, "( x -- flag )"}, {"0>", p_0gt, "( x -- flag )"},
        {"@", p_fetch, "( addr -- x )"}, {"!", p_store, "( x addr -- )"}, {"+!", p_pstore, "( n addr -- )"},
        {"c@", p_cfetch, "( addr -- c )"}, {"c!", p_cstore, "( c addr -- )"}, {"w@", p_wfetch, "( addr -- x )"},
        {"sw@", p_swfetch, "( addr -- x )"}, {"w!", p_wstore, "( x addr -- )"}, {"l@", p_lfetch, "( addr -- x )"},
        {"sl@", p_slfetch, "( addr -- x )"}, {"l!", p_lstore, "( x addr -- )"}, {"move", p_move, "( src dst n -- )"},
        {"fill", p_fill, "( addr n c -- )"}, {"cells", p_cells, "( n -- n' )"}, {"floats", p_floats, "( n -- n' )"},
        {"here", p_here, "( -- addr )"}, {"allot", p_allot, "( n -- )"}, {",", p_comma, "( x -- )"},
        {"c,", p_ccomma, "( c -- )"}, {"f,", p_fcomma, "( F: x -- )"}, {"align", p_align, "( -- )"},
        {"compare", p_compare, "( a1 n1 a2 n2 -- n )"}, {"unused", p_unused, "( -- n )"},
        {"f+", p_fadd, "( F: a b -- F: c )"}, {"f-", p_fsub, "( F: a b -- F: c )"}, {"f*", p_fmul, "( F: a b -- F: c )"},
        {"f/", p_fdiv, "( F: a b -- F: c )"}, {"fmin", p_fmin, "( F: a b -- F: c )"}, {"fmax", p_fmax, "( F: a b -- F: c )"},
        {"fatan2", p_fatan2, "( F: y x -- F: a )"}, {"f**", p_fpow, "( F: a b -- F: c )"}, {"fnegate", p_fnegate, "( F: x -- F: y )"},
        {"fabs", p_fabs, "( F: x -- F: y )"}, {"fsqrt", p_fsqrt, "( F: x -- F: y )"}, {"fsin", p_fsin, "( F: x -- F: y )"},
        {"fcos", p_fcos, "( F: x -- F: y )"}, {"ftan", p_ftan, "( F: x -- F: y )"}, {"fasin", p_fasin, "( F: x -- F: y )"},
        {"facos", p_facos, "( F: x -- F: y )"}, {"fexp", p_fexp, "( F: x -- F: y )"}, {"fln", p_fln, "( F: x -- F: y )"},
        {"ffloor", p_ffloor, "( F: x -- F: y )"}, {"fround", p_fround, "( F: x -- F: y )"},
        {"f<", p_flt, "( -- flag ) ( F: a b -- )"}, {"f>", p_fgt, "( -- flag ) ( F: a b -- )"},
        {"f<=", p_fle, "( -- flag ) ( F: a b -- )"}, {"f>=", p_fge, "( -- flag ) ( F: a b -- )"},
        {"f=", p_feq, "( -- flag ) ( F: a b -- )"}, {"f0=", p_f0eq, "( -- flag ) ( F: x -- )"},
        {"f0<", p_f0lt, "( -- flag ) ( F: x -- )"}, {"s>f", p_stof, "( n -- ) ( F: -- x )"},
        {"3s>f", p_3stof, "( x y z -- ) ( F: -- x y z )"}, {"3f>s", p_3ftos, "( -- x y z ) ( F: x y z -- )"},
        {"f>s", p_ftos, "( -- n ) ( F: x -- )"}, {"fdup", p_fdup, "( F: x -- F: x x )"}, {"fdrop", p_fdrop, "( F: x -- )"},
        {"fswap", p_fswap, "( F: a b -- F: b a )"}, {"fover", p_fover, "( F: a b -- F: a b a )"},
        {"frot", p_frot, "( F: a b c -- F: b c a )"}, {"fdepth", p_fdepth, "( -- n )"},
        {"f@", p_ffetch, "( addr -- ) ( F: -- x )"}, {"f!", p_fstore, "( addr -- ) ( F: x -- )"},
        {"sf@", p_sffetch, "( addr -- ) ( F: -- x )"}, {"sf!", p_sfstore, "( addr -- ) ( F: x -- )"},
        {".", p_dot, "( n -- )"}, {"u.", p_udot, "( u -- )"}, {"f.", p_fdot, "( F: x -- )"}, {".s", p_dots, "( -- )"},
        {"emit", p_emit, "( c -- )"}, {"type", p_type, "( addr len -- )"}, {"cr", p_cr, "( -- )"},
        {"space", p_space, "( -- )"}, {"spaces", p_spaces, "( n -- )"}, {"base", p_base, "( -- addr )"},
        {"state", p_state, "( -- addr )"},
        {":", p_colon, "( \"name\" -- )"}, {":noname", p_noname, "( -- xt )"}, {"immediate", p_immediate, "( -- )"},
        {"]", p_rbracket, "( -- )"}, {"inside-definition", p_inside_definition, "( -- )"},
        {"'", p_tick, "( \"name\" -- xt )"}, {"create", p_create, "( \"name\" -- )"}, {">body", p_tobody, "( xt -- addr )"},
        {"latest", p_latest, "( -- xt )"}, {"find", p_find, "( addr len -- xt|0 )"},
        {"parse-name", p_parse_name, "( \"name\" -- addr len )"}, {"parse-line", p_parse_line, "( -- addr len )"},
        {"char", p_char, "( \"c\" -- c )"}, {"include", p_include, NULL}, {"included", p_included, NULL},
        {"evaluate", p_evaluate, NULL}, {"execute", p_execute, NULL}, {"jump", p_jump, NULL},
        {"defer", p_defer, "( \"name\" -- )"}, {"(abort\")", p_abort_q, "( flag addr len -- )"},
        {"list", p_list, "( -- l )"}, {"list-free", p_list_free, "( l -- )"}, {"push", p_push, "( x l -- )"},
        {"pop", p_pop, "( l -- x )"}, {"nth", p_nth, "( i l -- x )"}, {"nth!", p_nth_store, "( x i l -- )"},
        {"length", p_length, "( l -- n )"}, {"list-clear", p_list_clear, "( l -- )"}, {"{", p_brace_open, NULL},
        {"}", p_brace_close, NULL},
        {"words", p_words, "( -- )"}, {"backtrace", p_backtrace, "( -- addr len )"}, {"see", p_see, "( \"name\" -- )"},
        {"enum:", p_enum, "( -- )"}, {"strings:", p_strings, "( -- )"}, {"vocabs", p_vocabs, "( -- )"},
        {"vocab-words", p_vocab_words, "( \"name\" -- )"},
        {"IN:", p_in, "( -- )"}, {"USING:", p_using, "( -- )"}, {"USE:", p_use, "( -- )"},
        {"<PRIVATE", p_private_begin, "( -- )"}, {"PRIVATE>", p_private_end, "( -- )"}, {"bye", p_bye, "( -- )"},
        {"catch", p_catch, NULL}, {"error-message", p_error_message, "( -- addr len )"},
        {"i", p_i, "( -- n )"}, {"j", p_j, "( -- n )"}, {"leave", p_leave, NULL}, {"unloop", p_unloop, NULL},
        {"yield", p_yield, "( -- )"}, {"wait", p_wait, "( frames -- )"}, {"spawn", p_spawn, "( xt -- id )"},
        {"spawn-held", p_spawn_held, "( xt -- id )"}, {"tasks-paused!", p_tasks_paused, "( flag -- )"},
        {"resume", p_resume, "( id -- flag )"}, {"restart", p_restart, NULL}, {"kill", p_kill, "( id -- )"},
        {"me", p_me, "( -- id )"}, {"frame", p_frame, "( -- n )"}, {"tick-tasks", p_tick_tasks, "( frames -- )"},
        {".tasks", p_tasks, "( -- )"}, {"effect.", p_effect_dot, "( \"name\" -- )"},
        {"dispose", p_dispose, "( obj -- )"}, {"rescue", p_rescue, "( obj -- obj )"}, {"object?", p_object_q, "( x -- flag )"},
        {"objects", p_objects, "( -- n )"}, {".objects", p_dot_objects, "( -- )"}, {"with-scratch", p_with_scratch, NULL},
        {"TUPLE:", p_tuple, "( -- )"}, {"GENERIC:", p_generic, "( -- )"}, {"M:", p_method, "( -- )"},
        {"class-of", p_class_of, "( obj -- class )"}, {"instance?", p_instance, "( x class -- flag )"},
        {".tuple", p_dot_tuple, "( obj -- )"}, {"clone", p_clone, "( obj -- obj' )"},
        {">string", p_to_string, "( addr len -- s len )"}, {"string", p_string, "( s -- addr len )"},
        {"string?", p_string_q, "( x -- flag )"}, {"s+", p_s_plus, "( a1 n1 a2 n2 -- a n )"},
        {"n>s", p_n_to_s, "( n -- a len )"}, {"f>string", p_f_to_string, "( -- a len ) ( F: x -- )"},
        {"string>number", p_string_to_number, "( a len -- n flag )"},
        {"string>float", p_string_to_float, "( a len -- flag ) ( F: -- F: x )"},
        {"s-find", p_s_find, "( a n a2 n2 -- i )"}, {"s-slice", p_s_slice, "( a n i len -- a' n' )"},
        {"s-trim", p_s_trim, "( a n -- a' n' )"}, {"s-starts?", p_s_starts, "( a n a2 n2 -- flag )"},
        {"s-ends?", p_s_ends, "( a n a2 n2 -- flag )"}, {"s-upper", p_s_upper, "( a n -- a' n )"},
        {"s-lower", p_s_lower, "( a n -- a' n )"}, {"s-split", p_s_split, "( a n a2 n2 -- l )"},
        {"s-join", p_s_join, "( l a n -- a' n' )"},
        {"stack-effect", p_stack_effect, "( xt -- in out fin fout kind )"}, {"warnings", p_warnings, "( -- n )"}, {".checked", p_dot_checked, "( -- )"},
    };
    static const struct {
        const char *name;
        Code code;
    } immediates[] = {
        {";", p_semicolon}, {"[", p_lbracket}, {"[']", p_brackettick}, {"literal", p_literal},
        {"fliteral", p_fliteral}, {"postpone", p_postpone}, {"recurse", p_recurse},
        {"does>", p_docompile}, {"s\"", p_squote}, {".\"", p_dotquote}, {".(", p_dotparen},
        {"(", p_paren}, {"\\", p_backslash}, {"[char]", p_bracketchar}, {"[:", p_quote_begin}, {"{:", p_locals_begin},
        {";]", p_quote_end},
    };
    size_t i;

    /* the words the compiler lays down (names in parentheses: not for direct use) */
    f->w_lit = prim_word(f, "(lit)", p_lit, 0, 0);
    f->w_flit = prim_word(f, "(flit)", p_flit, 0, 0);
    f->w_litstring = prim_word(f, "(litstring)", p_litstring, 0, 0xFF);
    f->w_exit = prim_word(f, "exit", p_exit, 0, 0);
    f->w_locals = prim_word(f, "(locals)", p_locals, 0, 1);
    f->w_unlocals = prim_word(f, "(unlocals)", p_unlocals, 0, 0);
    f->w_local_fetch = prim_word(f, "(local@)", p_local_fetch, 0, 1);
    f->w_local_store = prim_word(f, "(local!)", p_local_store, 0, 1);
    f->w_flocal_fetch = prim_word(f, "(flocal@)", p_flocal_fetch, 0, 1);
    f->w_flocal_store = prim_word(f, "(flocal!)", p_flocal_store, 0, 1);
    f->w_halt = prim_word(f, "(halt)", p_halt, 0, 0);
    f->w_qframe = prim_word(f, "(qframe)", p_qframe, 0, 1);
    f->w_unqframe = prim_word(f, "(unqframe)", p_unqframe, 0, 0);
    f->w_nop1 = prim_word(f, "(nop1)", p_nop1, 0, 1);
    f->w_outer_fetch = prim_word(f, "(outer@)", p_outer_fetch, 0, 2);
    f->w_outer_store = prim_word(f, "(outer!)", p_outer_store, 0, 2);
    f->w_fouter_fetch = prim_word(f, "(fouter@)", p_fouter_fetch, 0, 2);
    f->w_fouter_store = prim_word(f, "(fouter!)", p_fouter_store, 0, 2);
    f->w_closure = prim_word(f, "(closure)", p_closure, 0, 2);
    f->w_closure_call = prim_word(f, "(closure-call)", p_closure_call, 0, 1);
    f->w_does = prim_word(f, "(does>)", p_does, 0, 0);
    f->w_branch = prim_word(f, "branch", p_branch, 0, 1);
    f->w_0branch = prim_word(f, "0branch", p_0branch, 0, 1);
    f->w_do = prim_word(f, "(do)", p_do, 0, 1);
    f->w_qdo = prim_word(f, "(?do)", p_qdo, 0, 1);
    f->w_loop = prim_word(f, "(loop)", p_loop, 0, 1);
    f->w_ploop = prim_word(f, "(+loop)", p_ploop, 0, 1);
    for (i = 0; i < sizeof(prims) / sizeof(prims[0]); i++) {
        Word *w = forth_prim(f, prims[i].name, prims[i].code);

        if (prims[i].effect != NULL) {
            forth_set_effect(f, w, prims[i].effect);
        }
    }
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
    f->w_with_scratch = forth_find(f, "with-scratch", 12);
    f->w_with_scratch->effect.kind = EFFECT_HIGHER;
    f->w_with_scratch->effect_tried = 1;
    f->w_restart->effect.kind = EFFECT_ENDS;
    f->w_restart->effect_tried = 1;
    f->w_execute->effect.kind = f->w_catch->effect.kind = f->w_jump->effect.kind = EFFECT_HIGHER;
    f->w_execute->effect_tried = f->w_catch->effect_tried = f->w_jump->effect_tried = 1;
    forth_constant(f, "cell", (Cell)sizeof(Cell));
    forth_constant(f, "object", (Cell)&tuples(f)->object);   /* (the root class: `M: object g` is every tuple's) */
    forth_vocab(f, "accessors")->state = VOCAB_LOADED;   /* (the tuples' accessors: there before any TUPLE: makes one) */
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
    f->mem = malloc(dict_bytes);
    f->main = calloc(1, sizeof(Task));
    if (f->mem == NULL || f->main == NULL) {
        free(f->mem);
        free(f->main);
        free(f);
        return NULL;
    }
    forth_set_output(f, NULL, NULL);   /* (first: an error while the core is defined is said) */
    f->budget = -1;   /* (the interpreter has no limit; a task gets TASK_BUDGET each turn) */
    f->here = f->mem;
    f->end = f->mem + dict_bytes;
    f->base = 10;
    f->core = forth_vocab(f, "forth");
    f->core->state = VOCAB_LOADED;
    f->m.current = f->core;
    f->m.order[f->m.norder++] = f->core;
    snprintf(f->main->name, sizeof(f->main->name), "interpreter");
    f->t = f->main;
    define_core(f);
    f->listener = fresh_manifest(f, 1);
    return f;
}

void forth_free(Forth *f) {
    if (sCrashForth == f) {
        sCrashForth = NULL;
    }
    forth_actors_free(f);
    objects_free_all(f);
    tuples_free(f);
    free(f->guards);
    free(f->views);
    while (f->file_names != NULL) {
        struct FileName *n = f->file_names;

        f->file_names = n->next;
        free(n);
    }
    while (f->tasks != NULL) {
        Task *t = f->tasks;

        f->tasks = t->next;
        free(t);
    }
    while (f->nsrc > 0) {
        pop_source(f);
    }
    while (f->vocabs != NULL) {
        Vocab *v = f->vocabs;

        f->vocabs = v->next;
        free(v->table);
        free(v);
    }
    free(f->main);
    free(f->mem);
    free(f);
}

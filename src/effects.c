/* The stack checker: a stack comment read as an effect, a definition's code followed through every way it can go
 * and checked against its comment at `;`, and what is known of words kept in slots. It runs as code is compiled,
 * never as it runs. See docs/forth.md, "Stack effects". */
#include "forth_internal.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stack effects: a stack comment read as ( in -- out ), and a definition's code checked against it ---- */

void forth_declare_effect(Forth *f, const char *comment, const char *why) {
    if (forth_parse_effect(comment, strlen(comment), &f->declared)) {
        snprintf(f->declared_text, sizeof(f->declared_text), "%s", comment);
        snprintf(f->declared_why, sizeof(f->declared_why), "%s", why);
        f->declared_given = 1;
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
int forth_parse_effect(const char *s, size_t n, ForthEffect *e) {
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
    if (!forth_parse_effect(comment, strlen(comment), &e)) {
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

/* what the walk knows of an item on the data stack, beyond that it is there */
enum {
    TAG_NONE,
    TAG_LIT,       /* a literal: v its value (a word's, perhaps: ['] w) */
    TAG_SLOT,      /* the address a slot's word gave: v the word */
    TAG_TOKEN,     /* fetched from a slot: v the slot's word - a token of its effect */
    TAG_FETCHED,   /* fetched from somewhere else: a token (if it is run) that isn't the caller's */
    TAG_INPUT,     /* one of the word's own inputs, untouched: v which, from the top (1: the top) */
};
typedef struct Tag {
    int kind;
    Cell v;
} Tag;

#define TAGS_KEPT 4   /* the topmost items the walk keeps tags of */
typedef struct Tags {
    Tag t[TAGS_KEPT];
    int n;
} Tags;

static void tags_push(Tags *tags, int kind, Cell v) {
    if (tags->n == TAGS_KEPT) {
        memmove(&tags->t[0], &tags->t[1], (TAGS_KEPT - 1) * sizeof(Tag));
        tags->n--;
    }
    tags->t[tags->n++] = (Tag){kind, v};
}
static void tags_pop(Tags *tags, int n) { tags->n = tags->n > n ? tags->n - n : 0; }

typedef struct FlowState {
    Cell *ip;
    int d, fd;                  /* the depths here */
    int rd;                     /* what it has put on the return stack and not taken back (`>r`) */
    int nloops;
    Cell *loops[FLOW_LOOPS];    /* each open loop's exit */
    int other;                  /* (a flagged word's) the place's second depths: their record's index + 1 */
    int back;                   /* come to by a jump back (a loop's next turn) */
    int floor;                  /* the lowest the data stack has been on the way here: what is under it is the word's
                                 * inputs as its caller gave them */
} FlowState;

#define FLOW_LOCAL_TAGS 96   /* frame cells whose locals' tags are kept (a word's locals, its closures between) */
#define FLOW_TAKEN 4         /* inputs of one word found stored in slots */

enum { FLOW_OK, FLOW_UNKNOWN, FLOW_HIGHER, FLOW_UNBALANCED };
/* a way out with something of the word's own still on the return stack, over its return address: the run would
 * return into it (a crash) */
enum { STRANDED_NOTHING, STRANDED_ITEMS, STRANDED_LOOP };

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
    int stranded, stranded_items;   /* STRANDED_*: a way out over cells of its own (how many `>r` left) */
    Tag local_tag[FLOW_LOCAL_TAGS];      /* what each local was given (stored once: a second store, and it is unknown) */
    uint8_t local_stored[FLOW_LOCAL_TAGS], local_used[FLOW_LOCAL_TAGS];
    int unsure;                 /* a local's tag was used, then the local stored again: what was told by it is not */
    /* a token stored where words of another effect are kept: the word, where (a slot's word, or one that stores its
     * input), the effect wanted */
    Word *misfit, *misfit_in;
    ForthEffect misfit_wants, misfit_has;
    struct { int input; ForthEffect effect; } taken[FLOW_TAKEN];   /* inputs of the word's own stored in slots */
    int ntaken;
    /* the code followed for one call of it, the caller's tokens known: [n] the word (a literal's value) given as its
     * input n + 1 from the top, 0 not known - so a word that runs its input is followed with what it is given */
    Cell given[TAGS_KEPT];
} Flow;

static int infer_code(Forth *f, Word *self, Cell *start, int d0, ForthEffect *out, int nested, Flow *found,
                      const Cell *given);

/* ---- tokens kept in slots: a word that is a slot's (`variable on-hit  holds: ( damage -- )`: it gives the address
 * of a cell holding words of that effect) lets the checker follow `on-hit @ execute`, and check what is stored
 * there (`['] w on-hit !`). A word that stores one of its inputs in such a slot (`: on-hit! ( xt -- ) on-hit ! ;`) is
 * found to as it is compiled, so the check reaches its callers (`['] w on-hit!`). ---- */

static void note_token(Forth *f, Word *w, int input, const ForthEffect *e) {
    int k;

    for (k = 0; k < f->ntokens; k++) {
        if (f->tokens[k].w == w && f->tokens[k].input == input) {
            f->tokens[k].effect = *e;   /* (said again: the later) */
            return;
        }
    }
    if (f->ntokens == f->tokens_cap) {
        int cap = f->tokens_cap ? f->tokens_cap * 2 : 64;
        struct TokenNote *tokens = realloc(f->tokens, (size_t)cap * sizeof(f->tokens[0]));

        if (tokens == NULL) {
            return;   /* (unchecked, then) */
        }
        f->tokens = tokens;
        f->tokens_cap = cap;
    }
    f->tokens[f->ntokens++] = (struct TokenNote){w, input, *e};
    w->flags |= WORD_OF_TOKENS;
}

/* the slot w's tokens' effect, NULL if w is no slot's word */
static const ForthEffect *held_effect(const Forth *f, const Word *w) {
    int k;

    for (k = 0; (w->flags & WORD_OF_TOKENS) && k < f->ntokens; k++) {
        if (f->tokens[k].w == w && f->tokens[k].input == 0) {
            return &f->tokens[k].effect;
        }
    }
    return NULL;
}

/* holds: ( effect ) - the word just defined gives a slot that holds words of that effect */
PRIM(p_holds) {
    size_t len;
    const char *s = forth_parse_name(f, &len);
    ForthEffect e;

    if (len != 1 || s[0] != '(') {
        forth_error(f, "holds: a stack comment expected - holds: ( a -- b )");
    }
    s = forth_parse_until(f, ')', &len);
    if (!forth_parse_effect(s, len, &e) || e.kind != EFFECT_FIXED || e.either != 0) {
        forth_error(f, "holds: ( %.*s) isn't a fixed stack effect", (int)len, s);
    }
    if (f->latest == NULL) {
        forth_error(f, "holds: no word to say it of");
    }
    note_token(f, f->latest, 0, &e);
}


/* a lit's value as a word: an execution token laid down ([']) or a quotation's */
static Word *literal_word(Forth *f, Cell x) {
    Word *w = (Word *)x;
    Vocab *v;

    if ((const uint8_t *)w < f->mem || (const uint8_t *)w + sizeof(Word) > f->here || x % (Cell)sizeof(Cell) != 0) {
        return NULL;
    }
    if (w->len == 0) {   /* a quotation: headless colon code */
        return forth_code_of(w) == CODE_COLON ? w : NULL;
    }
    for (v = f->vocabs; v != NULL && v != w->vocab; v = v->next) {
    }
    return v != NULL && forth_in_vocab(v, w->name, w->len) == w ? w : NULL;
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
    if (forth_code_of(w) == CODE_CONSTANT || forth_code_of(w) == CODE_VARIABLE) {
        e.out = 1;
    } else if (forth_code_of(w) == CODE_FCONSTANT) {
        e.fout = 1;
    } else if (forth_code_of(w) == CODE_ENUM_NAME || forth_code_of(w) == CODE_STRINGS) {
        e.in = 1;
        e.out = 2;
    } else if (forth_code_of(w) == CODE_COLON && fl->nested < FLOW_NESTED && w->len == 0) {   /* a quotation, a handler */
        w->effect_tried = 1;   /* (first: a quotation calling itself isn't worked out forever) */
        if (infer_code(f, NULL, w->body, 0, &e, fl->nested + 1, NULL, NULL) != FLOW_OK) {
            e.kind = EFFECT_UNKNOWN;
        }
        w->effect = e;
    } else if (forth_code_of(w) == CODE_DOES && fl->nested < FLOW_NESTED) {   /* its body's address, then its DOES> code */
        w->effect_tried = 1;
        if (infer_code(f, NULL, w->does, 1, &e, fl->nested + 1, NULL, NULL) != FLOW_OK) {
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
    if (s->d < s->floor) {
        s->floor = s->d;
    }
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

    if (s->nloops > 0 || s->rd > 0) {   /* (an exit with a loop open - unloop forgotten - or a `>r` not taken back) */
        fl->stranded = s->nloops > 0 ? STRANDED_LOOP : STRANDED_ITEMS;
        fl->stranded_items = s->rd;
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
    return a->d == b->d && a->fd == b->fd && a->rd == b->rd && a->nloops == b->nloops;
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
                /* (met by a way that reached lower on the data stack: followed on again from here, for what is
                 * known as an input of the word's is what no way has reached) */
                FlowState *met = same_depths(t, s) ? t : &fl->seen[t->other - 1];

                if (s->floor < met->floor) {
                    met->floor = s->floor;
                    *fresh = 1;
                }
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

/* what is known of the item k down from the top (0: the top) */
static Tag item_tag(const Flow *fl, const FlowState *s, const Tags *tags, int k) {
    int at = s->d - 1 - k;   /* (its place: under 0, the word's inputs) */

    if (k < tags->n) {
        return tags->t[tags->n - 1 - k];
    }
    if (at >= 0 || at >= s->floor) {
        return (Tag){TAG_NONE, 0};
    }
    if (-at <= TAGS_KEPT && fl->given[-at - 1] != 0) {   /* (an input this call's caller gave a known word for) */
        return (Tag){TAG_LIT, fl->given[-at - 1]};
    }
    return (Tag){TAG_INPUT, -at};
}

/* the deferred word whose body's address is x (what `is name` lays down), NULL if x isn't one's */
static Word *deferred_of(Forth *f, Cell x) {
    Word *d = (Word *)((uint8_t *)x - offsetof(Word, body));

    return (UCell)x >= offsetof(Word, body) && forth_is_word(f, (Cell)d) && forth_code_of(d) == CODE_DEFER ? d : NULL;
}

/* a token (what is known of it) put where words of effect `wants` are kept, by `in` (a slot's word, a deferred word,
 * or a word that stores its input): a known word of another effect is a misfit, to be warned of; one of the word's
 * own inputs makes the word one that stores a token */
static void token_kept(Flow *fl, Tag token, const ForthEffect *wants, Word *in) {
    Forth *f = fl->f;
    Word *w;
    ForthEffect has;

    if (token.kind == TAG_INPUT && fl->self != NULL && fl->ntaken < FLOW_TAKEN && token.v <= INT8_MAX) {
        fl->taken[fl->ntaken].input = (int)token.v;
        fl->taken[fl->ntaken++].effect = *wants;
        return;
    }
    if (token.kind != TAG_LIT || fl->misfit != NULL || (w = literal_word(f, token.v)) == NULL) {
        return;
    }
    has = effect_of(fl, w);
    if (has.kind == EFFECT_FIXED &&
        (has.in != wants->in || has.out != wants->out || has.fin != wants->fin || has.fout != wants->fout)) {
        fl->misfit = w;
        fl->misfit_in = in;
        fl->misfit_wants = *wants;
        fl->misfit_has = has;
    }
}

/* before w (of a fixed effect) runs: the tokens it is given, where the checker knows what becomes of them - a store
 * into a slot or a deferred word, a call of a word that stores its input */
static void tokens_given(Flow *fl, const FlowState *s, const Tags *tags, Word *w) {
    Forth *f = fl->f;
    Tag top = item_tag(fl, s, tags, 0);
    const ForthEffect *wants;
    Word *d;
    int k;

    if (w == f->w_store) {
        if (top.kind == TAG_SLOT && (wants = held_effect(f, (Word *)top.v)) != NULL) {
            token_kept(fl, item_tag(fl, s, tags, 1), wants, (Word *)top.v);
        } else if (top.kind == TAG_LIT && (d = deferred_of(f, top.v)) != NULL && d->effect_tried &&
                   d->effect.kind == EFFECT_FIXED) {
            token_kept(fl, item_tag(fl, s, tags, 1), &d->effect, d);
        }
    }
    for (k = 0; (w->flags & WORD_OF_TOKENS) && k < f->ntokens; k++) {
        if (f->tokens[k].w == w && f->tokens[k].input > 0) {
            token_kept(fl, item_tag(fl, s, tags, f->tokens[k].input - 1), &f->tokens[k].effect, w);
        }
    }
}

/* after w (of the fixed effect e) has run: what is known of the items it left */
static void tags_after(Flow *fl, Tags *tags, Word *w, const ForthEffect *e, Tag top, Tag under) {
    Forth *f = fl->f;
    int i;

    if (w == f->w_dup) {
        tags_pop(tags, 1);
        tags_push(tags, top.kind, top.v);
        tags_push(tags, top.kind, top.v);
    } else if (w == f->w_swap) {
        tags_pop(tags, 2);
        tags_push(tags, top.kind, top.v);
        tags_push(tags, under.kind, under.v);
    } else if (w == f->w_over) {
        tags_pop(tags, 2);
        tags_push(tags, under.kind, under.v);
        tags_push(tags, top.kind, top.v);
        tags_push(tags, under.kind, under.v);
    } else {
        tags_pop(tags, e->in);
        for (i = 0; i < e->out; i++) {
            tags_push(tags, TAG_NONE, 0);
        }
        if (e->out > 0 && held_effect(f, w) != NULL) {   /* a slot's word: the slot's address */
            tags->t[tags->n - 1] = (Tag){TAG_SLOT, (Cell)w};
        } else if (w == f->w_fetch) {
            tags->t[tags->n - 1] = top.kind == TAG_SLOT ? (Tag){TAG_TOKEN, top.v} : (Tag){TAG_FETCHED, 0};
        }
    }
}

/* one way through the code from s, to its end or a place already seen */
static int flow_walk(Flow *fl, FlowState s) {
    Forth *f = fl->f;
    Cell last_lit = 0;
    int have_lit = 0, tagged = 0;
    Tags tags = {.n = 0};

    for (;;) {
        Word *w;
        int r, full, fresh;

        if ((const uint8_t *)s.ip < f->mem || (const uint8_t *)s.ip >= f->here) {
            return FLOW_UNKNOWN;
        }
        if (flow_seen(fl, &s, &full, &fresh) != NULL) {   /* (a place seen again: the depths must be the same) */
            /* (unless it is a loop's start: turns taking or leaving an item each - n stores in an n-turn loop - are
             * more than this can count) */
            /* (but a list's walk: a turn that takes or leaves one is a quotation of the wrong shape) */
            return s.back && (Word *)*s.ip != f->w_each_next ? FLOW_UNKNOWN : FLOW_UNBALANCED;
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
            tags_push(&tags, TAG_LIT, last_lit);
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
        } else if (w == f->w_each_next) {   /* a list's walk: its next item, or - no more - on to its end */
            FlowState over = s;
            ForthEffect item = EFFECT_OF(0, 1, 0, 0);

            over.ip = (Cell *)*s.ip;
            s.ip++;
            if ((r = flow_go(fl, &over)) != FLOW_OK) {
                return r;
            }
            flow_apply(fl, &s, &item, 1);
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
            if (w == f->w_unlocals || w == f->w_unqframe) {   /* (the frame dropped, and whatever was put over it) */
                s.rd = 0;
                s.nloops = 0;
            }
            tagged = 1;   /* (a frame is the return stack's: the data stack as it was) */
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
            tags_push(&tags, TAG_LIT, last_lit);
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
            Cell at = *s.ip++;

            /* (a local keeps what was known of the item it was given - an input, a token fetched - if it is given
             * one once) */
            if (w == f->w_local_store && at >= 0 && at < FLOW_LOCAL_TAGS) {
                if (fl->local_stored[at]) {
                    fl->unsure |= fl->local_used[at];
                    fl->local_tag[at] = (Tag){TAG_NONE, 0};
                } else {
                    fl->local_tag[at] = item_tag(fl, &s, &tags, 0);
                }
                fl->local_stored[at] = 1;
                tags_pop(&tags, 1);
                tagged = 1;
            } else if (w == f->w_local_fetch && at >= 0 && at < FLOW_LOCAL_TAGS) {
                Tag t = fl->local_stored[at] ? fl->local_tag[at] : (Tag){TAG_NONE, 0};

                fl->local_used[at] |= t.kind != TAG_NONE;
                tags_push(&tags, t.kind, t.v);
                tagged = 1;
            }
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
                    w == f->w_with_scratch) && item_tag(fl, &s, &tags, 0).kind == TAG_LIT &&
                   literal_word(f, item_tag(fl, &s, &tags, 0).v) != NULL) {
            /* a known word run: its effect (the token taken first); catch's flag after it */
            ForthEffect token = EFFECT_OF(1, 0, 0, 0);
            ForthEffect e = effect_of(fl, literal_word(f, item_tag(fl, &s, &tags, 0).v));

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
        } else if (w == f->w_execute && item_tag(fl, &s, &tags, 0).kind == TAG_TOKEN) {
            /* a token fetched from a slot (`on-hit @ execute`): the effect its slot holds */
            ForthEffect token = EFFECT_OF(1, 0, 0, 0);
            const ForthEffect *held = held_effect(f, (Word *)item_tag(fl, &s, &tags, 0).v);

            if (held == NULL) {
                return FLOW_UNKNOWN;
            }
            flow_apply(fl, &s, &token, 0);
            flow_apply(fl, &s, held, held->out);
        } else if (w == f->w_execute && item_tag(fl, &s, &tags, 0).kind == TAG_FETCHED) {
            /* a token fetched from memory: not one the caller gave - the word doesn't run its caller's token, its
             * code just can't be followed past here (its comment is what its callers are checked with) */
            return FLOW_UNKNOWN;
        } else {
            ForthEffect e = effect_of(fl, w);
            Tag top = item_tag(fl, &s, &tags, 0), under = item_tag(fl, &s, &tags, 1);

            /* (a walk's cells on the return stack: put there as it begins, gone as it ends) */
            s.rd += WALK_CELLS * ((w == f->w_each_begin) - (w == f->w_each_end || w == f->w_each_found));
            if (e.kind == EFFECT_HIGHER && forth_code_of(w) == CODE_COLON && fl->nested < FLOW_NESTED) {
                /* a word that runs a token it is given, given a known word (`['] w his`, `[: ... ;] with-him`): its
                 * code followed for this call, the token known where it is run */
                Cell given[TAGS_KEPT] = {0};
                int k, any = 0;

                for (k = 0; k < TAGS_KEPT; k++) {
                    Tag t = item_tag(fl, &s, &tags, k);

                    if (t.kind == TAG_LIT && literal_word(f, t.v) != NULL) {
                        given[k] = t.v;
                        any = 1;
                    }
                }
                if (!any || infer_code(f, NULL, w->body, 0, &e, fl->nested + 1, NULL, given) != FLOW_OK) {
                    return FLOW_HIGHER;
                }
            }
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
                tagged = w == f->w_qdup;   /* (`?dup if`: the item is still on top where it was true) */
            } else if (e.kind != EFFECT_FIXED) {
                return FLOW_UNKNOWN;
            } else {
                tokens_given(fl, &s, &tags, w);
                flow_apply(fl, &s, &e, e.out);
                tags_after(fl, &tags, w, &e, top, under);
                tagged = 1;
                s.rd += (w == f->w_tor) - (w == f->w_rfrom || w == f->w_rdrop);
            }
        }
        if (!tagged) {   /* (code the tags aren't followed through: nothing known of what is on the stack) */
            tags.n = 0;
        }
        tagged = 0;
        have_lit = 0;
    }
}

/* the effect of the code at start (d0 cells already pushed: a DOES> word's body address) */
static int infer_code(Forth *f, Word *self, Cell *start, int d0, ForthEffect *out, int nested, Flow *found,
                      const Cell *given) {
    Flow flow, *fl = &flow;
    FlowState s;
    int r = FLOW_OK;

    memset(fl, 0, sizeof(*fl));
    fl->f = f;
    fl->self = self;
    fl->nested = nested;
    fl->two_ways = self != NULL && f->declared.kind == EFFECT_FLAGGED;
    if (given != NULL) {
        memcpy(fl->given, given, sizeof(fl->given));
    }
    memset(&s, 0, sizeof(s));
    s.ip = start;
    s.d = d0;
    if (flow_go(fl, &s) != FLOW_OK) {
        r = FLOW_UNKNOWN;
    }
    while (r == FLOW_OK && fl->ntodo > 0) {
        r = flow_walk(fl, fl->todo[--fl->ntodo]);
    }
    if (fl->unsure) {   /* (a local's tag told something, and the local was stored again: nothing told by it stands) */
        r = r == FLOW_OK ? FLOW_UNKNOWN : r;
        fl->misfit = NULL;
        fl->ntaken = 0;
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
    if (found != NULL) {   /* (what the walk found besides the effect: `stranded`, tokens stored) */
        found->stranded = fl->stranded;
        found->stranded_items = fl->stranded_items;
        found->misfit = fl->misfit;
        found->misfit_in = fl->misfit_in;
        found->misfit_wants = fl->misfit_wants;
        found->misfit_has = fl->misfit_has;
        found->ntaken = fl->ntaken;
        memcpy(found->taken, fl->taken, sizeof(fl->taken));
    }
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

/* a way out over the code's own cells on the return stack: said whatever its comment, for the run would crash */
static void warn_stranded(Forth *f, const char *name, const Flow *found) {
    if (found->stranded == STRANDED_LOOP) {
        forth_warn(f, "%s: an exit inside a do loop without unloop (the loop's cells are over its return address)", name);
    } else if (found->stranded == STRANDED_ITEMS) {
        forth_warn(f, "%s: it returns with %d still on the return stack (a >r with no r>: over its return address)",
                   name, found->stranded_items);
    }
}

/* a word stored where words of another effect are kept: said whatever the definition's comment */
static void warn_misfit(Forth *f, const char *name, const Flow *found) {
    char has[48], wants[48];
    const Word *w = found->misfit, *in = found->misfit_in;
    const char *stored;

    if (w == NULL) {
        return;
    }
    format_effect(&found->misfit_has, has, sizeof(has));
    format_effect(&found->misfit_wants, wants, sizeof(wants));
    stored = w->len > 0 ? w->name : "a quotation";
    if (forth_code_of(in) == CODE_DEFER) {
        forth_warn(f, "%s: %s %s is set to %s, which is %s", name, stored, has, in->name, wants);
    } else if (held_effect(f, in) != NULL) {
        forth_warn(f, "%s: %s %s is stored in %s, which holds %s words", name, stored, has, in->name, wants);
    } else {
        forth_warn(f, "%s: %s %s is given to %s, which stores it where %s words are kept", name, stored, has, in->name,
                   wants);
    }
}

/* `;]`: a quotation's code followed for that alone (its effect is worked out where it is called) */
void forth_check_quotation(Forth *f, Word *q) {
    ForthEffect code;
    Flow found;

    (void)infer_code(f, NULL, q->body, 0, &code, 1, &found, NULL);
    warn_stranded(f, "a quotation", &found);
    warn_misfit(f, "a quotation", &found);
}

/* `;`: the definition's code worked out, checked against its comment; its effect kept for its callers */
void forth_check_definition(Forth *f, Word *w) {
    ForthEffect code, kept;
    char name[WORD_NAME_MAX + 8];
    Flow found;
    int r, r2;

    snprintf(name, sizeof(name), "%s", w->len > 0 || w->name[0] ? w->name : ":noname");   /* (a handler: named) */
    r = infer_code(f, w, w->body, 0, &code, 0, &found, NULL);
    warn_stranded(f, name, &found);
    warn_misfit(f, name, &found);
    for (r2 = 0; r2 < found.ntaken; r2++) {   /* (it stores an input in a slot: its callers' tokens are checked) */
        note_token(f, w, found.taken[r2].input, &found.taken[r2].effect);
    }
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
        forth_hold_warning(f, "%s %s: its code leaves the stacks at other depths where its ways meet (an if's two ways, a "
                   "loop's turns)", name, f->declared_text);
    } else if (r == FLOW_OK && code.kind != EFFECT_ENDS) {
        char said[48];

        format_effect(&code, said, sizeof(said));
        forth_hold_warning(f, "%s %s: its code is %s", name, f->declared_text, said);
    }
}

/* (is) ( xt body -- ) `' w is name` outside a definition: stored - and, name a deferred word with its effect said,
 * w's checked against it as it would be compiled */
PRIM(p_is) {
    Cell body = POP(), xt = POP();
    Word *d = deferred_of(f, body), *set = d != NULL ? literal_word(f, xt) : NULL;

    if (set != NULL && d->effect_tried && d->effect.kind == EFFECT_FIXED) {
        Flow fl;

        memset(&fl, 0, sizeof(fl));
        fl.f = f;
        token_kept(&fl, (Tag){TAG_LIT, xt}, &d->effect, d);
        warn_misfit(f, "is", &fl);
    }
    memcpy(forth_addr_n(f, body, sizeof(xt)), &xt, sizeof(xt));
}

/* ( "name" -- ) the effect a word has as its callers see it: "( 2 -- 1 )", or why not */
PRIM(p_effect_dot) {
    Word *x = forth_parse_find(f, "effect.");
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
    e = effect_of(&fl, forth_xt_arg(f, POP(), "stack-effect"));
    PUSH(e.in);
    PUSH(e.out);
    PUSH(e.fin);
    PUSH(e.fout);
    PUSH(e.kind);
}
/* .checked ( -- ) how much of the colon definitions loaded has an effect known (checked, or worked out), and how
 * much not - by vocabulary, the most unknown first: where giving C words their effects would check more */
PRIM(p_dot_checked) {
    long kinds[6] = {0}, all = 0, worked = 0;
    Vocab *v;
    Word *x;

    for (v = f->vocabs; v != NULL; v = v->next) {
        long known = 0, unknown = 0;

        for (x = v->latest; x != NULL; x = x->link) {
            if (forth_code_of(x) == CODE_COLON && x->len > 0 && !(x->flags & WORD_IMMEDIATE)) {
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

void forth_effects_words(Forth *f) {
    static const ForthPrim prims[] = {
        {"effect.", p_effect_dot, "( \"name\" -- )"}, {"holds:", p_holds, "( -- )"}, {"(is)", p_is, "( xt addr -- )"},
        {"stack-effect", p_stack_effect, "( xt -- in out fin fout kind )"}, {".checked", p_dot_checked, "( -- )"},
    };

    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
}

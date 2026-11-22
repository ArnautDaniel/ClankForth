/* The Forth system's own state, and what its files have of each other: the core (forth.c), the objects (objects.c),
 * the stack checker (effects.c), the actor kernel (actors.c). Not the hosts' API - that is forth.h, where a Forth is
 * a handle and what a host needs of it has a function. A host that includes this instead is built against the
 * system's insides, which change without notice. */
#ifndef FORTH_INTERNAL_H
#define FORTH_INTERNAL_H

/* (Windows, MinGW: longjmp without unwinding the frames it leaves - machine code's have no unwind tables, native.c) */
#if defined(_WIN64) && defined(__GNUC__) && !defined(__USE_MINGW_SETJMP_NON_SEH)
#define __USE_MINGW_SETJMP_NON_SEH 1
#endif
#include "forth.h"

#include <limits.h>
#include <setjmp.h>

#define FORTH_LOCALS 32   /* a definition's locals, all its {: :} together */
#define FORTH_SBUFS 16        /* s" at the prompt: the buffers it goes round */
#define FORTH_SBUF_BYTES 256  /* ... each this long, its 0 too */

#define ORDER_MAX 512

/* what a piece of source sees: where definitions go, and the vocabularies it uses */
typedef struct Manifest {
    Vocab *current;
    Vocab *order[ORDER_MAX];
    int norder;
    int listener;       /* the prompt: every loaded vocabulary is visible */
} Manifest;

/* text being interpreted (a string, or a file's contents) */
typedef struct Source {
    const char *text;
    size_t len, pos;
    const char *name;   /* for error messages */
    int line;
    char *owned;        /* freed when done (a file read by include) */
} Source;

#define SOURCE_DEPTH 16

#define NESTING_MAX 64   /* forth_execute and task turns within each other, kept for the call chain */
/* where a nesting began: the task that was running, its next cell, its return stack's depth */
typedef struct Nesting {
    Task *t;
    Cell *ip;
    int rp;
} Nesting;

/* memory lent to the scripts (a library file's bytes): reaching past its end is an error (forth_guard) */
typedef struct Guard {
    const uint8_t *start;
    size_t n;            /* its bytes */
    size_t span;         /* the bytes from start that count as its (its n and the padding past them) */
    const char *name;
} Guard;

struct Forth {
    /* the dictionary: one block of memory; `here` is the next free byte */
    uint8_t *mem, *here, *end;
    uint8_t *word_map;      /* a bit for each of its cells: a word begins there (an execution token is one of these) */
    /* a bit for each name's hash: a vocabulary other than the core has a word of that name (or had). Most of what a
     * script says is the core's (`dup`, `+`) or a number: its bit clear, a lookup goes straight to the core, past
     * every vocabulary the source uses */
    uint8_t *names_beyond_core;
    Word *latest;           /* the newest word (in any vocabulary) */
    /* `defer name ( effect )`: the deferred word just made, and where its name ended - a stack comment right after
     * it is its effect (its callers checked with it) */
    /* the words the checker knows tokens of (WORD_OF_TOKENS): a slot's (`holds: ( effect )`: input 0), and a word's
     * that stores its nth input from the top in such a slot (found as it is compiled) - the effect the token has */
    struct TokenNote {
        Word *w;
        int input;
        ForthEffect effect;
    } *tokens;
    int ntokens, tokens_cap;
    Word *w_fetch, *w_store, *w_dup, *w_qdup, *w_swap, *w_over;
    Word *deferred;
    const Source *deferred_in;
    size_t deferred_at;
    char deferred_text[256];   /* its comments so far: ( a -- b ) ( F: -- x ) both its */
    Vocab *vocabs;          /* every vocabulary */
    Vocab *core;            /* `forth`: always visible */
    Manifest m;             /* the current source's view */
    Manifest listener;      /* the prompt's (kept between lines) */
    Manifest *views;        /* the views put aside while a nearer source's is in force (an include's, the
                             * prompt's): a catch point unwinding to its depth gets its view back */
    int nviews, views_cap;
    /* where source files come from besides the disc (a zipped mod's scripts): the file's text
     * (malloc'd, NUL-terminated), NULL if it isn't one of its */
    ForthReader reader;
    void *reader_ctx;
    char roots[16][512];    /* where vocabularies' files are looked for (the game's, then mods') */
    int nroots;
    Cell compiling;         /* STATE: 0 interpreting, -1 compiling */
    Word *defining;         /* the word `:` (`:noname`) began, until `;`; NULL when none */
    Word *latest_before;    /* `latest` before it (put back if an error leaves it unfinished) */
    /* the definition being compiled: its locals' names ({: ... :}); a quotation saves them */
    struct ForthLocals {
        char names[FORTH_LOCALS][WORD_NAME_MAX + 1];
        char is_float[FORTH_LOCALS];   /* a float local (F:): fetched to the float stack */
        int at[FORTH_LOCALS];          /* each one's cell in the frame */
        int n;
        int cells;          /* the frame's cells: the locals', and its closures' (a quotation using these locals) */
        Cell *frame_size;   /* the frame's (locals) operand: a later {: grows it; NULL until there is a frame */
        int quotation;      /* a quotation's: its frame begun at its start, dropped at `;]` if it needs none */
        int captures;       /* it uses an enclosing definition's locals (a closure: it needs its frame) */
        Cell *exits[16];    /* (a quotation's) its exits' (unqframe)s, plain exits if it has no frame */
        int nexits;
        int jumps;          /* it has a `jump` (not with a frame) */
    } locals, locals_saved[4];
    int nlocals_saved;
    int top_sp, top_sp_saved[4];   /* the data stack's depth at the definition's (quotation's) start:
                                    * {: only there, not inside a do loop or an if (their frame
                                    * would hide the loop's index, or be dropped unmade) */
    Word *w_locals, *w_unlocals, *w_local_fetch, *w_local_store, *w_flocal_fetch, *w_flocal_store;
    Word *w_qframe, *w_unqframe, *w_nop1, *w_outer_fetch, *w_outer_store, *w_fouter_fetch, *w_fouter_store,
         *w_closure, *w_closure_call;
    /* the combinators compiled inline after literal quotations (core/prelude.fs's) */
    Word *w_dip, *w_2dip, *w_keep, *w_2keep, *w_bi, *w_tri, *w_bi_at, *w_when, *w_unless;
    Word *w_each, *w_map, *w_filter, *w_reduce, *w_find;   /* (the lists': a walk laid down where they stand) */
    Word *w_each_begin, *w_each_next, *w_each_item, *w_each_keep, *w_each_end, *w_each_found;   /* (the walk's) */
    struct { Cell *at; Word *q; int closure; } quoted[3];   /* the last quotations laid down as literals */
    int nquoted;
    Cell base;              /* number base */
    int bye;                /* `bye` was run: the host should stop */

    Task *t;                /* the task running now */
    Task *main;             /* the interpreter's task */
    Task *tasks;            /* spawned tasks */
    int next_task_id;
    Cell frame;             /* advanced by forth_run_tasks (a Cell: long is 32 bits on Windows) */
    int tasks_paused;       /* the scheduler stands still (frames don't count): the game paused */
    int depth;              /* nested forth_execute calls (yield may only leave the outermost) */
    Nesting nesting[NESTING_MAX];   /* [d]: where depth d + 1 began */
    int task_depth;         /* the depth a task runs at */
    int yielded;
    Cell budget;            /* steps a task may take in one turn before it is stopped (a loop that
                             * never yields would hang the game); < 0: no limit */
    long eval_budget;       /* the steps forth_eval and forth_include may take (forth_set_eval_budget); < 0: no limit */

    Source src[SOURCE_DEPTH];
    int nsrc;

    OutputFn out;
    void *out_ctx;
    char numbuf[128];

    /* the definition being compiled: its stack comment(s), as `;` checks its code against them */
    ForthEffect declared;
    char declared_text[256];
    int declared_given;     /* the declared effect is a defining word's (a handler's: its message's): a comment
                             * after must agree with it */
    char declared_why[200];
    ForthEffect (*literal_effect)(Forth *f, Word *w, Cell literal);   /* WORD_EFFECT_OF_LITERAL's */
    char held_warning[512]; /* a definition's warning, held until the next begins (`immediate` drops it) */
    Word *held_for;
    long warnings;          /* warnings given (a word hiding another, code not doing what its comment says) */
    Word *w_0branch, *w_branch, *w_do, *w_qdo, *w_loop, *w_ploop, *w_leave, *w_unloop, *w_execute, *w_catch,
         *w_restart, *w_pick, *w_with_scratch, *w_tor, *w_rfrom, *w_rdrop;

    jmp_buf *catch;         /* where an error unwinds to */
    int silent;             /* inside `catch`: errors are not printed */
    char error[256];        /* the last error message */
    char error_chain[1024]; /* the words running when it was raised (forth_backtrace; not inside `catch`) */
    void (*task_failed)(Forth *f, Task *t);   /* told of a task stopped by an error (the actor kernel's) */
    char said[256];         /* the last error message printed */
    long repeats;           /* times it came again since, not printed */
    int quiet;              /* the error being raised is a repeat (not printed: nor what callers add) */
    long errors;            /* errors raised outside `catch`, repeats too (the tests: none while...) */
    Cell throwing;          /* `throw`'s value, for the error it raises (0: an error of the system's) */
    Cell thrown;            /* what `catch` gives for the last error: throw's value, or -1 */
    uint64_t random[4];     /* the random numbers' state (xoshiro256**): each Forth its own */

    /* words the compiler uses */
    Word *w_lit, *w_flit, *w_litstring, *w_exit, *w_halt, *w_comma, *w_does, *w_type, *w_jump;
    char sbuf[FORTH_SBUFS][FORTH_SBUF_BYTES];   /* s" in interpret mode: rotating buffers (copy what must last) */
    int nsbuf;

    struct FileName *file_names;   /* the files included: their names last as long as the system (errors name them) */
    struct ForthActors *actors;   /* the actor kernel's (actors.h), if the host has it */
    struct ForthObjects *objects; /* the objects the scripts make (lists, tuples, strings), and their regions */
    struct ForthTuples *tuples;   /* the tuple classes, their slots, the generic words */
    struct ForthNative *native;   /* the native backend's (native.c): its machine code; NULL without it */
    int node;                     /* nodes.c: this Forth's node number (0: none yet) */
    void *node_pending;           /* nodes.c: what came for it, taken from its inbox, not yet handed to the actors */
    char **args;                  /* system.c: the program's arguments, `args` (the host's: forth_set_args) */
    struct ForthNet *net;         /* net.c's: its sockets and timers; NULL until forth_net */
    int (*event_wait)(Forth *f, long ms);   /* how a node's loop waits (net.c's: the sockets too); NULL its inbox */
    int nargs;
    struct ForthBytes *packing;   /* nodes.c: a message's objects as bytes, the buffer kept between sends */
    void (*node_setup)(Forth *f); /* the host's words, for a node start-node makes (forth_set_node_setup) */
    Guard *guards;          /* sorted by start */
    int nguards, guards_cap;
    int reloading;          /* `reload`s under way (reloading? true): the words a vocabulary had forward as it ends */
    const uint8_t *reload_mark;   /* here as the outermost began: words before it are the ones replaced */
    const uint8_t *prompt_mark;   /* here as forth_eval's text began (outside a definition): what is defined since, a
                                   * word defined again, forwards (its callers get it) */
};

/* ---- for every file of the system: the stacks, and what a primitive is written with ---- */

/* cells a Float takes in threaded code (one on 64-bit, two on 32-bit) */
#define FCELLS ((sizeof(Float) + sizeof(Cell) - 1) / sizeof(Cell))
#define CELLS_FOR(bytes) (((bytes) + sizeof(Cell) - 1) / sizeof(Cell))
#define CELL_BITS (sizeof(Cell) * CHAR_BIT)
/* nothing is mapped below this, nor at a "negative" address: a number there is a mistake */
#define LOWEST_ADDRESS 0x10000

#define PRIM(name) static void name(Forth *f, Word *w __attribute__((unused)))
#define PUSH(x) cell_push(f, (Cell)(x))
#define POP() cell_pop(f)
#define FPUSH(x) float_push(f, (Float)(x))
#define FPOP() float_pop(f)
#define IP (f->t->ip)

/* (in the header so that each file's primitives have them in line: forth_push and the rest are these, for a host) */
static inline void cell_push(Forth *f, Cell x) {
    Task *t = f->t;

    if (t->sp >= STACK_CELLS) {
        forth_error(f, "stack overflow");
    }
    t->ds[t->sp++] = x;
}

static inline Cell cell_pop(Forth *f) {
    Task *t = f->t;

    if (t->sp <= t->sfloor) {
        forth_error(f, "stack underflow");
    }
    return t->ds[--t->sp];
}

static inline void rpush(Forth *f, Cell x) {
    Task *t = f->t;

    if (t->rp >= STACK_CELLS) {
        forth_error(f, "return stack overflow");
    }
    t->rs[t->rp++] = x;
}

static inline Cell rpop(Forth *f) {
    Task *t = f->t;

    if (t->rp <= 0) {
        forth_error(f, "return stack underflow");
    }
    return t->rs[--t->rp];
}

static inline void float_push(Forth *f, Float x) {
    Task *t = f->t;

    if (t->fp >= FSTACK_CELLS) {
        forth_error(f, "float stack overflow");
    }
    t->fs[t->fp++] = x;
}

static inline Float float_pop(Forth *f) {
    Task *t = f->t;

    if (t->fp <= t->ffloor) {
        forth_error(f, "float stack underflow");
    }
    return t->fs[--t->fp];
}

/* the top items, checked (n = how many must be there) */
static inline Cell *top(Forth *f, int n) {
    if (f->t->sp - f->t->sfloor < n) {
        forth_error(f, "stack underflow");
    }
    return &f->t->ds[f->t->sp - 1];
}

/* ---- forth.c's (the core: the interpreter, the dictionary, the compiler), for the system's other files ---- */

/* a table of primitives, each with its stack effect as a stack comment says it (NULL: not a fixed one), defined */
typedef struct ForthPrim {
    const char *name;
    Code code;
    const char *effect;
} ForthPrim;
void forth_define_prims(Forth *f, const ForthPrim *prims, size_t n);
/* a primitive with its flags (WORD_*) and the cells it reads after it in threaded code */
Word *forth_prim_word(Forth *f, const char *name, Code code, int flags, int operands);
/* a word's head laid down in the current vocabulary (its body: what is laid after) */
Word *forth_new_word(Forth *f, const char *name, size_t len, Code code);
void forth_comma(Forth *f, Cell x);
/* an execution token from the stack: an error if it isn't a word's; an address the scripts reach size bytes at */
Word *forth_xt_arg(Forth *f, Cell x, const char *word);
void *forth_addr_n(Forth *f, Cell a, size_t size);
/* does a word begin at x? a vocabulary's word of that name (NULL: none) */
int forth_is_word(const Forth *f, Cell x);
Word *forth_in_vocab(Vocab *v, const char *name, size_t len);
/* the source being read; the input up to a delimiter; a name that must be there; a word that must be found */
Source *forth_source(Forth *f);
const char *forth_parse_until(Forth *f, char delim, size_t *len);
const char *forth_parse_required(Forth *f, size_t *len, const char *who);
Word *forth_parse_find(Forth *f, const char *who);
/* text as a whole number (the base, $ # % too) or a float: 1 if it is one */
int forth_parse_int(Forth *f, const char *s, size_t n, Cell *out);
int forth_parse_float(const char *s, size_t n, Float *out);
/* x's digits in the base (a buffer of the system's: used before the next) */
const char *forth_format_number(Forth *f, Cell x, int is_unsigned);
/* what kind of word w is, by its code field */
enum { CODE_OTHER, CODE_COLON, CODE_VARIABLE, CODE_CONSTANT, CODE_FCONSTANT, CODE_DOES, CODE_DEFER, CODE_ENUM_NAME,
       CODE_STRINGS };
int forth_code_of(const Word *w);
/* a definition's warning held until the next definition begins (`immediate` drops it) */
void forth_hold_warning(Forth *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- objects.c's: the lists, tuples and strings the scripts make ---- */

#define WALK_CELLS 3   /* a list's walk on the return stack: the list, its length, the next index (lay_walk) */
void forth_objects_words(Forth *f);
void forth_objects_free(Forth *f);

/* ---- effects.c's: the stack checker ---- */

/* the effect a stack comment's text gives; 0 if it isn't one */
int forth_parse_effect(const char *s, size_t n, ForthEffect *e);
/* (effects.c's, for forth.c) a word defined again with another effect: a warning, if any calls were compiled for it */
void forth_effect_changed(Forth *f, const Word *old, const Word *now, long callers);
/* `;` and `;]`: the code just compiled followed, checked against its comment, its effect kept for its callers */
void forth_check_definition(Forth *f, Word *w);
void forth_check_quotation(Forth *f, Word *q);
void forth_effects_words(Forth *f);

/* ---- native.c's: the native backend - colon definitions translated to machine code as they are finished, tasks
 * run as coroutines on native stacks of their own. Without it (another machine, CLANKFORTH_NATIVE=OFF,
 * CLANKFORTH_THREADED in the environment) these do nothing and every word stays threaded. ---- */

void forth_native_init(Forth *f);
void forth_native_free(Forth *f);
void forth_native_words(Forth *f);
void forth_native_compile(Forth *f, Word *w, const Cell *end);   /* w's threaded code (to end): machine code too */
void forth_donative(Forth *f, Word *w);   /* the code field of a word with machine code */
/* a word defined again (forth.c's forwarding): machine code calling `old` goes on to `to` (which has machine code);
 * 0 if old has none (nothing to do) */
int forth_native_forward(Forth *f, Word *old, Word *to);
/* (forth.c's, for net.c's repl) the definition being compiled let go, as if it had never begun */
void forth_drop_definition(Forth *f);
/* (forth.c's) a word as `see` shows it - for a handler, a quotation, any token */
void forth_see(Forth *f, Word *x);
/* (actors.c's, for forth_eval) the prompt's actor (`attach`): entered for each line of the prompt's text, the actor
 * the line ends in kept for the next; nothing without the actor kernel (-1) */
int forth_actors_prompt_enter(Forth *f);
void forth_actors_prompt_leave(Forth *f, int saved);
int forth_native_turn(Forth *f, Task *t);   /* a task's turn as a coroutine: 0 if it isn't one (run it threaded) */
void forth_native_suspend(Forth *f);        /* a wait (f->yielded set): out of the coroutine, till its next turn */
void forth_native_restart(Forth *f);        /* `restart`: back to the coroutine's start (doesn't return if one) */
void forth_native_task_reset(Forth *f, Task *t);   /* started again: its coroutine fresh */
void forth_native_task_free(Forth *f, Task *t);
/* (forth.c's, for it) */
void forth_run(Forth *f);                 /* threaded code from IP until it stops (or yields, with no coroutine) */
void forth_run_to(Forth *f, Cell *stop);  /* threaded code from IP until it comes back to stop */
_Noreturn void forth_ran_too_long(Forth *f);   /* the budget spent: the error */

/* ---- nodes.c's: other Forths of this process (nodes), each on a thread of its own, and messages between their
 * actors. An actor on another node is known by its address: the node's number over FORTH_NODE_SHIFT bits, its id
 * under them. ---- */

#define FORTH_NODE_SHIFT 32
#define FORTH_NODE_ID_MASK ((Cell)0xFFFFFFFF)   /* an address's id */
int forth_node_of(Forth *f);                 /* this Forth's node number (one taken the first time) */
Cell forth_node_address(Forth *f, int id);   /* an actor's here, as other nodes reach it (-1: -1) */
/* a message to an actor on another node, copied into its inbox; 0 if that node has ended */
int forth_node_send(Forth *f, Cell from, Cell to, const char *kind, const Cell *args, int n);
/* what one actor asks of another's on another node - or tells it - besides messages: supervision */
enum {
    FORTH_NODE_MONITOR = 1,   /* from watches to: told `died` when it is gone (at once if it is gone already) */
    FORTH_NODE_UNMONITOR,
    FORTH_NODE_LINK,          /* from and to linked (from has to among its links already): either gone, the other killed */
    FORTH_NODE_UNLINK,
    FORTH_NODE_KILL,          /* to killed */
    FORTH_NODE_EXIT,          /* from, linked to to, is gone (reason why): to killed of it */
    FORTH_NODE_DIED           /* from, watched by to, is gone (reason why): to told `died` */
};
/* sent to to's node (extra: bytes it carries - a failure's record); 0 if that node has ended */
int forth_node_control(Forth *f, int what, Cell from, Cell to, Cell reason, const void *extra, size_t n);
void forth_node_receive(Forth *f);           /* what has come for this node: to the actor kernel's queue */
void forth_node_leave(Forth *f);             /* (forth_free) */
void forth_nodes_words(Forth *f, Vocab *actors);
/* (actors.c's, for it) */
size_t forth_actors_body_bytes(Forth *f);
int forth_actors_accepts(Forth *f, int to, const char *kind, int n);
void forth_actors_arrive(Forth *f, Cell from, int to, int kind, const Cell *args, int n);
void forth_actors_dead_letter(Forth *f);
void forth_actors_control(Forth *f, int what, Cell from, int to, Cell reason, const void *extra, size_t n);
int forth_actors_declare(Forth *f, const char *name, int nargs);   /* a message kind from C (net.c's): its number */
void forth_net_free(Forth *f);   /* (forth_free) */
/* (nodes.c's, for net.c) wake: called - from any thread - as a message comes for f's node, so an event loop waiting
 * on more than the node's inbox wakes for it too */
void forth_node_set_wake(Forth *f, void (*wake)(void *ctx), void *ctx);
/* (objects.c's, for it) an object copied out as bytes (0: x is none, nothing added); made again from them (0: it
 * can't be here - a class this node hasn't) */
typedef struct ForthBytes {
    uint8_t *bytes;
    size_t n, cap;
} ForthBytes;
int forth_object_pack(Forth *f, Cell x, ForthBytes *b);
int forth_object_unpack(Forth *f, const uint8_t **p, const uint8_t *end, Cell *out);
int forth_object_fits(Forth *f, const uint8_t **p, const uint8_t *end);   /* could it be made here? (p past it) */
const char *forth_object_string(Forth *f, Cell x, size_t *len);   /* a string object's bytes; NULL if x isn't one */
/* (for system.c, and any file that makes objects) a new string object, list, byte buffer - in the running task's
 * region; a list's item added; a byte buffer's bytes (an error if b isn't one) */
char *forth_new_string(Forth *f, const char *s, size_t len);
Cell forth_new_list(Forth *f);
void forth_regions_unwind(Forth *f, Task *t, int to);   /* t's scratch regions ended till `to` is its current */
/* (for actors.c) regions that outlast a task: an actor's (made, closed - its objects freed) - a task whose region it is
 * opens a scratch region inside it as it first makes an object (a handler's); a message's objects into the post as it is sent, out as it is delivered (into
 * the task's region) or let go undelivered; the objects in a region and those inside it (counted, printed by kind) */
int forth_region_lasting(Forth *f);
void forth_region_close(Forth *f, int region);
void forth_objects_post(Forth *f, const Cell *args, int n);
void forth_objects_take(Forth *f, Task *t, const Cell *args, int n);
void forth_objects_drop(Forth *f, const Cell *args, int n);
void forth_object_keep(Forth *f, Cell x, int to, const char *word);   /* (rescue-actor's) into that region */
long forth_region_objects(Forth *f, int region);
int forth_object_describe(Forth *f, Cell x, char *buf, size_t n);   /* an object as a listing shows it (0: not one) */
void forth_region_print(Forth *f, int region);
void forth_list_push(Forth *f, Cell l, Cell x);
Cell forth_new_bytes(Forth *f, const void *data, size_t n);
const uint8_t *forth_bytes_of(Forth *f, Cell b, size_t *n, const char *word);

/* (and for the actor kernel) */

/* a string from the stack ( addr len ), checked as a fetch of its bytes would be: an error if the length is negative
 * or the address a number's ("" for length 0) */
const char *forth_string_arg(Forth *f, Cell addr, Cell n, const char *word, size_t *len);
/* the frame n frames from now (one at least), for a wait: never past the last frame there can be */
Cell forth_frame_after(const Forth *f, Cell n);

#endif

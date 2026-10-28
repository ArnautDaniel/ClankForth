/* The Forth system: a small, jonesforth-style Forth written in C, shaped for game scripting.
 *
 * - Indirect threaded. A word is a Word struct in the dictionary: its `code` is the C function
 *   that runs it. A colon definition's code is docol and its body is a list of Word pointers.
 * - Cells are intptr_t, and addresses are real pointers, so Forth can read and write C structs
 *   directly (with the field-offset words the bindings define). 64-bit only: a float fits a cell.
 * - Floats live on their own stack (doubles); memory floats are 32-bit (sf@ sf!), as game data has them.
 * - Every running piece of Forth is a Task with its own stacks. The interpreter is one task;
 *   game scripts spawn more, which run a little each frame and give way with `yield` or `wait`.
 * - Errors unwind to the nearest catch point (the interpreter prompt, or the task, which dies).
 * - Words live in vocabularies (as in Factor). A file says where its definitions go (`IN: name`)
 *   and which vocabularies it uses (`USING: a b c ;`), loading them from their files
 *   (scripts/a.fs, scripts/a/b.fs for a.b) as needed. It sees only those, its own words and the
 *   core (`forth`); two used vocabularies defining one name is an error (say `vocab:name`).
 *   The interactive prompt sees every loaded vocabulary.
 *
 * Most of the language (if/then, loops, variables...) is defined in Forth on top of these
 * primitives, in scripts/prelude.fs. See docs/forth.md for the dialect. */
#ifndef FORTH_H
#define FORTH_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

typedef intptr_t Cell;
typedef uintptr_t UCell;
typedef double Float;

#define FORTH_LOCALS 32   /* a definition's locals, all its {: :} together */

typedef struct Forth Forth;
typedef struct Word Word;
typedef struct Task Task;
typedef struct Vocab Vocab;

/* what executing a word does */
typedef void (*Code)(Forth *f, Word *w);

enum {
    WORD_IMMEDIATE = 1,   /* runs even while compiling */
    WORD_HIDDEN = 2,      /* not found by name (a definition being compiled) */
    WORD_EFFECT_OF_LITERAL = 4,   /* its stack effect is the literal's before it (a message kind: `send ping`) -
                                   * the host's literal_effect says */
};

#define WORD_NAME_MAX 31

/* a word's stack effect ( in -- out ), on the data stack and the float stack: its stack comment's (a C word's: its
 * host's, forth_set_effect), else worked out from its code as it is compiled. `;` checks a definition's code
 * against its comment and warns where they differ. */
enum {
    EFFECT_UNKNOWN,   /* not known: callers aren't checked past it */
    EFFECT_FIXED,     /* in -- out */
    EFFECT_FLAGGED,   /* ( -- x true | false ): `out` with a true flag on top, `out_false` with a false one */
    EFFECT_VARIES,    /* declared, but not a fixed count (`..`, alternatives of other sizes) */
    EFFECT_HIGHER,    /* runs an execution token it is given: its effect is that token's too */
    EFFECT_ENDS,      /* never returns to its caller (`restart`) */
};
typedef struct ForthEffect {
    int8_t in, out, fin, fout;
    int8_t out_false;
    int8_t either;    /* outputs on either stack, besides out and fout: ( F: x -- flag ) says the inputs are floats,
                       * not which stack its result is on (the code says) */
    uint8_t kind;     /* EFFECT_* */
} ForthEffect;

struct Word {
    Word *link;     /* the previous word in its vocabulary */
    Word *same;     /* the next older word in its vocabulary's table slot (the lookup's chain) */
    Vocab *vocab;
    Code code;
    Cell *does;     /* the threaded code after DOES>, for words made by a defining word */
    uint8_t flags;
    uint8_t len;
    uint8_t operands;   /* inline cells after it in threaded code (for `see`); 0xFF: a string */
    uint8_t effect_tried;   /* (a word's effect worked out when first called: done, known or not) */
    ForthEffect effect;
    char name[WORD_NAME_MAX + 1];
    Cell body[];    /* the parameter field: threaded code, a variable's value, ... */
};

enum { VOCAB_NEW, VOCAB_LOADING, VOCAB_LOADED };

#define VOCAB_NAME_MAX 63

struct Vocab {
    char name[VOCAB_NAME_MAX + 1];
    Word *latest;       /* its newest word */
    Word **table;       /* its named words by their name's hash, each slot newest first (malloc'd) */
    int slots, nwords;  /* the table's size (a power of two), the words in it */
    int state;          /* VOCAB_*: its file loaded? */
    Vocab *parent;      /* for name.private: name */
    Vocab *next;        /* all vocabularies, newest first */
};

#define ORDER_MAX 512

/* what a piece of source sees: where definitions go, and the vocabularies it uses */
typedef struct Manifest {
    Vocab *current;
    Vocab *order[ORDER_MAX];
    int norder;
    int listener;       /* the prompt: every loaded vocabulary is visible */
} Manifest;

#define STACK_CELLS 256
#define FSTACK_CELLS 64

enum { TASK_READY, TASK_WAITING, TASK_DONE };

struct Task {
    Cell ds[STACK_CELLS];   /* data stack (grows up; sp = number of items) */
    Cell rs[STACK_CELLS];   /* return stack */
    Float fs[FSTACK_CELLS]; /* float stack */
    int sp, rp, fp;
    int sfloor, ffloor;     /* below these the stacks count as empty (a message handler's: its arguments only) */
    int lp;                 /* the running word's locals: their frame on the return stack */
    int marks[16], nmarks;  /* `{`: where each list being gathered starts on the data stack */
    Cell *ip;               /* the next cell of threaded code to run; NULL when stopped */
    int closure_lp;         /* a closure being called: its word's frame + 1, for its quotation's (qframe); 0 none */
    Cell boot[2];           /* a task's first code: its word, then halt */
    int state;
    int held;               /* run only by `resume`, never by the scheduler */
    long wake;              /* TASK_WAITING: the frame it runs again */
    int id;
    char name[WORD_NAME_MAX + 1];
    Task *next;             /* the scheduler's list */
};

/* text being interpreted (a string, or a file's contents) */
typedef struct Source {
    const char *text;
    size_t len, pos;
    const char *name;   /* for error messages */
    int line;
    char *owned;        /* freed when done (a file read by include) */
} Source;

#define SOURCE_DEPTH 16

typedef void (*OutputFn)(void *ctx, const char *s, size_t n);

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
    Word *latest;           /* the newest word (in any vocabulary) */
    Vocab *vocabs;          /* every vocabulary */
    Vocab *core;            /* `forth`: always visible */
    Manifest m;             /* the current source's view */
    Manifest listener;      /* the prompt's (kept between lines) */
    Manifest *views;        /* the views put aside while a nearer source's is in force (an include's, the
                             * prompt's): a catch point unwinding to its depth gets its view back */
    int nviews, views_cap;
    /* where source files come from besides the disc (a zipped mod's scripts): the file's text
     * (malloc'd, NUL-terminated), NULL if it isn't one of its */
    char *(*reader)(void *ctx, const char *path, size_t *len);
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
    /* the combinators compiled inline after literal quotations (lib/prelude.fs's) */
    Word *w_dip, *w_2dip, *w_keep, *w_2keep, *w_bi, *w_tri, *w_bi_at, *w_when, *w_unless;
    struct { Cell *at; Word *q; int closure; } quoted[3];   /* the last quotations laid down as literals */
    int nquoted;
    Cell base;              /* number base */
    int bye;                /* `bye` was run: the host should stop */

    Task *t;                /* the task running now */
    Task *main;             /* the interpreter's task */
    Task *tasks;            /* spawned tasks */
    int next_task_id;
    long frame;             /* advanced by forth_run_tasks */
    int tasks_paused;       /* the scheduler stands still (frames don't count): the game paused */
    int depth;              /* nested forth_execute calls (yield may only leave the outermost) */
    Nesting nesting[NESTING_MAX];   /* [d]: where depth d + 1 began */
    int task_depth;         /* the depth a task runs at */
    int yielded;
    long budget;            /* steps a task may take in one turn before it is stopped (a loop that
                             * never yields would hang the game); < 0: no limit */

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
    ForthEffect (*literal_effect)(Forth *f, Word *w, Cell literal);   /* WORD_EFFECT_OF_LITERAL's */
    char held_warning[512]; /* a definition's warning, held until the next begins (`immediate` drops it) */
    Word *held_for;
    long warnings;          /* warnings given (a word hiding another, code not doing what its comment says) */
    Word *w_0branch, *w_branch, *w_do, *w_qdo, *w_loop, *w_ploop, *w_leave, *w_unloop, *w_execute, *w_catch,
         *w_restart, *w_pick;

    jmp_buf *catch;         /* where an error unwinds to */
    int silent;             /* inside `catch`: errors are not printed */
    char error[256];        /* the last error message */
    char error_chain[1024]; /* the words running when it was raised (forth_backtrace; not inside `catch`) */
    void (*task_failed)(Forth *f, Task *t);   /* told of a task stopped by an error (the actor kernel's) */
    char said[256];         /* the last error message printed */
    long repeats;           /* times it came again since, not printed */
    int quiet;              /* the error being raised is a repeat (not printed: nor what callers add) */
    long errors;            /* errors raised outside `catch`, repeats too (the tests: none while...) */

    /* words the compiler uses */
    Word *w_lit, *w_flit, *w_litstring, *w_exit, *w_halt, *w_comma, *w_does, *w_type, *w_jump;
    char sbuf[16][256];     /* s" in interpret mode: rotating buffers (copy what must last) */
    int nsbuf;

    struct FileName *file_names;   /* the files included: their names last as long as the system (errors name them) */
    struct ForthActors *actors;   /* the actor kernel's (actors.h), if the host has it */
    Guard *guards;          /* sorted by start */
    int nguards, guards_cap;
};

/* life */
Forth *forth_new(size_t dict_bytes);
/* the rest of the language (lib/prelude.fs, built into the library): load it first; 0 ok, -1 an error (said) */
int forth_prelude(Forth *f);
void forth_free(Forth *f);
void forth_set_output(Forth *f, OutputFn out, void *ctx);

/* interpret text; 0 on success, -1 after an error (the message has been printed) */
int forth_eval(Forth *f, const char *text, size_t len, const char *name);
int forth_include(Forth *f, const char *path);

/* vocabularies: find or make one; make it where definitions go; add a folder to look for
 * vocabularies' files in (scripts/) */
Vocab *forth_vocab(Forth *f, const char *name);
void forth_set_current(Forth *f, Vocab *v);
void forth_add_root(Forth *f, const char *dir);

/* find a word by name (case-insensitive) as the current source sees it; NULL if there is none */
Word *forth_find(Forth *f, const char *name, size_t len);
/* run a word (from C); errors propagate to the caller's catch point */
void forth_execute(Forth *f, Word *w);
/* run a word from C with its own catch point: 0 ok, -1 error (printed) */
int forth_call(Forth *f, Word *w);

/* defining words from C */
Word *forth_prim(Forth *f, const char *name, Code code);
/* a C word's stack effect, as a stack comment says it: "( a b -- c )", "( addr -- ) ( F: -- x )"; 0, or -1 if it
 * isn't one */
int forth_set_effect(Forth *f, Word *w, const char *comment);
/* the definition being compiled has this effect (a defining word's: an actor's handler, its message's); its own stack
 * comment must agree */
void forth_declare_effect(Forth *f, const char *comment);
/* a warning about the source being read, with its file and line (counted: `warnings`) */
void forth_warn(Forth *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* a definition's warning still held, said now (the next definition, the end of a source, an error say it) */
void forth_flush_warning(Forth *f);
Word *forth_constant(Forth *f, const char *name, Cell value);
Word *forth_fconstant(Forth *f, const char *name, Float value);

/* for words that parse: the next name in the input (length 0 at the end) */
const char *forth_parse_name(Forth *f, size_t *len);
/* lay down a literal in the definition being compiled */
void forth_compile_literal(Forth *f, Cell x);
/* lay down a call of `w` in the definition being compiled */
void forth_compile_word(Forth *f, Word *w);

/* tasks: run a word as a task; each frame forth_run_tasks gives every ready task a turn */
Task *forth_spawn(Forth *f, Word *w);
void forth_run_tasks(Forth *f);

/* a task of the host's own, not the scheduler's (the actor kernel's: an actor's handlers run in it): made, set to run
 * a word (its stacks emptied - push its arguments after), given turns by its owner, freed */
Task *forth_task_new(Forth *f, const char *name);
void forth_task_start(Forth *f, Task *t, Word *w);
enum { FORTH_TURN_DONE, FORTH_TURN_WAITING, FORTH_TURN_FAILED };
/* a turn: until it ends, waits (yield, wait: then FORTH_TURN_WAITING), or fails (the error said) */
int forth_task_turn(Forth *f, Task *t);
/* may a waiting task go on (its wait over)? */
int forth_task_ready(Forth *f, const Task *t);
void forth_task_free(Forth *f, Task *t);

/* the actor kernel (src/actors.h), built in: NULL until forth_actors */
struct ForthActors;
void forth_actors_free(Forth *f);

/* lists: growable arrays of cells, made by `list` or `{ ... }`; an address on the stack. They
 * live until `list-free` (lists made while loading scripts usually live for good) */
typedef struct List {
    Cell n, cap;
    Cell *items;
} List;

/* the stacks, for primitives */
void forth_push(Forth *f, Cell x);
Cell forth_pop(Forth *f);
void forth_fpush(Forth *f, Float x);
Float forth_fpop(Forth *f);

/* report an error: prints it and the words running, and unwinds to the catch point (does not return) */
_Noreturn void forth_error(Forth *f, const char *fmt, ...);

/* the words running now, innermost first ("inner <- outer <- areas:on show-scene"); its length */
size_t forth_backtrace(Forth *f, char *buf, size_t n);
/* a crash (a bad address, a sanitizer's report) says which words were running: the process's one Forth */
void forth_report_crashes(Forth *f);

/* lend the scripts n bytes at start, padded to span (span >= n: the allocation's bytes past them): in a
 * debug build (NDEBUG unset) a fetch or store from start that reaches past n is an error naming it */
void forth_guard(Forth *f, const void *start, size_t n, size_t span, const char *name);

/* output */
void forth_type(Forth *f, const char *s, size_t n);
void forth_printf(Forth *f, const char *fmt, ...);

/* the `forth` tool (src/tool.c): files run, then a prompt if asked; bind adds a host's words (NULL: none).
 * A host's own tool is a main calling it (OpenMizzurna's: with its actor kernel) */
int forth_tool_main(int argc, char **argv, void (*bind)(Forth *f));

#endif

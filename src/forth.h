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
 * primitives, in lib/prelude.fs. See docs/forth.md for the dialect.
 *
 * This is the hosts' API. A Forth is a handle: what a host needs of it has a function here (its state itself is
 * forth_internal.h's, the system's own). Words, vocabularies and tasks are structs a primitive may read. */
#ifndef FORTH_H
#define FORTH_H

#include <stddef.h>
#include <stdint.h>

typedef intptr_t Cell;
typedef uintptr_t UCell;
typedef double Float;

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
    WORD_COMPILED_ONLY = 8,   /* the compiler's: it reads the cells after it in threaded code (a literal's value, a
                               * branch's target), so it runs only from there - not typed at the prompt, nor executed */
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
    int region;             /* where the objects it makes go: 0 the system's (kept), else a scratch region */
    Cell boot[2];           /* a task's first code: its word, then halt */
    int state;
    int held;               /* run only by `resume`, never by the scheduler */
    long wake;              /* TASK_WAITING: the frame it runs again */
    int id;
    char name[WORD_NAME_MAX + 1];
    Task *next;             /* the scheduler's list */
};

typedef void (*OutputFn)(void *ctx, const char *s, size_t n);

/* life */
Forth *forth_new(size_t dict_bytes);
/* the rest of the language (lib/prelude.fs, built into the library): load it first; 0 ok, -1 an error (said) */
int forth_prelude(Forth *f);
void forth_free(Forth *f);
void forth_set_output(Forth *f, OutputFn out, void *ctx);
/* where output goes now (to put back after sending it elsewhere a while) */
void forth_get_output(const Forth *f, OutputFn *out, void **ctx);
/* where source files come from besides the disc (a zipped mod's scripts): the file's text (malloc'd,
 * NUL-terminated, its length in *len), NULL if it isn't one of the reader's */
typedef char *(*ForthReader)(void *ctx, const char *path, size_t *len);
void forth_set_reader(Forth *f, ForthReader reader, void *ctx);

/* the system as it stands, for a host's loop and prompt: inside a definition (a prompt says " ok" only outside one)?
 * `bye` was run (the host should stop)? */
int forth_compiling(const Forth *f);
int forth_bye(const Forth *f);
/* the task running now (its stacks: `sp` items on `ds`, for a primitive that leaves them as it found them) */
Task *forth_task(Forth *f);
/* the vocabulary definitions go to (forth_set_current's); the newest word */
Vocab *forth_current(const Forth *f);
Word *forth_latest(const Forth *f);
/* the steps the running code may still take before it is stopped ("ran too long"); < 0: no limit. Set, it gives
 * what it was: a host limiting one call puts that back after. (A task's turn and an eval set their own.) */
long forth_set_budget(Forth *f, long steps);

/* errors: how many were raised outside `catch` (repeats too), and warnings given; the last error's message and the
 * words that were running (forth_backtrace's, "" inside `catch`); was it the one before again (so not printed)?
 * forth_forget_error: the next error is printed even if it is the last one again (what is typed at a prompt) */
long forth_errors(const Forth *f);
long forth_warnings(const Forth *f);
const char *forth_last_error(const Forth *f);
const char *forth_last_error_chain(const Forth *f);
int forth_error_repeated(const Forth *f);
void forth_forget_error(Forth *f);
/* told of a task stopped by an error (the actor kernel takes this for itself: a host with forth_actors leaves it) */
void forth_set_task_failed(Forth *f, void (*task_failed)(Forth *f, Task *t));

/* interpret text; 0 on success, -1 after an error (the message has been printed) */
int forth_eval(Forth *f, const char *text, size_t len, const char *name);
int forth_include(Forth *f, const char *path);
/* the steps one forth_eval or forth_include may take before it is stopped with an error ("ran too long": a script
 * that loops for ever as it loads would hang the host); < 0, as at first: no limit. A task has its own each turn. */
void forth_set_eval_budget(Forth *f, long steps);

/* vocabularies: find or make one; make it where definitions go; add a folder to look for
 * vocabularies' files in (scripts/) */
Vocab *forth_vocab(Forth *f, const char *name);
void forth_set_current(Forth *f, Vocab *v);
void forth_add_root(Forth *f, const char *dir);

/* find a word by name (case-insensitive) as the current source sees it; NULL if there is none */
Word *forth_find(Forth *f, const char *name, size_t len);
/* run a word (from C); errors propagate to the caller's catch point */
void forth_execute(Forth *f, Word *w);
/* run a word from C with its own catch point: 0 ok, -1 error (printed; the return and float stacks and the locals
 * as they were - a primitive's caller goes on - the data stack the caller's to put right) */
int forth_call(Forth *f, Word *w);

/* defining words from C */
Word *forth_prim(Forth *f, const char *name, Code code);
/* a C word's stack effect, as a stack comment says it: "( a b -- c )", "( addr -- ) ( F: -- x )"; 0, or -1 if it
 * isn't one */
int forth_set_effect(Forth *f, Word *w, const char *comment);
/* the definition being compiled has this effect (a defining word's: an actor's handler, its message's); its own stack
 * comment must agree - else a warning saying why ("its message carries 1") */
void forth_declare_effect(Forth *f, const char *comment, const char *why);
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

/* lists: growable arrays of cells, made by `list` or `{ ... }`; an address on the stack. Objects, as tuples and
 * strings are: each known by its address, kept until `dispose` (made in a scratch region: until it ends) */
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

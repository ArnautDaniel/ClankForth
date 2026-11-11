/* The Forth system's own state: what forth.c and the actor kernel (actors.c) work on. Not the hosts' API - that is
 * forth.h, where a Forth is a handle and what a host needs of it has a function. A host that includes this instead is
 * built against the system's insides, which change without notice. */
#ifndef FORTH_INTERNAL_H
#define FORTH_INTERNAL_H

#include "forth.h"

#include <setjmp.h>

#define FORTH_LOCALS 32   /* a definition's locals, all its {: :} together */

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
    Word *deferred;
    const Source *deferred_in;
    size_t deferred_at;
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
    /* the combinators compiled inline after literal quotations (lib/prelude.fs's) */
    Word *w_dip, *w_2dip, *w_keep, *w_2keep, *w_bi, *w_tri, *w_bi_at, *w_when, *w_unless;
    Cell compose_rest[1];   /* where a composed quotation's first part returns to: (compose-rest), which runs its second */
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
    long frame;             /* advanced by forth_run_tasks */
    int tasks_paused;       /* the scheduler stands still (frames don't count): the game paused */
    int depth;              /* nested forth_execute calls (yield may only leave the outermost) */
    Nesting nesting[NESTING_MAX];   /* [d]: where depth d + 1 began */
    int task_depth;         /* the depth a task runs at */
    int yielded;
    long budget;            /* steps a task may take in one turn before it is stopped (a loop that
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

    /* words the compiler uses */
    Word *w_lit, *w_flit, *w_litstring, *w_exit, *w_halt, *w_comma, *w_does, *w_type, *w_jump;
    char sbuf[16][256];     /* s" in interpret mode: rotating buffers (copy what must last) */
    int nsbuf;

    struct FileName *file_names;   /* the files included: their names last as long as the system (errors name them) */
    struct ForthActors *actors;   /* the actor kernel's (actors.h), if the host has it */
    struct ForthObjects *objects; /* the objects the scripts make (lists, tuples, strings), and their regions */
    struct ForthTuples *tuples;   /* the tuple classes, their slots, the generic words */
    Guard *guards;          /* sorted by start */
    int nguards, guards_cap;
};

/* ---- forth.c's, for the actor kernel ---- */

/* a string from the stack ( addr len ), checked as a fetch of its bytes would be: an error if the length is negative
 * or the address a number's ("" for length 0) */
const char *forth_string_arg(Forth *f, Cell addr, Cell n, const char *word, size_t *len);
/* x, if it is an object made in a scratch region, kept past the region's end (till `dispose`), with what it holds:
 * what a message carries (a number that is no object is left alone) */
void forth_keep_object(Forth *f, Cell x);
/* the frame n frames from now (one at least), for a wait: never past the last frame there can be */
long forth_frame_after(const Forth *f, Cell n);

#endif

/* The native backend: subroutine threading.
 *
 * A colon definition is compiled as ever, to threaded code - the cells `see` shows and the stack-effect checker
 * follows - and then, as it is finished (`;`, `;]`), translated to x86-64 machine code: a call of each word in turn,
 * its branches and loops jumps. The threaded code stays the definition's meaning (and what the tools read); the
 * machine code is how it runs. A definition that can't be translated (one with `does>` or `jump`) stays threaded,
 * and the two call each other freely.
 *
 * What stays as it was: the stacks, in the Task; the return stack's contents - a call still leaves there where its
 * caller goes on in the threaded code, so the call chain, locals, loops, `catch` and a return stack too deep read it
 * as before; the errors.
 *
 * Tasks run as coroutines, each on a native stack of its own, so machine code can wait (yield, wait, await) in the
 * middle of a word and go on: its frames stay on the task's stack until its next turn. The interpreter's own task
 * runs on the host's stack, where nothing waits.
 *
 * Built for x86-64 with the System V calling convention (Linux, the BSDs: ELF); elsewhere, or with the environment
 * variable CLANKFORTH_THREADED set, every word stays threaded and these functions do nothing. */
#include "forth_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(CLANKFORTH_NATIVE) && defined(__x86_64__) && defined(__ELF__)
#define NATIVE_X86_64 1
#endif

#ifndef NATIVE_X86_64

void forth_native_init(Forth *f) { f->native = NULL; }
void forth_native_free(Forth *f) { (void)f; }
void forth_native_compile(Forth *f, Word *w, const Cell *end) { (void)f; (void)w; (void)end; }
void forth_donative(Forth *f, Word *w) { forth_error(f, "%s: native code, in a system without it", w->name); }
static void p_native_q(Forth *f, Word *w) { (void)forth_xt_arg(f, forth_pop(f), "native?"); forth_push(f, 0); }
static void p_dot_native(Forth *f, Word *w) { forth_printf(f, "no native backend: every definition threaded\n"); }
void forth_native_words(Forth *f) {
    forth_set_effect(f, forth_prim(f, "native?", p_native_q), "( xt -- flag )");
    forth_set_effect(f, forth_prim(f, ".native", p_dot_native), "( -- )");
}
int forth_native_turn(Forth *f, Task *t) { (void)f; (void)t; return 0; }
void forth_native_suspend(Forth *f) { (void)f; }
void forth_native_restart(Forth *f) { (void)f; }
void forth_native_task_reset(Forth *f, Task *t) { (void)f; (void)t; }
void forth_native_task_free(Forth *f, Task *t) { (void)f; (void)t; }

#else

#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>

#if defined(__SANITIZE_ADDRESS__)
#define NATIVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NATIVE_ASAN 1
#endif
#endif
#ifdef NATIVE_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

#define CODE_CHUNK_BYTES (1u << 20)   /* machine code is kept in blocks this big (a larger word gets its own) */
#define CODE_ALIGN 16                 /* each word's code starts on this */
#define CO_STACK_BYTES (1u << 20)     /* a task's native stack: reserved, its pages taken as they are touched */
#define CO_GUARD_BYTES 4096u          /* at its low end, a page nothing may touch: an overflow is a crash, not damage */

/* ---- machine code: where it is kept ---- */

typedef struct CodeChunk {
    struct CodeChunk *next;
    uint8_t *start, *used, *end;
} CodeChunk;

/* the words whose fast path is written in line (their C primitive still run when its checks fail) */
enum {
    FAST_DUP, FAST_DROP, FAST_SWAP, FAST_OVER, FAST_NIP,
    FAST_ADD, FAST_SUB, FAST_AND, FAST_OR, FAST_XOR, FAST_MUL,
    FAST_EQ, FAST_NE, FAST_LT, FAST_GT, FAST_LE, FAST_GE, FAST_ULT, FAST_UGT,
    FAST_0EQ, FAST_0NE, FAST_0LT, FAST_0GT,
    FAST_1ADD, FAST_1SUB, FAST_INVERT, FAST_NEGATE, FAST_CELLS,
    FAST_I, FAST_RFETCH, FAST_TOR, FAST_RFROM,
    FAST_WORDS
};
static const char *const fast_names[FAST_WORDS] = {
    "dup", "drop", "swap", "over", "nip",
    "+", "-", "and", "or", "xor", "*",
    "=", "<>", "<", ">", "<=", ">=", "u<", "u>",
    "0=", "0<>", "0<", "0>",
    "1+", "1-", "invert", "negate", "cells",
    "i", "r@", ">r", "r>",
};

struct ForthNative {
    CodeChunk *chunks;      /* the newest first */
    long translated;        /* definitions given machine code */
    long refused;           /* and those left threaded */
    Word *fast[FAST_WORDS]; /* the core's words of those names */
    int inline_off;         /* (CLANKFORTH_CALLS_ONLY set: every word called, none in line - to compare) */
};

static uint8_t *code_space(Forth *f, size_t n) {
    struct ForthNative *nat = f->native;
    CodeChunk *c = nat->chunks;
    uint8_t *p;

    n = (n + CODE_ALIGN - 1) & ~(size_t)(CODE_ALIGN - 1);
    if (c == NULL || (size_t)(c->end - c->used) < n) {
        size_t bytes = n > CODE_CHUNK_BYTES ? n : CODE_CHUNK_BYTES;
        void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (m == MAP_FAILED || (c = malloc(sizeof(CodeChunk))) == NULL) {
            if (m != MAP_FAILED) {
                munmap(m, bytes);
            }
            return NULL;
        }
        c->start = c->used = m;
        c->end = c->start + bytes;
        c->next = nat->chunks;
        nat->chunks = c;
    }
    p = c->used;
    c->used += n;
    return p;
}

/* ---- machine code: writing it (x86-64) ---- */

typedef struct Emit {
    uint8_t *buf;
    size_t n, cap;
    int failed;   /* out of memory: the word stays threaded */
} Emit;

static void byte(Emit *e, unsigned x) {
    if (e->n == e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 1024;
        uint8_t *b = realloc(e->buf, cap);

        if (b == NULL) {
            e->failed = 1;
            e->n = 0;
            return;
        }
        e->buf = b;
        e->cap = cap;
    }
    e->buf[e->n++] = (uint8_t)x;
}

static void bytes(Emit *e, const uint8_t *s, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        byte(e, s[i]);
    }
}
#define BYTES(e, ...) bytes((e), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

static void u32(Emit *e, uint32_t x) {
    int i;

    for (i = 0; i < 4; i++) {
        byte(e, (x >> (8 * i)) & 0xFF);
    }
}

static void u64(Emit *e, uint64_t x) {
    int i;

    for (i = 0; i < 8; i++) {
        byte(e, (unsigned)(x >> (8 * i)) & 0xFF);
    }
}

/* The registers the machine code keeps (callee-saved, so the C it calls leaves them): rbx the Forth, r12 the task
 * running (f->t - the same task through a word's code, even across a wait). rax, rcx, rsi, rdi are scratch. */
#define TASK_IP ((uint32_t)offsetof(Task, ip))
#define FORTH_TASK ((uint32_t)offsetof(struct Forth, t))
#define FORTH_BUDGET ((uint32_t)offsetof(struct Forth, budget))

static void mov_rax(Emit *e, uint64_t x) { BYTES(e, 0x48, 0xB8); u64(e, x); }   /* movabs rax, x */
static void mov_rcx(Emit *e, uint64_t x) { BYTES(e, 0x48, 0xB9); u64(e, x); }   /* movabs rcx, x */
static void mov_rsi(Emit *e, uint64_t x) { BYTES(e, 0x48, 0xBE); u64(e, x); }   /* movabs rsi, x */
static void arg_forth(Emit *e) { BYTES(e, 0x48, 0x89, 0xDF); }                   /* mov rdi, rbx */
static void call_rax(Emit *e) { BYTES(e, 0xFF, 0xD0); }                          /* call rax */
static void ret(Emit *e) { BYTES(e, 0xC3); }

static void call_c(Emit *e, const void *fn) {
    uint64_t a;

    memcpy(&a, &fn, sizeof(a));
    mov_rax(e, a);
    call_rax(e);
}

static uint64_t at(const void *p) { return (uint64_t)(uintptr_t)p; }

/* IP (f->t->ip) as the threaded code would have it: the operands a word reads, where an error's call chain starts */
static void set_ip(Emit *e, const Cell *ip) {
    mov_rax(e, at(ip));
    BYTES(e, 0x49, 0x89, 0x84, 0x24);   /* mov [r12 + TASK_IP], rax */
    u32(e, TASK_IP);
}

static void load_ip(Emit *e) {
    BYTES(e, 0x49, 0x8B, 0x84, 0x24);   /* mov rax, [r12 + TASK_IP] */
    u32(e, TASK_IP);
}

enum { CC_ALWAYS = -1, CC_E = 0x4, CC_NE = 0x5, CC_S = 0x8, CC_NS = 0x9 };

/* a jump (CC_ALWAYS) or a conditional one, its target to come: where its offset is, for patch() */
static size_t jump(Emit *e, int cc) {
    if (cc == CC_ALWAYS) {
        BYTES(e, 0xE9);
    } else {
        BYTES(e, 0x0F, (uint8_t)(0x80 + cc));
    }
    u32(e, 0);
    return e->n - 4;
}

static void patch(Emit *e, size_t where, size_t target) {
    uint32_t rel = (uint32_t)((int64_t)target - (int64_t)(where + 4));

    if (!e->failed) {
        memcpy(e->buf + where, &rel, sizeof(rel));
    }
}

/* the cmp of IP with an address of the threaded code, for a jump on it after */
static void ip_is(Emit *e, const Cell *ip) {
    load_ip(e);
    mov_rcx(e, at(ip));
    BYTES(e, 0x48, 0x39, 0xC8);   /* cmp rax, rcx */
}

/* a step counted against the budget (f->budget, < 0 no limit): at a word's start and each jump back, so a loop that
 * never ends is stopped as threaded code's is ("ran too long") */
static void count_step(Emit *e, const Cell *ip) {
    size_t unlimited, within;

    BYTES(e, 0x48, 0x8B, 0x83);   /* mov rax, [rbx + FORTH_BUDGET] */
    u32(e, FORTH_BUDGET);
    BYTES(e, 0x48, 0x85, 0xC0);   /* test rax, rax */
    unlimited = jump(e, CC_S);
    BYTES(e, 0x48, 0x83, 0xE8, 0x01);   /* sub rax, 1 */
    BYTES(e, 0x48, 0x89, 0x83);   /* mov [rbx + FORTH_BUDGET], rax */
    u32(e, FORTH_BUDGET);
    within = jump(e, CC_NS);
    set_ip(e, ip);
    arg_forth(e);
    call_c(e, (const void *)forth_ran_too_long);
    patch(e, unlimited, e->n);
    patch(e, within, e->n);
}

/* ---- a definition translated ----
 *
 * Its code has two ways in. The first, at its start, is C's (forth_donative calls it, given the Forth): it keeps rbx
 * and r12, sets them, and calls the second, INNER_ENTRY bytes on - the way machine code calls machine code, the
 * registers already set. A call from machine code is as docol's: where its caller goes on, pushed on the return
 * stack (`exit` takes it off), then the call. */

#define OUTER_ENTRY_BYTES 30
#define INNER_ENTRY 32   /* (aligned) */

typedef void (*NativeEntry)(Forth *f);

static const uint8_t *inner_entry(const Word *w) { return (const uint8_t *)w->does + INNER_ENTRY; }

static void outer_entry(Emit *e) {
    BYTES(e, 0x53);                          /* push rbx */
    BYTES(e, 0x41, 0x54);                    /* push r12 */
    BYTES(e, 0x48, 0x83, 0xEC, 0x08);        /* sub rsp, 8 (calls from here on 16-aligned) */
    BYTES(e, 0x48, 0x89, 0xFB);              /* mov rbx, rdi */
    BYTES(e, 0x4C, 0x8B, 0xA3);              /* mov r12, [rbx + FORTH_TASK] */
    u32(e, FORTH_TASK);
    BYTES(e, 0xE8);                          /* call INNER_ENTRY */
    u32(e, (uint32_t)(INNER_ENTRY - (e->n + 4)));
    BYTES(e, 0x48, 0x83, 0xC4, 0x08);        /* add rsp, 8 */
    BYTES(e, 0x41, 0x5C);                    /* pop r12 */
    BYTES(e, 0x5B);                          /* pop rbx */
    ret(e);
    while (e->n < INNER_ENTRY) {
        BYTES(e, 0xCC);                      /* (int3: never run) */
    }
}

static void leave_word(Emit *e) {
    BYTES(e, 0x48, 0x83, 0xC4, 0x08);        /* add rsp, 8 */
    ret(e);
}

/* helpers the machine code calls */
static int popped_zero(Forth *f) { return POP() == 0; }   /* 0branch's test */
static void push_return(Forth *f, Cell x) { rpush(f, x); }   /* a call's: where its caller goes on */
static void enter_closure(Forth *f) { f->t->closure_lp = f->t->lp + 1; }   /* (closure-call)'s, before its call */

/* the cells after w that are its operands (p: where w is) */
static size_t operand_cells(Forth *f, const Word *w, const Cell *p) {
    if (w == f->w_lit) {
        return 1;
    }
    if (w == f->w_flit) {
        return FCELLS;
    }
    if (w == f->w_litstring) {
        return 1 + CELLS_FOR((size_t)p[1]);
    }
    return (w->flags & WORD_COMPILED_ONLY) ? w->operands : 0;
}

static int branches(Forth *f, const Word *w) {   /* its operand is an address in the threaded code */
    return w == f->w_branch || w == f->w_0branch || w == f->w_do || w == f->w_qdo || w == f->w_loop ||
           w == f->w_ploop || w == f->w_each_next;
}

typedef struct Fixup {
    size_t where;   /* the jump's offset in the code */
    size_t to;      /* the cell it goes to */
} Fixup;

typedef struct Translation {
    Forth *f;
    Word *self;
    const Cell *body;
    size_t n;            /* its cells */
    signed char *op;     /* [i]: 1 if a word to run begins at cell i (reached), 0 not */
    signed char *target; /* [i]: a branch goes to cell i (so it can't be joined to the word before) */
    size_t *label;       /* [i]: where its machine code is */
    size_t *loop_exit;   /* [i]: for a do loop's start, where it goes on after; 0 none */
    Fixup *fixups;
    size_t nfixups, fixups_cap;
    struct Stub *stubs;
    size_t nstubs, stubs_cap;
    Emit e;
} Translation;

static int fixup(Translation *tr, size_t where, size_t to) {
    if (tr->nfixups == tr->fixups_cap) {
        size_t cap = tr->fixups_cap ? tr->fixups_cap * 2 : 64;
        Fixup *x = realloc(tr->fixups, cap * sizeof(Fixup));

        if (x == NULL) {
            return 0;
        }
        tr->fixups = x;
        tr->fixups_cap = cap;
    }
    tr->fixups[tr->nfixups++] = (Fixup){where, to};
    return 1;
}

/* the cell a branch at cell i goes to; n (none) if it isn't one of this definition's */
static size_t target_of(const Translation *tr, size_t i) {
    const Cell *to = (const Cell *)tr->body[i + 1];

    if (to < tr->body || to >= tr->body + tr->n) {
        return tr->n;
    }
    return (size_t)(to - tr->body);
}

/* which cells begin a word that runs, following the branches from the start (a quotation's header and code, lying
 * inside its word's, are jumped over - never read as code). 0 if it has what can't be translated. */
static int find_ops(Translation *tr) {
    Forth *f = tr->f;
    size_t *todo = malloc(tr->n * sizeof(size_t)), ntodo = 0;
    int ok = 1;

    if (todo == NULL) {
        return 0;
    }
    todo[ntodo++] = 0;
    while (ok && ntodo > 0) {
        size_t i = todo[--ntodo];

        while (ok && i < tr->n && !tr->op[i]) {
            Word *w = (Word *)tr->body[i];
            size_t next;

            if (!forth_is_word(f, (Cell)w) || w == f->w_does || w == f->w_jump || w == f->w_halt) {
                ok = 0;
                break;
            }
            tr->op[i] = 1;
            next = i + 1 + operand_cells(f, w, &tr->body[i]);
            if (next > tr->n) {
                ok = 0;
                break;
            }
            if (branches(f, w)) {
                size_t to = target_of(tr, i);

                if (to == tr->n) {
                    ok = 0;
                    break;
                }
                if (w == f->w_do || w == f->w_qdo) {
                    tr->loop_exit[i] = to;
                }
                tr->target[to] = 1;
                if (!tr->op[to]) {
                    if (ntodo == tr->n) {
                        ok = 0;
                        break;
                    }
                    todo[ntodo++] = to;
                }
                if (w == f->w_branch) {
                    break;
                }
            }
            if (w == f->w_exit) {
                break;
            }
            i = next;
        }
        if (i >= tr->n) {   /* (ran off the end: a definition ends in exit) */
            ok = 0;
        }
    }
    free(todo);
    return ok;
}

/* the exit of the innermost do loop around cell i (where `leave` goes); n none */
static size_t leave_target(const Translation *tr, size_t i) {
    size_t s, best = tr->n, best_start = 0;

    for (s = 0; s < i; s++) {
        if (tr->loop_exit[s] != 0 && tr->loop_exit[s] > i && (best == tr->n || s > best_start)) {
            best = tr->loop_exit[s];
            best_start = s;
        }
    }
    return best;
}

/* a call of a word with machine code: where the caller goes on pushed (as docol does), then its inner entry */
static void call_native(Translation *tr, const Word *w, const Cell *cont) {
    Emit *e = &tr->e;

    set_ip(e, cont);
    arg_forth(e);
    mov_rsi(e, at(cont));
    call_c(e, (const void *)push_return);
    if (w == tr->self) {
        BYTES(e, 0xE8);                                  /* call INNER_ENTRY (a word calling itself) */
        u32(e, (uint32_t)((int64_t)INNER_ENTRY - (int64_t)(e->n + 4)));
    } else {
        mov_rax(e, at(inner_entry(w)));
        call_rax(e);
    }
}

/* a call of a word by its code field (a C primitive, a host's word, a constant ...), IP at its operands */
static void call_code(Translation *tr, Word *w, const Cell *ip) {
    Emit *e = &tr->e;
    uint64_t code;

    memcpy(&code, &w->code, sizeof(code));
    set_ip(e, ip);
    arg_forth(e);
    mov_rsi(e, at(w));
    mov_rax(e, code);
    call_rax(e);
}

/* After a call by its code field, IP is where the threaded code would go on - past the word's operands - unless the
 * word went on in threaded code itself (docol: a definition left threaded; execute, a deferred word, a generic one,
 * a closure: of one): then that code is run until it comes back there. */
static void went_on(Translation *tr, Cell *cont) {
    Emit *e = &tr->e;
    size_t same;

    ip_is(e, cont);
    same = jump(e, CC_E);
    arg_forth(e);
    mov_rsi(e, at(cont));
    call_c(e, (const void *)forth_run_to);
    patch(e, same, e->n);
}

static int translate_generic(Translation *tr, size_t i) {
    Forth *f = tr->f;
    Emit *e = &tr->e;
    Word *w = (Word *)tr->body[i];
    const Cell *ip = &tr->body[i + 1];
    Cell *cont = (Cell *)&tr->body[i + 1 + operand_cells(f, w, &tr->body[i])];
    size_t to = branches(f, w) ? target_of(tr, i) : 0;

    if (w == f->w_branch) {
        if (to <= i) {
            count_step(e, ip);
        }
        return fixup(tr, jump(e, CC_ALWAYS), to);
    }
    if (w == f->w_0branch) {
        set_ip(e, ip);
        arg_forth(e);
        call_c(e, (const void *)popped_zero);
        BYTES(e, 0x85, 0xC0);   /* test eax, eax */
        return fixup(tr, jump(e, CC_NE), to);
    }
    if (w == f->w_qdo || w == f->w_each_next) {   /* (start = limit, a list's walk over: to where it goes on) */
        call_code(tr, w, ip);
        ip_is(e, &tr->body[to]);
        return fixup(tr, jump(e, CC_E), to);
    }
    if (w == f->w_loop || w == f->w_ploop) {
        size_t done;

        call_code(tr, w, ip);
        ip_is(e, &tr->body[to]);
        done = jump(e, CC_NE);
        count_step(e, ip);
        if (!fixup(tr, jump(e, CC_ALWAYS), to)) {
            return 0;
        }
        patch(e, done, e->n);
        return 1;
    }
    if (w == f->w_leave) {
        size_t exit = leave_target(tr, i);

        call_code(tr, w, ip);
        if (exit == tr->n) {   /* (not inside a loop of this word: an error at run time) */
            return 1;
        }
        return fixup(tr, jump(e, CC_ALWAYS), exit);
    }
    if (w == f->w_exit) {
        call_code(tr, w, ip);
        leave_word(e);
        return 1;
    }
    if (w == f->w_closure_call) {
        Word *q = (Word *)tr->body[i + 1];

        if (q->code == forth_donative) {
            set_ip(e, ip);
            arg_forth(e);
            call_c(e, (const void *)enter_closure);
            call_native(tr, q, cont);
            return 1;
        }
        call_code(tr, w, ip);
        went_on(tr, cont);
        return 1;
    }
    if (w == tr->self || w->code == forth_donative) {
        call_native(tr, w, cont);
        return 1;
    }
    call_code(tr, w, ip);
    went_on(tr, cont);
    return 1;
}

/* ---- the fast paths: the hot words written in line ----
 *
 * A word's fast path does what its C primitive does when nothing is wrong. Its checks (enough items, room, a loop
 * open) come before it changes anything, and when one fails it jumps to a stub, out of the way after the word's code,
 * that does what the generic translation does - IP set, the C primitive called - and comes back: so an error, or an
 * edge, is exactly the primitive's. The data stack's depth (sp) stays in the Task, so the C called between sees it. */

#define TASK_SP ((uint32_t)offsetof(Task, sp))
#define TASK_SFLOOR ((uint32_t)offsetof(Task, sfloor))
#define TASK_RP ((uint32_t)offsetof(Task, rp))
#define TASK_LP ((uint32_t)offsetof(Task, lp))
#define TASK_DS ((int32_t)offsetof(Task, ds))
#define TASK_RS ((int32_t)offsetof(Task, rs))
#define CELL_BYTES ((int32_t)sizeof(Cell))
#define TOP(k) (TASK_DS + (k) * CELL_BYTES)   /* the data stack's cell k from sp: TOP(-1) the top, TOP(0) the next free */
#define STUB_GUARDS 4

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };
enum { CC_B = 0x2, CC_A = 0x7, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };   /* (with CC_E ... above) */
#define REX_W_B 0x49   /* 64 bits, r12 the base */
#define REX_B 0x41     /* 32 bits, r12 the base */
#define REX_W 0x48
#define MOV_LOAD 0x8B
#define MOV_STORE 0x89

typedef struct Stub {
    size_t first, end;          /* the cells whose words it runs generically */
    size_t guards[STUB_GUARDS]; /* the jumps to it */
    int nguards;
    size_t back;                /* where it comes back to */
} Stub;

static Stub *stub(Translation *tr, size_t first, size_t end) {
    Stub *s;

    if (tr->nstubs == tr->stubs_cap) {
        size_t cap = tr->stubs_cap ? tr->stubs_cap * 2 : 64;
        Stub *x = realloc(tr->stubs, cap * sizeof(Stub));

        if (x == NULL) {
            return NULL;
        }
        tr->stubs = x;
        tr->stubs_cap = cap;
    }
    s = &tr->stubs[tr->nstubs++];
    memset(s, 0, sizeof(*s));
    s->first = first;
    s->end = end;
    return s;
}

static void guard(Translation *tr, Stub *s, int cc) {
    size_t where = jump(&tr->e, cc);

    if (s->nguards < STUB_GUARDS) {
        s->guards[s->nguards++] = where;
    } else {
        tr->e.failed = 1;   /* (never: no fast path has more) */
    }
}

/* an instruction on [r12 + field]: a field of the task's */
static void task_field(Emit *e, unsigned rex, unsigned opcode, int reg, uint32_t field) {
    BYTES(e, (uint8_t)rex, (uint8_t)opcode, (uint8_t)(0x84 | reg << 3), 0x24);
    u32(e, field);
}

/* an instruction on [r12 + index*8 + disp] (index rax: a data stack cell; rcx: a return stack cell, by rp or lp) */
static void cell_op(Emit *e, const uint8_t *op, size_t n, int reg, int index, int32_t disp) {
    uint8_t sib = (uint8_t)(0xC0 | index << 3 | 4);

    byte(e, REX_W_B);
    bytes(e, op, n);
    if (disp >= -128 && disp <= 127) {
        BYTES(e, (uint8_t)(0x44 | reg << 3), sib, (uint8_t)(disp & 0xFF));
    } else {
        BYTES(e, (uint8_t)(0x84 | reg << 3), sib);
        u32(e, (uint32_t)disp);
    }
}
#define DS_OP(e, reg, disp, ...) cell_op((e), (const uint8_t[]){__VA_ARGS__}, \
                                         sizeof((const uint8_t[]){__VA_ARGS__}), (reg), RAX, (disp))
#define RS_OP(e, reg, disp, ...) cell_op((e), (const uint8_t[]){__VA_ARGS__}, \
                                         sizeof((const uint8_t[]){__VA_ARGS__}), (reg), RCX, (disp))

static void load_sp(Emit *e) { task_field(e, REX_B, MOV_LOAD, RAX, TASK_SP); }     /* mov eax, [sp] */
static void store_sp(Emit *e) { task_field(e, REX_B, MOV_STORE, RAX, TASK_SP); }   /* mov [sp], eax */
static void load_rp(Emit *e) { task_field(e, REX_B, MOV_LOAD, RCX, TASK_RP); }     /* mov ecx, [rp] */
static void store_rp(Emit *e) { task_field(e, REX_B, MOV_STORE, RCX, TASK_RP); }   /* mov [rp], ecx */

static void sp_add(Emit *e, int k) {
    if (k == 1) {
        BYTES(e, 0xFF, 0xC0);                          /* inc eax */
    } else if (k == -1) {
        BYTES(e, 0xFF, 0xC8);                          /* dec eax */
    } else if (k != 0) {
        BYTES(e, 0x83, 0xC0, (uint8_t)(k & 0xFF));     /* add eax, k */
    }
}

/* k items on the data stack (above its floor), or to the stub */
static void need_items(Translation *tr, Stub *s, int k) {
    Emit *e = &tr->e;

    BYTES(e, 0x89, 0xC1);                              /* mov ecx, eax */
    task_field(e, REX_B, 0x2B, RCX, TASK_SFLOOR);      /* sub ecx, [sfloor] */
    BYTES(e, 0x83, 0xF9, (uint8_t)k);                  /* cmp ecx, k */
    guard(tr, s, CC_L);
}

/* room for k more */
static void need_room(Translation *tr, Stub *s, int k) {
    BYTES(&tr->e, 0x3D);                               /* cmp eax, STACK_CELLS - k */
    u32(&tr->e, (uint32_t)(STACK_CELLS - k));
    guard(tr, s, CC_G);
}

/* a flag (-1 / 0) from the condition, in rdx */
static void flag_of(Emit *e, int cc) {
    BYTES(e, 0x0F, (uint8_t)(0x90 + cc), 0xC2);        /* setcc dl */
    BYTES(e, 0x0F, 0xB6, 0xD2);                        /* movzx edx, dl */
    BYTES(e, REX_W, 0xF7, 0xDA);                       /* neg rdx */
}

static int is_compare(int fast) { return fast >= FAST_EQ && fast <= FAST_UGT; }
static int is_zero_compare(int fast) { return fast >= FAST_0EQ && fast <= FAST_0GT; }

static int compare_cc(int fast) {
    static const int ccs[] = {CC_E, CC_NE, CC_L, CC_G, CC_LE, CC_GE, CC_B, CC_A};
    static const int zero_ccs[] = {CC_E, CC_NE, CC_L, CC_G};

    if (is_compare(fast)) {
        return ccs[fast - FAST_EQ];
    }
    return is_zero_compare(fast) ? zero_ccs[fast - FAST_0EQ] : CC_E;   /* (never another: the callers ask) */
}

/* the ALU instruction on a cell, with an immediate (81 /ext id) and with a register (op r/m64, r64) */
static const struct { int fast; uint8_t ext, op; } alu[] = {
    {FAST_ADD, 0, 0x01}, {FAST_OR, 1, 0x09}, {FAST_AND, 4, 0x21}, {FAST_SUB, 5, 0x29}, {FAST_XOR, 6, 0x31},
};
static int alu_of(int fast) {
    size_t k;

    for (k = 0; k < sizeof(alu) / sizeof(alu[0]); k++) {
        if (alu[k].fast == fast) {
            return (int)k;
        }
    }
    return -1;
}

static int fast_of(Translation *tr, const Word *w) {
    struct ForthNative *nat = tr->f->native;
    int k;

    for (k = 0; k < FAST_WORDS; k++) {
        if (nat->fast[k] == w) {
            return k;
        }
    }
    return -1;
}

static size_t next_op(Translation *tr, size_t i) {
    return i + 1 + operand_cells(tr->f, (Word *)tr->body[i], &tr->body[i]);
}

/* can the word at cell j be joined to the one before it (reached, and no branch lands on it)? */
static int joinable(Translation *tr, size_t j) { return j < tr->n && tr->op[j] && !tr->target[j]; }

static int fits_imm32(Cell x) { return x >= INT32_MIN && x <= INT32_MAX; }

/* a literal x followed by an ALU word, a comparison or `*` on it: one instruction with x in it */
static int fast_literal_op(Translation *tr, Stub *s, Cell x, int fast) {
    Emit *e = &tr->e;
    int k = alu_of(fast);

    load_sp(e);
    need_items(tr, s, 1);
    if (k >= 0) {
        DS_OP(e, alu[k].ext, TOP(-1), 0x81);           /* op qword [top], x */
        u32(e, (uint32_t)x);
    } else if (fast == FAST_MUL) {
        DS_OP(e, RCX, TOP(-1), 0x69);                  /* imul rcx, [top], x */
        u32(e, (uint32_t)x);
        DS_OP(e, RCX, TOP(-1), MOV_STORE);
    } else {
        DS_OP(e, 7, TOP(-1), 0x81);                    /* cmp qword [top], x */
        u32(e, (uint32_t)x);
        flag_of(e, compare_cc(fast));
        DS_OP(e, RDX, TOP(-1), MOV_STORE);
    }
    return 1;
}

/* a comparison and the 0branch after it: compare, then jump where the flag would be false */
static int fast_compare_branch(Translation *tr, Stub *s, int fast, size_t to, int literal, Cell x) {
    Emit *e = &tr->e;
    int taken = literal || is_zero_compare(fast) ? 1 : 2;

    load_sp(e);
    need_items(tr, s, taken);
    if (literal) {
        DS_OP(e, 7, TOP(-1), 0x81);                    /* cmp qword [top], x */
        u32(e, (uint32_t)x);
    } else if (is_zero_compare(fast)) {
        DS_OP(e, 7, TOP(-1), 0x83);                    /* cmp qword [top], 0 */
        byte(e, 0);
    } else {
        DS_OP(e, RCX, TOP(-2), MOV_LOAD);              /* mov rcx, [second] */
        DS_OP(e, RCX, TOP(-1), 0x3B);                  /* cmp rcx, [top] */
    }
    BYTES(e, 0x8D, 0x40, (uint8_t)(-taken & 0xFF));    /* lea eax, [rax - taken] (the flags kept for the jump) */
    store_sp(e);
    return 1;   /* (the conditional jump: the caller's) */
}

static int fast_word(Translation *tr, Stub *s, int fast) {
    Emit *e = &tr->e;
    int k = alu_of(fast);

    if (fast == FAST_I || fast == FAST_RFETCH) {       /* the return stack's top (i: a loop's index, 3 deep) */
        load_rp(e);
        BYTES(e, 0x83, 0xF9, fast == FAST_I ? 3 : 1);  /* cmp ecx, needed */
        guard(tr, s, CC_L);
        load_sp(e);
        need_room(tr, s, 1);
        RS_OP(e, RCX, TASK_RS - CELL_BYTES, MOV_LOAD); /* mov rcx, [rs + rp*8 - 8] */
        DS_OP(e, RCX, TOP(0), MOV_STORE);
        sp_add(e, 1);
        store_sp(e);
        return 1;
    }
    if (fast == FAST_TOR) {
        load_sp(e);
        need_items(tr, s, 1);
        load_rp(e);
        BYTES(e, 0x81, 0xF9);                          /* cmp ecx, STACK_CELLS */
        u32(e, STACK_CELLS);
        guard(tr, s, CC_GE);
        DS_OP(e, RDX, TOP(-1), MOV_LOAD);
        RS_OP(e, RDX, TASK_RS, MOV_STORE);             /* mov [rs + rp*8], rdx */
        BYTES(e, 0xFF, 0xC1);                          /* inc ecx */
        store_rp(e);
        sp_add(e, -1);
        store_sp(e);
        return 1;
    }
    if (fast == FAST_RFROM) {
        load_rp(e);
        BYTES(e, 0x85, 0xC9);                          /* test ecx, ecx */
        guard(tr, s, CC_LE);
        load_sp(e);
        need_room(tr, s, 1);
        BYTES(e, 0xFF, 0xC9);                          /* dec ecx */
        RS_OP(e, RDX, TASK_RS, MOV_LOAD);
        DS_OP(e, RDX, TOP(0), MOV_STORE);
        store_rp(e);
        sp_add(e, 1);
        store_sp(e);
        return 1;
    }
    load_sp(e);
    switch (fast) {
    case FAST_DUP:
        need_items(tr, s, 1);
        need_room(tr, s, 1);
        DS_OP(e, RCX, TOP(-1), MOV_LOAD);
        DS_OP(e, RCX, TOP(0), MOV_STORE);
        sp_add(e, 1);
        break;
    case FAST_DROP:
        need_items(tr, s, 1);
        sp_add(e, -1);
        break;
    case FAST_SWAP:
        need_items(tr, s, 2);
        DS_OP(e, RCX, TOP(-1), MOV_LOAD);
        DS_OP(e, RDX, TOP(-2), MOV_LOAD);
        DS_OP(e, RDX, TOP(-1), MOV_STORE);
        DS_OP(e, RCX, TOP(-2), MOV_STORE);
        return 1;   /* (sp as it was) */
    case FAST_OVER:
        need_items(tr, s, 2);
        need_room(tr, s, 1);
        DS_OP(e, RCX, TOP(-2), MOV_LOAD);
        DS_OP(e, RCX, TOP(0), MOV_STORE);
        sp_add(e, 1);
        break;
    case FAST_NIP:
        need_items(tr, s, 2);
        DS_OP(e, RCX, TOP(-1), MOV_LOAD);
        DS_OP(e, RCX, TOP(-2), MOV_STORE);
        sp_add(e, -1);
        break;
    case FAST_MUL:
        need_items(tr, s, 2);
        DS_OP(e, RCX, TOP(-2), MOV_LOAD);
        DS_OP(e, RCX, TOP(-1), 0x0F, 0xAF);            /* imul rcx, [top] */
        DS_OP(e, RCX, TOP(-2), MOV_STORE);
        sp_add(e, -1);
        break;
    case FAST_1ADD: case FAST_1SUB: case FAST_INVERT: case FAST_NEGATE: case FAST_CELLS:
        need_items(tr, s, 1);
        if (fast == FAST_1ADD || fast == FAST_1SUB) {
            DS_OP(e, fast == FAST_1ADD ? 0 : 1, TOP(-1), 0xFF);   /* inc / dec qword [top] */
        } else if (fast == FAST_CELLS) {
            DS_OP(e, 4, TOP(-1), 0xC1);                /* shl qword [top], 3 */
            byte(e, 3);
        } else {
            DS_OP(e, fast == FAST_INVERT ? 2 : 3, TOP(-1), 0xF7);   /* not / neg qword [top] */
        }
        return 1;
    default:
        if (k >= 0) {   /* + - and or xor */
            need_items(tr, s, 2);
            DS_OP(e, RCX, TOP(-1), MOV_LOAD);
            DS_OP(e, RCX, TOP(-2), alu[k].op);         /* op [second], rcx */
            sp_add(e, -1);
        } else if (is_compare(fast)) {
            need_items(tr, s, 2);
            DS_OP(e, RCX, TOP(-2), MOV_LOAD);
            DS_OP(e, RCX, TOP(-1), 0x3B);              /* cmp rcx, [top] */
            flag_of(e, compare_cc(fast));
            DS_OP(e, RDX, TOP(-2), MOV_STORE);
            sp_add(e, -1);
        } else {   /* 0= 0<> 0< 0> */
            need_items(tr, s, 1);
            DS_OP(e, 7, TOP(-1), 0x83);                /* cmp qword [top], 0 */
            byte(e, 0);
            flag_of(e, compare_cc(fast));
            DS_OP(e, RDX, TOP(-1), MOV_STORE);
            return 1;
        }
        break;
    }
    store_sp(e);
    return 1;
}

/* the compiler's words: a literal, a local's fetch and store, 0branch, (loop), exit; -1 none of these */
static int fast_compiled(Translation *tr, Stub *s, size_t i) {
    Forth *f = tr->f;
    Emit *e = &tr->e;
    Word *w = (Word *)tr->body[i];
    Cell operand = tr->body[i + 1];

    if (w == f->w_lit) {
        load_sp(e);
        need_room(tr, s, 1);
        mov_rcx(e, (uint64_t)operand);
        DS_OP(e, RCX, TOP(0), MOV_STORE);
        sp_add(e, 1);
        store_sp(e);
        return 1;
    }
    if (w == f->w_local_fetch || w == f->w_local_store) {   /* rs[lp + i]: no check, as the primitive has none */
        int32_t at = TASK_RS + (int32_t)operand * CELL_BYTES;

        load_sp(e);
        if (w == f->w_local_fetch) {
            need_room(tr, s, 1);
        } else {
            need_items(tr, s, 1);
            DS_OP(e, RDX, TOP(-1), MOV_LOAD);
        }
        task_field(e, REX_W_B, 0x63, RCX, TASK_LP);    /* movsxd rcx, [lp] */
        if (w == f->w_local_fetch) {
            RS_OP(e, RDX, at, MOV_LOAD);
            DS_OP(e, RDX, TOP(0), MOV_STORE);
            sp_add(e, 1);
        } else {
            RS_OP(e, RDX, at, MOV_STORE);
            sp_add(e, -1);
        }
        store_sp(e);
        return 1;
    }
    if (w == f->w_0branch) {
        load_sp(e);
        need_items(tr, s, 1);
        sp_add(e, -1);
        store_sp(e);
        DS_OP(e, RCX, TOP(0), MOV_LOAD);               /* (the flag just taken) */
        BYTES(e, REX_W, 0x85, 0xC9);                   /* test rcx, rcx */
        return fixup(tr, jump(e, CC_E), target_of(tr, i));
    }
    if (w == f->w_loop) {   /* as loop_step by 1: the index one on, done as it crosses limit - 1 to limit */
        size_t done;

        load_rp(e);
        BYTES(e, 0x83, 0xF9, 3);                       /* cmp ecx, 3 */
        guard(tr, s, CC_L);
        RS_OP(e, RDX, TASK_RS - CELL_BYTES, MOV_LOAD);      /* rdx: the index */
        RS_OP(e, RSI, TASK_RS - 2 * CELL_BYTES, MOV_LOAD);  /* rsi: the limit */
        BYTES(e, REX_W, 0x89, 0xD7);                   /* mov rdi, rdx */
        BYTES(e, REX_W, 0x29, 0xF7);                   /* sub rdi, rsi: d */
        BYTES(e, REX_W, 0xFF, 0xC2);                   /* inc rdx */
        RS_OP(e, RDX, TASK_RS - CELL_BYTES, MOV_STORE);
        BYTES(e, REX_W, 0x89, 0xFE);                   /* mov rsi, rdi */
        BYTES(e, REX_W, 0x83, 0xC6, 0x01);             /* add rsi, 1: d + 1 */
        BYTES(e, REX_W, 0x31, 0xF7);                   /* xor rdi, rsi: the sign changed? */
        done = jump(e, CC_S);
        count_step(e, &tr->body[i + 1]);
        if (!fixup(tr, jump(e, CC_ALWAYS), target_of(tr, i))) {
            return 0;
        }
        patch(e, done, e->n);
        load_rp(e);                                    /* (count_step's call may have taken rcx) */
        BYTES(e, 0x83, 0xE9, 3);                       /* sub ecx, 3 */
        store_rp(e);
        return 1;
    }
    if (w == f->w_exit) {
        load_rp(e);
        BYTES(e, 0x85, 0xC9);                          /* test ecx, ecx */
        guard(tr, s, CC_LE);
        BYTES(e, 0xFF, 0xC9);                          /* dec ecx */
        store_rp(e);
        leave_word(e);
        return 1;
    }
    return -1;
}

/* the word at cell i (and those joined to it) by a fast path, with its stub: the cell after them; 0 if it has none
 * (then the generic translation) */
static size_t translate_fast(Translation *tr, size_t i) {
    Forth *f = tr->f;
    Emit *e = &tr->e;
    Word *w = (Word *)tr->body[i];
    size_t j = next_op(tr, i), k = joinable(tr, j) ? next_op(tr, j) : j, end;   /* (j, k: words only if reached) */
    int fast = fast_of(tr, w), next = joinable(tr, j) ? fast_of(tr, (Word *)tr->body[j]) : -1, r;
    Stub *s;

    if (f->native->inline_off) {
        return 0;
    }
    if (w == f->w_lit && joinable(tr, j) && fits_imm32(tr->body[i + 1]) && is_compare(next) && joinable(tr, k) &&
        (Word *)tr->body[k] == f->w_0branch) {   /* x <  0branch */
        end = next_op(tr, k);
        if ((s = stub(tr, i, end)) == NULL) {
            return 0;
        }
        fast_compare_branch(tr, s, next, 0, 1, tr->body[i + 1]);
        r = fixup(tr, jump(e, compare_cc(next) ^ 1), target_of(tr, k));   /* (x86's conditions pair: ^1) */
    } else if (w == f->w_lit && joinable(tr, j) && fits_imm32(tr->body[i + 1]) &&
               (alu_of(next) >= 0 || next == FAST_MUL || is_compare(next))) {   /* x +  */
        end = k;
        if ((s = stub(tr, i, end)) == NULL) {
            return 0;
        }
        r = fast_literal_op(tr, s, tr->body[i + 1], next);
    } else if ((is_compare(fast) || is_zero_compare(fast)) && joinable(tr, j) &&
               (Word *)tr->body[j] == f->w_0branch) {   /* <  0branch */
        end = k;
        if ((s = stub(tr, i, end)) == NULL) {
            return 0;
        }
        fast_compare_branch(tr, s, fast, 0, 0, 0);
        r = fixup(tr, jump(e, compare_cc(fast) ^ 1), target_of(tr, j));
    } else if (fast >= 0) {
        end = j;
        if ((s = stub(tr, i, end)) == NULL) {
            return 0;
        }
        r = fast_word(tr, s, fast);
    } else if ((w->flags & WORD_COMPILED_ONLY) || w == f->w_exit) {
        end = j;
        if ((s = stub(tr, i, end)) == NULL) {
            return 0;
        }
        if ((r = fast_compiled(tr, s, i)) < 0) {
            tr->nstubs--;   /* (no fast path after all) */
            return 0;
        }
    } else {
        return 0;
    }
    if (r == 0) {
        e->failed = 1;
        return 0;
    }
    s->back = e->n;
    return end;
}

/* the stubs, after the word's code: the generic translation of what their fast path does, then back */
static int translate_stubs(Translation *tr) {
    Emit *e = &tr->e;
    size_t k, i;
    int g;

    for (k = 0; k < tr->nstubs; k++) {
        Stub *s = &tr->stubs[k];

        for (g = 0; g < s->nguards; g++) {
            patch(e, s->guards[g], e->n);
        }
        for (i = s->first; i < s->end; i = next_op(tr, i)) {
            if (!translate_generic(tr, i)) {
                return 0;
            }
        }
        patch(e, jump(e, CC_ALWAYS), s->back);
    }
    return 1;
}

static int translate(Translation *tr) {
    Emit *e = &tr->e;
    size_t i, end;

    if (!find_ops(tr)) {
        return 0;
    }
    outer_entry(e);
    if (e->n != INNER_ENTRY) {
        return 0;
    }
    BYTES(e, 0x48, 0x83, 0xEC, 0x08);   /* sub rsp, 8 (calls from here on 16-aligned) */
    count_step(e, tr->body);
    for (i = 0; i < tr->n; i++) {
        if (!tr->op[i]) {
            continue;
        }
        tr->label[i] = e->n;
        if ((end = translate_fast(tr, i)) != 0) {
            i = end - 1;   /* (past the words joined to it) */
        } else if (e->failed || !translate_generic(tr, i)) {
            return 0;
        }
    }
    if (!translate_stubs(tr)) {
        return 0;
    }
    for (i = 0; i < tr->nfixups; i++) {
        if (!tr->op[tr->fixups[i].to]) {
            return 0;
        }
        patch(e, tr->fixups[i].where, tr->label[tr->fixups[i].to]);
    }
    return !e->failed;
}

void forth_native_compile(Forth *f, Word *w, const Cell *end) {
    Translation tr;
    uint8_t *code;
    int ok;

    if (f->native == NULL || w->code == forth_donative || end <= w->body) {
        return;
    }
    memset(&tr, 0, sizeof(tr));
    tr.f = f;
    tr.self = w;
    tr.body = w->body;
    tr.n = (size_t)(end - w->body);
    tr.op = calloc(tr.n, 1);
    tr.label = calloc(tr.n, sizeof(size_t));
    tr.loop_exit = calloc(tr.n, sizeof(size_t));
    tr.target = calloc(tr.n, 1);
    ok = tr.op != NULL && tr.label != NULL && tr.loop_exit != NULL && tr.target != NULL && translate(&tr);
    if (ok && (code = code_space(f, tr.e.n)) != NULL) {
        memcpy(code, tr.e.buf, tr.e.n);
        w->does = (Cell *)code;   /* (a colon definition's does is otherwise unused) */
        w->code = forth_donative;
        f->native->translated++;
    } else {
        f->native->refused++;
    }
    free(tr.op);
    free(tr.label);
    free(tr.loop_exit);
    free(tr.target);
    free(tr.stubs);
    free(tr.fixups);
    free(tr.e.buf);
}

/* the code field of a word with machine code: run it, as docol would its threaded code (where the caller goes on,
 * pushed; `exit` takes it off) */
void forth_donative(Forth *f, Word *w) {
    Cell *ip = f->t->ip;
    NativeEntry entry;

    rpush(f, (Cell)ip);
    memcpy(&entry, &w->does, sizeof(entry));
    entry(f);
    f->t->ip = ip;
}

static void p_native_q(Forth *f, Word *w) {   /* native? ( xt -- flag ) has it machine code? */
    forth_push(f, forth_xt_arg(f, forth_pop(f), "native?")->code == forth_donative ? -1 : 0);
}
static void p_dot_native(Forth *f, Word *w) {   /* .native ( -- ) how many definitions have it, how many not */
    if (f->native == NULL) {
        forth_printf(f, "the native backend is off (CLANKFORTH_THREADED): every definition threaded\n");
        return;
    }
    forth_printf(f, "%ld definitions translated to machine code, %ld left threaded\n", f->native->translated,
                 f->native->refused);
}
void forth_native_words(Forth *f) {
    int k;

    if (f->native != NULL) {   /* (the core's, by name: a script's own word of that name is called) */
        for (k = 0; k < FAST_WORDS; k++) {
            f->native->fast[k] = forth_in_vocab(f->core, fast_names[k], strlen(fast_names[k]));
        }
        f->native->inline_off = getenv("CLANKFORTH_CALLS_ONLY") != NULL;
    }
    forth_set_effect(f, forth_prim(f, "native?", p_native_q), "( xt -- flag )");
    forth_set_effect(f, forth_prim(f, ".native", p_dot_native), "( -- )");
}

/* ---- tasks as coroutines ---- */

enum { CO_FRESH, CO_RUNNING, CO_SUSPENDED, CO_DONE, CO_FAILED };

struct ForthCoroutine {
    uint8_t *stack;         /* its mapping: the guard page, then the stack */
    size_t bytes;
    void *sp;               /* its stack pointer while it is switched out */
    void *caller_sp;        /* the scheduler's while it runs */
    jmp_buf entry;          /* its start: `restart` comes back here, its frames dropped */
    jmp_buf failed;         /* an error inside it unwinds to here, on its own stack */
    int state;              /* CO_* */
#ifdef NATIVE_ASAN
    void *caller_fake, *own_fake;   /* (the sanitizer's: each side's fake stack while switched out) */
    const void *caller_bottom;
    size_t caller_size;
#endif
};

/* the switch: the callee-saved registers pushed, the stack pointer saved in *from, `to` taken, its registers popped -
 * the last a return into where it left (or, the first time, into co_trampoline) */
void clank_co_switch(void **from, void *to);
void clank_co_trampoline(void);
__attribute__((used, visibility("hidden"))) void clank_co_main(Forth *f);
__asm__(
    ".text\n"
    ".globl clank_co_switch\n"
    ".hidden clank_co_switch\n"
    ".type clank_co_switch, @function\n"
    "clank_co_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    ".size clank_co_switch, .-clank_co_switch\n"
    ".globl clank_co_trampoline\n"
    ".hidden clank_co_trampoline\n"
    ".type clank_co_trampoline, @function\n"
    "clank_co_trampoline:\n"   /* (the Forth in rbx, put there by co_prepare) */
    "    movq %rbx, %rdi\n"
    "    call clank_co_main\n"
    "    ud2\n"
    ".size clank_co_trampoline, .-clank_co_trampoline\n");

#define CO_SAVED_REGISTERS 6   /* rbp rbx r12 r13 r14 r15 */
#define CO_SAVED_RBX 4         /* (from the stack pointer: r15 r14 r13 r12 rbx) */

static void co_prepare(struct ForthCoroutine *co, Forth *f) {
    void **sp = (void **)(co->stack + co->bytes);   /* (page-aligned: the return slot below lands 8 off 16) */
    void (*trampoline)(void) = clank_co_trampoline;
    int i;

#ifdef NATIVE_ASAN
    /* (the frames of its last run were dropped, not returned from: their red zones still marked) */
    __asan_unpoison_memory_region(co->stack + CO_GUARD_BYTES, co->bytes - CO_GUARD_BYTES);
#endif
    sp -= 1;
    memcpy(sp, &trampoline, sizeof(trampoline));
    for (i = 0; i < CO_SAVED_REGISTERS; i++) {
        *--sp = NULL;
    }
    sp[CO_SAVED_RBX] = f;
    co->sp = sp;
}

static void switch_into(struct ForthCoroutine *co) {
#ifdef NATIVE_ASAN
    __sanitizer_start_switch_fiber(&co->caller_fake, co->stack + CO_GUARD_BYTES, co->bytes - CO_GUARD_BYTES);
#endif
    clank_co_switch(&co->caller_sp, co->sp);
#ifdef NATIVE_ASAN
    __sanitizer_finish_switch_fiber(co->caller_fake, NULL, NULL);
#endif
}

static void switch_out(struct ForthCoroutine *co, int for_good) {
#ifdef NATIVE_ASAN
    __sanitizer_start_switch_fiber(for_good ? NULL : &co->own_fake, co->caller_bottom, co->caller_size);
#else
    (void)for_good;
#endif
    clank_co_switch(&co->sp, co->caller_sp);
#ifdef NATIVE_ASAN
    __sanitizer_finish_switch_fiber(co->own_fake, &co->caller_bottom, &co->caller_size);
#endif
}

/* a coroutine's first frame, on its own stack: the task's threaded code from its start (a word with machine code
 * runs that) until it ends or fails - then back to the scheduler for good. `restart` comes back to `entry`. */
void clank_co_main(Forth *f) {
    struct ForthCoroutine *volatile co = f->t->co;

#ifdef NATIVE_ASAN
    __sanitizer_finish_switch_fiber(NULL, &co->caller_bottom, &co->caller_size);
#endif
    if (setjmp(co->failed) == 0) {
        setjmp(co->entry);
        f->catch = &co->failed;
        forth_run(f);
        co->state = CO_DONE;
    } else {
        co->state = CO_FAILED;
    }
    switch_out(co, 1);
    abort();   /* (never switched back into: started again, it has a new first frame) */
}

static struct ForthCoroutine *coroutine_new(Forth *f, Task *t) {
    struct ForthCoroutine *co = calloc(1, sizeof(*co));
    void *m;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
#ifdef MAP_STACK
    flags |= MAP_STACK;
#endif
    if (co == NULL) {
        forth_error(f, "task %s: out of memory for its stack", t->name);
    }
    m = mmap(NULL, CO_STACK_BYTES, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (m == MAP_FAILED) {
        free(co);
        forth_error(f, "task %s: out of memory for its stack", t->name);
    }
    mprotect(m, CO_GUARD_BYTES, PROT_NONE);
    co->stack = m;
    co->bytes = CO_STACK_BYTES;
    co->state = CO_FRESH;
    t->co = co;
    return co;
}

int forth_native_turn(Forth *f, Task *t) {
    jmp_buf *outer = f->catch;
    struct ForthCoroutine *co;

    if (f->native == NULL) {
        return 0;
    }
    co = t->co != NULL ? t->co : coroutine_new(f, t);
    if (co->state != CO_SUSPENDED) {
        co_prepare(co, f);
    }
    co->state = CO_RUNNING;
    f->catch = &co->failed;
    switch_into(co);
    f->catch = outer;
    if (co->state == CO_FAILED) {   /* (the error said, inside: on to the turn's own unwinding, on this stack) */
        co->state = CO_FRESH;
        longjmp(*outer, 1);
    }
    f->yielded = co->state == CO_SUSPENDED;
    return 1;
}

void forth_native_suspend(Forth *f) {
    struct ForthCoroutine *co = f->t->co;

    if (co == NULL || co->state != CO_RUNNING) {   /* (no coroutine: the inner interpreter sees `yielded`) */
        return;
    }
    co->state = CO_SUSPENDED;
    switch_out(co, 0);
    f->yielded = 0;   /* (its turn again: going on) */
}

void forth_native_restart(Forth *f) {
    struct ForthCoroutine *co = f->t->co;

    if (co != NULL && co->state == CO_RUNNING) {
        longjmp(co->entry, 1);
    }
}

void forth_native_task_reset(Forth *f, Task *t) {
    (void)f;
    if (t->co != NULL && t->co->state != CO_RUNNING) {
        t->co->state = CO_FRESH;
    }
}

void forth_native_task_free(Forth *f, Task *t) {
    (void)f;
    if (t->co != NULL) {
        munmap(t->co->stack, t->co->bytes);
        free(t->co);
        t->co = NULL;
    }
}

void forth_native_init(Forth *f) {
    f->native = getenv("CLANKFORTH_THREADED") == NULL ? calloc(1, sizeof(struct ForthNative)) : NULL;
}

void forth_native_free(Forth *f) {
    CodeChunk *c, *next;

    if (f->native == NULL) {
        return;
    }
    for (c = f->native->chunks; c != NULL; c = next) {
        next = c->next;
        munmap(c->start, (size_t)(c->end - c->start));
        free(c);
    }
    free(f->native);
    f->native = NULL;
}

#endif

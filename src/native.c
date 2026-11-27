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
 * Built for x86-64: with the System V calling convention (Linux, the BSDs: ELF), or Windows' (built with gcc or clang
 * - MinGW - for the GNU assembler; a task's stack is a fiber there). Elsewhere (MSVC, another machine), or with the
 * environment variable CLANKFORTH_THREADED set, every word stays threaded and these functions do nothing. */
#include "forth_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(CLANKFORTH_NATIVE) && defined(__x86_64__) && defined(__ELF__)
#define NATIVE_X86_64 1
#elif defined(CLANKFORTH_NATIVE) && defined(__x86_64__) && defined(_WIN64) && defined(__GNUC__)
#define NATIVE_X86_64 1
#define NATIVE_WIN64 1
#endif

#ifndef NATIVE_X86_64

void forth_native_init(Forth *f) { f->native = NULL; }
void forth_native_free(Forth *f) { (void)f; }
void forth_native_compile(Forth *f, Word *w, const Cell *end) { (void)f; (void)w; (void)end; }
void forth_donative(Forth *f, Word *w) { forth_error(f, "%s: native code, in a system without it", w->name); }
int forth_native_forward(Forth *f, Word *old, Word *to) { (void)f, (void)old, (void)to; return 0; }
void forth_native_redefined(Forth *f, const Word *old) { (void)f; (void)old; }
void forth_native_forgot(Forth *f, const uint8_t *p) { (void)f; (void)p; }
static void p_native_q(Forth *f, Word *w) { (void)forth_xt_arg(f, forth_pop(f), "native?"); forth_push(f, 0); }
static void p_dot_native(Forth *f, Word *w) { forth_printf(f, "no native backend: every definition threaded\n"); }
static void p_native_inlined(Forth *f, Word *w) { (void)forth_xt_arg(f, forth_pop(f), "native-inlined"); forth_push(f, 0); }
static void p_native_bytes(Forth *f, Word *w) { (void)forth_xt_arg(f, forth_pop(f), "native-bytes"); forth_push(f, 0); }
void forth_native_bind(Forth *f) { (void)f; }
void forth_native_words(Forth *f) {
    forth_set_effect(f, forth_prim(f, "native?", p_native_q), "( xt -- flag )");
    forth_set_effect(f, forth_prim(f, ".native", p_dot_native), "( -- )");
    forth_set_effect(f, forth_prim(f, "native-inlined", p_native_inlined), "( xt -- n )");
    forth_set_effect(f, forth_prim(f, "native-bytes", p_native_bytes), "( xt -- n )");
}
int forth_native_turn(Forth *f, Task *t) { (void)f; (void)t; return 0; }
void forth_native_suspend(Forth *f) { (void)f; }
int forth_native_in_coroutine(const Forth *f) { (void)f; return 0; }
void forth_native_restart(Forth *f) { (void)f; }
void forth_native_task_reset(Forth *f, Task *t) { (void)f; (void)t; }
void forth_native_task_free(Forth *f, Task *t) { (void)f; (void)t; }
void forth_native_bases(Forth *f, int *depth, int *silent) { (void)f; *depth = *silent = 0; }

#else

#include <stddef.h>
#include <stdio.h>
#ifdef NATIVE_WIN64
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   /* (Vista's: IsThreadAFiber) */
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

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
#define CO_POOL_MAX 64                /* stacks kept for the next turns (a task holds one only while it waits) */
#define WORD_CODE_AT ((uint8_t)offsetof(Word, code))   /* a Word's code field: a byte's displacement from it */
#define CODE_MAP_UNNAMED "[: ;]"       /* a quotation in CLANKFORTH_CODE_MAP's list: it has no name */
#define FORWARD_BYTES 12              /* a jump written over a word's inner entry: movabs rax, imm64; jmp rax */
_Static_assert(offsetof(Word, code) < 0x80, "a Word's code field within a byte's displacement");

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
    FAST_0EQ, FAST_0NE, FAST_0LT, FAST_0GT, FAST_0LE, FAST_0GE,
    FAST_1ADD, FAST_1SUB, FAST_INVERT, FAST_NEGATE, FAST_CELLS,
    FAST_I, FAST_RFETCH, FAST_TOR, FAST_RFROM,
    FAST_FETCH, FAST_CFETCH, FAST_STORE, FAST_CSTORE,
    FAST_TRUE, FAST_FALSE, FAST_WITHIN, FAST_J,
    FAST_WORDS
};
static const char *const fast_names[FAST_WORDS] = {
    "dup", "drop", "swap", "over", "nip",
    "+", "-", "and", "or", "xor", "*",
    "=", "<>", "<", ">", "<=", ">=", "u<", "u>",
    "0=", "0<>", "0<", "0>", "0<=", "0>=",
    "1+", "1-", "invert", "negate", "cells",
    "i", "r@", ">r", "r>",
    "@", "c@", "!", "c!",
    "true", "false", "within", "j",
};

struct ForthNative {
    CodeChunk *chunks;      /* the newest first */
    long translated;        /* definitions given machine code */
    long refused;           /* and those left threaded */
    Word *fast[FAST_WORDS]; /* the core's words of those names */
    Word *rdrop;            /* (and rdrop: with r>, what may take a caller's return address) */
    int inline_off;         /* (CLANKFORTH_CALLS_ONLY set: every word called, none in line - to compare) */
    int stubs_only;         /* (CLANKFORTH_STUBS_ONLY set: a run's paths each go by their stub - to compare) */
    int converted;          /* (Windows) this Forth's tasks made its thread a fiber: undone as it is freed */
    uint8_t *pool[CO_POOL_MAX];   /* (ELF) tasks' stacks not in use, for the next turns */
    int npool;
    FILE *map;              /* (CLANKFORTH_CODE_MAP set) where each definition's code is listed; NULL: nowhere */
    struct Inlined *inlined;   /* each definition written into another's code (inline_call), with that other */
    size_t ninlined, inlined_cap;
};

/* `callee`'s words were written into `caller`'s machine code, not called: if callee is defined again, caller's code
 * is made anew (forth_native_redefined) - end: where caller's threaded code ends, to translate it again */
typedef struct Inlined {
    const Word *callee;
    Word *caller;
    const Cell *end;
} Inlined;

/* memory machine code is run from (NULL: none) - never writable and runnable at once (W^X): read and run, made
 * writable only for the moment a word's code is copied in (code_open, code_close: the pages it touches), so a stray
 * write elsewhere can't become code, and a system refusing memory both writable and runnable runs it */
#define PAGE_BYTES_ASSUMED 4096   /* (where the system doesn't say: the smallest page there is) */
#ifdef NATIVE_WIN64
static void *code_map(size_t bytes) { return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READ); }
static void code_unmap(void *m, size_t bytes) { (void)bytes; VirtualFree(m, 0, MEM_RELEASE); }
static size_t page_bytes(void) {
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    return si.dwPageSize > 0 ? si.dwPageSize : PAGE_BYTES_ASSUMED;
}
static int code_protect(void *p, size_t n, int writable) {
    DWORD old;

    if (!VirtualProtect(p, n, writable ? PAGE_READWRITE : PAGE_EXECUTE_READ, &old)) {
        return 0;
    }
    if (!writable) {
        FlushInstructionCache(GetCurrentProcess(), p, n);
    }
    return 1;
}
#else
static void *code_map(size_t bytes) {
    void *m = mmap(NULL, bytes, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return m != MAP_FAILED ? m : NULL;
}
static void code_unmap(void *m, size_t bytes) { munmap(m, bytes); }
static size_t page_bytes(void) {
    long n = sysconf(_SC_PAGESIZE);

    return n > 0 ? (size_t)n : PAGE_BYTES_ASSUMED;
}
static int code_protect(void *p, size_t n, int writable) {
    return mprotect(p, n, writable ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC) == 0;
}
#endif
/* the pages under [p, p+n): writable (and not runnable), or runnable again (and not writable) */
static int code_pages(uint8_t *p, size_t n, int writable) {
    size_t page = page_bytes();
    uintptr_t from = (uintptr_t)p & ~(uintptr_t)(page - 1), to = ((uintptr_t)p + n + page - 1) & ~(uintptr_t)(page - 1);

    return code_protect((void *)from, (size_t)(to - from), writable);
}

static uint8_t *code_space(Forth *f, size_t n) {
    struct ForthNative *nat = f->native;
    CodeChunk *c = nat->chunks;
    uint8_t *p;

    n = (n + CODE_ALIGN - 1) & ~(size_t)(CODE_ALIGN - 1);
    if (c == NULL || (size_t)(c->end - c->used) < n) {
        size_t bytes = n > CODE_CHUNK_BYTES ? n : CODE_CHUNK_BYTES;
        void *m = code_map(bytes);

        if (m == NULL || (c = malloc(sizeof(CodeChunk))) == NULL) {
            if (m != NULL) {
                code_unmap(m, bytes);
            }
            return NULL;
        }
        c->start = c->used = m;
        c->end = c->start + bytes;
        if (n > CODE_CHUNK_BYTES && nat->chunks != NULL) {   /* (a word of its own: the newest block stays first) */
            c->next = nat->chunks->next;
            nat->chunks->next = c;
        } else {
            c->next = nat->chunks;
            nat->chunks = c;
        }
    }
    p = c->used;
    c->used += n;
    return p;
}

/* ---- machine code: writing it (x86-64) ---- */

enum { RUN_OFF, RUN_MEASURED, RUN_ON };   /* (a run of fast paths: translate_run) */

#define CACHE_MAX 2   /* the data stack's top items kept in registers, at most (rsi and rdi) */

/* the data stack as a fast path's code is written: where its items are, from where eax has sp */
typedef struct Stacked {
    int depth;            /* the items more than as the path - or its run - began (fewer: negative) */
    int n;                /* the top n of them are in registers, their cells not yet written */
    int reg[CACHE_MAX];   /* ... which: reg[0] the top's */
    int settled;          /* all of it written back, the registers and sp: the path, or the run, has ended */
    int lp;               /* r8 has the locals frame (the task's lp), read for a local before */
} Stacked;

typedef struct Emit {
    uint8_t *buf;
    size_t n, cap;
    int failed;   /* out of memory: the word stays threaded */
    /* a run of fast paths, one after another with nothing between: the data stack's depth is kept in eax from one
     * to the next, its top in registers, and what they need of the stack - so many items, so much room - checked
     * once, as the run begins */
    int run;        /* RUN_OFF; RUN_MEASURED: written once to find what it needs (and unwritten); RUN_ON: written */
    Stacked st;     /* the stack as the path being written finds it */
    int looped;     /* r13 and r14 have the return stack's top two cells - a do loop's index and its limit - and
                     * there are the three cells of a loop there: whatever way the code here is come to */
    int need, room; /* (measured) the items the run needs under it as it begins, the room over it */
} Emit;

static void emit_byte(Emit *e, unsigned x) {
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
        emit_byte(e, s[i]);
    }
}
#define BYTES(e, ...) bytes((e), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

static void u32(Emit *e, uint32_t x) {
    int i;

    for (i = 0; i < 4; i++) {
        emit_byte(e, (x >> (8 * i)) & 0xFF);
    }
}

static void u64(Emit *e, uint64_t x) {
    int i;

    for (i = 0; i < 8; i++) {
        emit_byte(e, (unsigned)(x >> (8 * i)) & 0xFF);
    }
}

/* The registers the machine code keeps (callee-saved, so the C it calls leaves them): rbx the Forth, r12 the task
 * running (f->t - the same task through a word's code, even across a wait). rax, rcx, rdx, rsi, rdi, r8 are scratch
 * (on Windows rsi and rdi are callee-saved: the outer entry keeps them for its caller) - through a run of fast paths
 * rsi and rdi have the data stack's top items, r8 the locals frame, and nothing is called. r13 and r14 have a do
 * loop's index and limit (the loop's registers, below) - callee-saved, so the C a word calls leaves them, and the
 * outer entry keeps them for the C that calls a word; a word called by its inner entry may use them for a loop of
 * its own, so after a call they are read again.
 *
 * The C it calls takes its first two arguments in rdi and rsi (System V) or rcx and rdx (Windows - where a call also
 * leaves 32 bytes of the stack, under the return address, for the callee: FRAME_BYTES has them). */
#define TASK_IP ((uint32_t)offsetof(Task, ip))
#define FORTH_TASK ((uint32_t)offsetof(struct Forth, t))
#define FORTH_BUDGET ((uint32_t)offsetof(struct Forth, budget))
#define FORTH_END ((uint32_t)offsetof(struct Forth, end))
#define FORTH_NGUARDS ((uint32_t)offsetof(struct Forth, nguards))
_Static_assert(sizeof(((struct Forth *)0)->budget) == 8, "the machine code counts the budget in 64 bits");

static void mov_rax(Emit *e, uint64_t x) { BYTES(e, 0x48, 0xB8); u64(e, x); }   /* movabs rax, x */
static void mov_rcx(Emit *e, uint64_t x) { BYTES(e, 0x48, 0xB9); u64(e, x); }   /* movabs rcx, x */
#ifdef NATIVE_WIN64
#define FRAME_BYTES 0x28   /* a word's frame: 8 (calls 16-aligned) and the callee's 32 */
static void mov_arg2(Emit *e, uint64_t x) { BYTES(e, 0x48, 0xBA); u64(e, x); }  /* movabs rdx, x */
static void arg_forth(Emit *e) { BYTES(e, 0x48, 0x89, 0xD9); }                   /* mov rcx, rbx */
static void code_of_arg2(Emit *e) { BYTES(e, 0x48, 0x8B, 0x42, WORD_CODE_AT); }   /* mov rax, [rdx + code] */
#else
#define FRAME_BYTES 0x08   /* a word's frame: calls from it 16-aligned */
static void mov_arg2(Emit *e, uint64_t x) { BYTES(e, 0x48, 0xBE); u64(e, x); }  /* movabs rsi, x */
static void arg_forth(Emit *e) { BYTES(e, 0x48, 0x89, 0xDF); }                   /* mov rdi, rbx */
static void code_of_arg2(Emit *e) { BYTES(e, 0x48, 0x8B, 0x46, WORD_CODE_AT); }   /* mov rax, [rsi + code] */
#endif
static void frame_enter(Emit *e) { BYTES(e, 0x48, 0x83, 0xEC, FRAME_BYTES); }   /* sub rsp, FRAME_BYTES */
static void frame_leave(Emit *e) { BYTES(e, 0x48, 0x83, 0xC4, FRAME_BYTES); }   /* add rsp, FRAME_BYTES */
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
 * never ends is stopped as threaded code's is ("ran too long") - or a handler's slice over, it gives way there (the
 * call returns as it goes on: nothing is kept in a register across it) */
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
    call_c(e, (const void *)forth_slice_over);
    patch(e, unlimited, e->n);
    patch(e, within, e->n);
}

/* ---- a definition translated ----
 *
 * Its code has two ways in. The first, at its start, is C's (forth_donative calls it, given the Forth): it keeps rbx
 * and r12, sets them, and calls the second, INNER_ENTRY bytes on - the way machine code calls machine code, the
 * registers already set. A call from machine code is as docol's: where its caller goes on, pushed on the return
 * stack (`exit` takes it off), then the call. */

#ifdef NATIVE_WIN64
#define INNER_ENTRY 48   /* (aligned: the outer entry's 42 bytes, padded) */
#else
#define INNER_ENTRY 48   /* (aligned: the outer entry's 38 bytes, padded) */
#endif

typedef void (*NativeEntry)(Forth *f);

#define CODE_BYTES_AT (INNER_ENTRY - 4)   /* a definition's code's length: kept there, in its first entry's padding */
static const uint8_t *inner_entry(const Word *w) { return (const uint8_t *)w->does + INNER_ENTRY; }

static void outer_entry(Emit *e) {
    BYTES(e, 0x53);                          /* push rbx */
    BYTES(e, 0x41, 0x54);                    /* push r12 */
    BYTES(e, 0x41, 0x55);                    /* push r13 */
    BYTES(e, 0x41, 0x56);                    /* push r14 */
#ifdef NATIVE_WIN64
    BYTES(e, 0x57);                          /* push rdi */
    BYTES(e, 0x56);                          /* push rsi */
    frame_enter(e);
    BYTES(e, 0x48, 0x89, 0xCB);              /* mov rbx, rcx */
#else
    frame_enter(e);
    BYTES(e, 0x48, 0x89, 0xFB);              /* mov rbx, rdi */
#endif
    BYTES(e, 0x4C, 0x8B, 0xA3);              /* mov r12, [rbx + FORTH_TASK] */
    u32(e, FORTH_TASK);
    BYTES(e, 0xE8);                          /* call INNER_ENTRY */
    u32(e, (uint32_t)(INNER_ENTRY - (e->n + 4)));
    frame_leave(e);
#ifdef NATIVE_WIN64
    BYTES(e, 0x5E);                          /* pop rsi */
    BYTES(e, 0x5F);                          /* pop rdi */
#endif
    BYTES(e, 0x41, 0x5E);                    /* pop r14 */
    BYTES(e, 0x41, 0x5D);                    /* pop r13 */
    BYTES(e, 0x41, 0x5C);                    /* pop r12 */
    BYTES(e, 0x5B);                          /* pop rbx */
    ret(e);
    while (e->n < INNER_ENTRY) {
        BYTES(e, 0xCC);                      /* (int3: never run) */
    }
}

static void leave_word(Emit *e) {
    frame_leave(e);
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
    signed char *back;   /* [i]: the branches that jump back to cell i (BACK_MANY: more than one) */
    signed char *lost;   /* [i]: a branch to cell i was written where the loop's registers were not to be trusted */
    signed char *kept;   /* [i]: cell i is the first of a loop that keeps its index and limit in the registers */
    size_t at;           /* the cell whose fast path is being written */
    Fixup *fixups;
    size_t nfixups, fixups_cap;
    struct Stub *stubs;
    size_t nstubs, stubs_cap;
    const Word **inlined;   /* the definitions written into this one (their callers' list is added to if it ends well) */
    size_t ninlined, inlined_cap;
    Emit e;
} Translation;

#define BACK_MANY 2

static int own_code(const Translation *tr) { return tr->body == tr->self->body; }   /* (not a word's written into it) */

static int fixup(Translation *tr, size_t where, size_t to) {
    if (!tr->e.looped && own_code(tr) && to < tr->n) {   /* (what is there can't count on the loop's registers) */
        tr->lost[to] = 1;
    }
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
    int *held = malloc(tr->n * sizeof(int));   /* (each cell to do: the >r items of its own the way there holds) */
    int ok = 1;

    if (todo == NULL || held == NULL) {
        free(todo);
        free(held);
        return 0;
    }
    held[ntodo] = 0;
    todo[ntodo++] = 0;
    while (ok && ntodo > 0) {
        size_t i = todo[--ntodo];
        int own = held[ntodo];

        while (ok && i < tr->n && !tr->op[i]) {
            Word *w = (Word *)tr->body[i];
            size_t next;

            if (!forth_is_word(f, (Cell)w) || w == f->w_does || w == f->w_jump || w == f->w_halt) {
                ok = 0;
                break;
            }
            tr->op[i] = 1;
            /* (r> or rdrop with none of its own >r's items there takes its caller's return address - `r> drop`,
             *  `r> cell+ >r`: it goes on where its caller wouldn't have; `exit` with one of its own still there
             *  goes where that says. Machine code returns by `ret`, whatever the return stack holds: such a word
             *  is left threaded. By the way to each cell, not the definition's totals - `r> ... >r` has as many
             *  of each.) */
            if (f->native != NULL && w != NULL && w == f->native->fast[FAST_TOR]) {
                own++;
            } else if (f->native != NULL && w != NULL &&
                       (w == f->native->fast[FAST_RFROM] || w == f->native->rdrop)) {
                if (own == 0) {
                    ok = 0;
                    break;
                }
                own--;
            } else if (w == f->w_exit && own != 0) {
                ok = 0;
                break;
            }
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
                } else if (to <= i && tr->back[to] < BACK_MANY) {
                    tr->back[to]++;
                }
                tr->target[to] = 1;
                if (!tr->op[to]) {
                    if (ntodo == tr->n) {
                        ok = 0;
                        break;
                    }
                    held[ntodo] = own;
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
    free(held);
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

/* the first cell of the innermost do loop around cell i (where its `loop` jumps back to); n none */
static size_t loop_head(const Translation *tr, size_t i) {
    size_t s, best = tr->n;

    for (s = 0; s < i; s++) {
        if (tr->loop_exit[s] != 0 && tr->loop_exit[s] > i) {
            best = s + 2;   /* ((do) and its operand) */
        }
    }
    return best;
}

/* does the loop begun at cell d - its (do) or (?do) - keep its index and limit in the registers all the way round:
 * ended by (loop), which leaves them right as it jumps back, and nothing else jumping back to its first cell */
static int loop_kept(const Translation *tr, size_t d) {
    size_t head = d + 2, exit = tr->loop_exit[d], l;

    if (exit < head + 2 || exit > tr->n) {
        return 0;
    }
    l = exit - 2;   /* ((loop) and its operand, just before where the loop goes on) */
    return tr->op[l] && (Word *)tr->body[l] == tr->f->w_loop && target_of(tr, l) == head && tr->back[head] == 1;
}

static void return_pushed(Emit *e, const Cell *cont);

/* a call of a word with machine code: where the caller goes on pushed (as docol does), then its inner entry */
static void call_native(Translation *tr, const Word *w, const Cell *cont) {
    Emit *e = &tr->e;

    return_pushed(e, cont);
    if (w == tr->self) {
        BYTES(e, 0xE8);                                  /* call INNER_ENTRY (a word calling itself) */
        u32(e, (uint32_t)((int64_t)INNER_ENTRY - (int64_t)(e->n + 4)));
    } else {
        mov_rax(e, at(inner_entry(w)));
        call_rax(e);
    }
}

/* a call of a word by its code field (a C primitive, a host's word, a constant ...), IP at its operands - the field
 * read as it runs, not as it was: a word defined again forwards (forth.c's doforward) */
static void call_code(Translation *tr, Word *w, const Cell *ip) {
    Emit *e = &tr->e;

    set_ip(e, ip);
    arg_forth(e);
    mov_arg2(e, at(w));
    code_of_arg2(e);
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
    mov_arg2(e, at(cont));
    call_c(e, (const void *)forth_run_to);
    patch(e, same, e->n);
}

/* a conditional jump (cc) from the word at cell `from` to cell `to`. One that goes back - begin ... until's - counts
 * a step on its way, as `branch` and the loops' words do: it counted none, so a handler whose work was such a loop
 * never gave way (its node held), and a script's that never ended was never stopped. */
static int jump_counted(Translation *tr, int cc, size_t from, size_t to) {
    Emit *e = &tr->e;
    size_t on;

    if (to > from) {
        return fixup(tr, jump(e, cc), to);
    }
    on = jump(e, cc ^ 1);   /* (x86's conditions pair: ^1 the other) */
    count_step(e, &tr->body[from + 1]);
    if (!fixup(tr, jump(e, CC_ALWAYS), to)) {
        return 0;
    }
    patch(e, on, e->n);
    return 1;
}

static int translate_generic(Translation *tr, size_t i) {
    Forth *f = tr->f;
    Emit *e = &tr->e;
    Word *w = (Word *)tr->body[i];
    const Cell *ip = &tr->body[i + 1];
    Cell *cont = (Cell *)&tr->body[i + 1 + operand_cells(f, w, &tr->body[i])];
    size_t to = branches(f, w) ? target_of(tr, i) : 0;

    if (w != f->w_branch && w != f->w_0branch) {   /* (anything else may leave the return stack another, or call a
                                                    *  word with a loop of its own) */
        e->looped = 0;
    }
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
        return jump_counted(tr, CC_NE, i, to);
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
 * edge, is exactly the primitive's. Between paths that are not of one run the data stack is all in the Task - its
 * depth (sp), every cell - so the C called between sees it; through a path, and a run of them, its top is in
 * registers (below: the data stack, as a path's code is written). */

#define TASK_SP ((uint32_t)offsetof(Task, sp))
#define TASK_SFLOOR ((uint32_t)offsetof(Task, sfloor))
#define TASK_RP ((uint32_t)offsetof(Task, rp))
#define TASK_LP ((uint32_t)offsetof(Task, lp))
#define TASK_SYS ((uint32_t)offsetof(Task, sys))
#define TASK_DS ((int32_t)offsetof(Task, ds))
#define TASK_RS ((int32_t)offsetof(Task, rs))
#define CELL_BYTES ((int32_t)sizeof(Cell))
#define STUB_GUARDS 4
#define LOCALS_IN_LINE_MAX 16   /* a frame's cells zeroed in line, at most (more: the primitive's loop) */

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14 };
#define REX_R 0x04   /* (the register field is one of r8 ... r15) */
#define REX_X 0x02   /* (... the index) */
#define REX_B_ 0x01  /* (... the base, or the r/m register) */
enum { CC_B = 0x2, CC_A = 0x7, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };   /* (with CC_E ... above) */
#define REX_W_B 0x49   /* 64 bits, r12 the base */
#define REX_W_R_B 0x4D /* ... and r8 the register */
#define REX_B 0x41     /* 32 bits, r12 the base */
#define REX_W 0x48
#define MOV_LOAD 0x8B
#define MOV_STORE 0x89

typedef struct Stub {
    const Cell *body;           /* the threaded code its cells are in: the word's own, or one written into it */
    size_t first, end;          /* the cells whose words it runs generically */
    size_t guards[STUB_GUARDS]; /* the jumps to it */
    int nguards;
    size_t back;                /* where it comes back to */
    int in_run;                 /* its fast path is inside a run: the stack there is as `in` has it as the path
                                 * begins, as `out` as it ends - written back as the stub begins, read as it ends */
    Stacked in, out;
    int fills;                  /* its path reads the loop's registers: the stub, the primitive run, does too */
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
    s->body = tr->body;
    s->in = tr->e.st;
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

/* an instruction on [r12 + index*8 + disp] (index rax: a data stack cell; rcx: a return stack cell, by rp or lp;
 * R8: a local's, by the frame kept there) */
static void cell_op(Emit *e, const uint8_t *op, size_t n, int reg, int index, int32_t disp) {
    uint8_t sib = (uint8_t)(0xC0 | (index & 7) << 3 | 4);

    emit_byte(e, (uint8_t)(REX_W_B | (reg >= R8 ? REX_R : 0) | (index >= R8 ? REX_X : 0)));
    bytes(e, op, n);
    if (disp >= -128 && disp <= 127) {
        BYTES(e, (uint8_t)(0x44 | (reg & 7) << 3), sib, (uint8_t)(disp & 0xFF));
    } else {
        BYTES(e, (uint8_t)(0x84 | (reg & 7) << 3), sib);
        u32(e, (uint32_t)disp);
    }
}
#define RS_OP(e, reg, disp, ...) cell_op((e), (const uint8_t[]){__VA_ARGS__}, \
                                         sizeof((const uint8_t[]){__VA_ARGS__}), (reg), RCX, (disp))

static void sp_read(Emit *e) { task_field(e, REX_B, MOV_LOAD, RAX, TASK_SP); }      /* mov eax, [sp] */
static void load_rp(Emit *e) { task_field(e, REX_B, MOV_LOAD, RCX, TASK_RP); }     /* mov ecx, [rp] */
static void store_rp(Emit *e) { task_field(e, REX_B, MOV_STORE, RCX, TASK_RP); }   /* mov [rp], ecx */

/* IP at `cont`, and cont pushed on the return stack, as docol has it for the word about to be called - in line: a
 * call of C for each call a word makes was most of what calling a word cost. The stack full: the primitive, which
 * says so (and doesn't come back). */
static void return_pushed(Emit *e, const Cell *cont) {
    size_t room;

    set_ip(e, cont);                                   /* (rax: cont) */
    load_rp(e);
    BYTES(e, 0x81, 0xF9);                              /* cmp ecx, RSTACK_CELLS */
    u32(e, RSTACK_CELLS);
    room = jump(e, CC_L);
    arg_forth(e);
    mov_arg2(e, at(cont));
    call_c(e, (const void *)push_return);
    patch(e, room, e->n);
    RS_OP(e, RAX, TASK_RS, MOV_STORE);                 /* mov [rs + rp*8], rax */
    BYTES(e, 0xFF, 0xC1);                              /* inc ecx */
    store_rp(e);
}

/* ---- the data stack, as a path's code is written (e->st) ----
 *
 * eax has sp as the path began - or, in a run, as its run began - and is left so: what the words push and drop is
 * counted as their code is written (st.depth), and a cell of the stack is at a known place from eax. The top one or
 * two items are in registers (rsi, rdi: st.reg), not in their cells: `1 +` is an instruction on a register, `swap`
 * and `drop` none at all. All of it is written back - the registers to their cells, sp to the task - where the C,
 * or other code, will look: as a path alone ends, as a run ends, before its jump (settle), and as a stub begins
 * (stub_enter: what its path began with; stub_leave: the registers as its path leaves them). */

/* a path's beginning: sp into eax (in a run eax has it already, from where the run began) */
static void load_sp(Emit *e) {
    if (e->run == RUN_OFF) {
        sp_read(e);
    }
}

/* the stack's item k (as TOP's: -1 the top, -2 under it): its register, or -1 (in its cell) */
static int slot_reg(const Emit *e, int k) { return k < 0 && -k <= e->st.n ? e->st.reg[-k - 1] : -1; }
/* ... and its cell, from eax */
static int32_t slot_at(const Emit *e, int k) { return TASK_DS + (e->st.depth + k) * CELL_BYTES; }

/* an instruction (64 bits, `op reg, r/m` or `op r/m, reg`) on a register and item k, wherever that is */
static void slot_op(Emit *e, const uint8_t *op, size_t n, int reg, int k) {
    int r = slot_reg(e, k);

    if (r < 0) {
        cell_op(e, op, n, reg, RAX, slot_at(e, k));
    } else {
        emit_byte(e, (uint8_t)(REX_W | (reg >= R8 ? REX_R : 0)));
        bytes(e, op, n);
        emit_byte(e, (uint8_t)(0xC0 | (reg & 7) << 3 | r));
    }
}
#define SLOT_OP(e, reg, k, ...) slot_op((e), (const uint8_t[]){__VA_ARGS__}, \
                                        sizeof((const uint8_t[]){__VA_ARGS__}), (reg), (k))
/* ... and on two registers: op reg, rm (or rm, reg: the opcode's) */
static void reg_op(Emit *e, unsigned op, int reg, int rm) {
    BYTES(e, (uint8_t)(REX_W | (reg >= R8 ? REX_R : 0) | (rm >= R8 ? REX_B_ : 0)), (uint8_t)op,
          (uint8_t)(0xC0 | (reg & 7) << 3 | (rm & 7)));
}

/* ---- a do loop's index and limit in registers (e->looped) ----
 *
 * The return stack has a loop's three cells - where it goes on after, its limit, its index - and they stay right:
 * (loop) writes the index it has counted, so `j`, `leave`, an error's report, a word called, all find what they
 * always did. r13 and r14 are those two cells read once: `i` is a register, (loop) counts and compares registers.
 * They are to be trusted (e->looped) from where they were read - (do), which makes them, or an `i` or a (loop)
 * that found them not - until something may have changed the return stack's top or the registers: any word called
 * (it may have a loop of its own), >r r>, a locals frame; and where code is come to by a jump, only if every way
 * there trusts them (tr->lost; a jump back: only a loop's own, to its first cell - tr->kept). */
static void loop_regs_read(Emit *e) {   /* (ecx: rp) */
    cell_op(e, (const uint8_t[]){MOV_LOAD}, 1, R13, RCX, TASK_RS - CELL_BYTES);       /* mov r13, [rs + rp*8 - 8] */
    cell_op(e, (const uint8_t[]){MOV_LOAD}, 1, R14, RCX, TASK_RS - 2 * CELL_BYTES);   /* mov r14, [rs + rp*8 - 16] */
}

/* a register no item is in (there is one: fewer than CACHE_MAX are) */
static int free_reg(const Emit *e) { return e->st.n > 0 && e->st.reg[0] == RSI ? RDI : RSI; }

/* an item pushed: the register it is to be put in. Both in use: the deeper of them goes to its cell. */
static int pushed(Emit *e) {
    int r;

    if (e->st.n == CACHE_MAX) {
        r = e->st.reg[1];
        cell_op(e, (const uint8_t[]){MOV_STORE}, 1, r, RAX, slot_at(e, -2));
        e->st.n = 1;
    } else {
        r = free_reg(e);
    }
    e->st.reg[1] = e->st.reg[0];
    e->st.reg[0] = r;
    e->st.n++;
    e->st.depth++;
    return r;
}

/* the top item dropped (no instruction: it is counted) */
static void popped(Emit *e) {
    e->st.depth--;
    if (e->st.n > 0) {
        e->st.reg[0] = e->st.reg[1];
        e->st.n--;
    }
}

/* a register for the top item, whose value is about to be replaced: the one it is in, or a free one (nothing read) */
static int top_reg_new(Emit *e) {
    if (e->st.n == 0) {
        e->st.reg[0] = RSI;
        e->st.n = 1;
    }
    return e->st.reg[0];
}

/* the top item in a register: read from its cell if it isn't */
static int top_reg(Emit *e) {
    if (e->st.n == 0) {
        cell_op(e, (const uint8_t[]){MOV_LOAD}, 1, RSI, RAX, slot_at(e, -1));
    }
    return top_reg_new(e);
}

/* the top two, both in registers */
static void both_regs(Emit *e) {
    (void)top_reg(e);
    if (e->st.n == 1) {
        int r = free_reg(e);

        cell_op(e, (const uint8_t[]){MOV_LOAD}, 1, r, RAX, slot_at(e, -2));
        e->st.reg[1] = r;
        e->st.n = CACHE_MAX;
    }
}

/* item k in a register, to read: its own, or `scratch` with it read from its cell */
static int slot_in(Emit *e, int k, int scratch) {
    int r = slot_reg(e, k);

    if (r < 0) {
        SLOT_OP(e, scratch, k, MOV_LOAD);
        r = scratch;
    }
    return r;
}

/* the locals frame in r8: read from the task the first time a path - or, in a run, any of its paths - asks */
static void frame_reg(Emit *e) {
    if (!e->st.lp) {
        task_field(e, REX_W_R_B, 0x63, 0, TASK_LP);    /* movsxd r8, [lp] */
        e->st.lp = 1;
    }
}

/* the items in registers, to their cells */
static void flush(Emit *e) {
    int i;

    for (i = 0; i < e->st.n; i++) {
        cell_op(e, (const uint8_t[]){MOV_STORE}, 1, e->st.reg[i], RAX, slot_at(e, -1 - i));
    }
    e->st.n = 0;
}

/* sp + depth to the task, depth not 0 (by lea and mov: a comparison's flags are kept for the jump after). Through a
 * path or a run the task's sp is as eax has it - where they began: only what moves it is written. */
static void sp_settled(Emit *e, int depth) {
    if (depth < INT8_MIN || depth > INT8_MAX) {
        e->failed = 1;   /* (never: a run ends before it is RUN_DEPTH_MAX from where it began) */
    }
    if (depth != 0) {
        BYTES(e, 0x8D, 0x48, (uint8_t)(depth & 0xFF)); /* lea ecx, [rax + depth] */
        task_field(e, REX_B, MOV_STORE, RCX, TASK_SP); /* mov [sp], ecx */
    }
}

/* everything written back: the registers to their cells, sp to the task - a path alone ending, or a run */
static void settle(Emit *e) {
    if (e->st.settled) {
        return;
    }
    flush(e);
    sp_settled(e, e->st.depth);
    e->st.settled = 1;
}

/* a stub of a path in a run begins: the stack as its path began with it (`in`), written back for the primitives */
static void stub_enter(Emit *e, const Stacked *in) {
    Stacked st = e->st;

    e->st = *in;
    flush(e);
    sp_settled(e, in->depth);
    e->st = st;
}

/* ... and ends: eax, and the task's sp, where the run has them (the primitives moved sp: the run's end writes it
 * only if the run moved it), the registers with what its path leaves in them (`out`) */
static void stub_leave(Emit *e, const Stacked *out) {
    int i;

    if (out->settled) {   /* (its path ended the run, and wrote all of it back - as the primitives have) */
        return;
    }
    sp_read(e);
    if (out->depth != 0) {
        BYTES(e, 0x83, 0xE8, (uint8_t)(out->depth & 0xFF));   /* sub eax, depth */
        task_field(e, REX_B, MOV_STORE, RAX, TASK_SP);        /* mov [sp], eax */
    }
    for (i = 0; i < out->n; i++) {
        cell_op(e, (const uint8_t[]){MOV_LOAD}, 1, out->reg[i], RAX, TASK_DS + (out->depth - 1 - i) * CELL_BYTES);
    }
    if (out->lp) {
        task_field(e, REX_W_R_B, 0x63, 0, TASK_LP);    /* movsxd r8, [lp] */
    }
}

/* a path's check, in a run, when every path is to go by its stub (CLANKFORTH_STUBS_ONLY): the jump to it */
static void stub_always(Translation *tr, Stub *s) {
    if (tr->f->native->stubs_only && s->nguards == 0) {
        guard(tr, s, CC_ALWAYS);
    }
}

/* k items on the data stack (above its floor), or to the stub */
static void need_items(Translation *tr, Stub *s, int k) {
    Emit *e = &tr->e;

    if (e->run == RUN_MEASURED && k - e->st.depth > e->need) {   /* (so many under where the run began) */
        e->need = k - e->st.depth;
    }
    if (e->run == RUN_ON) {   /* (checked as the run began) */
        stub_always(tr, s);
        return;
    }
    BYTES(e, 0x89, 0xC1);                              /* mov ecx, eax */
    task_field(e, REX_B, 0x2B, RCX, TASK_SFLOOR);      /* sub ecx, [sfloor] */
    BYTES(e, 0x83, 0xF9, (uint8_t)k);                  /* cmp ecx, k */
    guard(tr, s, CC_L);
}

/* room for k more */
static void need_room(Translation *tr, Stub *s, int k) {
    Emit *e = &tr->e;

    if (e->run == RUN_MEASURED && e->st.depth + k > e->room) {
        e->room = e->st.depth + k;
    }
    if (e->run == RUN_ON) {
        stub_always(tr, s);
        return;
    }
    BYTES(&tr->e, 0x3D);                               /* cmp eax, STACK_CELLS - k */
    u32(&tr->e, (uint32_t)(STACK_CELLS - k));
    guard(tr, s, CC_G);
}

/* a flag (-1 / 0) from the condition, in register r (rdx, rsi, rdi: REX_BYTE has their low bytes sil and dil) */
#define REX_BYTE 0x40
static void flag_of(Emit *e, int cc, int r) {
    BYTES(e, REX_BYTE, 0x0F, (uint8_t)(0x90 + cc), (uint8_t)(0xC0 | r));        /* setcc r (its low byte) */
    BYTES(e, REX_BYTE, 0x0F, 0xB6, (uint8_t)(0xC0 | r << 3 | r));               /* movzx r (32 bits), its low byte */
    BYTES(e, REX_W, 0xF7, (uint8_t)(0xD8 | r));                                 /* neg r */
}

/* ... the top item's value now: in its register (or a free one) - or, a path alone, straight to its cell */
static void top_flag(Emit *e, int cc) {
    if (e->st.n == 0 && e->run == RUN_OFF) {
        flag_of(e, cc, RDX);
        SLOT_OP(e, RDX, -1, MOV_STORE);
    } else {
        flag_of(e, cc, top_reg_new(e));
    }
}

static int is_compare(int fast) { return fast >= FAST_EQ && fast <= FAST_UGT; }
static int is_zero_compare(int fast) { return fast >= FAST_0EQ && fast <= FAST_0GE; }

static int compare_cc(int fast) {
    static const int ccs[] = {CC_E, CC_NE, CC_L, CC_G, CC_LE, CC_GE, CC_B, CC_A};
    static const int zero_ccs[] = {CC_E, CC_NE, CC_L, CC_G, CC_LE, CC_GE};

    if (is_compare(fast)) {
        return ccs[fast - FAST_EQ];
    }
    return is_zero_compare(fast) ? zero_ccs[fast - FAST_0EQ] : CC_E;   /* (never another: the callers ask) */
}

/* the ALU instruction on an item, with an immediate (81 /ext id) and with a register (op r/m64, r64; ALU_TO_REG
 * more: op r64, r/m64) */
#define ALU_TO_REG 2
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

/* ( a b -- a·b ) by `op` (its `reg, r/m` form): the result in a register, the top item's now */
static void binary(Emit *e, const uint8_t *op, size_t n, int commutes) {
    int a;

    if (e->st.n == 1 && commutes) {   /* b in its register, a in its cell: b op= a */
        slot_op(e, op, n, e->st.reg[0], -2);
        e->st.depth--;
        return;
    }
    if (e->st.n == CACHE_MAX) {
        a = e->st.reg[1];
    } else {
        a = free_reg(e);
        cell_op(e, (const uint8_t[]){MOV_LOAD}, 1, a, RAX, slot_at(e, -2));
    }
    slot_op(e, op, n, a, -1);   /* a op= b, wherever b is */
    popped(e);
    e->st.reg[0] = a;
    e->st.n = 1;
}

/* the flags of the second item compared with the top */
static void compare_slots(Emit *e) {
    int b = slot_reg(e, -1);

    if (b >= 0) {
        SLOT_OP(e, b, -2, 0x39);                       /* cmp second, top */
    } else {
        SLOT_OP(e, RCX, -2, MOV_LOAD);
        SLOT_OP(e, RCX, -1, 0x3B);                     /* cmp rcx, [top] */
    }
}

/* a literal x followed by an ALU word, a comparison or `*` on it: one instruction with x in it */
static int fast_literal_op(Translation *tr, Stub *s, Cell x, int fast) {
    Emit *e = &tr->e;
    int k = alu_of(fast);

    load_sp(e);
    need_items(tr, s, 1);
    need_room(tr, s, 1);   /* (as the literal's push would: a full stack overflows) */
    if (k >= 0) {
        SLOT_OP(e, alu[k].ext, -1, 0x81);              /* op top, x */
        u32(e, (uint32_t)x);
    } else if (fast == FAST_MUL) {
        int r = slot_reg(e, -1);

        SLOT_OP(e, r >= 0 ? r : RCX, -1, 0x69);        /* imul r, top, x */
        u32(e, (uint32_t)x);
        if (r < 0) {
            SLOT_OP(e, RCX, -1, MOV_STORE);
        }
    } else {
        SLOT_OP(e, 7, -1, 0x81);                       /* cmp top, x */
        u32(e, (uint32_t)x);
        top_flag(e, compare_cc(fast));
    }
    return 1;
}

/* a comparison and the 0branch after it: compare, then jump where the flag would be false */
static int fast_compare_branch(Translation *tr, Stub *s, int fast, int literal, Cell x) {
    Emit *e = &tr->e;
    int taken = literal || is_zero_compare(fast) ? 1 : 2, k;

    load_sp(e);
    need_items(tr, s, taken);
    if (literal) {
        need_room(tr, s, 1);   /* (as the literal's push would) */
        SLOT_OP(e, 7, -1, 0x81);                       /* cmp top, x */
        u32(e, (uint32_t)x);
    } else if (is_zero_compare(fast)) {
        SLOT_OP(e, 7, -1, 0x83);                       /* cmp top, 0 */
        emit_byte(e, 0);
    } else {
        compare_slots(e);
    }
    for (k = 0; k < taken; k++) {
        popped(e);
    }
    settle(e);   /* (the flags kept) */
    return 1;    /* (the conditional jump: the caller's) */
}

/* rdx is an address the scripts may reach `size` bytes at, or to the stub - forth.c's addr_n, its checks in line:
 * not a small number nor a negative one (one signed compare), not the dictionary's last bytes with fewer than size
 * left (end - address in 1 .. size-1: a byte always fits); and in a build with its asserts, no memory lent by the
 * host (guard_check's: the primitive looks it up) */
static void need_address(Translation *tr, Stub *s, int size) {
    Emit *e = &tr->e;

    BYTES(e, REX_W, 0x81, 0xFA);                       /* cmp rdx, LOWEST_ADDRESS */
    u32(e, LOWEST_ADDRESS);
    guard(tr, s, CC_L);
    if (size > 1) {
        BYTES(e, REX_W, 0x8B, 0x8B);                   /* mov rcx, [rbx + FORTH_END] */
        u32(e, FORTH_END);
        BYTES(e, REX_W, 0x29, 0xD1);                   /* sub rcx, rdx */
        BYTES(e, REX_W, 0xFF, 0xC9);                   /* dec rcx */
        BYTES(e, REX_W, 0x83, 0xF9, (uint8_t)(size - 1));   /* cmp rcx, size - 1 */
        guard(tr, s, CC_B);
    }
#ifndef NDEBUG
    BYTES(e, 0x83, 0xBB);                              /* cmp dword [rbx + FORTH_NGUARDS], 0 */
    u32(e, FORTH_NGUARDS);
    emit_byte(e, 0);
    guard(tr, s, CC_NE);
#endif
}

/* a word's fast path. Its checks come before it changes anything - a cell, a register an item is in, the counts
 * of them (e->st): a stub begins with what its path began with. */
static int fast_word(Translation *tr, Stub *s, int fast) {
    Emit *e = &tr->e;
    int k = alu_of(fast), r;

    if (fast == FAST_I && e->looped) {                 /* the loop's index: in its register */
        load_sp(e);
        need_room(tr, s, 1);
        reg_op(e, MOV_LOAD, pushed(e), R13);           /* mov r, r13 */
        return 1;
    }
    if (fast == FAST_I || fast == FAST_RFETCH || fast == FAST_J) {   /* a cell of the return stack: its top (i: a
                                                         * loop's index, 3 deep; j: the loop's round it, 6) */
        size_t head = fast == FAST_I && own_code(tr) ? loop_head(tr, tr->at) : tr->n;

        load_rp(e);
        BYTES(e, 0x83, 0xF9, fast == FAST_I ? 3 : fast == FAST_J ? 6 : 1);   /* cmp ecx, needed */
        guard(tr, s, CC_L);
        load_sp(e);
        need_room(tr, s, 1);
        if (head != tr->n && tr->kept[head]) {   /* (in a loop that keeps them: read, for what comes after too) */
            loop_regs_read(e);
            e->looped = 1;
            s->fills = 1;
            reg_op(e, MOV_LOAD, pushed(e), R13);
            return 1;
        }
        r = pushed(e);
        RS_OP(e, r, TASK_RS - (fast == FAST_J ? 4 : 1) * CELL_BYTES, MOV_LOAD);   /* mov r, [rs + rp*8 - 8] */
        return 1;
    }
    if (fast == FAST_TOR) {
        load_sp(e);
        need_items(tr, s, 1);
        load_rp(e);
        BYTES(e, 0x81, 0xF9);                          /* cmp ecx, RSTACK_CELLS */
        u32(e, RSTACK_CELLS);
        guard(tr, s, CC_GE);
        RS_OP(e, slot_in(e, -1, RDX), TASK_RS, MOV_STORE);   /* mov [rs + rp*8], top */
        BYTES(e, 0xFF, 0xC1);                          /* inc ecx */
        store_rp(e);
        popped(e);
        e->looped = 0;   /* (the return stack's top: another) */
        return 1;
    }
    if (fast == FAST_RFROM) {
        load_rp(e);
        BYTES(e, 0x85, 0xC9);                          /* test ecx, ecx */
        guard(tr, s, CC_LE);
        load_sp(e);
        need_room(tr, s, 1);
        BYTES(e, 0xFF, 0xC9);                          /* dec ecx */
        r = pushed(e);
        RS_OP(e, r, TASK_RS, MOV_LOAD);
        store_rp(e);
        e->looped = 0;
        return 1;
    }
    load_sp(e);
    switch (fast) {
    case FAST_FETCH: case FAST_CFETCH:
        need_items(tr, s, 1);
        SLOT_OP(e, RDX, -1, MOV_LOAD);                 /* mov rdx, top */
        need_address(tr, s, fast == FAST_FETCH ? CELL_BYTES : 1);
        r = top_reg_new(e);
        if (fast == FAST_FETCH) {
            BYTES(e, REX_W, 0x8B, (uint8_t)(r << 3 | RDX));   /* mov r, [rdx] */
        } else {
            BYTES(e, 0x0F, 0xB6, (uint8_t)(r << 3 | RDX));    /* movzx r (its 32 bits), byte [rdx] */
        }
        break;
    case FAST_STORE: case FAST_CSTORE:
        need_items(tr, s, 2);
        SLOT_OP(e, RDX, -1, MOV_LOAD);
        need_address(tr, s, fast == FAST_STORE ? CELL_BYTES : 1);
        SLOT_OP(e, RCX, -2, MOV_LOAD);
        if (fast == FAST_STORE) {
            BYTES(e, REX_W, 0x89, 0x0A);               /* mov [rdx], rcx */
        } else {
            BYTES(e, 0x88, 0x0A);                      /* mov [rdx], cl */
        }
        popped(e);
        popped(e);
        break;
    case FAST_TRUE: case FAST_FALSE:
        need_room(tr, s, 1);
        r = pushed(e);
        BYTES(e, REX_W, 0xC7, (uint8_t)(0xC0 | r));    /* mov r, -1 / 0 */
        u32(e, fast == FAST_TRUE ? 0xFFFFFFFFu : 0);
        break;
    case FAST_WITHIN:   /* ( x lo hi -- flag ) x - lo u< hi - lo, as the prelude's */
        need_items(tr, s, 3);
        need_room(tr, s, 1);   /* (as the prelude's `over` would: a full stack its error here too) */
        flush(e);   /* (three items: from their cells) */
        SLOT_OP(e, RCX, -3, MOV_LOAD);
        SLOT_OP(e, RCX, -2, 0x2B);                     /* sub rcx, [lo] */
        SLOT_OP(e, RDX, -1, MOV_LOAD);
        SLOT_OP(e, RDX, -2, 0x2B);                     /* sub rdx, [lo] */
        BYTES(e, REX_W, 0x39, 0xD1);                   /* cmp rcx, rdx */
        popped(e);
        popped(e);
        top_flag(e, CC_B);
        break;
    case FAST_DUP:
        need_items(tr, s, 1);
        need_room(tr, s, 1);
        r = pushed(e);
        SLOT_OP(e, r, -2, MOV_LOAD);                   /* (what was the top, wherever it is) */
        break;
    case FAST_DROP:
        need_items(tr, s, 1);
        popped(e);
        break;
    case FAST_SWAP:   /* both in registers: which is which, said the other way round */
        need_items(tr, s, 2);
        both_regs(e);
        r = e->st.reg[0];
        e->st.reg[0] = e->st.reg[1];
        e->st.reg[1] = r;
        break;
    case FAST_OVER:
        need_items(tr, s, 2);
        need_room(tr, s, 1);
        if (e->st.n == CACHE_MAX) {
            (void)pushed(e);   /* (the second went to its cell: its register, the new top's, has it yet) */
        } else {
            r = pushed(e);
            SLOT_OP(e, r, -3, MOV_LOAD);
        }
        break;
    case FAST_NIP:   /* the top in its register, one cell lower */
        need_items(tr, s, 2);
        (void)top_reg(e);
        e->st.n = 1;
        e->st.depth--;
        break;
    case FAST_MUL:
        need_items(tr, s, 2);
        binary(e, (const uint8_t[]){0x0F, 0xAF}, 2, 1);   /* imul r, r/m */
        break;
    case FAST_1ADD: case FAST_1SUB: case FAST_INVERT: case FAST_NEGATE: case FAST_CELLS:
        need_items(tr, s, 1);
        if (fast == FAST_1ADD || fast == FAST_1SUB) {
            SLOT_OP(e, fast == FAST_1ADD ? 0 : 1, -1, 0xFF);   /* inc / dec top */
        } else if (fast == FAST_CELLS) {
            SLOT_OP(e, 4, -1, 0xC1);                   /* shl top, 3 */
            emit_byte(e, 3);
        } else {
            SLOT_OP(e, fast == FAST_INVERT ? 2 : 3, -1, 0xF7);   /* not / neg top */
        }
        break;
    default:
        if (k >= 0) {   /* + - and or xor */
            need_items(tr, s, 2);
            if (e->run == RUN_OFF) {   /* (alone: on the cells, an instruction fewer than through a register) */
                SLOT_OP(e, RCX, -1, MOV_LOAD);
                SLOT_OP(e, RCX, -2, alu[k].op);        /* op [second], rcx */
                popped(e);
            } else {
                binary(e, (const uint8_t[]){(uint8_t)(alu[k].op + ALU_TO_REG)}, 1, fast != FAST_SUB);
            }
        } else if (is_compare(fast)) {
            need_items(tr, s, 2);
            compare_slots(e);
            popped(e);
            top_flag(e, compare_cc(fast));
        } else {   /* 0= 0<> 0< 0> 0<= 0>= */
            need_items(tr, s, 1);
            SLOT_OP(e, 7, -1, 0x83);                   /* cmp top, 0 */
            emit_byte(e, 0);
            top_flag(e, compare_cc(fast));
        }
        break;
    }
    return 1;
}

/* register r (rsi, rdi, rdx): the number x */
static void load_number(Emit *e, int r, Cell x) {
    if (fits_imm32(x)) {
        BYTES(e, REX_W, 0xC7, (uint8_t)(0xC0 | r));    /* mov r, x (its 32 bits, the sign's above) */
        u32(e, (uint32_t)x);
    } else {
        BYTES(e, REX_W, (uint8_t)(0xB8 + r));          /* movabs r, x */
        u64(e, (uint64_t)x);
    }
}

/* this code has w's cell written into it (inline_call's words; a constant's or a variable's address): noted, so that
 * w defined again has it made again (forth_native_redefined). 0: no memory */
static int depends_on(Translation *tr, const Word *w) {
    if (tr->ninlined == tr->inlined_cap) {
        size_t cap = tr->inlined_cap ? tr->inlined_cap * 2 : 8;
        const Word **more = realloc(tr->inlined, cap * sizeof(*more));

        if (more == NULL) {
            tr->e.failed = 1;
            return 0;
        }
        tr->inlined = more;
        tr->inlined_cap = cap;
    }
    tr->inlined[tr->ninlined++] = w;
    return 1;
}

/* A word that only says where its cell is, or what is in it - a variable's (CREATE's) address, a constant's or a
 * value's number - without the call: the address a number in the code, the number read from it (a value's changes;
 * a constant's is read too, so one changed by hand is the same here as called). They were a quarter of the calls of
 * C a web request's Forth made. */
static int data_word(const Word *w) {
    size_t at;

    return forth_is_variable(w) || forth_code_of(w) == CODE_CONSTANT || forth_task_variable(w, &at) >= 0;
}
static int fast_data(Translation *tr, Stub *s, const Word *w) {
    Emit *e = &tr->e;
    size_t cells_at;
    long var = forth_task_variable(w, &cells_at);
    int r;

    load_sp(e);
    need_room(tr, s, 1);
    if (var >= 0) {   /* a task-variable: the running task's cell of it - its task's (r12) own block, the cell in it.
                       * (A page of a web app asked C for one two thousand times.) No block yet: the primitive's. */
        task_field(e, REX_W_B, MOV_LOAD, RDX, TASK_SYS);   /* mov rdx, [the task's sys] */
        BYTES(e, REX_W, 0x85, 0xD2);                       /* test rdx, rdx */
        guard(tr, s, CC_E);
        r = pushed(e);
        BYTES(e, REX_W, 0x8D, (uint8_t)(0x80 | r << 3 | RDX));   /* lea r, [rdx + its cell] */
        u32(e, (uint32_t)(cells_at + (size_t)var * sizeof(Cell)));
        return e->run == RUN_MEASURED || depends_on(tr, w);
    }
    r = pushed(e);
    BYTES(e, REX_W, (uint8_t)(0xB8 + r));              /* movabs r, its body */
    u64(e, at(w->body));
    if (forth_code_of(w) == CODE_CONSTANT) {
        BYTES(e, REX_W, MOV_LOAD, (uint8_t)(r << 3 | r));   /* mov r, [r] */
    }
    return e->run == RUN_MEASURED || depends_on(tr, w);
}

/* the compiler's words: a literal, a string's, a locals frame, a local's fetch and store, 0branch, (loop), exit; -1
 * none of these */
static int fast_compiled(Translation *tr, Stub *s, size_t i) {
    Forth *f = tr->f;
    Emit *e = &tr->e;
    Word *w = (Word *)tr->body[i];
    Cell operand = tr->body[i + 1];
    int r;

    if (w == f->w_lit) {
        load_sp(e);
        need_room(tr, s, 1);
        load_number(e, pushed(e), operand);
        return 1;
    }
    if (w == f->w_litstring) {   /* ( -- addr len ) its bytes are in the code after its length: where, and how many */
        load_sp(e);
        need_room(tr, s, 2);
        load_number(e, pushed(e), (Cell)at(&tr->body[i + 2]));
        load_number(e, pushed(e), operand);
        return 1;
    }
    if (w == f->w_local_fetch || w == f->w_local_store) {   /* rs[lp + i]: no check, as the primitive has none */
        int32_t at = TASK_RS + (int32_t)operand * CELL_BYTES;

        load_sp(e);
        if (w == f->w_local_fetch) {
            need_room(tr, s, 1);
            frame_reg(e);
            r = pushed(e);
            cell_op(e, (const uint8_t[]){MOV_LOAD}, 1, r, R8, at);    /* mov r, [rs + lp*8 + at] */
        } else {
            need_items(tr, s, 1);
            frame_reg(e);
            cell_op(e, (const uint8_t[]){MOV_STORE}, 1, slot_in(e, -1, RDX), R8, at);
            popped(e);
        }
        return 1;
    }
    if (w == f->w_locals) {   /* p_locals: where the frame before was, pushed; this one's cells, zeroed */
        int j;

        if (operand < 0 || operand > LOCALS_IN_LINE_MAX) {
            return -1;
        }
        load_rp(e);
        BYTES(e, 0x81, 0xF9);                          /* cmp ecx, RSTACK_CELLS - (them and lp) */
        u32(e, (uint32_t)(RSTACK_CELLS - ((int)operand + 1)));
        guard(tr, s, CC_G);
        task_field(e, REX_W_B, 0x63, RDX, TASK_LP);    /* movsxd rdx, [lp] */
        RS_OP(e, RDX, TASK_RS, MOV_STORE);
        BYTES(e, 0xFF, 0xC1);                          /* inc ecx */
        task_field(e, REX_B, MOV_STORE, RCX, TASK_LP); /* mov [lp], ecx */
        BYTES(e, 0x31, 0xD2);                          /* xor edx, edx */
        for (j = 0; j < (int)operand; j++) {
            RS_OP(e, RDX, TASK_RS + j * CELL_BYTES, MOV_STORE);
        }
        if (operand > 0) {
            BYTES(e, 0x83, 0xC1, (uint8_t)operand);    /* add ecx, the locals */
        }
        store_rp(e);
        e->st.lp = 0;   /* (another frame) */
        e->looped = 0;
        return 1;
    }
    if (w == f->w_unlocals) {   /* p_unlocals: the return stack back to the frame, the frame before taken off */
        task_field(e, REX_W_B, 0x63, RCX, TASK_LP);    /* movsxd rcx, [lp] */
        BYTES(e, 0x85, 0xC9);                          /* test ecx, ecx */
        guard(tr, s, CC_LE);
        BYTES(e, 0xFF, 0xC9);                          /* dec ecx */
        RS_OP(e, RDX, TASK_RS, MOV_LOAD);
        store_rp(e);
        task_field(e, REX_B, MOV_STORE, RDX, TASK_LP); /* mov [lp], edx */
        e->st.lp = 0;
        e->looped = 0;
        return 1;
    }
    if (w == f->w_0branch) {
        load_sp(e);
        need_items(tr, s, 1);
        r = slot_reg(e, -1);
        if (r >= 0) {
            reg_op(e, 0x85, r, r);                     /* test r, r */
        } else {
            SLOT_OP(e, 7, -1, 0x83);                   /* cmp qword [top], 0 */
            emit_byte(e, 0);
        }
        popped(e);
        settle(e);   /* (the flags kept) */
        return jump_counted(tr, CC_E, i, target_of(tr, i));
    }
    if ((w == f->w_do || w == f->w_qdo) && own_code(tr)) {   /* p_do: where the loop goes on after, its limit and its
                                                              * index, pushed - and those two in their registers */
        size_t exit = target_of(tr, i);

        load_sp(e);
        need_items(tr, s, 2);
        load_rp(e);
        BYTES(e, 0x81, 0xF9);                          /* cmp ecx, RSTACK_CELLS - 3 */
        u32(e, (uint32_t)(RSTACK_CELLS - 3));
        guard(tr, s, CC_G);
        SLOT_OP(e, R14, -2, MOV_LOAD);                 /* mov r14, the limit */
        SLOT_OP(e, R13, -1, MOV_LOAD);                 /* mov r13, where it starts */
        popped(e);
        popped(e);
        e->looped = 0;
        if (w == f->w_qdo) {   /* (the same: no loop at all - to where it goes on, nothing pushed) */
            reg_op(e, 0x39, R13, R14);                 /* cmp r14, r13 */
            settle(e);   /* (the flags kept) */
            if (!fixup(tr, jump(e, CC_E), exit)) {
                return 0;
            }
            load_rp(e);   /* (settle may have taken rcx) */
        }
        BYTES(e, REX_W, 0xBA);                         /* movabs rdx, where it goes on (as the threaded code has it) */
        u64(e, (uint64_t)operand);
        RS_OP(e, RDX, TASK_RS, MOV_STORE);             /* mov [rs + rp*8], rdx */
        cell_op(e, (const uint8_t[]){MOV_STORE}, 1, R14, RCX, TASK_RS + CELL_BYTES);
        cell_op(e, (const uint8_t[]){MOV_STORE}, 1, R13, RCX, TASK_RS + 2 * CELL_BYTES);
        BYTES(e, 0x83, 0xC1, 3);                       /* add ecx, 3 */
        store_rp(e);
        e->looped = 1;
        s->fills = 1;
        tr->kept[i + 2] = (signed char)loop_kept(tr, i);
        return 1;
    }
    if (w == f->w_loop) {   /* as loop_step by 1: the index one on, done as it reaches the limit */
        size_t done, head = target_of(tr, i);
        int kept = own_code(tr) && head < tr->n && tr->kept[head];   /* (its registers right as it jumps back) */

        load_rp(e);
        if (!e->looped) {
            BYTES(e, 0x83, 0xF9, 3);                   /* cmp ecx, 3 */
            guard(tr, s, CC_L);
            if (kept) {
                loop_regs_read(e);
                s->fills = 1;
            }
        }
        if (e->looped || kept) {
            BYTES(e, REX_W_B, 0xFF, 0xC5);             /* inc r13 */
            cell_op(e, (const uint8_t[]){MOV_STORE}, 1, R13, RCX, TASK_RS - CELL_BYTES);   /* (the cell kept right) */
            reg_op(e, 0x39, R14, R13);                 /* cmp r13, r14 */
        } else {
            RS_OP(e, RDX, TASK_RS - CELL_BYTES, MOV_LOAD);      /* rdx: the index */
            BYTES(e, REX_W, 0xFF, 0xC2);               /* inc rdx */
            RS_OP(e, RDX, TASK_RS - CELL_BYTES, MOV_STORE);
            RS_OP(e, RDX, TASK_RS - 2 * CELL_BYTES, 0x3B); /* cmp rdx, [the limit] */
        }
        done = jump(e, CC_E);
        e->looped = kept;   /* (as its first cell finds them) */
        count_step(e, &tr->body[i + 1]);
        if (!fixup(tr, jump(e, CC_ALWAYS), head)) {
            return 0;
        }
        patch(e, done, e->n);
        BYTES(e, 0x83, 0xE9, 3);                       /* sub ecx, 3 (rcx: rp yet - the jump here is before the step) */
        store_rp(e);
        e->looped = 0;   /* (the loop's cells gone) */
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

/* ---- numbers worked out as the code is written ----
 *
 * A literal, and what is done to it by words that depend on nothing else - `2 cells`, `60 1000 *`, `1 20 lshift`'s
 * kind - is one number in the code: the words' own arithmetic (sums wrap, a comparison's flag is -1 or 0), done here.
 * As many as follow each other: `2 3 + 4 *` is 20. The room the words would have needed is still asked for (a full
 * stack overflows as it did). */
static int fold_one(int fast, Cell *v) {   /* v by a word of one number; 0: not such a word */
    UCell u = (UCell)*v;

    switch (fast) {
    case FAST_1ADD: u += 1; break;
    case FAST_1SUB: u -= 1; break;
    case FAST_INVERT: u = ~u; break;
    case FAST_NEGATE: u = (UCell)0 - u; break;
    case FAST_CELLS: u *= sizeof(Cell); break;
    case FAST_0EQ: u = *v == 0 ? (UCell)-1 : 0; break;
    case FAST_0NE: u = *v != 0 ? (UCell)-1 : 0; break;
    case FAST_0LT: u = *v < 0 ? (UCell)-1 : 0; break;
    case FAST_0GT: u = *v > 0 ? (UCell)-1 : 0; break;
    case FAST_0LE: u = *v <= 0 ? (UCell)-1 : 0; break;
    case FAST_0GE: u = *v >= 0 ? (UCell)-1 : 0; break;
    default: return 0;
    }
    *v = (Cell)u;
    return 1;
}
static int fold_two(int fast, Cell *v, Cell b) {   /* v and b by a word of two; 0: not such a word */
    UCell x = (UCell)*v, y = (UCell)b;
    int flag;

    switch (fast) {
    case FAST_ADD: *v = (Cell)(x + y); return 1;
    case FAST_SUB: *v = (Cell)(x - y); return 1;
    case FAST_MUL: *v = (Cell)(x * y); return 1;
    case FAST_AND: *v = (Cell)(x & y); return 1;
    case FAST_OR: *v = (Cell)(x | y); return 1;
    case FAST_XOR: *v = (Cell)(x ^ y); return 1;
    case FAST_EQ: flag = *v == b; break;
    case FAST_NE: flag = *v != b; break;
    case FAST_LT: flag = *v < b; break;
    case FAST_GT: flag = *v > b; break;
    case FAST_LE: flag = *v <= b; break;
    case FAST_GE: flag = *v >= b; break;
    case FAST_ULT: flag = x < y; break;
    case FAST_UGT: flag = x > y; break;
    default: return 0;
    }
    *v = flag ? -1 : 0;
    return 1;
}
/* the literal at cell i and the words after it that only work on it: the cell after them (i's own next: none did),
 * *v what they make of it, *room the most cells they would have had on the stack at once */
static size_t folded(Translation *tr, size_t i, Cell *v, int *room) {
    Forth *f = tr->f;
    size_t p = next_op(tr, i);

    *v = tr->body[i + 1];
    *room = 1;
    for (;;) {
        size_t q;

        if (!joinable(tr, p)) {
            break;
        }
        if (fold_one(fast_of(tr, (Word *)tr->body[p]), v)) {
            p = next_op(tr, p);
        } else if ((Word *)tr->body[p] == f->w_lit && joinable(tr, q = next_op(tr, p)) &&
                   fold_two(fast_of(tr, (Word *)tr->body[q]), v, tr->body[p + 1])) {
            *room = 2;
            p = next_op(tr, q);
        } else {
            break;
        }
    }
    return p;
}

/* what a fast path may be of a run: any of it; its last only (it ends in a jump); none (it calls C, or leaves) */
enum { PATH_PLAIN, PATH_BRANCH, PATH_ALONE };

/* the word at cell i (and those joined to it) by a fast path, with its stub: the cell after them; 0 if it has none
 * (then the generic translation). *kind: PATH_* */
static size_t translate_fast(Translation *tr, size_t i, int *kind) {
    Forth *f = tr->f;
    Emit *e = &tr->e;
    Word *w = (Word *)tr->body[i];
    size_t j = next_op(tr, i), k = joinable(tr, j) ? next_op(tr, j) : j, end;   /* (j, k: words only if reached) */
    int fast = fast_of(tr, w), next = joinable(tr, j) ? fast_of(tr, (Word *)tr->body[j]) : -1, r, room;
    Cell number;
    Stub *s;

    *kind = PATH_PLAIN;
    if (f->native->inline_off) {
        return 0;
    }
    if (e->run == RUN_OFF) {   /* (alone: the stack as the task has it) */
        memset(&e->st, 0, sizeof(e->st));
    }
    tr->at = i;
    if (w == f->w_lit && (end = folded(tr, i, &number, &room)) != j) {   /* 2 3 +  : one number */
        int after = joinable(tr, end) ? fast_of(tr, (Word *)tr->body[end]) : -1;

        if (fits_imm32(number) && (alu_of(after) >= 0 || after == FAST_MUL || is_compare(after))) {   /* 2 cells +  */
            size_t whole = next_op(tr, end);

            if ((s = stub(tr, i, whole)) == NULL) {
                return 0;
            }
            load_sp(e);
            need_room(tr, s, room);
            r = fast_literal_op(tr, s, number, after);
            end = whole;
        } else {
            if ((s = stub(tr, i, end)) == NULL) {
                return 0;
            }
            load_sp(e);
            need_room(tr, s, room);
            load_number(e, pushed(e), number);
            r = 1;
        }
    } else if (w == f->w_lit && joinable(tr, j) && fits_imm32(tr->body[i + 1]) && is_compare(next) && joinable(tr, k) &&
        (Word *)tr->body[k] == f->w_0branch) {   /* x <  0branch */
        end = next_op(tr, k);
        if ((s = stub(tr, i, end)) == NULL) {
            return 0;
        }
        *kind = PATH_BRANCH;
        fast_compare_branch(tr, s, next, 1, tr->body[i + 1]);
        r = jump_counted(tr, compare_cc(next) ^ 1, k, target_of(tr, k));   /* (x86's conditions pair: ^1) */
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
        *kind = PATH_BRANCH;
        fast_compare_branch(tr, s, fast, 0, 0);
        r = jump_counted(tr, compare_cc(fast) ^ 1, j, target_of(tr, j));
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
        *kind = w == f->w_0branch || w == f->w_qdo ? PATH_BRANCH
              : w == f->w_loop || w == f->w_exit ? PATH_ALONE : PATH_PLAIN;
        if ((r = fast_compiled(tr, s, i)) < 0) {
            tr->nstubs--;   /* (no fast path after all) */
            return 0;
        }
    } else if (data_word(w)) {
        end = j;
        if ((s = stub(tr, i, end)) == NULL) {
            return 0;
        }
        r = fast_data(tr, s, w);
    } else {
        return 0;
    }
    if (r == 0) {
        e->failed = 1;
        return 0;
    }
    if (e->run == RUN_OFF) {   /* (alone: what it left in registers, and sp, written back) */
        settle(e);
    }
    s->back = e->n;
    s->in_run = e->run == RUN_ON;
    s->out = e->st;
    return end;
}

/* ---- a call written in line ----
 *
 * A small definition, all of whose words have fast paths, is written into its caller where the caller calls it: no
 * call, no frame, no step counted, and - the gain - its words part of the caller's run, the stack's depth in the
 * register through them, one check for all. What a call leaves on the return stack is left all the same (where the
 * caller goes on, pushed as the words begin and taken off as they end): the words find it as they would called, and
 * an error among them names its callers as ever. The return stack full: the caller's cell is run generically - the
 * call itself, which says so.
 *
 * Only what is straight through to its `exit`: no branch, no loop, and no word of the return stack (r> could take
 * what was pushed for it). And only where every word has its fast path or is such a definition itself
 * (INLINE_DEPTH_MAX deep): found by writing it - anything else, and what was written is unwritten, the call a call. */
#define INLINE_CELLS_MAX 48   /* a definition's threaded code, at most, to be written into its callers */
#define INLINE_DEPTH_MAX 3    /* ... and those it calls into it, so deep */

/* w's cells, to its exit and with it, if it may be written into tr's word (0: not); op: where each of its words
 * begins */
static size_t inline_cells(Translation *tr, const Word *w, signed char *op) {
    Forth *f = tr->f;
    const struct ForthNative *nat = f->native;
    size_t i = 0;

    if (w == tr->self || w->code != forth_donative) {
        return 0;
    }
    memset(op, 0, INLINE_CELLS_MAX);
    while (i < INLINE_CELLS_MAX && (const uint8_t *)&w->body[i + 2] <= f->here) {
        const Word *x = (const Word *)w->body[i];
        int fast;

        if (!forth_is_word(f, (Cell)x)) {
            return 0;
        }
        op[i] = 1;
        if (x == f->w_exit) {
            return i + 1;
        }
        fast = fast_of(tr, x);
        if (branches(f, x) || x == f->w_leave || x == f->w_does || x == f->w_jump || x == f->w_halt ||
            x == nat->rdrop || fast == FAST_TOR || fast == FAST_RFROM || fast == FAST_RFETCH || fast == FAST_I ||
            fast == FAST_J) {
            return 0;
        }
        i += 1 + operand_cells(f, x, &w->body[i]);
    }
    return 0;
}

/* the call at cell j of the code being translated, written in line (in a run: measured, or on): the cell after it,
 * *paths more by the fast paths written; 0 if it can't be (nothing written) */
static size_t inline_call(Translation *tr, size_t j, int depth, int *paths) {
    Forth *f = tr->f;
    Emit *e = &tr->e;
    Word *w = (Word *)tr->body[j];
    const Cell *body = tr->body, *cont = &tr->body[j + 1];
    signed char op[INLINE_CELLS_MAX], target[INLINE_CELLS_MAX] = {0};
    signed char *op_was = tr->op, *target_was = tr->target;
    size_t n_was = tr->n, n, i, end = 0, entry = tr->nstubs;
    size_t code = e->n, nfixups = tr->nfixups, ninlined = tr->ninlined;
    Stacked st = e->st;
    int need = e->need, room = e->room, looped = e->looped, count = 0, ok = 1, kind;

    if (f->native->inline_off) {   /* (CLANKFORTH_CALLS_ONLY: every word called - a call of one with no fast word
                                    *  in it was written in line all the same, the mode not what it said) */
        return 0;
    }
    if (e->run == RUN_OFF || depth > INLINE_DEPTH_MAX || !forth_is_word(f, (Cell)w) ||
        (n = inline_cells(tr, w, op)) == 0 || stub(tr, j, j + 1) == NULL) {
        return 0;
    }
    BYTES(e, REX_W, 0xBA);                             /* movabs rdx, cont */
    u64(e, at(cont));
    load_rp(e);
    BYTES(e, 0x81, 0xF9);                              /* cmp ecx, RSTACK_CELLS */
    u32(e, RSTACK_CELLS);
    guard(tr, &tr->stubs[entry], CC_GE);
    RS_OP(e, RDX, TASK_RS, MOV_STORE);                 /* mov [rs + rp*8], rdx */
    BYTES(e, 0xFF, 0xC1);                              /* inc ecx */
    store_rp(e);
    tr->body = w->body;   /* (its words: translated as the word's own are) */
    tr->n = n;
    tr->op = op;
    tr->target = target;
    e->looped = 0;   /* (through its words the return stack's top is theirs - where the caller goes on, a frame) */
    for (i = 0; ok && i + 1 < n; i = end) {
        end = translate_fast(tr, i, &kind);
        if (end != 0 && kind == PATH_PLAIN) {
            count++;
        } else if (end != 0 || (end = inline_call(tr, i, depth + 1, &count)) == 0) {
            ok = 0;
        }
    }
    tr->body = body;
    tr->n = n_was;
    tr->op = op_was;
    tr->target = target_was;
    e->looped = looped;   /* (and as it was after them: none of them a word that takes the registers) */
    if (!ok || e->failed) {   /* (unwritten: its stubs with it) */
        e->n = code;
        tr->nstubs = entry;
        tr->nfixups = nfixups;
        tr->ninlined = ninlined;
        e->st = st;
        e->need = need;
        e->room = room;
        return 0;
    }
    load_rp(e);                                        /* (where the caller goes on: taken off, as exit does) */
    BYTES(e, 0xFF, 0xC9);                              /* dec ecx */
    store_rp(e);
    tr->stubs[entry].back = e->n;
    tr->stubs[entry].in_run = e->run == RUN_ON;
    tr->stubs[entry].out = e->st;
    if (e->run == RUN_ON && !depends_on(tr, w)) {
        return 0;
    }
    *paths += count + 2;   /* (its words, and the two round them) */
    return j + 1;
}

/* the word at cell j as part of a run: its fast path, or - a call - the words it calls */
static size_t run_path(Translation *tr, size_t j, int *kind, int *paths) {
    size_t end = translate_fast(tr, j, kind);

    if (end != 0) {
        (*paths)++;
        return end;
    }
    *kind = PATH_PLAIN;
    return tr->e.failed ? 0 : inline_call(tr, j, 1, paths);
}

/* A run: the fast paths from cell i on, one after another with nothing between them - no call, no word a branch
 * lands on. Each alone reads sp from the task, checks it has the items and the room, and writes sp back: a dozen
 * instructions round a word's two or three. In a run sp stays in eax from one path to the next - as the run began:
 * what the paths push and drop is counted, not done - the stack's top items in registers, and the checks are
 * made once as it begins - for the most items any of its paths needs under where the run begins, the most room over
 * it. They fail: the run's stub does all of it generically, each word's primitive in turn, so the one that hasn't
 * what it needs says so exactly as it would have. What a path needs is found by writing the run once (measured,
 * and unwritten): the paths themselves say, not a table that could differ from them. A path that ends in a jump
 * (a comparison's, 0branch) can only be a run's last: before its jump, and as any run ends, the registers go to
 * their cells and sp to the task (settle) - what a branch lands on, and a call, find the stack in the task. The
 * cell after the run; 0 if there is no fast path here. */
#define RUN_PATHS_MIN 2      /* fewer: the path alone, as ever */
#define RUN_DEPTH_MAX 100    /* a run needing more items or room than this ends before (its checks' operands: a byte) */

static size_t translate_run(Translation *tr, size_t i) {
    Emit *e = &tr->e;
    size_t code = e->n, nstubs = tr->nstubs, nfixups = tr->nfixups, ninlined = tr->ninlined, j, end, run_end = i;
    int kind, paths = 0, need, room, looped = e->looped;
    Stub *all;   /* (the run's own stub: at tr->stubs[nstubs], wherever the stubs made after move them) */

    e->run = RUN_MEASURED;
    memset(&e->st, 0, sizeof(e->st));
    e->need = e->room = 0;
    for (j = i; j < tr->n && tr->op[j] && (j == i || !tr->target[j]); j = end) {
        Stacked st = e->st;
        int need_was = e->need, room_was = e->room, paths_was = paths, looped_was = e->looped;

        end = run_path(tr, j, &kind, &paths);
        if (end == 0 || kind == PATH_ALONE || e->need > RUN_DEPTH_MAX || e->room > RUN_DEPTH_MAX) {
            e->st = st;   /* (not of the run) */
            e->looped = looped_was;
            e->need = need_was;
            e->room = room_was;
            paths = paths_was;
            break;
        }
        run_end = end;
        if (kind == PATH_BRANCH) {
            break;
        }
    }
    need = e->need;
    room = e->room;
    e->n = code;   /* (unwritten: its stubs and jumps with it) */
    tr->nstubs = nstubs;
    tr->nfixups = nfixups;
    tr->ninlined = ninlined;
    e->run = RUN_OFF;
    memset(&e->st, 0, sizeof(e->st));
    e->looped = looped;   /* (as the run begins: written again, it finds what it found) */
    if (e->failed) {
        return 0;
    }
    if (paths < RUN_PATHS_MIN) {
        return translate_fast(tr, i, &kind);
    }
    if ((all = stub(tr, i, run_end)) == NULL) {
        e->failed = 1;
        return 0;
    }
    sp_read(e);
    if (need > 0) {
        need_items(tr, all, need);
    }
    if (room > 0) {
        need_room(tr, all, room);
    }
    e->run = RUN_ON;
    for (j = i; j < run_end; j = end) {
        if (j != i) {   /* (the run's first: where the run begins - its checks, sp read - as translate has it) */
            tr->label[j] = e->n;
        }
        if ((end = run_path(tr, j, &kind, &paths)) == 0) {   /* (never: it was written a moment ago) */
            e->failed = 1;
            break;
        }
    }
    settle(e);   /* (unless its last path has, before its jump) */
    e->run = RUN_OFF;
    tr->stubs[nstubs].back = e->n;
    return e->failed ? 0 : run_end;
}

/* the stubs, after the word's code: the generic translation of what their fast path does, then back */
static int translate_stubs(Translation *tr) {
    Emit *e = &tr->e;
    const Cell *body = tr->body;
    size_t k, i;
    int g;

    for (k = 0; k < tr->nstubs; k++) {
        Stub *s = &tr->stubs[k];

        if (s->nguards == 0) {   /* (a path in a run, with nothing of its own to check: never come to) */
            continue;
        }
        tr->body = s->body;   /* (its cells: the word's own, or those of one written into it) */
        for (g = 0; g < s->nguards; g++) {
            patch(e, s->guards[g], e->n);
        }
        if (s->in_run) {   /* (the primitives find the stack in the task) */
            stub_enter(e, &s->in);
        }
        for (i = s->first; i < s->end; i = next_op(tr, i)) {
            if (!translate_generic(tr, i)) {
                return 0;
            }
        }
        if (s->in_run) {
            stub_leave(e, &s->out);
        }
        if (s->fills) {
            load_rp(e);
            loop_regs_read(e);
        }
        patch(e, jump(e, CC_ALWAYS), s->back);
    }
    tr->body = body;
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
    frame_enter(e);   /* (these two begin with FORWARD_BYTES and more in which nothing is called, nothing jumped to) */
    count_step(e, tr->body);
    for (i = 0; i < tr->n; i++) {
        if (!tr->op[i]) {
            continue;
        }
        tr->label[i] = e->n;
        if (tr->back[i]) {   /* (jumped back to: the loop's registers only as a loop that keeps them has them) */
            e->looped = e->looped && tr->kept[i] && !tr->lost[i];
        } else if (tr->target[i]) {
            e->looped = e->looped && !tr->lost[i];
        }
        if ((end = translate_run(tr, i)) != 0) {
            i = end - 1;   /* (past the words joined to it, and the rest of its run) */
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
    if (!e->failed) {   /* (how long it is, in the last of the bytes that pad its first entry: native-bytes') */
        uint32_t n = (uint32_t)e->n;

        memcpy(e->buf + CODE_BYTES_AT, &n, sizeof(n));
    }
    return !e->failed;
}

/* the definitions written into w's code noted with it: one of them defined again, w's code is made anew */
static void inlined_into(Forth *f, const Translation *tr, Word *w, const Cell *end) {
    struct ForthNative *nat = f->native;
    size_t k;

    for (k = 0; k < tr->ninlined; k++) {
        if (nat->ninlined == nat->inlined_cap) {
            size_t cap = nat->inlined_cap ? nat->inlined_cap * 2 : 64;
            Inlined *more = realloc(nat->inlined, cap * sizeof(Inlined));

            if (more == NULL) {
                return;   /* (not noted: its copy of that one would stay as it is) */
            }
            nat->inlined = more;
            nat->inlined_cap = cap;
        }
        nat->inlined[nat->ninlined++] = (Inlined){tr->inlined[k], w, end};
    }
}

/* w's threaded code (to `end`) translated, its code field then the machine code's; again: one that has machine code
 * already given new (forth_native_redefined) */
static void native_compile(Forth *f, Word *w, const Cell *end, int again) {
    Translation tr;
    uint8_t *code;
    int ok;

    if (f->native == NULL || (w->code == forth_donative && !again) || end <= w->body) {
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
    tr.back = calloc(tr.n, 1);
    tr.lost = calloc(tr.n, 1);
    tr.kept = calloc(tr.n, 1);
    ok = tr.op != NULL && tr.label != NULL && tr.loop_exit != NULL && tr.target != NULL && tr.back != NULL &&
         tr.lost != NULL && tr.kept != NULL && translate(&tr);
    if (ok && (code = code_space(f, tr.e.n)) != NULL && code_pages(code, tr.e.n, 1)) {
        memcpy(code, tr.e.buf, tr.e.n);
        if (!code_pages(code, tr.e.n, 0)) {   /* (it can't be run: this word stays threaded) */
            f->native->refused++;
            goto done;
        }
        w->does = (Cell *)code;   /* (a colon definition's does is otherwise unused) */
        w->code = forth_donative;
        f->native->translated++;
        inlined_into(f, &tr, w, end);
        if (f->native->map != NULL) {   /* (perf's form: where in hex, how long in hex, the name) */
            fprintf(f->native->map, "%llx %zx %s\n", (unsigned long long)at(code), tr.e.n,
                    w->name[0] != '\0' ? w->name : CODE_MAP_UNNAMED);
            fflush(f->native->map);   /* (each line at once: a process killed has them all - and Windows' C library
                                       *  has no line buffering to ask for, _IOLBF is a full buffer there) */
        }
    } else {
        f->native->refused++;
    }
done:
    free(tr.op);
    free(tr.label);
    free(tr.loop_exit);
    free(tr.target);
    free(tr.back);
    free(tr.lost);
    free(tr.kept);
    free(tr.stubs);
    free(tr.fixups);
    free(tr.inlined);
    free(tr.e.buf);
}

void forth_native_compile(Forth *f, Word *w, const Cell *end) { native_compile(f, w, end, 0); }

/* clang's -fsanitize=function (in -fsanitize=undefined) reads a type signature just before what an indirect call
 * goes to: machine code has none, and at a page's start that read is of the page before - unmapped, a crash */
#if defined(__clang__)
#define NOT_TYPE_CHECKED __attribute__((no_sanitize("function")))
#else
#define NOT_TYPE_CHECKED
#endif

/* the code field of a word with machine code: run it, as docol would its threaded code (where the caller goes on,
 * pushed; `exit` takes it off) */
NOT_TYPE_CHECKED void forth_donative(Forth *f, Word *w) {
    Cell *ip = f->t->ip;
    NativeEntry entry;

    rpush(f, (Cell)ip);
    memcpy(&entry, &w->does, sizeof(entry));
    entry(f);
    f->t->ip = ip;
}

/* a jump to `target` written over the inner entry of the code at `code` (W^X: writable for the moment) */
static int entry_forwarded(uint8_t *code, const uint8_t *target) {
    uint8_t jump[FORWARD_BYTES] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,   /* movabs rax, target */
                                   0xFF, 0xE0};                          /* jmp rax */
    uint64_t to = at(target);

    memcpy(jump + 2, &to, sizeof(to));
    if (!code_pages(code + INNER_ENTRY, FORWARD_BYTES, 1)) {
        return 0;
    }
    memcpy(code + INNER_ENTRY, jump, FORWARD_BYTES);
    return code_pages(code + INNER_ENTRY, FORWARD_BYTES, 0);
}

/* machine code calling old goes on to to's: a jump over old's inner entry (its first bytes are the frame and the
 * step's count - nothing returns into them, no jump lands in them - so code still running old, past them, goes on
 * as it was); to's outer entry for C (forth_donative) */
int forth_native_forward(Forth *f, Word *old, Word *to) {
    if (f->native == NULL || old->code != forth_donative || to->code != forth_donative) {
        return 0;
    }
    return entry_forwarded((uint8_t *)old->does, inner_entry(to));
}

/* `old` has been defined again (forth.c's forward: it forwards now). The words whose machine code has old's words
 * written in it would go on running those: each is translated again - old a call in it now, to the new one - and
 * what calls its old code sent on to the new. (One whose old code is running goes on in it till it returns.) */
void forth_native_redefined(Forth *f, const Word *old) {
    struct ForthNative *nat = f->native;
    size_t k = 0;

    while (nat != NULL && k < nat->ninlined) {
        Inlined in = nat->inlined[k];
        uint8_t *code;
        size_t j, n = 0;

        if (in.callee != old) {
            k++;
            continue;
        }
        for (j = 0; j < nat->ninlined; j++) {   /* (what was noted for this caller: noted anew as it is translated) */
            if (nat->inlined[j].caller != in.caller) {
                nat->inlined[n++] = nat->inlined[j];
            }
        }
        nat->ninlined = n;
        k = 0;
        if (in.caller->code != forth_donative) {   /* (defined again itself since: nothing calls its code) */
            continue;
        }
        code = (uint8_t *)in.caller->does;
        native_compile(f, in.caller, in.end, 1);
        if ((uint8_t *)in.caller->does == code || !entry_forwarded(code, inner_entry(in.caller))) {
            forth_printf(f, "(%s:%s: machine code compiled with the old %s in it keeps it)\n", in.caller->vocab->name,
                         in.caller->name, old->name);
        }
    }
}

/* the dictionary given back down to p (forth.c's here_back): the words from there on are words no more - nothing
 * noted of them is kept (a word made there later is another) */
void forth_native_forgot(Forth *f, const uint8_t *p) {
    struct ForthNative *nat = f->native;
    size_t j, n = 0;

    for (j = 0; nat != NULL && j < nat->ninlined; j++) {
        if ((const uint8_t *)nat->inlined[j].caller < p && (const uint8_t *)nat->inlined[j].callee < p) {
            nat->inlined[n++] = nat->inlined[j];
        }
    }
    if (nat != NULL) {
        nat->ninlined = n;
    }
}

static void p_native_q(Forth *f, Word *w) {   /* native? ( xt -- flag ) has it machine code? */
    forth_push(f, forth_xt_arg(f, forth_pop(f), "native?")->code == forth_donative ? -1 : 0);
}
static void p_native_inlined(Forth *f, Word *w) {   /* native-inlined ( xt -- n ) the calls written in line in its code */
    const Word *x = forth_xt_arg(f, forth_pop(f), "native-inlined");
    size_t k, n = 0;

    for (k = 0; f->native != NULL && k < f->native->ninlined; k++) {
        n += f->native->inlined[k].caller == x;
    }
    forth_push(f, (Cell)n);
}
static void p_native_bytes(Forth *f, Word *w) {   /* native-bytes ( xt -- n ) its machine code's bytes (0: it has none) */
    const Word *x = forth_xt_arg(f, forth_pop(f), "native-bytes");
    uint32_t n = 0;

    if (x->code == forth_donative) {
        memcpy(&n, (const uint8_t *)x->does + CODE_BYTES_AT, sizeof(n));
    }
    forth_push(f, (Cell)n);
}
static void p_dot_native(Forth *f, Word *w) {   /* .native ( -- ) how many definitions have it, how many not */
    if (f->native == NULL) {
        forth_printf(f, "the native backend is off (CLANKFORTH_THREADED): every definition threaded\n");
        return;
    }
    forth_printf(f, "%ld definitions translated to machine code, %ld left threaded; %zu calls written in line\n",
                 f->native->translated, f->native->refused, f->native->ninlined);
}
/* the words written in line found (the core's, by name: a script's own word of that name is called) - as the
 * primitives are made, and again once the prelude has defined its own (within, true, 0>= ...) */
void forth_native_bind(Forth *f) {
    int k;

    if (f->native != NULL) {
        for (k = 0; k < FAST_WORDS; k++) {
            f->native->fast[k] = forth_in_vocab(f->core, fast_names[k], strlen(fast_names[k]));
        }
        f->native->rdrop = forth_in_vocab(f->core, "rdrop", 5);
    }
}
void forth_native_words(Forth *f) {
    forth_native_bind(f);
    if (f->native != NULL) {
        f->native->inline_off = getenv("CLANKFORTH_CALLS_ONLY") != NULL;
        f->native->stubs_only = getenv("CLANKFORTH_STUBS_ONLY") != NULL;
    }
    forth_set_effect(f, forth_prim(f, "native?", p_native_q), "( xt -- flag )");
    forth_set_effect(f, forth_prim(f, ".native", p_dot_native), "( -- )");
    forth_set_effect(f, forth_prim(f, "native-inlined", p_native_inlined), "( xt -- n )");
    forth_set_effect(f, forth_prim(f, "native-bytes", p_native_bytes), "( xt -- n )");
}

/* ---- tasks as coroutines ---- */

enum { CO_FRESH, CO_RUNNING, CO_SUSPENDED, CO_DONE, CO_FAILED };
#define CO_NESTING_MAX 16   /* a waiting task's depths whose call chain is kept (more: its chain ends there) */

struct ForthCoroutine {
#ifdef NATIVE_WIN64
    void *fiber;            /* its stack, a fiber's (Windows keeps a thread's stack bounds as fibers switch) */
    void *caller;           /* the fiber that switched into it (the scheduler's) */
    Forth *f;
    jmp_buf top;            /* its first frame's: started afresh while it waited, it comes back here */
    int midway;             /* it waits inside a run (not at its top, its run over) */
    int afresh;             /* (started afresh: back to its top as it is next switched into) */
#else
    uint8_t *stack;         /* its mapping: the guard page, then the stack */
    size_t bytes;
    void *sp;               /* its stack pointer while it is switched out */
    void *caller_sp;        /* the scheduler's while it runs */
#endif
    jmp_buf entry;          /* its start: `restart` comes back here, its frames dropped */
    jmp_buf failed;         /* an error inside it unwinds to here, on its own stack */
    int state;              /* CO_* */
    /* A wait inside a `catch` (or more within each other): what is the system's but the task's while it runs, kept
     * while it waits - the catch an error unwinds to, how deep it is (forth_execute within forth_execute), the
     * catches it is in, the call chain of those depths - and put back for its next turn, from wherever that is. */
    int base, silent_base;  /* f->depth and f->silent this turn began at */
    int extra, silent_extra;   /* (waiting) how much deeper than those it was */
    jmp_buf *catch;         /* (waiting) f->catch */
    Nesting nesting[CO_NESTING_MAX];   /* (waiting) f->nesting from base on */
#ifdef NATIVE_ASAN
    void *caller_fake, *own_fake;   /* (the sanitizer's: each side's fake stack while switched out) */
    const void *caller_bottom;
    size_t caller_size;
#endif
};

/* the task's threaded code from its start (a word with machine code runs that) until it ends or fails, on the
 * coroutine's own stack. `restart` comes back to `entry`. */
static void co_run(Forth *f, struct ForthCoroutine *volatile co) {
    if (setjmp(co->failed) == 0) {
        setjmp(co->entry);
        f->catch = &co->failed;
        forth_run(f);
        co->state = CO_DONE;
    } else {
        co->state = CO_FAILED;
    }
}

#ifdef NATIVE_WIN64

/* (Windows) a fiber of the scheduler's thread: the thread made one the first time */
static void co_caller(struct ForthCoroutine *co) {
    if (!IsThreadAFiber()) {
        if (ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH) == NULL) {
            forth_error(co->f, "tasks: this thread can't be made a fiber (their stacks are fibers)");
        }
        co->f->native->converted = 1;
    }
    co->caller = GetCurrentFiber();
}

static void co_prepare(struct ForthCoroutine *co, Forth *f) {
    (void)f;
    co->afresh = co->midway;   /* (one at its top already starts there) */
}

static void switch_into(struct ForthCoroutine *co) {
    co_caller(co);
    SwitchToFiber(co->fiber);
}

static void switch_out(struct ForthCoroutine *co, int for_good) {
    co->midway = !for_good;
    SwitchToFiber(co->caller);
    co->midway = 0;
    if (co->afresh) {   /* (it waited, and is started afresh: its frames dropped) */
        co->afresh = 0;
        longjmp(co->top, 1);
    }
}

/* the fiber: a run each time it is started, then back to the scheduler */
static void WINAPI co_fiber(void *arg) {
    struct ForthCoroutine *volatile co = arg;

    setjmp(co->top);
    for (;;) {
        co_run(co->f, co);
        switch_out(co, 1);
    }
}

static struct ForthCoroutine *coroutine_new(Forth *f, Task *t) {
    struct ForthCoroutine *co = calloc(1, sizeof(*co));

    if (co == NULL) {
        forth_error(f, "task %s: out of memory for its stack", t->name);
    }
    co->f = f;
    co->fiber = CreateFiberEx(0, CO_STACK_BYTES, FIBER_FLAG_FLOAT_SWITCH, co_fiber, co);
    if (co->fiber == NULL) {
        free(co);
        forth_error(f, "task %s: out of memory for its stack", t->name);
    }
    co->state = CO_FRESH;
    t->co = co;
    return co;
}

static void coroutine_free(struct ForthCoroutine *co) { DeleteFiber(co->fiber); }

/* (a fiber's stack is its own: committed as it is touched, so nothing to share) */
static void co_stack(Forth *f, Task *t, struct ForthCoroutine *co) { (void)f; (void)t; (void)co; }
static void co_unstack(Forth *f, struct ForthCoroutine *co) { (void)f; (void)co; }

#else

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

/* a coroutine's first frame, on its own stack: its run, then back to the scheduler for good */
void clank_co_main(Forth *f) {
    struct ForthCoroutine *co = f->t->co;

#ifdef NATIVE_ASAN
    __sanitizer_finish_switch_fiber(NULL, &co->caller_bottom, &co->caller_size);
#endif
    co_run(f, co);
    switch_out(co, 1);
    abort();   /* (never switched back into: started again, it has a new first frame) */
}

static struct ForthCoroutine *coroutine_new(Forth *f, Task *t) {
    struct ForthCoroutine *co = calloc(1, sizeof(*co));

    if (co == NULL) {
        forth_error(f, "task %s: out of memory for its stack", t->name);
    }
    co->bytes = CO_STACK_BYTES;   /* (its stack taken as it runs: co_stack) */
    co->state = CO_FRESH;
    t->co = co;
    return co;
}

/* A task holds a stack only while its turn runs or it waits: a turn over, the stack goes to the pool. So the
 * mappings are as many as the tasks waiting (each is two: the stack, its guard page - and a process may have only so
 * many), not as many as there are tasks. */
static void co_stack(Forth *f, Task *t, struct ForthCoroutine *co) {
    struct ForthNative *nat = f->native;
    void *m;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    if (co->stack != NULL) {
        return;
    }
    if (nat->npool > 0) {
        co->stack = nat->pool[--nat->npool];
        return;
    }
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
#ifdef MAP_STACK
    flags |= MAP_STACK;
#endif
    m = mmap(NULL, CO_STACK_BYTES, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (m == MAP_FAILED) {
        forth_error(f, "task %s: out of memory for its stack", t->name);
    }
    mprotect(m, CO_GUARD_BYTES, PROT_NONE);
    co->stack = m;
}

static void co_unstack(Forth *f, struct ForthCoroutine *co) {   /* (its turn over, nothing waiting on it) */
    struct ForthNative *nat = f->native;

    if (co->stack == NULL) {
        return;
    }
    if (nat->npool < CO_POOL_MAX) {
        nat->pool[nat->npool++] = co->stack;
    } else {
        munmap(co->stack, co->bytes);
    }
    co->stack = NULL;
}

static void coroutine_free(struct ForthCoroutine *co) {
    if (co->stack != NULL) {
        munmap(co->stack, co->bytes);
    }
}

#endif

int forth_native_turn(Forth *f, Task *t) {
    jmp_buf *outer = f->catch;
    struct ForthCoroutine *co;

    if (f->native == NULL) {
        return 0;
    }
    co = t->co != NULL ? t->co : coroutine_new(f, t);
    if (co->state == CO_RUNNING) {   /* (its turn under way further out: forth.c gives it no other, but never twice) */
        forth_error(f, "task %s: its turn is under way already", t->name);
    }
    co_stack(f, t, co);
    if (co->state != CO_SUSPENDED) {
        co_prepare(co, f);
    }
    co->state = CO_RUNNING;
    co->base = f->depth;
    co->silent_base = f->silent;
    f->catch = &co->failed;
    switch_into(co);
    f->catch = outer;
    f->silent = co->silent_base;   /* (waiting inside a catch: not this side's) */
    if (co->state != CO_SUSPENDED) {
        co_unstack(f, co);
    }
    if (co->state == CO_FAILED) {   /* (the error said, inside: on to the turn's own unwinding, on this stack) */
        co->state = CO_FRESH;
        longjmp(*outer, 1);
    }
    f->yielded = co->state == CO_SUSPENDED;
    return 1;
}

/* the running task is on a coroutine of its own: it can stop in the middle of a primitive and go on there */
int forth_native_in_coroutine(const Forth *f) { return f->t->co != NULL && f->t->co->state == CO_RUNNING; }
void forth_native_suspend(Forth *f) {
    struct ForthCoroutine *co = f->t->co;
    int i;

    if (co == NULL || co->state != CO_RUNNING) {   /* (no coroutine: the inner interpreter sees `yielded`) */
        return;
    }
    co->extra = f->depth - co->base;
    co->silent_extra = f->silent - co->silent_base;
    co->catch = f->catch;
    for (i = 0; i < co->extra && i < CO_NESTING_MAX && co->base + i < NESTING_MAX; i++) {
        co->nesting[i] = f->nesting[co->base + i];
    }
    co->state = CO_SUSPENDED;
    switch_out(co, 0);
    /* its turn again (forth_native_turn set base and silent_base anew): as deep as it was, from there */
    f->catch = co->catch;
    f->depth = f->task_depth = co->base + co->extra;
    f->silent = co->silent_base + co->silent_extra;
    for (i = 0; i < co->extra && co->base + i < NESTING_MAX; i++) {
        f->nesting[co->base + i] = i < CO_NESTING_MAX ? co->nesting[i] : (Nesting){NULL, NULL, 0};
    }
    f->yielded = 0;   /* (its turn again: going on) */
}

void forth_native_restart(Forth *f) {
    struct ForthCoroutine *co = f->t->co;

    if (co != NULL && co->state == CO_RUNNING) {
        f->depth = f->task_depth = co->base;   /* (from inside catches: they are gone) */
        f->silent = co->silent_base;
        longjmp(co->entry, 1);
    }
}

void forth_native_bases(Forth *f, int *depth, int *silent) {
    struct ForthCoroutine *co = f->t != NULL ? f->t->co : NULL;

    if (co != NULL && co->state == CO_RUNNING) {
        *depth = co->base;
        *silent = co->silent_base;
    } else {
        *depth = *silent = 0;
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
        coroutine_free(t->co);
        free(t->co);
        t->co = NULL;
    }
}

/* CLANKFORTH_CODE_MAP names a file: each definition's machine code listed there as it is made - a profiler's samples
 * in machine code are addresses, and this says whose (perf reads it as /tmp/perf-<pid>.map; tools/word-costs.py with
 * callgrind's output). The process's first Forth empties the file (a run before's addresses aren't this one's); its
 * nodes' add to it. */
static void code_map_open(Forth *f) {
    static int emptied;   /* (set by the first Forth, before it starts a node: no two threads here at once) */
    const char *path = getenv("CLANKFORTH_CODE_MAP");
    FILE *was;

    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (!emptied && (was = fopen(path, "w")) != NULL) {
        fclose(was);
    }
    emptied = 1;
    f->native->map = fopen(path, "a");
}

void forth_native_init(Forth *f) {
    f->native = getenv("CLANKFORTH_THREADED") == NULL ? calloc(1, sizeof(struct ForthNative)) : NULL;
    if (f->native != NULL) {
        code_map_open(f);
    }
}

void forth_native_free(Forth *f) {
    CodeChunk *c, *next;

    if (f->native == NULL) {
        return;
    }
    for (c = f->native->chunks; c != NULL; c = next) {
        next = c->next;
        code_unmap(c->start, (size_t)(c->end - c->start));
        free(c);
    }
    if (f->native->map != NULL) {
        fclose(f->native->map);
    }
    free(f->native->inlined);
#ifdef NATIVE_WIN64
    if (f->native->converted && IsThreadAFiber()) {
        ConvertFiberToThread();
    }
#else
    while (f->native->npool > 0) {
        munmap(f->native->pool[--f->native->npool], CO_STACK_BYTES);
    }
#endif
    free(f->native);
    f->native = NULL;
}

#endif

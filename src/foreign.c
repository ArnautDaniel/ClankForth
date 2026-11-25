/* The c vocabulary: a C library's functions as words.
 *
 *   LIBRARY: sqlite libsqlite3.so.0
 *   sqlite FUNCTION: sqlite3_libversion ( -- cell )
 *   sqlite FUNCTION: sqlite3_open ( path db* -- int )
 *
 * A library is opened as the system's loader opens one (dlopen, LoadLibrary) and known by a small number; a function
 * is looked up in it once, as its word is defined, and called with the cells its stack comment counts - whole
 * numbers and addresses, eight at most - giving back what the comment's last item says: `int` or `uint` (32 bits),
 * `cell` (64: a long, a size, an address), or nothing. No floats, no structure passed whole, no function taking a
 * count of arguments that varies (printf), none calling back - a first version.
 *
 * This is the one vocabulary where a mistake is not an error but a crash: a comment that doesn't say what the C
 * function takes, an address it writes past. A host gives it (forth_c) only to scripts it trusts with its process -
 * docs/c.md. */
#include "forth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CLANKFORTH_FOREIGN) && defined(_WIN32)
#include <windows.h>
#define FOREIGN_LOADER 1
#elif defined(CLANKFORTH_FOREIGN)
#include <dlfcn.h>
#define FOREIGN_LOADER 1
#endif

#define FOREIGN_ARGS_MAX 8        /* a function's arguments, at most: each a cell */
#define FOREIGN_PATH_BYTES 1024   /* a library's path, a function's name, at most (its 0 too) */
#define FOREIGN_WHY_BYTES 256     /* why the loader couldn't, as it says */
#define LIBRARIES_FIRST 8

enum { RESULT_NONE, RESULT_CELL, RESULT_INT, RESULT_UINT };   /* what a function gives back */
enum { BODY_FUNCTION, BODY_ARGS, BODY_RESULT, BODY_CELLS };   /* a function word's body */
static const char slow_job[] = "SLOW-FUNCTION:";               /* (its job's name: found again as its word goes on) */

/* the libraries a Forth has opened: each known to the scripts by its place here, from 1 (a number that is no
 * library's is an error, not an address handed to the loader) */
struct ForthForeign {
    void **libraries;
    int n, cap;
};

/* clang's -fsanitize=function reads a type signature before what an indirect call goes to: a C function called
 * with cells where it takes ints has another */
#if defined(__clang__)
#define NOT_TYPE_CHECKED __attribute__((no_sanitize("function")))
#else
#define NOT_TYPE_CHECKED
#endif

/* ---- the loader ---- */

#if defined(FOREIGN_LOADER) && defined(_WIN32)
static void *library_open(const char *path, char *why, size_t n) {
    void *lib = path[0] != '\0' ? (void *)LoadLibraryA(path) : (void *)GetModuleHandleA(NULL);

    if (lib == NULL) {
        snprintf(why, n, "the system's error %lu", (unsigned long)GetLastError());
    }
    return lib;
}
static void *library_symbol(void *lib, const char *name) {
    FARPROC p = GetProcAddress((HMODULE)lib, name);
    void *at;

    memcpy(&at, &p, sizeof(at));   /* (a function's address as data: not a cast C has) */
    return at;
}
#elif defined(FOREIGN_LOADER)
static void *library_open(const char *path, char *why, size_t n) {   /* ("": the program itself, and what it has loaded) */
    void *lib = dlopen(path[0] != '\0' ? path : NULL, RTLD_NOW | RTLD_LOCAL);

    if (lib == NULL) {
        const char *said = dlerror();

        snprintf(why, n, "%s", said != NULL ? said : "no reason given");
    }
    return lib;
}
static void *library_symbol(void *lib, const char *name) { return dlsym(lib, name); }
#else
static void *library_open(const char *path, char *why, size_t n) {
    (void)path;
    snprintf(why, n, "this system was built without a way to load one");
    return NULL;
}
static void *library_symbol(void *lib, const char *name) { (void)lib; (void)name; return NULL; }
#endif

static struct ForthForeign *foreign(Forth *f) {
    if (f->foreign == NULL && (f->foreign = calloc(1, sizeof(struct ForthForeign))) == NULL) {
        forth_error(f, "c: out of memory");
    }
    return f->foreign;
}

/* the library opened, its number */
static Cell open_library(Forth *f, const char *path, const char *word) {
    struct ForthForeign *o = foreign(f);
    char why[FOREIGN_WHY_BYTES];
    void *lib = library_open(path, why, sizeof(why));

    if (lib == NULL) {
        forth_error(f, "%s: can't open %s: %s", word, path[0] != '\0' ? path : "the program itself", why);
    }
    if (o->n == o->cap) {
        int cap = o->cap ? o->cap * 2 : LIBRARIES_FIRST;
        void **more = realloc(o->libraries, (size_t)cap * sizeof(void *));

        if (more == NULL) {
            forth_error(f, "%s: out of memory", word);
        }
        o->libraries = more;
        o->cap = cap;
    }
    o->libraries[o->n++] = lib;
    return o->n;
}

static void *library_arg(Forth *f, Cell x, const char *word) {
    const struct ForthForeign *o = f->foreign;

    if (o == NULL || x < 1 || x > o->n) {
        forth_error(f, "%s: not a library (what LIBRARY: or c-library gave): %ld", word, (long)x);
    }
    return o->libraries[x - 1];
}

/* a function of a library, by its name: its address - an error if the library has none */
static void *symbol_of(Forth *f, void *lib, const char *name, const char *word) {
    void *at = library_symbol(lib, name);

    if (at == NULL) {
        forth_error(f, "%s: the library has no %s", word, name);
    }
    return at;
}

/* the text on the stack ( a n ) as C's, in buf (FOREIGN_PATH_BYTES) */
static const char *text_arg(Forth *f, char *buf, const char *word) {
    Cell n = forth_pop(f), a = forth_pop(f);
    size_t len;
    const char *s = forth_string_arg(f, a, n, word, &len);

    if (len >= FOREIGN_PATH_BYTES) {
        forth_error(f, "%s: a name of %zu bytes (%d at most)", word, len, FOREIGN_PATH_BYTES - 1);
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    return buf;
}

/* ---- the call ---- */

/* the function at `at` called with n cells, as C calls one taking that many whole numbers or addresses */
NOT_TYPE_CHECKED static Cell call_cells(const void *at, const Cell *a, int n) {
    void (*fn)(void);   /* (the one function type a cast to any other is no warning from) */

    memcpy(&fn, &at, sizeof(fn));
    switch (n) {
    case 0: return ((Cell (*)(void))fn)();
    case 1: return ((Cell (*)(Cell))fn)(a[0]);
    case 2: return ((Cell (*)(Cell, Cell))fn)(a[0], a[1]);
    case 3: return ((Cell (*)(Cell, Cell, Cell))fn)(a[0], a[1], a[2]);
    case 4: return ((Cell (*)(Cell, Cell, Cell, Cell))fn)(a[0], a[1], a[2], a[3]);
    case 5: return ((Cell (*)(Cell, Cell, Cell, Cell, Cell))fn)(a[0], a[1], a[2], a[3], a[4]);
    case 6: return ((Cell (*)(Cell, Cell, Cell, Cell, Cell, Cell))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7:
        return ((Cell (*)(Cell, Cell, Cell, Cell, Cell, Cell, Cell))fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    default:
        return ((Cell (*)(Cell, Cell, Cell, Cell, Cell, Cell, Cell, Cell))fn)(a[0], a[1], a[2], a[3], a[4], a[5],
                                                                               a[6], a[7]);
    }
}

/* what the function gave, as its word's comment says it: an int's 32 bits with their sign, a uint's without (the
 * rest of the register is whatever was there), a cell whole - or nothing kept */
static void push_result(Forth *f, Cell r, Cell kind) {
    if (kind == RESULT_INT) {
        forth_push(f, (Cell)(int32_t)(uint32_t)r);
    } else if (kind == RESULT_UINT) {
        forth_push(f, (Cell)(uint32_t)r);
    } else if (kind == RESULT_CELL) {
        forth_push(f, r);
    }
}

static int take_args(Forth *f, const Word *w, Cell *args) {   /* (the last on top) */
    int n = (int)w->body[BODY_ARGS], i;

    for (i = n - 1; i >= 0; i--) {
        args[i] = forth_pop(f);
    }
    return n;
}

/* a function's word: its arguments taken, the function called here, what it gave pushed */
static void doforeign(Forth *f, Word *w) {
    Cell args[FOREIGN_ARGS_MAX], r;
    int n = take_args(f, w, args);

    r = call_cells((const void *)w->body[BODY_FUNCTION], args, n);
    push_result(f, r, w->body[BODY_RESULT]);
}

/* A slow function's word: in an actor's handler the call is made on a worker thread (jobs.c) while the handler waits
 * and its node's other actors go on; elsewhere, here. Its arguments go as cells: what they point at must stay where
 * it is till the call is over (a handler's objects do; net-input's bytes don't). */
static void run_call(ForthJob *j) {   /* ctx: the function; data: its arguments; result: what it gave */
    Cell args[FOREIGN_ARGS_MAX] = {0};

    if (j->data_n > 0) {
        memcpy(args, j->data, j->data_n);
    }
    j->result = (long long)call_cells(j->ctx, args, (int)j->arg);
}
static void doforeign_slow(Forth *f, Word *w) {
    ForthJob *j = forth_job_taken(f, slow_job);   /* (threaded: run again as its task goes on, the call made) */

    if (j == NULL) {
        Cell args[FOREIGN_ARGS_MAX];
        int n = take_args(f, w, args);

        j = forth_job_new(f, run_call, slow_job);
        j->ctx = (const void *)w->body[BODY_FUNCTION];
        j->arg = n;
        if (n > 0) {
            j->data = forth_job_copy(f, j, args, (size_t)n * sizeof(Cell));
            j->data_n = (size_t)n * sizeof(Cell);
        }
        if (!forth_job_may(f, w)) {
            j->run(j);
        } else if ((j = forth_job_wait(f, j)) == NULL) {
            return;
        }
    }
    push_result(f, (Cell)j->result, w->body[BODY_RESULT]);
    forth_job_free(j);
}

/* ---- the defining words ---- */

/* a word `name ( args -- result )` read from the source and made, calling the function `at` (NULL: the function of
 * that name in lib) - its arguments as many as the comment's items before --, its result what the item after says */
static void define_function(Forth *f, void *lib, const void *at, Code code, const char *word) {
    size_t len, tl;
    const char *t = forth_parse_name(f, &len);
    char name[WORD_NAME_MAX + 1], effect[4 * FOREIGN_ARGS_MAX + 16];
    int nargs = 0, result = RESULT_NONE, results = 0, i;
    size_t used;
    Word *w;

    if (len == 0 || len > WORD_NAME_MAX) {
        forth_error(f, "%s: a function's name expected (%d characters at most: a longer one by c-symbol and c-function)",
                    word, WORD_NAME_MAX);
    }
    memcpy(name, t, len);
    name[len] = '\0';
    t = forth_parse_name(f, &tl);
    if (tl != 1 || t[0] != '(') {
        forth_error(f, "%s %s: its stack comment expected, ( args -- result )", word, name);
    }
    for (;;) {   /* its arguments: counted */
        t = forth_parse_name(f, &tl);
        if (tl == 0 || (tl == 1 && t[0] == ')')) {
            forth_error(f, "%s %s: the stack comment needs -- and )", word, name);
        }
        if (tl == 2 && t[0] == '-' && t[1] == '-') {
            break;
        }
        nargs++;
    }
    for (;;) {   /* its result: one, of a kind said */
        t = forth_parse_name(f, &tl);
        if (tl == 0) {
            forth_error(f, "%s %s: the stack comment isn't closed", word, name);
        }
        if (tl == 1 && t[0] == ')') {
            break;
        }
        result = tl == 3 && memcmp(t, "int", 3) == 0 ? RESULT_INT : tl == 4 && memcmp(t, "uint", 4) == 0 ? RESULT_UINT
               : tl == 4 && memcmp(t, "cell", 4) == 0 ? RESULT_CELL : RESULT_NONE;
        if (result == RESULT_NONE || ++results > 1) {
            forth_error(f, "%s %s: its result is int, uint (32 bits), cell (64: a long, an address), or none", word, name);
        }
    }
    if (nargs > FOREIGN_ARGS_MAX) {
        forth_error(f, "%s %s: %d arguments (%d at most)", word, name, nargs, FOREIGN_ARGS_MAX);
    }
    if (at == NULL) {
        at = symbol_of(f, lib, name, word);
    }
    w = forth_new_word(f, name, len, code);
    forth_comma(f, (Cell)at);
    forth_comma(f, nargs);
    forth_comma(f, result);
    used = (size_t)snprintf(effect, sizeof(effect), "(");   /* (what the checker reads: so many in, one out or none) */
    for (i = 0; i < nargs; i++) {
        used += (size_t)snprintf(effect + used, sizeof(effect) - used, " x");
    }
    snprintf(effect + used, sizeof(effect) - used, result != RESULT_NONE ? " -- x )" : " -- )");
    forth_set_effect(f, w, effect);
}

static void p_function(Forth *f, Word *w) {   /* FUNCTION: ( lib "name" "( args -- result )" -- ) */
    (void)w;
    define_function(f, library_arg(f, forth_pop(f), "FUNCTION:"), NULL, doforeign, "FUNCTION:");
}
static void p_slow_function(Forth *f, Word *w) {   /* SLOW-FUNCTION: ( lib "name" "( args -- result )" -- ) */
    (void)w;
    define_function(f, library_arg(f, forth_pop(f), slow_job), NULL, doforeign_slow, slow_job);
}
static void p_c_function(Forth *f, Word *w) {   /* c-function ( addr "name" "( args -- result )" -- ) a function by its address */
    Cell at = forth_pop(f);

    (void)w;
    define_function(f, NULL, forth_addr_n(f, at, 1), doforeign, "c-function");
}

static void p_library(Forth *f, Word *w) {   /* LIBRARY: ( "name" "path" -- ) the library opened: name gives it */
    size_t len, pl;
    const char *t = forth_parse_name(f, &len);
    char name[WORD_NAME_MAX + 1], path[FOREIGN_PATH_BYTES];

    (void)w;
    if (len == 0 || len > WORD_NAME_MAX) {
        forth_error(f, "LIBRARY: a name (%d characters at most), then the library's path, expected", WORD_NAME_MAX);
    }
    memcpy(name, t, len);
    name[len] = '\0';
    t = forth_parse_name(f, &pl);
    if (pl == 0 || pl >= sizeof(path)) {
        forth_error(f, "LIBRARY: %s: the library's path expected (one with a space in it: c-library)", name);
    }
    memcpy(path, t, pl);
    path[pl] = '\0';
    forth_constant(f, name, open_library(f, path, "LIBRARY:"));
}

static void p_c_library(Forth *f, Word *w) {   /* c-library ( path n -- lib ) opened ("": the program itself) */
    char path[FOREIGN_PATH_BYTES];

    (void)w;
    forth_push(f, open_library(f, text_arg(f, path, "c-library"), "c-library"));
}

static void p_c_symbol(Forth *f, Word *w) {   /* c-symbol ( name n lib -- addr ) where a function (or a variable) of it is */
    void *lib = library_arg(f, forth_pop(f), "c-symbol");
    char name[FOREIGN_PATH_BYTES];

    (void)w;
    forth_push(f, (Cell)symbol_of(f, lib, text_arg(f, name, "c-symbol"), "c-symbol"));
}

static void p_z_to_string(Forth *f, Word *w) {   /* z>string ( z -- s len ) C's text (up to its 0) as a new string */
    const char *z = forth_addr_n(f, forth_pop(f), 1);
    size_t len = strlen(z);

    (void)w;
    forth_push(f, (Cell)forth_new_string(f, z, len));
    forth_push(f, (Cell)len);
}

void forth_foreign_free(Forth *f) {   /* (the libraries stay open: what they started - threads, handlers - may go on) */
    if (f->foreign != NULL) {
        free(f->foreign->libraries);
        free(f->foreign);
        f->foreign = NULL;
    }
}

void forth_c(Forth *f) {
    static const ForthPrim prims[] = {
        {"LIBRARY:", p_library, "( -- )"}, {"FUNCTION:", p_function, "( lib -- )"},
        {"SLOW-FUNCTION:", p_slow_function, "( lib -- )"}, {"c-function", p_c_function, "( addr -- )"},
        {"c-library", p_c_library, "( path n -- lib )"}, {"c-symbol", p_c_symbol, "( name n lib -- addr )"},
        {"z>string", p_z_to_string, "( z -- s len )"},
    };
    Vocab *was = forth_current(f), *v = forth_vocab(f, "c");

    forth_set_current(f, v);   /* scripts say USING: c ; */
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_set_current(f, was);
}

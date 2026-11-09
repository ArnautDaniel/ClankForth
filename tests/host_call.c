/* The host's side, from C: what a test in Forth can't reach. A primitive that calls a word with forth_call (a
 * host's hook), the word failing: the word that ran the primitive goes on, its locals and its return as they were. */
#include "forth.h"

#include <stdio.h>
#include <string.h>

#define DICTIONARY_BYTES (1 << 20)
#define TURNS_TEXT "100 "   /* turns: a return stack left deeper each time would overflow well before this many */

static Word *sHook;

static void p_call_hook(Forth *f, Word *w) {   /* ( -- result ) the hook run: 0 it ran, -1 it failed */
    (void)w;
    forth_push(f, forth_call(f, sHook));
}

static void quiet(void *ctx, const char *s, size_t n) {   /* (the failures' messages: expected) */
    (void)ctx, (void)s, (void)n;
}

static int eval(Forth *f, const char *text) { return forth_eval(f, text, strlen(text), "host_call"); }

int main(void) {
    static const char kWords[] =
        ": failing ( -- )  {: | x :}  [: x drop  0 @ drop ;] with-scratch ;\n"   /* (fails in a region, with locals) */
        ": caller ( a b -- result sum )  {: a b :}  call-hook  a b + ;\n"   /* (its locals read after the hook) */
        ": turns ( -- ok? )  true  " TURNS_TEXT "0 do  1 2 caller  3 =  swap -1 =  and  and  loop ;\n";
    Forth *f = forth_new(DICTIONARY_BYTES);
    int failed = 0;

    forth_prim(f, "call-hook", p_call_hook);
    if (forth_prelude(f) != 0 || eval(f, kWords) != 0) {
        fprintf(stderr, "host_call: the words didn't load\n");
        return 1;
    }
    if (eval(f, "' failing") != 0) {
        return 1;
    }
    sHook = (Word *)forth_pop(f);
    forth_set_output(f, quiet, NULL);
    if (eval(f, "turns") != 0) {
        failed = 1;
    } else if (forth_pop(f) != -1) {
        failed = 1;
    }
    forth_free(f);
    printf("host_call: %s\n", failed ? "FAILED: a word's frame lost to a failing hook" : "ok");
    return failed;
}

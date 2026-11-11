/* The host's side, from C: what a test in Forth can't reach. A primitive that calls a word with forth_call (a
 * host's hook), the word failing: the word that ran the primitive goes on, its locals and its return as they were.
 * Then what a host asks of the system (forth.h's functions: its state is a handle), the limit on text that
 * runs too long, and messages sent from C that no actor could be handed. */
#include "actors.h"
#include "forth.h"

#include <stdio.h>
#include <string.h>

#define DICTIONARY_BYTES (1 << 20)
#define TURNS_TEXT "100 "   /* turns: a return stack left deeper each time would overflow well before this many */
#define EVAL_STEPS 100000L  /* the limit on one eval: far more than the checks here take, far fewer than for ever */
#define CALL_STEPS 1000L

static Word *sHook;

static void p_call_hook(Forth *f, Word *w) {   /* ( -- result ) the hook run: 0 it ran, -1 it failed */
    (void)w;
    forth_push(f, forth_call(f, sHook));
}

static void quiet(void *ctx, const char *s, size_t n) {   /* (the failures' messages: expected) */
    (void)ctx, (void)s, (void)n;
}

static int eval(Forth *f, const char *text) { return forth_eval(f, text, strlen(text), "host_call"); }

static int check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "host_call: FAILED: %s\n", what);
    }
    return !ok;
}

/* what a host asks of the system; the failures counted */
static int the_system(Forth *f) {
    OutputFn out;
    void *ctx;
    long errors = forth_errors(f), was;
    Vocab *was_current;
    int failed = 0;

    forth_get_output(f, &out, &ctx);
    failed += check(out == quiet && ctx == NULL, "forth_get_output: what forth_set_output set");
    failed += check(eval(f, ": half") == 0 && forth_compiling(f), "forth_compiling: inside a definition");
    failed += check(eval(f, "1 ;") == 0 && !forth_compiling(f), "forth_compiling: outside one");
    failed += check(strcmp(forth_latest(f)->name, "half") == 0, "forth_latest: the newest word");
    was_current = forth_current(f);
    failed += check(forth_find_vocab(f, "host-call") == NULL, "forth_find_vocab: none of that name yet, none made");
    forth_set_current(f, forth_vocab(f, "host-call"));
    failed += check(forth_find_vocab(f, "host-call") == forth_vocab(f, "host-call"), "forth_find_vocab: the one made");
    failed += check(forth_current(f) == forth_vocab(f, "host-call"), "forth_current: where definitions go");
    forth_set_current(f, was_current);
    failed += check(eval(f, "1 2 3") == 0 && forth_task(f)->sp == 3, "forth_task: the running task's stack");
    forth_task(f)->sp = 0;
    failed += check(eval(f, "0 @") != 0 && forth_errors(f) == errors + 1, "forth_errors: one more");
    failed += check(strcmp(forth_last_error(f), "null address") == 0, "forth_last_error: its message");
    failed += check(eval(f, "0 @") != 0 && forth_error_repeated(f), "forth_error_repeated: the same again");
    forth_forget_error(f);
    failed += check(eval(f, "0 @") != 0 && !forth_error_repeated(f), "forth_forget_error: said anew");
    failed += check(!forth_bye(f), "forth_bye: not yet");

    /* text that never ends: stopped, the limit off again after; a short one is untouched */
    failed += check(eval(f, ": for-ever ( -- ) begin again ;") == 0, "for-ever defined");
    forth_set_eval_budget(f, EVAL_STEPS);
    failed += check(eval(f, "for-ever") != 0, "forth_set_eval_budget: an endless eval stopped");
    failed += check(strstr(forth_last_error(f), "ran too long") != NULL, "its error says so");
    failed += check(eval(f, "1 2 + drop") == 0, "a short eval within the limit");
    forth_set_eval_budget(f, -1);

    /* one call limited by the host, the limit it had put back */
    failed += check(eval(f, "' for-ever") == 0, "for-ever found");
    was = forth_set_budget(f, CALL_STEPS);
    failed += check(was < 0, "forth_set_budget: no limit at first");
    failed += check(forth_call(f, (Word *)forth_pop(f)) != 0, "forth_set_budget: an endless call stopped");
    forth_set_budget(f, was);
    failed += check(eval(f, "bye") == 0 && forth_bye(f), "forth_bye: after bye");
    return failed;
}

/* messages from C: one of a kind there is goes; one of no kind, or with more arguments than a message carries, doesn't */
static int the_actors(Forth *f) {
    Cell args[FORTH_MSG_ARGS + 1] = {0};
    int failed = 0;

    forth_actors(f, 0);
    failed += check(forth_actor_kind(f, "tick") == FORTH_KIND_TICK, "forth_actor_kind: the kernel's own");
    failed += check(forth_actor_kind(f, "no-such-kind") == -1, "forth_actor_kind: none of that name");
    forth_actor_send(f, -1, 0, FORTH_KINDS_MAX, args, 0);
    forth_actor_send(f, -1, 0, -1, args, 0);
    forth_actor_send(f, -1, 0, FORTH_KIND_DIED, args, FORTH_MSG_ARGS + 1);
    forth_actor_broadcast(f, -1, FORTH_KINDS_MAX, args, 0);
    failed += check(eval(f, "actors:queued") == 0 && forth_pop(f) == 0, "forth_actor_send: what can't be a message isn't sent");
    forth_actor_send(f, -1, 0, FORTH_KIND_DIED, args, 1);
    failed += check(eval(f, "actors:queued actors:deliver actors:queued") == 0 && forth_pop(f) == 0 && forth_pop(f) == 1,
                    "forth_actor_send: a message queued, and delivered (to nobody: a dead letter)");
    return failed;
}

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
    failed += check(!failed, "a word's frame lost to a failing hook");
    failed += the_actors(f);
    failed += the_system(f);
    forth_free(f);
    printf("host_call: %s\n", failed ? "FAILED" : "ok");
    return failed != 0;
}

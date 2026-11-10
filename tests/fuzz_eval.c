/* A fuzzer's way in (libFuzzer: clang, -DCLANKFORTH_FUZZ=ON): each input is text for a fresh Forth system to
 * interpret, then a few frames for the tasks and actors it began. What it looks for is a script's mistake that is a
 * crash (or the sanitizers' report) where it should be an error.
 *
 *   cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCLANKFORTH_FUZZ=ON && ninja -C build-fuzz
 *   build-fuzz/fuzz_eval -dict=tests/fuzz.dict -max_len=256 corpus/
 *
 * A real address still reaches anything (docs/forth.md: scripts aren't safe to run untrusted), so a crash it finds
 * is to be read before it is believed: `variable v  v 1000000 + @` is the language, not a fault. */
#include "actors.h"

#include <stddef.h>
#include <stdint.h>

#define DICTIONARY_BYTES (1 << 20)
#define EVAL_STEPS 200000L   /* (an input that loops for ever is stopped, not a hang reported) */
#define FRAMES 4

static void quiet(void *ctx, const char *s, size_t n) { (void)ctx, (void)s, (void)n; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Forth *f = forth_new(DICTIONARY_BYTES);
    int i;

    if (f == NULL) {
        return 0;
    }
    forth_set_output(f, quiet, NULL);
    forth_actors(f, 0);
    if (forth_prelude(f) == 0) {
        forth_set_eval_budget(f, EVAL_STEPS);
        forth_eval(f, (const char *)data, size, "fuzz");
        for (i = 0; i < FRAMES && !forth_bye(f); i++) {
            forth_run_tasks(f);
            forth_actors_frame(f);
        }
    }
    forth_free(f);
    return 0;
}

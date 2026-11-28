/* A fuzzer for what a server reads from the internet (libFuzzer: clang, -DCLANKFORTH_FUZZ=ON): an HTTP request's head
 * and body (chunked too), the parameters and cookies read from it, a multipart upload, JSON (a live event's, an API
 * body's), percent-decoding, gzip and DEFLATE, a JWT. The first byte picks which; the rest is the input. One Forth,
 * made once with the vocabularies; each input read in a scratch region, its errors caught - what it looks for is a
 * crash or the sanitizers' report where the parsers should say no.
 *
 *   cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCLANKFORTH_FUZZ=ON && ninja -C build-fuzz
 *   build-fuzz/fuzz_http -max_len=4096 corpus-http/ */
#include "forth_internal.h"
#include "actors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DICTIONARY_BYTES (1 << 22)
#define EVAL_STEPS 2000000L   /* (an input that makes a parser loop for ever is stopped: reported as a hang) */

static const char driver[] =
    "USING: http http.private http.cookies http.uploads json compression urls.encoding base64 crypto.jwt ;\n"
    "variable chunks\n"
    ": read-request ( a n -- )\n"
    "    {: a n :}  0 new-request {: r :}  0 chunks !\n"
    "    a n >seq r chunks request-at 0> if\n"
    "        r keep-alive? drop  r req-path 2drop\n"
    "        s\" a\" r query-param if  2drop  then  s\" x\" r form-param if  2drop  then\n"
    "        s\" sid\" r req-cookie if  2drop  then  r req-parts if  drop  then\n"
    "    then ;\n"
    ": read-json ( a n -- )  json> >json 2drop ;\n"
    ": read-decoded ( a n -- )  {: a n :}  a n url-decode 2drop  a n path-decode 2drop ;\n"
    ": read-gzip ( a n -- )  {: a n :}  a n 1000000 gunzip if  2drop  then  a n 1000000 inflate if  2drop  then ;\n"
    "s\\\" {\\\"keys\\\":[{\\\"kty\\\":\\\"RSA\\\",\\\"kid\\\":\\\"k\\\",\\\"n\\\":\\\"AQAB\\\",\\\"e\\\":\\\"AQAB\\\"}]}\" json> constant fuzz-keys\n"
    ": read-jwt ( a n -- )  {: a n :}  a n base64> if  2drop  then  a n fuzz-keys jwt-verify drop ;\n"
    ": fuzz-one ( s kind -- )\n"
    "    {: s kind :}  s string {: a n :}\n"
    "    [:  kind 6 mod case\n"
    "            0 of  a n read-request  endof   1 of  a n read-json  endof   2 of  a n read-decoded  endof\n"
    "            3 of  a n read-gzip  endof   4 of  a n read-jwt  endof   5 of  a n read-request  endof\n"
    "        endcase ;] ['] with-scratch catch if  drop  then  s dispose ;\n"
    "' fuzz-one\n";   /* (its token left for the harness) */

static Forth *f;
static Word *fuzz_one;

static void quiet(void *ctx, const char *s, size_t n) { (void)ctx, (void)s, (void)n; }

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc, (void)argv;
    f = forth_new(DICTIONARY_BYTES);
    if (f == NULL) {
        return 0;
    }
    forth_actors(f, 0);
    forth_system(f);
    forth_net(f);
    if (forth_prelude(f) != 0 || forth_eval(f, driver, sizeof(driver) - 1, "fuzz_http") != 0) {
        forth_free(f);
        f = NULL;
        return 0;
    }
    fuzz_one = (Word *)forth_pop(f);
    forth_set_output(f, quiet, NULL);   /* (what the driver says while it loads: shown; the inputs' errors: not) */
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Cell s;

    if (f == NULL || fuzz_one == NULL || size == 0) {
        return 0;
    }
    forth_set_eval_budget(f, EVAL_STEPS);
    s = forth_new_string(f, (const char *)data + 1, size - 1);
    forth_push(f, s);
    forth_push(f, (Cell)data[0]);
    forth_call(f, fuzz_one);   /* (a parser's "no" is an error: caught there, the input let go) */
    return 0;
}

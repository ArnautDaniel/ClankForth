/* The stack checker left out (CLANKFORTH_CHECKER=OFF): what the rest of the system and a host call of it, doing
 * nothing. A stack comment is a comment; `;` checks nothing; no word's effect is known or asked for. The words that
 * ordinary scripts say of it stay, so a script written for the checker loads without it: `holds:` (read, and let
 * go) and `is`'s store. Its own words for looking (`effect.`, `stack-effect`, `.checked`) are not here.
 * `effects-checked?` says which this is. */
#include "forth_internal.h"

#include <string.h>

int forth_parse_effect(const char *s, size_t n, ForthEffect *e) {
    (void)s, (void)n, (void)e;
    return 0;   /* (not one: a comment only) */
}

int forth_set_effect(Forth *f, Word *w, const char *comment) {
    (void)f, (void)w, (void)comment;
    return 0;
}

void forth_declare_effect(Forth *f, const char *comment, const char *why) { (void)f, (void)comment, (void)why; }
void forth_check_definition(Forth *f, Word *w) { (void)f, (void)w; }
void forth_check_quotation(Forth *f, Word *q) { (void)f, (void)q; }

PRIM(p_holds) {   /* holds: ( effect ) - what a slot holds: for the checker, so read past */
    size_t len;
    const char *s = forth_parse_name(f, &len);

    if (len != 1 || s[0] != '(') {
        forth_error(f, "holds: a stack comment expected - holds: ( a -- b )");
    }
    (void)forth_parse_until(f, ')', &len);
}

PRIM(p_is) {   /* (is) ( xt body -- ) `' w is name` outside a definition: stored */
    Cell body = POP(), xt = POP();

    memcpy(forth_addr_n(f, body, sizeof(xt)), &xt, sizeof(xt));
}

void forth_effects_words(Forth *f) {
    forth_prim(f, "holds:", p_holds);
    forth_prim(f, "(is)", p_is);
    forth_constant(f, "effects-checked?", 0);
}

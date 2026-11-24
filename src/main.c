/* `clankforth`: the tool with the language and its actor kernel (src/tool.c). */
#include "actors.h"

static void bind(Forth *f) {   /* (actors with no bodies: no host's world - and no table of the host's by slot) */
    forth_actors(f, 0);
    forth_set_actors_max(f, FORTH_ACTOR_SLOTS_MAX);
    forth_system(f);   /* (a program's tool: its files, its arguments, the clock) */
    forth_net(f);      /* (and the network) */
    forth_set_slice(f, FORTH_SLICE_STEPS);   /* (a server: its handlers sliced - a long one doesn't hold its node) */
}

FORTH_SANITIZER_DEFAULTS   /* (a sanitizer's build: its fake stacks off - forth.h) */

int main(int argc, char **argv) { return forth_tool_main(argc, argv, bind); }

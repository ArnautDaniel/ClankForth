/* `clankforth`: the tool with the language and its actor kernel (src/tool.c). */
#include "actors.h"

static void bind(Forth *f) { forth_actors(f, 0); }   /* (actors with no bodies: no host's world) */

int main(int argc, char **argv) { return forth_tool_main(argc, argv, bind); }

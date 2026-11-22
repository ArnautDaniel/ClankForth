/* The `forth` tool (forth.h's forth_tool_main): the Forth system on its own, for trying things and for tests.
 *
 *   forth                  a prompt
 *   forth a.fs b.fs        run the files, then exit (status: `exit-status` if a file set it,
 *                          1 if a file had an error, else 0)
 *   forth -i a.fs          run the files, then a prompt
 *   forth a.fs -- x y      run the file, its `args` (the system vocabulary's) x and y
 *   forth --version        say the version (forth.h's CLANKFORTH_VERSION) and exit
 *
 * Each file's folder is a root its USING:s are looked for in. */
#include "actors.h"
#include "forth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_BYTES 1024
#define PATH_MAX_BYTES 1024

static int sStatus;

static void p_exit_status(Forth *f, Word *w) {   /* ( n -- ) the tool's exit status */
    (void)w;
    sStatus = (int)forth_pop(f);
}

static void repl(Forth *f) {
    char line[LINE_MAX_BYTES];

    while (!forth_bye(f) && fgets(line, sizeof(line), stdin) != NULL) {
        if (forth_eval(f, line, strlen(line), "input") == 0 && !forth_compiling(f)) {
            int attached = forth_actor_attached(f);

            if (attached >= 0) {   /* (the actor its lines run as: `attach`) */
                printf(" ok %s#%d\n", forth_actor_name(f, attached), attached);
            } else {
                printf(" ok\n");
            }
        }
    }
}

int forth_tool_main(int argc, char **argv, void (*bind)(Forth *f)) {
    Forth *f;
    int i, interactive = argc == 1, failed = 0;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("clankforth %s\n", forth_version());
        return 0;
    }
    f = forth_new(16 << 20);

    forth_prim(f, "exit-status", p_exit_status);
    if (bind != NULL) {
        bind(f);
        forth_set_node_setup(f, bind);   /* (a node it starts has the host's words too) */
    }
    forth_report_crashes(f);
    if (forth_prelude(f) != 0) {
        return 1;
    }
    for (i = 1; i < argc; i++) {   /* (after --: the scripts' arguments, not files) */
        if (strcmp(argv[i], "--") == 0) {
            forth_set_args(f, argc - i - 1, argv + i + 1);
            argc = i;
            interactive = argc == 1;   /* (no file before it: a prompt) */
            break;
        }
    }
    for (i = 1; i < argc && !forth_bye(f); i++) {
        if (strcmp(argv[i], "-i") == 0) {
            interactive = 1;
        } else {
            char root[PATH_MAX_BYTES], *slash;

            snprintf(root, sizeof(root), "%s", argv[i]);   /* its folder: for its USING:s */
            slash = strrchr(root, '/');
            if (slash != NULL) {
                *slash = 0;
                forth_add_root(f, root);
            }
            if (forth_include(f, argv[i]) != 0) {
                failed = 1;
            }
        }
    }
    if (interactive) {
        repl(f);
    }
    forth_free(f);
    return sStatus != 0 ? sStatus : failed;
}

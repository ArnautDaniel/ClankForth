/* The system vocabulary: what a program outside a game wants of its machine - files, its arguments and environment,
 * other programs, the clock. A host gives it to its scripts with forth_system (the tool does); one that doesn't - a
 * game whose mods shouldn't touch the disk - has none of these words. Scripts say USING: system ;
 * Docs: docs/forth.md, "The system vocabulary". */
#include "forth_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#define open_pipe _popen
#define close_pipe _pclose
#else
#include <sys/wait.h>
#include <unistd.h>
#define make_directory(path) mkdir((path), 0777)
#define remove_directory(path) rmdir(path)
#define open_pipe popen
#define close_pipe pclose
#endif

#define PATH_BYTES 4096        /* a path, a command, a variable's name: no longer */
#define READ_CHUNK 65536       /* a file or a program's output read this much at a time */
#define MS_PER_S 1000
#define US_PER_S 1000000
#define NS_PER_MS 1000000L
#define NS_PER_US 1000

/* the text ( a n ) as a C string, in buf: an error if it doesn't fit, or holds a 0 (it would be cut there) */
static const char *c_string(Forth *f, Cell a, Cell n, char *buf, const char *word) {
    size_t len;
    const char *s = forth_string_arg(f, a, n, word, &len);

    if (len >= PATH_BYTES) {
        forth_error(f, "%s: %zu bytes, longer than %d", word, len, PATH_BYTES - 1);
    }
    if (memchr(s, 0, len) != NULL) {
        forth_error(f, "%s: a 0 byte in \"%.*s\"", word, (int)len, s);
    }
    memcpy(buf, s, len);
    buf[len] = 0;
    return buf;
}

static const char *path_arg(Forth *f, char *buf, const char *word) {   /* ( a n -- ) */
    Cell n = forth_pop(f), a = forth_pop(f);

    return c_string(f, a, n, buf, word);
}

static void push_text(Forth *f, const char *s, size_t len) {   /* ( -- s len ) a new string */
    forth_push(f, (Cell)forth_new_string(f, s, len));
    forth_push(f, (Cell)len);
}

/* a whole file's bytes, malloc'd (*n of them); an error says why it couldn't be read */
static char *slurp(Forth *f, const char *path, size_t *n, const char *word) {
    FILE *in = fopen(path, "rb");
    char *data = NULL;
    size_t len = 0, cap = 0, got;

    if (in == NULL) {
        forth_error(f, "%s: %s: %s", word, path, strerror(errno));
    }
    do {
        if (cap - len < READ_CHUNK) {
            char *more = realloc(data, cap + READ_CHUNK);

            if (more == NULL) {
                free(data);
                fclose(in);
                forth_error(f, "%s: %s: out of memory at %zu bytes", word, path, len);
            }
            data = more;
            cap += READ_CHUNK;
        }
        got = fread(data + len, 1, cap - len, in);
        len += got;
    } while (got > 0);
    if (ferror(in)) {
        free(data);
        fclose(in);
        forth_error(f, "%s: %s: a read failed", word, path);
    }
    fclose(in);
    *n = len;
    return data;
}

/* read-file ( path n -- s len ) a file's bytes, a new string */
static void p_read_file(Forth *f, Word *w) {
    char path[PATH_BYTES];
    size_t len;
    char *data = slurp(f, path_arg(f, path, "read-file"), &len, "read-file");
    char *s = NULL;

    (void)w;
    s = forth_new_string(f, data, len);   /* (out of memory here: data lost - so little matters little) */
    free(data);
    forth_push(f, (Cell)s);
    forth_push(f, (Cell)len);
}

/* read-bytes ( path n -- b ) a file's bytes, a new byte buffer */
static void p_read_bytes(Forth *f, Word *w) {
    char path[PATH_BYTES];
    size_t len;
    char *data = slurp(f, path_arg(f, path, "read-bytes"), &len, "read-bytes");
    Cell b;

    (void)w;
    b = forth_new_bytes(f, data, len);
    free(data);
    forth_push(f, b);
}

static void spill(Forth *f, const char *path, const void *data, size_t n, const char *mode, const char *word) {
    FILE *out = fopen(path, mode);

    if (out == NULL) {
        forth_error(f, "%s: %s: %s", word, path, strerror(errno));
    }
    if (fwrite(data, 1, n, out) != n) {
        fclose(out);
        forth_error(f, "%s: %s: a write failed", word, path);
    }
    if (fclose(out) != 0) {
        forth_error(f, "%s: %s: %s", word, path, strerror(errno));
    }
}

static void write_text(Forth *f, const char *mode, const char *word) {   /* ( a n path pn -- ) */
    char path[PATH_BYTES];
    const char *p = path_arg(f, path, word);
    Cell n = forth_pop(f), a = forth_pop(f);
    size_t len;
    const char *s = forth_string_arg(f, a, n, word, &len);

    spill(f, p, s, len, mode, word);
}
static void p_write_file(Forth *f, Word *w) { (void)w; write_text(f, "wb", "write-file"); }   /* ( a n path pn -- ) */
static void p_append_file(Forth *f, Word *w) { (void)w; write_text(f, "ab", "append-file"); }
static void p_write_bytes(Forth *f, Word *w) {   /* write-bytes ( b path pn -- ) */
    char path[PATH_BYTES];
    const char *p = path_arg(f, path, "write-bytes");
    size_t n;
    const uint8_t *data = forth_bytes_of(f, forth_pop(f), &n, "write-bytes");

    (void)w;
    spill(f, p, data, n, "wb", "write-bytes");
}

static void p_file_exists(Forth *f, Word *w) {   /* file-exists? ( path n -- flag ) a file or a folder */
    char path[PATH_BYTES];
    struct stat st;

    (void)w;
    forth_push(f, stat(path_arg(f, path, "file-exists?"), &st) == 0 ? -1 : 0);
}
static void p_file_size(Forth *f, Word *w) {   /* file-size ( path n -- n ) */
    char path[PATH_BYTES];
    struct stat st;
    const char *p = path_arg(f, path, "file-size");

    (void)w;
    if (stat(p, &st) != 0) {
        forth_error(f, "file-size: %s: %s", p, strerror(errno));
    }
    forth_push(f, (Cell)st.st_size);
}
static void p_delete_file(Forth *f, Word *w) {   /* delete-file ( path n -- ) a file, or an empty folder */
    char path[PATH_BYTES];
    const char *p = path_arg(f, path, "delete-file");
    struct stat st;

    (void)w;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode) ? remove_directory(p) != 0 : remove(p) != 0) {
        forth_error(f, "delete-file: %s: %s", p, strerror(errno));
    }
}
static void p_rename_file(Forth *f, Word *w) {   /* rename-file ( from n to n2 -- ) */
    char from[PATH_BYTES], to[PATH_BYTES];
    const char *t = path_arg(f, to, "rename-file"), *fr = path_arg(f, from, "rename-file");

    (void)w;
    if (rename(fr, t) != 0) {
        forth_error(f, "rename-file: %s to %s: %s", fr, t, strerror(errno));
    }
}
static void p_make_dir(Forth *f, Word *w) {   /* make-dir ( path n -- ) (one there already: nothing) */
    char path[PATH_BYTES];
    const char *p = path_arg(f, path, "make-dir");

    (void)w;
    if (make_directory(p) != 0 && errno != EEXIST) {
        forth_error(f, "make-dir: %s: %s", p, strerror(errno));
    }
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* dir-list ( path n -- l ) the names in a folder (not . and ..), sorted: a list of new strings */
static void p_dir_list(Forth *f, Word *w) {
    char path[PATH_BYTES], **names = NULL;
    const char *p = path_arg(f, path, "dir-list");
    DIR *d = opendir(p);
    struct dirent *e;
    size_t n = 0, cap = 0, i;
    Cell l;

    (void)w;
    if (d == NULL) {
        forth_error(f, "dir-list: %s: %s", p, strerror(errno));
    }
    while ((e = readdir(d)) != NULL) {   /* (the names first, C's: no error can come between them and the list) */
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            char **more = realloc(names, (cap = cap ? cap * 2 : 64) * sizeof(char *));

            if (more == NULL) {
                break;
            }
            names = more;
        }
        if ((names[n] = malloc(strlen(e->d_name) + 1)) == NULL) {
            break;
        }
        strcpy(names[n++], e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char *), by_name);
    l = forth_new_list(f);
    for (i = 0; i < n; i++) {
        forth_list_push(f, l, (Cell)forth_new_string(f, names[i], strlen(names[i])));
        free(names[i]);
    }
    free(names);
    forth_push(f, l);
}

/* read-line ( -- s len true | false ) the next line of the input (without its end), false at its end */
static void p_read_line(Forth *f, Word *w) {
    char *line = NULL;
    size_t len = 0, cap = 0;
    int c;

    (void)w;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            char *more = realloc(line, cap = cap ? cap * 2 : 256);

            if (more == NULL) {
                free(line);
                forth_error(f, "read-line: out of memory at %zu bytes", len);
            }
            line = more;
        }
        line[len++] = (char)c;
    }
    if (c == EOF && len == 0) {
        free(line);
        forth_push(f, 0);
        return;
    }
    if (len > 0 && line[len - 1] == '\r') {   /* (a Windows line's end) */
        len--;
    }
    push_text(f, line != NULL ? line : "", len);
    free(line);
    forth_push(f, -1);
}

void forth_set_args(Forth *f, int argc, char **argv) {
    f->args = argv;
    f->nargs = argc;
}

static void p_args(Forth *f, Word *w) {   /* args ( -- l ) the program's arguments (the host's to give): new strings */
    Cell l = forth_new_list(f);
    int i;

    (void)w;
    for (i = 0; i < f->nargs; i++) {
        forth_list_push(f, l, (Cell)forth_new_string(f, f->args[i], strlen(f->args[i])));
    }
    forth_push(f, l);
}

static void p_getenv(Forth *f, Word *w) {   /* getenv ( name n -- s len true | false ) */
    char name[PATH_BYTES];
    const char *v = getenv(path_arg(f, name, "getenv"));

    (void)w;
    if (v == NULL) {
        forth_push(f, 0);
        return;
    }
    push_text(f, v, strlen(v));
    forth_push(f, -1);
}

static void p_exit_with(Forth *f, Word *w) {   /* exit-with ( n -- ) the program ends now, with that status */
    int status = (int)forth_pop(f);

    (void)w;
    fflush(stdout);
    exit(status);
}

static int exit_status(int raw) {   /* (a command's: its own status, as the shell would say it) */
#ifdef _WIN32
    return raw;
#else
    return raw == -1 ? -1 : WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
}

static void p_shell(Forth *f, Word *w) {   /* shell ( command n -- status ) run by the system's shell */
    char cmd[PATH_BYTES];
    const char *c = path_arg(f, cmd, "shell");

    (void)w;
    fflush(stdout);
    forth_push(f, exit_status(system(c)));
}

/* shell-output ( command n -- s len status ) run by the shell, its output a new string */
static void p_shell_output(Forth *f, Word *w) {
    char cmd[PATH_BYTES], *out = NULL;
    const char *c = path_arg(f, cmd, "shell-output");
    size_t len = 0, cap = 0, got;
    FILE *p;
    int status;

    (void)w;
    fflush(stdout);
    if ((p = open_pipe(c, "r")) == NULL) {
        forth_error(f, "shell-output: %s: %s", c, strerror(errno));
    }
    do {
        if (cap - len < READ_CHUNK) {
            char *more = realloc(out, cap + READ_CHUNK);

            if (more == NULL) {
                break;
            }
            out = more;
            cap += READ_CHUNK;
        }
        got = fread(out + len, 1, cap - len, p);
        len += got;
    } while (got > 0);
    status = exit_status(close_pipe(p));
    push_text(f, out != NULL ? out : "", len);
    free(out);
    forth_push(f, status);
}

static long long monotonic_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * (NS_PER_MS * MS_PER_S) + ts.tv_nsec;
}
static void p_ms_now(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)(monotonic_ns() / NS_PER_MS)); }   /* ( -- ms ) */
static void p_us_now(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)(monotonic_ns() / NS_PER_US)); }   /* ( -- us ) */
static void p_time_now(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)time(NULL)); }   /* ( -- s ) since 1970 */
static void p_sleep_ms(Forth *f, Word *w) {   /* sleep-ms ( ms -- ) the thread sleeps (its Forth with it) */
    Cell ms = forth_pop(f);
    struct timespec ts;

    (void)w;
    if (ms <= 0) {
        return;
    }
    ts.tv_sec = (time_t)(ms / MS_PER_S);
    ts.tv_nsec = (long)(ms % MS_PER_S) * NS_PER_MS;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void forth_system(Forth *f) {
    static const ForthPrim prims[] = {
        {"read-file", p_read_file, "( path n -- s len )"}, {"read-bytes", p_read_bytes, "( path n -- b )"},
        {"write-file", p_write_file, "( a n path pn -- )"}, {"append-file", p_append_file, "( a n path pn -- )"},
        {"write-bytes", p_write_bytes, "( b path pn -- )"}, {"file-exists?", p_file_exists, "( path n -- flag )"},
        {"file-size", p_file_size, "( path n -- n )"}, {"delete-file", p_delete_file, "( path n -- )"},
        {"rename-file", p_rename_file, "( from n to n2 -- )"}, {"make-dir", p_make_dir, "( path n -- )"},
        {"dir-list", p_dir_list, "( path n -- l )"}, {"read-line", p_read_line, "( -- s len true | false )"},
        {"args", p_args, "( -- l )"}, {"getenv", p_getenv, "( name n -- s len true | false )"},
        {"exit-with", p_exit_with, "( n -- )"}, {"shell", p_shell, "( command n -- status )"},
        {"shell-output", p_shell_output, "( command n -- s len status )"},
        {"ms-now", p_ms_now, "( -- ms )"}, {"us-now", p_us_now, "( -- us )"}, {"time-now", p_time_now, "( -- s )"},
        {"sleep-ms", p_sleep_ms, "( ms -- )"},
    };
    Vocab *was = forth_current(f), *v = forth_vocab(f, "system");

    forth_set_current(f, v);   /* scripts say USING: system ; */
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_set_current(f, was);
}

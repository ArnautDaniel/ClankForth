/* The system vocabulary: what a program outside a game wants of its machine - files, its arguments and environment,
 * other programs, the clock. A host gives it to its scripts with forth_system (the tool does); one that doesn't - a
 * game whose mods shouldn't touch the disk - has none of these words. Scripts say USING: system ;
 * Docs: docs/forth.md, "The system vocabulary". */
#include "forth_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#define open_pipe _popen
#define close_pipe _pclose
#define APPEND_FLAGS (_O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY)
#define APPEND_MODE (_S_IREAD | _S_IWRITE)
#define write_fd _write
#define close_fd _close
#define open_fd _open
#else
#include <sys/wait.h>
#include <unistd.h>
#define make_directory(path) mkdir((path), 0777)
#define remove_directory(path) rmdir(path)
#define open_pipe popen
#define close_pipe pclose
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#define APPEND_FLAGS (O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC)
#define APPEND_MODE 0666
#define write_fd write
#define close_fd close
#define open_fd open
#endif

#define PATH_BYTES 4096        /* a path, a command, a variable's name: no longer */
#define READ_CHUNK 65536       /* a file or a program's output read this much at a time */
#define MS_PER_S 1000
#define US_PER_S 1000000
#define NS_PER_MS 1000000L
#define NS_PER_US 1000
#define FILES_MAX 64           /* files open at once (file-open-append's), each Forth */
#define SHUTDOWN_HOOKS_MAX 16  /* on-shutdown's words, each Forth */
#define SHUTDOWN_GRACE_MS 30000   /* the main loop's wait for the nodes draining, after a signal, at most */

/* the files a script holds open (file-open-append): a handle is its slot + 1, each slot's descriptor -1 when free;
 * the words to run when the process is told to stop (on-shutdown), and whether they have run */
struct ForthSystem {
    int fd[FILES_MAX];
    Word *hooks[SHUTDOWN_HOOKS_MAX];
    int nhooks, hooks_ran;
};

/* (the process's, every node's: a signal is the process's) SIGTERM or SIGINT came */
static volatile sig_atomic_t sShutdown;
static volatile sig_atomic_t sHandlers;   /* the handlers set (the first on-shutdown's) */
static long sGraceMs = SHUTDOWN_GRACE_MS;

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
static void p_file_modified(Forth *f, Word *w) {   /* file-modified ( path n -- s ) last written: seconds since 1970 */
    char path[PATH_BYTES];
    struct stat st;
    const char *p = path_arg(f, path, "file-modified");

    (void)w;
    if (stat(p, &st) != 0) {
        forth_error(f, "file-modified: %s: %s", p, strerror(errno));
    }
    forth_push(f, (Cell)st.st_mtime);
}
static void p_dir_q(Forth *f, Word *w) {   /* dir? ( path n -- flag ) a folder there */
    char path[PATH_BYTES];
    struct stat st;

    (void)w;
    forth_push(f, stat(path_arg(f, path, "dir?"), &st) == 0 && S_ISDIR(st.st_mode) ? -1 : 0);
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
/* rename-file ( from n to n2 -- ) a file there already replaced - at once, as one step, where the system can (POSIX's
 * rename; Windows' MoveFileEx, written through) */
static void p_rename_file(Forth *f, Word *w) {
    char from[PATH_BYTES], to[PATH_BYTES];
    const char *t = path_arg(f, to, "rename-file"), *fr = path_arg(f, from, "rename-file");

    (void)w;
#ifdef _WIN32
    if (!MoveFileExA(fr, t, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        forth_error(f, "rename-file: %s to %s: Windows error %lu", fr, t, (unsigned long)GetLastError());
    }
#else
    if (rename(fr, t) != 0) {
        forth_error(f, "rename-file: %s to %s: %s", fr, t, strerror(errno));
    }
#endif
}

/* ---- files held open: appended to, and made to last (fsync) - a log's ---- */

static struct ForthSystem *files_of(Forth *f) {   /* (made the first time) */
    int i;

    if (f->system == NULL) {
        if ((f->system = calloc(1, sizeof(struct ForthSystem))) == NULL) {
            forth_error(f, "system: out of memory");
        }
        for (i = 0; i < FILES_MAX; i++) {
            f->system->fd[i] = -1;
        }
    }
    return f->system;
}

static int file_slot(Forth *f, const char *word) {   /* ( file -- ) its slot: an error for a handle not open */
    Cell h = forth_pop(f);

    if (f->system == NULL || h < 1 || h > FILES_MAX || f->system->fd[h - 1] < 0) {
        forth_error(f, "%s: %ld is no file open (file-open-append's)", word, (long)h);
    }
    return (int)h - 1;
}
static int file_arg(Forth *f, const char *word) {   /* ( file -- ) its descriptor */
    int slot = file_slot(f, word);

    return f->system->fd[slot];
}

/* file-open-append ( path n -- file ) a file to write at its end (made if it isn't there): a handle for file-write,
 * file-sync, file-close */
static void p_file_open_append(Forth *f, Word *w) {
    char path[PATH_BYTES];
    const char *p = path_arg(f, path, "file-open-append");
    struct ForthSystem *s = files_of(f);
    int slot, fd;

    (void)w;
    for (slot = 0; slot < FILES_MAX && s->fd[slot] >= 0; slot++) {
    }
    if (slot == FILES_MAX) {
        forth_error(f, "file-open-append: %s: %d files open already, the most there may be", p, FILES_MAX);
    }
    if ((fd = open_fd(p, APPEND_FLAGS, APPEND_MODE)) < 0) {
        forth_error(f, "file-open-append: %s: %s", p, strerror(errno));
    }
    s->fd[slot] = fd;
    forth_push(f, slot + 1);
}

/* file-write ( a n file -- ) the bytes at its end, all of them (the system may hold them a while: file-sync) */
static void p_file_write(Forth *f, Word *w) {
    int fd = file_arg(f, "file-write");
    Cell n = forth_pop(f), a = forth_pop(f);
    size_t len, done = 0;
    const char *s = forth_string_arg(f, a, n, "file-write", &len);

    (void)w;
    while (done < len) {
        size_t chunk = len - done > READ_CHUNK ? READ_CHUNK : len - done;
        long put = (long)write_fd(fd, s + done, (unsigned)chunk);

        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            forth_error(f, "file-write: %s (%zu of %zu bytes written)", strerror(errno), done, len);
        }
        done += (size_t)put;
    }
}

/* file-sync ( file -- ) what was written to it on the disk itself before this returns: fsync (fdatasync where there is
 * one; F_FULLFSYNC on macOS, whose fsync leaves it in the drive's cache; _commit on Windows) */
static void p_file_sync(Forth *f, Word *w) {
    int fd = file_arg(f, "file-sync"), r;

    (void)w;
#if defined(_WIN32)
    r = _commit(fd);
#elif defined(F_FULLFSYNC)
    r = fcntl(fd, F_FULLFSYNC);
    if (r != 0) {
        r = fsync(fd);
    }
#elif defined(__linux__)
    r = fdatasync(fd);
#else
    r = fsync(fd);
#endif
    if (r != 0) {
        forth_error(f, "file-sync: %s", strerror(errno));
    }
}

static void p_file_close(Forth *f, Word *w) {   /* file-close ( file -- ) */
    int slot = file_slot(f, "file-close"), fd = f->system->fd[slot];

    (void)w;
    f->system->fd[slot] = -1;
    if (close_fd(fd) != 0) {
        forth_error(f, "file-close: %s", strerror(errno));
    }
}

/* dir-sync ( path n -- ) a folder's names on the disk (a file made, renamed or deleted in it lasts): fsync of the
 * folder (POSIX; nothing on Windows, whose MoveFileEx is written through) */
static void p_dir_sync(Forth *f, Word *w) {
    char path[PATH_BYTES];
    const char *p = path_arg(f, path, "dir-sync");

    (void)w;
#ifndef _WIN32
    {
        int fd = open(p, O_RDONLY | O_CLOEXEC), r;

        if (fd < 0) {
            forth_error(f, "dir-sync: %s: %s", p, strerror(errno));
        }
        r = fsync(fd);
        close(fd);
        if (r != 0 && errno != EINVAL) {   /* (a file system that can't: nothing to make last) */
            forth_error(f, "dir-sync: %s: %s", p, strerror(errno));
        }
    }
#else
    {
        struct stat st;   /* (nothing to sync: but a folder that isn't there is an error, as on POSIX) */

        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
            forth_error(f, "dir-sync: %s: no such folder", p);
        }
    }
#endif
}

/* ---- stopping when told to: SIGTERM (a service manager's), SIGINT (Ctrl-C) ---- */

static void shutdown_signal(int sig) {
    (void)sig;
    sShutdown = 1;
}

/* on-shutdown ( xt -- ) xt run when the process is told to stop (SIGTERM, SIGINT): from this Forth's loop (`serve`, a
 * node's), at its top - not in the signal. Then that loop ends: a node's at once (the main loop waits for the nodes
 * with words of their own to have run them). Once on-shutdown is said anywhere, Ctrl-C stops the process so. */
static void p_on_shutdown(Forth *f, Word *w) {
    Word *x = forth_xt_arg(f, forth_pop(f), "on-shutdown");
    struct ForthSystem *s = files_of(f);

    (void)w;
    if (s->nhooks == SHUTDOWN_HOOKS_MAX) {
        forth_error(f, "on-shutdown: %d words already, the most there may be", SHUTDOWN_HOOKS_MAX);
    }
    s->hooks[s->nhooks++] = x;
    forth_node_draining(f, 1);   /* (the main loop waits for it) */
    if (!sHandlers) {
        sHandlers = 1;
        signal(SIGINT, shutdown_signal);
        signal(SIGTERM, shutdown_signal);
    }
}
static void p_shutting_down(Forth *f, Word *w) { (void)w; forth_push(f, sShutdown ? -1 : 0); }   /* ( -- flag ) */
static void p_shutdown_grace(Forth *f, Word *w) {   /* shutdown-grace! ( ms -- ) the main loop's wait for the nodes */
    (void)w;
    sGraceMs = (long)forth_pop(f);
}

int forth_shutdown_signalled(void) { return sShutdown != 0; }
int forth_shutdown_hooked(Forth *f) { return f->system != NULL && f->system->nhooks > 0; }
long forth_shutdown_grace(void) { return sGraceMs; }
/* this Forth's on-shutdown words run, once, in the order they were said (one that fails: said, the rest still run) */
void forth_shutdown_run(Forth *f) {
    struct ForthSystem *s = f->system;
    int i;

    if (s == NULL || s->hooks_ran) {
        return;
    }
    s->hooks_ran = 1;
    for (i = 0; i < s->nhooks; i++) {
        forth_call(f, s->hooks[i]);
    }
    forth_node_draining(f, 0);
}

void forth_system_free(Forth *f) {
    int i;

    if (f->system == NULL) {
        return;
    }
    for (i = 0; i < FILES_MAX; i++) {
        if (f->system->fd[i] >= 0) {
            close_fd(f->system->fd[i]);
        }
    }
    free(f->system);
    f->system = NULL;
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
        {"file-size", p_file_size, "( path n -- n )"}, {"file-modified", p_file_modified, "( path n -- s )"},
        {"dir?", p_dir_q, "( path n -- flag )"}, {"delete-file", p_delete_file, "( path n -- )"},
        {"rename-file", p_rename_file, "( from n to n2 -- )"}, {"make-dir", p_make_dir, "( path n -- )"},
        {"dir-list", p_dir_list, "( path n -- l )"}, {"read-line", p_read_line, "( -- s len true | false )"},
        {"args", p_args, "( -- l )"}, {"getenv", p_getenv, "( name n -- s len true | false )"},
        {"exit-with", p_exit_with, "( n -- )"}, {"shell", p_shell, "( command n -- status )"},
        {"shell-output", p_shell_output, "( command n -- s len status )"},
        {"ms-now", p_ms_now, "( -- ms )"}, {"us-now", p_us_now, "( -- us )"}, {"time-now", p_time_now, "( -- s )"},
        {"sleep-ms", p_sleep_ms, "( ms -- )"},
        {"file-open-append", p_file_open_append, "( path n -- file )"}, {"file-write", p_file_write, "( a n file -- )"},
        {"file-sync", p_file_sync, "( file -- )"}, {"file-close", p_file_close, "( file -- )"},
        {"dir-sync", p_dir_sync, "( path n -- )"},
        {"on-shutdown", p_on_shutdown, "( xt -- )"}, {"shutting-down?", p_shutting_down, "( -- flag )"},
        {"shutdown-grace!", p_shutdown_grace, "( ms -- )"},
    };
    Vocab *was = forth_current(f), *v = forth_vocab(f, "system");

    forth_set_current(f, v);   /* scripts say USING: system ; */
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_set_current(f, was);
}

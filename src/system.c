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
#include <malloc.h>
#define PSAPI_VERSION 2   /* (GetProcessMemoryInfo from kernel32: no psapi.lib to link) */
#include <psapi.h>
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
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
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
#define LOCK_AT_LOW 0xFFFFFFFEu   /* (Windows) file-lock's byte: at 2^63 - 2, past any file's end */
#define LOCK_AT_HIGH 0x7FFFFFFFu
#define PART_BYTES_MAX (16 * 1024 * 1024)   /* read-file-part's count, at most: a piece, not the file */
#define MS_PER_S 1000
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

/* ---- the work a job does (jobs.c: on a worker thread, or here where the task can't wait) - C only, nothing of a
 * Forth's: what it needs copied in, what it makes copied out, why it couldn't written in j->why ---- */

static void job_failed(ForthJob *j, const char *what) {
    j->failed = 1;
    snprintf(j->why, sizeof(j->why), "%s: %s: %s", j->word, j->in != NULL ? j->in : "", what);
}
static void run_slurp(ForthJob *j) {   /* in: a path; out: its bytes */
    FILE *in = fopen(j->in, "rb");
    size_t cap = 0, got;

    if (in == NULL) {
        job_failed(j, strerror(errno));
        return;
    }
    do {
        if (cap - j->out_n < READ_CHUNK) {
            char *more = realloc(j->out, cap + READ_CHUNK);

            if (more == NULL) {
                job_failed(j, "out of memory");
                fclose(in);
                return;
            }
            j->out = more;
            cap += READ_CHUNK;
        }
        got = fread(j->out + j->out_n, 1, cap - j->out_n, in);
        j->out_n += got;
    } while (got > 0);
    if (ferror(in)) {
        job_failed(j, "a read failed");
    }
    fclose(in);
}
static void run_spill(ForthJob *j) {   /* in: a path; data: the bytes; arg: 1 to append (else the file made anew) */
    FILE *out = fopen(j->in, j->arg ? "ab" : "wb");

    if (out == NULL) {
        job_failed(j, strerror(errno));
        return;
    }
    if (fwrite(j->data, 1, j->data_n, out) != j->data_n) {
        job_failed(j, "a write failed");
        fclose(out);
        return;
    }
    if (fclose(out) != 0) {
        job_failed(j, strerror(errno));
    }
}

/* in: a path; arg: where in the file to start; arg2: how many bytes at most; out: them (fewer: the file ends) */
static void run_slurp_part(ForthJob *j) {
    FILE *in = fopen(j->in, "rb");
    size_t want = (size_t)j->arg2;

    if (in == NULL) {
        job_failed(j, strerror(errno));
        return;
    }
#ifdef _WIN32
    if (_fseeki64(in, j->arg, SEEK_SET) != 0) {
#else
    if (fseeko(in, (off_t)j->arg, SEEK_SET) != 0) {
#endif
        job_failed(j, strerror(errno));
        fclose(in);
        return;
    }
    if (want > 0 && (j->out = malloc(want)) == NULL) {
        job_failed(j, "out of memory");
        fclose(in);
        return;
    }
    while (j->out_n < want) {
        size_t got = fread(j->out + j->out_n, 1, want - j->out_n, in);

        if (got == 0) {
            break;
        }
        j->out_n += got;
    }
    if (ferror(in)) {
        job_failed(j, "a read failed");
    }
    fclose(in);
}

/* the job asked: on the pool where the task may wait (NULL: threaded, the primitive runs again as it goes on), else
 * done here and now */
static ForthJob *asked(Forth *f, ForthJob *j, const Word *w) {
    if (forth_job_may(f, w)) {
        return forth_job_wait(f, j);
    }
    j->run(j);
    return j;
}
/* a job done: if it failed, freed and the error raised */
static void job_ok(Forth *f, ForthJob *j) {
    char why[sizeof(j->why)];

    if (!j->failed) {
        return;
    }
    memcpy(why, j->why, sizeof(why));
    forth_job_free(j);
    forth_error(f, "%s", why);
}
/* a job of the word's - its path from the stack - found done as the primitive runs again, or asked now */
static ForthJob *path_job(Forth *f, void (*run)(ForthJob *), const char *word, const Word *w) {
    ForthJob *j = forth_job_taken(f, word);
    char path[PATH_BYTES];

    if (j != NULL) {
        return j;
    }
    path_arg(f, path, word);
    j = forth_job_new(f, run, word);
    j->in = forth_job_copy(f, j, path, strlen(path));
    j->in_n = strlen(path);
    return asked(f, j, w);
}

/* read-file ( path n -- s len ) a file's bytes, a new string */
static void p_read_file(Forth *f, Word *w) {
    ForthJob *j = path_job(f, run_slurp, "read-file", w);
    char *s;

    (void)w;
    if (j == NULL) {
        return;
    }
    job_ok(f, j);
    s = forth_new_string(f, j->out != NULL ? j->out : "", j->out_n);
    forth_push(f, (Cell)s);
    forth_push(f, (Cell)j->out_n);
    forth_job_free(j);
}

/* read-file-part ( path n from count -- s len ) so many of a file's bytes from that place (fewer: it ends there), a
 * new string - a file sent on a piece at a time is never all in memory */
static void p_read_file_part(Forth *f, Word *w) {
    ForthJob *j = forth_job_taken(f, "read-file-part");
    char *s;

    if (j == NULL) {
        Cell count = forth_pop(f), from = forth_pop(f);
        char path[PATH_BYTES];

        path_arg(f, path, "read-file-part");
        if (from < 0 || count < 0 || count > PART_BYTES_MAX) {
            forth_error(f, "read-file-part: from %lld, %lld bytes - from 0 on, %d bytes at most", (long long)from,
                        (long long)count, PART_BYTES_MAX);
        }
        j = forth_job_new(f, run_slurp_part, "read-file-part");
        j->in = forth_job_copy(f, j, path, strlen(path));
        j->in_n = strlen(path);
        j->arg = from;
        j->arg2 = count;
        if ((j = asked(f, j, w)) == NULL) {
            return;
        }
    }
    job_ok(f, j);
    s = forth_new_string(f, j->out != NULL ? j->out : "", j->out_n);
    forth_push(f, (Cell)s);
    forth_push(f, (Cell)j->out_n);
    forth_job_free(j);
}

/* read-bytes ( path n -- b ) a file's bytes, a new byte buffer */
static void p_read_bytes(Forth *f, Word *w) {
    ForthJob *j = path_job(f, run_slurp, "read-bytes", w);
    Cell b;

    (void)w;
    if (j == NULL) {
        return;
    }
    job_ok(f, j);
    b = forth_new_bytes(f, j->out != NULL ? j->out : "", j->out_n);
    forth_push(f, b);
    forth_job_free(j);
}

/* ( bytes path pn -- ) the bytes written (append: at the end) - the bytes a string's (a n) or a byte buffer's (b) */
static void write_job(Forth *f, const Word *w, const char *word, int append, int bytes) {
    ForthJob *j = forth_job_taken(f, word);

    if (j == NULL) {
        char path[PATH_BYTES];
        const char *data;
        size_t len;

        path_arg(f, path, word);
        if (bytes) {
            data = (const char *)forth_bytes_of(f, forth_pop(f), &len, word);
        } else {
            Cell n = forth_pop(f), a = forth_pop(f);

            data = forth_string_arg(f, a, n, word, &len);
        }
        j = forth_job_new(f, run_spill, word);
        j->in = forth_job_copy(f, j, path, strlen(path));
        j->data = forth_job_copy(f, j, data, len);
        j->data_n = len;
        j->arg = append;
        if ((j = asked(f, j, w)) == NULL) {
            return;
        }
    }
    job_ok(f, j);
    forth_job_free(j);
}
static void p_write_file(Forth *f, Word *w) { write_job(f, w, "write-file", 0, 0); }   /* ( a n path pn -- ) */
static void p_append_file(Forth *f, Word *w) { write_job(f, w, "append-file", 1, 0); }
static void p_write_bytes(Forth *f, Word *w) { write_job(f, w, "write-bytes", 0, 1); }   /* ( b path pn -- ) */

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
/* file-lock ( file -- flag ) the open file taken as this process's alone, if no other has it: true - false if one
 * has (nothing waited for). A lock the system keeps: gone when the file is closed, or the process ends however it
 * ends - none is left behind by a crash. It keeps no one from reading or writing the file: it is for programs that
 * ask before they do (a database's folder, one process's at a time). */
static void p_file_lock(Forth *f, Word *w) {
    Cell handle = forth_pop(f);
    struct ForthSystem *s = files_of(f);
    int fd, locked;

    (void)w;
    if (handle < 1 || handle > FILES_MAX || (fd = s->fd[handle - 1]) < 0) {
        forth_error(f, "file-lock: %ld is no open file", (long)handle);
    }
#ifdef _WIN32
    {
        OVERLAPPED at;

        memset(&at, 0, sizeof(at));
        at.Offset = LOCK_AT_LOW;   /* (a byte far past any file's end: what is read and written is never in its way) */
        at.OffsetHigh = LOCK_AT_HIGH;
        locked = LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                            &at) != 0;
    }
#else
    locked = flock(fd, LOCK_EX | LOCK_NB) == 0;
#endif
    forth_push(f, locked ? -1 : 0);
}

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
static void run_write_fd(ForthJob *j) {   /* arg: the descriptor; data: the bytes */
    size_t done = 0;

    while (done < j->data_n) {
        size_t chunk = j->data_n - done > READ_CHUNK ? READ_CHUNK : j->data_n - done;
        long put = (long)write_fd((int)j->arg, j->data + done, (unsigned)chunk);

        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            j->failed = 1;
            snprintf(j->why, sizeof(j->why), "file-write: %s (%zu of %zu bytes written)", strerror(errno), done,
                     j->data_n);
            return;
        }
        done += (size_t)put;
    }
}
static void p_file_write(Forth *f, Word *w) {
    ForthJob *j = forth_job_taken(f, "file-write");

    (void)w;
    if (j == NULL) {
        int fd = file_arg(f, "file-write");
        Cell n = forth_pop(f), a = forth_pop(f);
        size_t len;
        const char *data = forth_string_arg(f, a, n, "file-write", &len);

        j = forth_job_new(f, run_write_fd, "file-write");
        j->data = forth_job_copy(f, j, data, len);
        j->data_n = len;
        j->arg = fd;
        if ((j = asked(f, j, w)) == NULL) {
            return;
        }
    }
    job_ok(f, j);
    forth_job_free(j);
}

/* file-sync ( file -- ) what was written to it on the disk itself before this returns: fsync (fdatasync where there is
 * one; F_FULLFSYNC on macOS, whose fsync leaves it in the drive's cache; _commit on Windows) - on a worker, where the
 * task may wait: the node's other actors go on meanwhile */
static void run_sync(ForthJob *j) {   /* arg: the descriptor */
    int fd = (int)j->arg, r;

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
        j->failed = 1;
        snprintf(j->why, sizeof(j->why), "file-sync: %s", strerror(errno));
    }
}
static void p_file_sync(Forth *f, Word *w) {
    ForthJob *j = forth_job_taken(f, "file-sync");

    (void)w;
    if (j == NULL) {
        int fd = file_arg(f, "file-sync");

        j = forth_job_new(f, run_sync, "file-sync");
        j->arg = fd;
        if ((j = asked(f, j, w)) == NULL) {
            return;
        }
    }
    job_ok(f, j);
    forth_job_free(j);
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
static void run_dir_sync(ForthJob *j) {   /* in: the folder */
#ifndef _WIN32
    int fd = open(j->in, O_RDONLY | O_CLOEXEC), r;

    if (fd < 0) {
        job_failed(j, strerror(errno));
        return;
    }
    r = fsync(fd);
    close(fd);
    if (r != 0 && errno != EINVAL) {   /* (a file system that can't: nothing to make last) */
        job_failed(j, strerror(errno));
    }
#else
    struct stat st;   /* (nothing to sync: but a folder that isn't there is an error, as on POSIX) */

    if (stat(j->in, &st) != 0 || !S_ISDIR(st.st_mode)) {
        job_failed(j, "no such folder");
    }
#endif
}
static void p_dir_sync(Forth *f, Word *w) {
    ForthJob *j = path_job(f, run_dir_sync, "dir-sync", w);

    (void)w;
    if (j == NULL) {
        return;
    }
    job_ok(f, j);
    forth_job_free(j);
}

/* ---- stopping when told to: SIGTERM (a service manager's), SIGINT (Ctrl-C) ---- */

/* the first: noted, for the loops to see; a second (Ctrl-C again, a manager's SIGTERM after it waited) - or one while
 * no loop is looking (a long load, the prompt) - ends the process as it would have */
static void shutdown_signal(int sig) {
    if (sShutdown) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    sShutdown = 1;
#ifdef _WIN32
    forth_nodes_shutdown_seen();   /* (Windows runs this on a thread of its own, interrupting no wait: the loops are
                                    * woken from here. Elsewhere the thread interrupted sees it, and wakes the rest) */
#endif
}

/* on-shutdown ( xt -- ) xt run when the process is told to stop (SIGTERM, SIGINT): from this Forth's loop (`serve`, a
 * node's), at its top - not in the signal. Then that loop ends: a node's at once (the main loop waits for the nodes
 * with words of their own to have run them). Once on-shutdown is said anywhere, Ctrl-C stops the process so; a second
 * signal ends it at once, as before (for a program not yet in `serve`, or one that doesn't drain). */
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
/* this process asked to stop from inside it (a node's supervisor says so, or has gone) - as the signal asks: the
 * loops see it and run their on-shutdown words; a program with none ends here, as the signal would have ended it.
 * From any thread. */
void forth_shutdown_ask(void) {
    if (!sHandlers) {
        fflush(NULL);
        _exit(0);
    }
    if (!sShutdown) {
        sShutdown = 1;
        forth_nodes_shutdown_seen();
    }
}
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
    if (n > 1) {   /* (an empty folder has no names: nothing to hand qsort, which wants an address) */
        qsort(names, n, sizeof(char *), by_name);
    }
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

#ifdef _WIN32
/* The command as cmd.exe is to have it. Given more than two quotes, cmd takes the first and the last away - so
 * `"a b.exe" "c d"` was `a b.exe" "c d`, no program's name. In a pair of its own, those are what it takes. (NULL: no
 * memory.) */
static char *shell_command(const char *command) {
    size_t n = strlen(command);
    char *quoted = malloc(n + 3);

    if (quoted != NULL) {
        quoted[0] = '"';
        memcpy(quoted + 1, command, n);
        quoted[n + 1] = '"';
        quoted[n + 2] = 0;
    }
    return quoted;
}
#else
static char *shell_command(const char *command) { return strdup(command); }
#endif

static void run_shell(ForthJob *j) {   /* in: the command; result: its status */
    char *command = shell_command(j->in);

    if (command == NULL) {
        job_failed(j, "out of memory");
        return;
    }
    fflush(stdout);
    j->result = exit_status(system(command));
    free(command);
}
static void p_shell(Forth *f, Word *w) {   /* shell ( command n -- status ) run by the system's shell */
    ForthJob *j = path_job(f, run_shell, "shell", w);

    (void)w;
    if (j == NULL) {
        return;
    }
    forth_push(f, (Cell)j->result);
    forth_job_free(j);
}

/* shell-output ( command n -- s len status ) run by the shell, its output a new string */
static void run_shell_output(ForthJob *j) {   /* in: the command; out: what it wrote; result: its status */
    size_t cap = 0, got;
    char *command = shell_command(j->in);
    FILE *p;

    if (command == NULL) {
        job_failed(j, "out of memory");
        return;
    }
    fflush(stdout);
    p = open_pipe(command, "r");
    free(command);
    if (p == NULL) {
        job_failed(j, strerror(errno));
        return;
    }
    do {
        if (cap - j->out_n < READ_CHUNK) {
            char *more = realloc(j->out, cap + READ_CHUNK);

            if (more == NULL) {
                break;
            }
            j->out = more;
            cap += READ_CHUNK;
        }
        got = fread(j->out + j->out_n, 1, cap - j->out_n, p);
        j->out_n += got;
    } while (got > 0);
    j->result = exit_status(close_pipe(p));
}
static void p_shell_output(Forth *f, Word *w) {
    ForthJob *j = path_job(f, run_shell_output, "shell-output", w);

    (void)w;
    if (j == NULL) {
        return;
    }
    job_ok(f, j);
    push_text(f, j->out != NULL ? j->out : "", j->out_n);
    forth_push(f, (Cell)j->result);
    forth_job_free(j);
}

static long long monotonic_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * (NS_PER_MS * MS_PER_S) + ts.tv_nsec;
}
/* ---- memory: what was freed, handed back ----
 * free() gives memory to the allocator, which keeps most of it for the next malloc: a process that was large once
 * stays large. A trim hands the whole pages among it back to the system. */
void forth_memory_trim(Forth *f) {
#if defined(_WIN32)
    HeapCompact(GetProcessHeap(), 0);   /* (free blocks joined, the pages wholly free decommitted) */
    _heapmin();                         /* (the C library's heap, where it has one of its own) */
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
    f->trim_freed = forth_objects_freed(f);
    f->trims++;
}

/* the memory the process holds, resident (Task Manager's, top's RES); 0 where the system doesn't say */
size_t forth_process_bytes(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;

    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? (size_t)pmc.WorkingSetSize : 0;
#elif defined(__linux__)
    unsigned long total = 0, resident = 0;
    FILE *fp = fopen("/proc/self/statm", "r");

    if (fp == NULL) {
        return 0;
    }
    if (fscanf(fp, "%lu %lu", &total, &resident) != 2) {
        resident = 0;
    }
    fclose(fp);
    return (size_t)resident * (size_t)sysconf(_SC_PAGESIZE);
#else
    return 0;
#endif
}
static void p_process_bytes(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)forth_process_bytes()); }   /* ( -- n ) */
static void p_trim_memory(Forth *f, Word *w) { (void)w; forth_memory_trim(f); }                           /* ( -- ) */
static void p_memory_trims(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)f->trims); }                 /* ( -- n ) */

static void p_ms_now(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)(monotonic_ns() / NS_PER_MS)); }   /* ( -- ms ) */
static void p_us_now(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)(monotonic_ns() / NS_PER_US)); }   /* ( -- us ) */
static void p_time_now(Forth *f, Word *w) { (void)w; forth_push(f, (Cell)time(NULL)); }   /* ( -- s ) since 1970 */
/* sleep-ms ( ms -- ) so long gone by before it goes on: in a handler (sliced), it waits - the node's other actors
 * go on, and no thread is held; elsewhere the thread sleeps, its Forth with it */
static void p_sleep_ms(Forth *f, Word *w) {
    Cell ms;
    struct timespec ts;

    if (forth_task_slept(f)) {   /* (threaded: run again, its time up) */
        return;
    }
    ms = forth_pop(f);
    if (ms <= 0) {
        return;
    }
    if (forth_task_may_wait(f, w) && forth_task_sleep(f, (long)ms)) {
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
        {"read-file-part", p_read_file_part, "( path n from count -- s len )"},
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
        {"process-bytes", p_process_bytes, "( -- n )"}, {"trim-memory", p_trim_memory, "( -- )"},
        {"memory-trims", p_memory_trims, "( -- n )"},
        {"file-open-append", p_file_open_append, "( path n -- file )"}, {"file-write", p_file_write, "( a n file -- )"},
        {"file-sync", p_file_sync, "( file -- )"}, {"file-close", p_file_close, "( file -- )"},
        {"file-lock", p_file_lock, "( file -- flag )"},
        {"dir-sync", p_dir_sync, "( path n -- )"},
        {"on-shutdown", p_on_shutdown, "( xt -- )"}, {"shutting-down?", p_shutting_down, "( -- flag )"},
        {"shutdown-grace!", p_shutdown_grace, "( ms -- )"},
    };
    Vocab *was = forth_current(f), *v = forth_vocab(f, "system");

    forth_set_current(f, v);   /* scripts say USING: system ; */
    v->state = VOCAB_LOADED;
    forth_define_prims(f, prims, sizeof(prims) / sizeof(prims[0]));
    forth_pool_words(f);   /* (supervisor.c's: as-nodes, node-index ...) */
    forth_set_current(f, was);
}

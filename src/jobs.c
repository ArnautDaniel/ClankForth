/* Jobs: work that would hold a node's thread - a file read or written, an fsync, another program, a slow hash - done
 * on a worker thread of a pool the process shares, while the actor that asked waits (busy, as an await) and its node
 * runs its other actors. A worker runs C only: what a job works on is copied in, what it makes copied out (malloc'd),
 * and the primitive that asked makes Forth's objects of it on its own thread once the job is done. The node is woken
 * (forth_node_poke) as one of its jobs ends.
 *
 * A primitive asks (forth_job_wait) only where its task may wait: an actor's handler, at its own level
 * (forth_job_may). With a coroutine (machine code) the task stops in the middle of the primitive and goes on there;
 * threaded, the primitive is run again as the task goes on, and finds its job done (forth_job_taken). Elsewhere - the
 * prompt, a task of its own, inside atomically - the primitive does the work itself, as it always did.
 * Docs: docs/actors.md, "Work that blocks". */
#include "forth_internal.h"
#include "actors.h"

#include <stdlib.h>
#include <string.h>

#if defined(CLANKFORTH_NODES)

#include <pthread.h>

#define WORKERS_MAX 8   /* a pool's threads, at most: as many as there are cores up to this */

enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE };

static pthread_mutex_t sPool = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sWork = PTHREAD_COND_INITIALIZER;
static ForthJob *sFirst, *sLast;   /* the jobs waiting for a worker, first in first out */
static int sWorkers;               /* threads started (they last as long as the process) */

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        ForthJob *j;
        int node, abandoned;

        pthread_mutex_lock(&sPool);
        while (sFirst == NULL) {
            pthread_cond_wait(&sWork, &sPool);
        }
        j = sFirst;
        sFirst = j->next;
        if (sFirst == NULL) {
            sLast = NULL;
        }
        j->state = JOB_RUNNING;
        pthread_mutex_unlock(&sPool);

        j->run(j);

        pthread_mutex_lock(&sPool);
        j->state = JOB_DONE;
        node = j->node;
        abandoned = j->abandoned;
        pthread_mutex_unlock(&sPool);
        if (abandoned) {   /* (its task gone while it worked: no one to give it to) */
            forth_job_free(j);
        } else {
            forth_node_poke(node);
        }
    }
    return NULL;
}

/* the pool's threads: one more while there are fewer than the cores (up to WORKERS_MAX) - 0 if none could start */
static int workers_started(Forth *f) {
    int want = forth_cores(f), started;

    if (want > WORKERS_MAX) {
        want = WORKERS_MAX;
    }
    if (want < 2) {
        want = 2;   /* (one core still waits on a disk: a second worker lets one fsync not hold up a read) */
    }
    pthread_mutex_lock(&sPool);
    while (sWorkers < want) {
        pthread_t th;
        pthread_attr_t attr;

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, worker, NULL) != 0) {
            pthread_attr_destroy(&attr);
            break;
        }
        pthread_attr_destroy(&attr);
        sWorkers++;
    }
    started = sWorkers;
    pthread_mutex_unlock(&sPool);
    return started;
}

int forth_job_may(Forth *f) {
    return forth_actor_self(f) >= 0 && f->t != f->main && f->depth == f->task_depth &&
           f->defining == f->turn_defining && forth_task_sys_ok(f);
}

ForthJob *forth_job_wait(Forth *f, ForthJob *j) {
    ForthJob **slot = forth_task_job(f, f->t);

    j->node = forth_node_of(f);
    if (workers_started(f) == 0) {   /* (no thread to run it: done here, now) */
        j->run(j);
        j->state = JOB_DONE;
        return j;
    }
    *slot = j;
    pthread_mutex_lock(&sPool);
    j->state = JOB_QUEUED;
    j->next = NULL;
    if (sLast != NULL) {
        sLast->next = j;
    } else {
        sFirst = j;
    }
    sLast = j;
    pthread_cond_signal(&sWork);
    pthread_mutex_unlock(&sPool);
    f->yielded = 1;
    if (forth_native_in_coroutine(f)) {   /* (stopped here; its next turn - the job done - goes on from here) */
        forth_native_suspend(f);
        *forth_task_job(f, f->t) = NULL;
        return j;
    }
    f->t->ip--;   /* (threaded: this primitive again as the task goes on - it finds the job done: forth_job_taken) */
    return NULL;
}

ForthJob *forth_job_taken(Forth *f, const char *word) {
    ForthJob **slot = forth_task_job_if(f->t), *j;

    if (slot == NULL || (j = *slot) == NULL || strcmp(j->word, word) != 0 || forth_job_pending(j)) {
        return NULL;
    }
    *slot = NULL;
    return j;
}

int forth_job_pending(const ForthJob *j) {
    int pending;

    pthread_mutex_lock(&sPool);
    pending = j->state != JOB_DONE;
    pthread_mutex_unlock(&sPool);
    return pending;
}

void forth_job_let_go(ForthJob *j) {
    int done;

    if (j == NULL) {
        return;
    }
    pthread_mutex_lock(&sPool);
    done = j->state == JOB_DONE;
    j->abandoned = 1;   /* (still queued or working: the worker frees it as it ends) */
    pthread_mutex_unlock(&sPool);
    if (done) {
        forth_job_free(j);
    }
}

#else   /* (no threads: every job done where it is asked) */

int forth_job_may(Forth *f) { (void)f; return 0; }
ForthJob *forth_job_wait(Forth *f, ForthJob *j) {
    (void)f;
    j->run(j);
    return j;
}
ForthJob *forth_job_taken(Forth *f, const char *word) { (void)f; (void)word; return NULL; }
int forth_job_pending(const ForthJob *j) { (void)j; return 0; }
void forth_job_let_go(ForthJob *j) { forth_job_free(j); }

#endif

ForthJob *forth_job_new(Forth *f, void (*run)(ForthJob *), const char *word) {
    ForthJob *j = calloc(1, sizeof(ForthJob));

    if (j == NULL) {
        forth_error(f, "%s: out of memory", word);
    }
    j->run = run;
    j->word = word;
    return j;
}

void forth_job_free(ForthJob *j) {
    if (j != NULL) {
        free(j->in);
        free(j->data);
        free(j->out);
        free(j);
    }
}

/* a copy of the bytes for a job (malloc'd, a 0 after them: a path, a command, as C wants) */
char *forth_job_copy(Forth *f, ForthJob *j, const void *p, size_t n) {
    char *c = malloc(n + 1);

    if (c == NULL) {
        forth_job_free(j);
        forth_error(f, "%s: out of memory", j->word);
    }
    memcpy(c, p, n);
    c[n] = 0;
    return c;
}

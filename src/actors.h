/* The actor kernel: actors with private state, a behaviour (a table of message handlers written in Forth) and
 * messages queued between them. C keeps the actors, their mailboxes and the schedule; Forth declares messages,
 * behaviours and state types and writes the handlers. Each actor's handlers run in a task of its own, so a handler
 * may wait (`wait`, `yield`, `await`): its actor is busy until it ends, its messages kept for it meanwhile.
 *
 * A frame: the busy actors whose waits are over go on; `tick` to its subscribers, deliver; `frame-end` to its
 * subscribers, deliver. Deliver runs in rounds - what a round sends waits for the next - until the queue is empty
 * (or a limit: the rest waits for the next frame). Everything is first in, first out, and broadcasts go in spawn
 * order, so a frame always runs the same way.
 *
 * A host gives each actor a body of its own (forth_actors' body_bytes: where it is, as the host has it) and its
 * own words for it. Docs: docs/actors.md. */
#ifndef CLANKFORTH_ACTORS_H
#define CLANKFORTH_ACTORS_H

#include "forth.h"

#define FORTH_ACTORS_MAX 256   /* actors alive at once */
#define FORTH_MSG_ARGS 16      /* cells a message carries */
#define FORTH_KINDS_MAX 512    /* message kinds */
#define FORTH_ACTOR_NAME_MAX 31

/* the kernel's own message kinds */
enum {
    FORTH_KIND_TICK,          /* ( -- ) each frame, to its subscribers */
    FORTH_KIND_FRAME_END,     /* ( -- ) after the frame's deliveries */
    FORTH_KIND_SPAWNED,       /* ( -- ) an actor's first message */
    FORTH_KIND_KILLED,        /* ( -- ) its last words, run as it is killed */
    FORTH_KIND_HELD_TICK,     /* ( -- ) each frame the world is held (a menu up) */
    FORTH_KIND_ACTOR_FAILED,  /* ( record -- ) to the errors actor (errors-to): a handler's or a task's error */
    FORTH_KIND_START_OVER,    /* ( -- ) the errors actor's ask, to one that failed */
    FORTH_KIND_DIED,          /* ( id -- ) to those monitoring it: it is gone */
    FORTH_KERNEL_KINDS
};

/* the words (vocabulary `actors`); each actor with body_bytes of the host's, zeroed as it is spawned */
void forth_actors(Forth *f, size_t body_bytes);
void forth_actors_free(Forth *f);

/* actor `id`'s body (NULL: no such actor); the actor being run (-1: none); its name ("" if none); a message kind by
 * name (-1 if there is none) */
void *forth_actor_body(Forth *f, int id);
int forth_actor_self(Forth *f);
const char *forth_actor_name(Forth *f, int id);
int forth_actor_kind(Forth *f, const char *name);

/* one frame of the actors (the host calls it each tick, after forth_run_tasks) */
void forth_actors_frame(Forth *f);
/* a frame while the world is held (a menu up): `held-tick` to its subscribers only, then deliver - messages still go
 * to everyone, busy actors still go on */
void forth_actors_held_frame(Forth *f);
/* forget every actor and queued message (behaviours, kinds and state types stay) */
void forth_actors_reset(Forth *f);
/* from C: queue a message (from -1: the engine) / to every subscriber */
void forth_actor_send(Forth *f, int from, int to, int kind, const Cell *args, int n);
void forth_actor_broadcast(Forth *f, int from, int kind, const Cell *args, int n);

#endif

# The actor kernel

Built into ClankForth (`src/actors.c`, `src/actors.h`): actors with private state, a behaviour - a table of message
handlers written in Forth - and messages queued between them. C keeps the actors, their mailboxes and the schedule,
so delivery stays fast and traceable; Forth declares the messages, behaviours and state types and writes the
handlers. A host turns it on with `forth_actors` and gives each actor a body of its own (where it is in the host's
world: its own words read and move it). Scripts say `USING: actors ;`.

Every actor has an **id**: a number, given by `spawn`, that messages are addressed to. An id is its actor's alone:
kept after the actor died it names nobody, never a newcomer (`alive?` false, a `send` to it an error). It is made of
the actor's **slot** - its place among the actors, 0 .. 255, the next free one after the last given - and, over
that, how many have had the slot before. The first to have a slot has it for its id (0 .. 255); the next has 256
more, and so on. A table with a cell for each actor is indexed by `actor-slot ( id -- n )`, not by the id.

## Messages

```forth
message heard ( loud room tri door source -- )   \ a kind, and its arity: the stack comment is the declaration
loud room tri door src  listener send heard      \ queued for `listener`
broadcast tick                                    \ to everyone subscribed to `tick`
30 self send-after wake                           \ queued for itself, 30 frames from now
```

A message has a kind and up to 16 cell arguments. A float travels as a cell (`f>cell` / `cell>f`; a point as
`xyz>cells` / `cells>xyz`, with a heading `xyzw>cells` / `cells>xyzw`). `send`, `broadcast`, `subscribe`, `await`
parse the message's name: a misspelt one is an error as the script is compiled, and their stack effect is the
message's - `5 swap send ping` is checked like any word (docs/forth.md, "Stack effects").

`args frames id send-after kind` queues the message when so many frames have gone by (the system's, as `wait`
counts them: standing still while the tasks are paused; none or fewer is the next frame's). It is how an actor does
something later without a handler that waits - which would be busy meanwhile, its messages kept and its ticks
missed: it sends itself a message, and goes on. One asked for can't be taken back: the handler checks whether it
still applies. `send-afters ( -- n )` counts those waiting.

**A message owns what it carries.** A list, tuple, string or made quotation (`curry`, `compose`) among its arguments
is taken out of the scratch regions its sender made it in, with what it holds (a list's items ...), as it is sent: it
is there when the message is read, whenever that is. It is the receiver's from then - to keep, or `dispose` of when
done. It is not copied: a sender that goes on using it shares it.

A message sent to `nobody` (`sender` when the engine sent it, a name `actor-named` didn't find) is a dead letter:
dropped and counted (`.actors`), no one's - `broadcast` is the word for everyone subscribed. One sent to an id whose
actor is gone is an error where it is sent.

## Behaviours

```forth
behaviour listening
  on heard ( loud room tri door src -- )  ... ;
  on tick ( -- )  ... ;
end-behaviour

behaviour stunned  extends listening   \ unhandled messages fall through to `listening`
  on tick ( -- )  ... ;
end-behaviour
```

A handler runs with its message's arguments on the stack and must consume them. Its stack effect is its message's:
a comment that says otherwise is a warning as it is compiled (`its message carries 1`), and so is code that doesn't
take them (`its code is ( 1 -- 2 )`). If it leaves something anyway, or fails, the kernel names the actor and the
handler; its stacks are its own, so nobody else's are spoiled. The call chain names handlers `behaviour:on kind`. A
message the behaviour (and its parents) doesn't handle is dropped, counted, and reported once per behaviour and
kind (`.unhandled`). The kernel's own messages are dropped quietly. `become ( behaviour -- )` switches the current
actor to another behaviour from the next message on.

## A handler that waits

Each actor's handlers run in a task of its own, so a handler may wait - `n wait`, `yield`, or for a message:

```forth
behaviour door-keeper
  on knocked ( who -- )
      {: who :}  who send who-is-it   await answered ( name )  ...   \ goes on when `answered` comes
      30 wait  ... ;
end-behaviour
```

- While a handler waits its actor is **busy**: the messages that come for it are kept (its mailbox, `actor-mail ( id --
  n )`) and handled in order once it is done - one handler at a time, as ever. `actor-busy? ( id -- flag )`.
- The frame's own messages (`tick`, `frame-end`, `held-tick`) aren't kept: a busy actor misses them.
- `await kind ( -- args )`: the handler waits for a message of that kind (others kept meanwhile) and goes on with
  its arguments; `sender` is its sender then. `frames await-within kind ( -- args true | false )` gives up after so
  many frames. With no room on its stack for what it awaited, the handler fails there (as by any error of its own).
- A wait counts the system's frames (`forth_run_tasks`), and stands still while the tasks are paused
  (`tasks-paused!`). Each frame the busy actors whose waits are over go on first, in slot order.
- Killed while busy, an actor's last words run and the waiting handler is let go. Last words can't wait.
- A long sequence that is no actor's (a cutscene the engine runs) can still be a task of its own (`start-task`;
  `spawn` and `kill` are the actors' alone).

## State

```forth
state: listener-state
  cell  field threshold      \ quieter than this, it doesn't hear
  cell  field heard-loud
end-state
```

A field word gives the field's address in the **current** actor's state (`( -- addr )`): the one whose message is
being handled (`self`). Using a field with no current actor, or with an actor whose state is of another type, is an
error. A state is at most 64 KiB: a field past that is an error where it is declared.

## Actors

```forth
listening listener-state  s" Debilitas" spawn  ( -- id )   \ its first message: `spawned`
id subscribe tick          \ (or inside a handler: self subscribe tick)
id kill                    \ its `killed` handler (its last words) first; killing itself, it goes after the handler
self  ( -- id )            \ the actor being run
sender  ( -- id )          \ who sent the message being handled (-1: the engine)
id monitor                 \ this actor told `died ( id reason -- )` when that one is gone (unmonitor)
id link                    \ the two killed together: either gone, the other is killed (unlink)
```

`died`'s reason is 0 if the actor was killed, or the record of the failure it died of (below: `failure-error` ...
read it); one linked to it dies of the same.

`alive? ( id -- flag )`, `actor-slot ( id -- n )` (-1 for `nobody`), `actor-name ( id -- addr len )`, `actor-named ( addr len -- id | -1 )`, `behaviour-of ( id
-- behaviour )`, `nobody` (-1), `actors-max` (256).

## Failures

A handler's error ends that handler, and is recorded. What becomes of the actor is its supervisor's to say:

- `id errors-to` names an **errors actor**: every handler's failure and every task's is told to it as `actor-failed
  ( record -- )` (not its own: those are only said). The actor that failed lives on; the errors actor asks it to put
  itself right (`start-over`), or kills it.
- With **no errors actor**, nobody would put it right: the actor **dies of it** - its last words (`killed`), those
  linked to it killed, those monitoring it told `died` with the failure's record for the reason.

 The record by its number: `failure-actor ( r -- id
)` (nobody for a task's), `failure-who`, `failure-what` (the message), `failure-error`, `failure-chain` (the words
running) ( r -- addr len ), `failure-frame ( r -- n )`; the latest 32 are kept. `start-over ( -- )` is a kind for the
errors actor to ask one that failed to put itself right.

## The schedule and tools

- `actors-frame ( -- )`: one frame - the busy actors whose waits are over go on, `tick` to its subscribers, deliver,
  `frame-end` to its subscribers, deliver. A host calls it each tick (after `forth_run_tasks`); tests call it.
- A held frame (the host's world held: a menu up): `held-tick` to its subscribers, then deliver. Messages still reach
  every actor.
- `deliver ( -- )`: deliver what is queued now, in rounds - what a round sends waits for the next - until the queue
  is empty (or 64 rounds: the rest waits for the next frame). First in, first out; broadcasts in spawn order.
- `.actors` ends with the dead letters and the messages still to be sent later.
- `trace-on` / `trace-off`: every message printed as it is delivered (`frame  from -> to  kind args`).
- `.actors`: every actor with its behaviour, its state type, its count of queued messages - and, busy, the handler
  waiting, what it awaits, and its mail.
- `.state ( id -- )`: every field of an actor's state, by name. `.recent ( n -- )`: the last n messages delivered
  (256 kept), oldest first - not the frames' own, nor one the same as its sender's last of that kind.
- `enter ( id -- )` / `leave-actor`: the prompt inside an actor - its fields, `self`, `become` and sends are that
  actor's. `queued ( -- n )`, `actors-frames ( -- n )`, `actors-reset`.

## C (`actors.h`)

```c
void forth_actors(Forth *f, size_t body_bytes);   /* the words; each actor with body_bytes of the host's */
void *forth_actor_body(Forth *f, int id);         /* NULL: no such actor */
int forth_actor_self(Forth *f);                   /* the actor being run, -1 none */
const char *forth_actor_name(Forth *f, int id);
int forth_actor_kind(Forth *f, const char *name); /* a message kind by name, -1 none */
void forth_actors_frame(Forth *f);
void forth_actors_held_frame(Forth *f);
void forth_actors_reset(Forth *f);
void forth_actor_send(Forth *f, int from, int to, int kind, const Cell *args, int n);
void forth_actor_broadcast(Forth *f, int from, int kind, const Cell *args, int n);
```

`int forth_actor_slot(int id)` is an id's slot (0 .. `FORTH_ACTORS_MAX - 1`; -1 for nobody's): what a host's table
with an entry for each actor is indexed by. An id itself is past `FORTH_ACTORS_MAX` once its slot has been taken
again; `forth_actor_body(f, id)` is NULL for an id whose actor is gone.

`forth_actor_send` and `forth_actor_broadcast` send nothing (and say so) given a kind there isn't, or more than
`FORTH_MSG_ARGS` arguments.

The kernel's kinds come first (`FORTH_KIND_TICK` ...): `tick`, `frame-end`, `spawned`, `killed`, `held-tick`,
`actor-failed`, `start-over`, `died`. Tests: `tests/test_actors.fs`.

## Rules

- Actors may read facts (the host's world and data) directly, but never another actor's state.
- Replies are messages: to ask an actor something, send it a message - and `await` its answer, or handle it.

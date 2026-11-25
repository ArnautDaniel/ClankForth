# The actor kernel

Built into ClankForth (`src/actors.c`, `src/actors.h`): actors with private state, a behaviour - a table of message
handlers written in Forth - and messages queued between them. C keeps the actors, their mailboxes and the schedule,
so delivery stays fast and traceable; Forth declares the messages, behaviours and state types and writes the
handlers. A host turns it on with `forth_actors` and gives each actor a body of its own (where it is in the host's
world: its own words read and move it). Scripts say `USING: actors ;`.

Every actor has an **id**: a number, given by `spawn`, that messages are addressed to. An id is its actor's alone:
kept after the actor died it names nobody, not a newcomer (`alive?` false, a `send` to it an error) - until its slot
has been taken 2048 times more, when the ids go round. It is made of
the actor's **slot** - its place among the actors - and, over that, how many have had the slot before. The first to
have a slot has it for its id (0, 1, 2 ...); the next has 2^20 more, and so on. A table with a cell for each actor is
indexed by `actor-slot ( id -- n )`, not by the id.

The actors' table grows as they are spawned: as many may be alive at once as the host allows - `actors-max ( -- n )`,
256 unless the host says more (`forth_set_actors_max`; the `clankforth` tool allows every slot, 2^20). A slot let go
is taken again once 64 others are waiting (or no new one may be made), the longest let go first - so a stale id names
nobody for that much longer. (At the limit, with few let go, a slot is taken again at once: ids go round sooner.)
`actors-reset` starts the ids from the first again - but on a node (once it has a node number), whose actors' addresses
other nodes may hold, it doesn't: the slots are kept and their ids go on. `actors-alive ( -- n )` counts them.

## Messages

```forth
message heard ( loud room tri door source -- )   \ a kind, and its arity: the stack comment is the declaration
loud room tri door src  listener send heard      \ queued for `listener`
broadcast tick                                    \ to everyone subscribed to `tick`
```

A message has a kind and up to 16 cell arguments. A float travels as a cell (`f>cell` / `cell>f`; a point as
`xyz>cells` / `cells>xyz`, with a heading `xyzw>cells` / `cells>xyzw`). `send`, `broadcast`, `subscribe`, `await`
parse the message's name: a misspelt one is an error as the script is compiled, and their stack effect is the
message's - `5 swap send ping` is checked like any word (docs/forth.md, "Stack effects").

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
kind (`.unhandled`). The kernel's own messages are dropped quietly, and so are those of a kind marked
`quiet-message name` - an answer that may come after its asker stopped waiting for it. `become ( behaviour -- )` switches the current
actor to another behaviour from the next message on.

A behaviour is its vocabulary's: defined again there (a script loaded again, `reload`) it starts afresh, actors
keeping it - a handler running or waiting finishes as it began, the next message runs the new one; one of the same
name in another vocabulary is another behaviour (a host's plug-in may name its own `showing` without touching the
host's). A state type is the same.

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
  its arguments - one that came earlier, while the handler ran, and was kept, is taken at once; `sender` is its sender then. `ms await-within-ms kind ( -- args true | false )` gives up after so many milliseconds (the clock's: a server's timeout), `frames await-within kind` after so
  many frames. With no room on its stack for what it awaited, the handler fails there (as by any error of its own).
  Inside a `catch` too: `[: await reply  ... ;] catch` - an error after the answer comes is the `catch`'s.
- A wait counts the system's frames (`forth_run_tasks`), and stands still while the tasks are paused
  (`tasks-paused!`). Each frame the busy actors whose waits are over go on first, in slot order.
- Killed while busy, an actor's last words run and the waiting handler is let go. Last words can't wait.
- A handler may wait inside `with-scratch` (its objects kept till it goes on and the region ends; let go, killed, the
  region ends with it) - not inside `catch`, which is C's. Its own region is kept the same way (An actor's objects).
- A long sequence that is no actor's (a cutscene the engine runs) can still be a task of its own (`start-task`;
  `spawn` and `kill` are the actors' alone).

## What a handler may keep where

Another actor's handler may run between any two words of a handler - while it waits, and when its slice is over
(Slices, below). So a handler keeps what it is working on in its locals, its actor's state, or a `task-variable` (each
task its own) - never in a plain `variable`, which every actor on the node shares. What the node's actors do share -
a cache, a counter - is read and changed in one primitive (`+!`), or inside `[: ... ;] atomically`; an object another
actor may let go is copied atomically before it is used (`[: cache @ string >string ;] atomically`). The libraries
keep to this (their parsers' places are task-variables); `base`, `error-message` and `n>str` are each task's own.

## Work that blocks

A file read or written, an fsync, another program (`shell`, `shell-output`), PBKDF2 - in a sliced handler (the
`clankforth` tool's) each is done on a worker thread (a pool the process shares, as many as the cores up to 8), and
`sleep-ms` is a timed wait (no thread held), the handler waiting as on an `await` and its node's other actors going on
meanwhile; it goes on with the answer, or the error, as if the word had done it there. The words are the same;
nothing to say. At the prompt, in a task of its own, inside `atomically` or code C runs - and, threaded, called
through `execute` - the word does the work itself, as it always has. A handler's `yield` waits for the next frame,
as ever; one that gave way at its slice's end goes on between frames. (`file-exists?`, `file-size`, `dir-list` - quick -
stay where they are.) A database's fsync (jsondb's leader), a log's writes: the node's other actors aren't held up.

## An actor's objects

Objects (lists, tuples, strings, maps, byte buffers: docs/forth.md, Tuples) an actor makes are freed for it - nothing
to `dispose` in the common case, and nothing left behind when it goes:

```forth
behaviour room
  on spawned ( -- )  list rescue members ! ;          \ kept: rescued, the actor's till it dies
  on said ( s who -- )
      {: s who :}  s" says: " s string s+ drop  {: line :}   \ made here: freed as the handler ends
      members @ [: {: m :}  line m send heard ;] each ;      \ sent: the receivers' (the last one's to free)
end-behaviour
```

- **A handler runs in a scratch region of its own**, inside its actor's region: what it makes is freed as it ends -
  done, failed, or let go as its actor is killed (one that waits keeps its objects till it goes on and ends).
- **`rescue ( obj -- obj )`** in a handler gives one to its actor: kept till the actor dies (or `dispose`).
  `rescue-actor ( obj -- obj )` does so from any region the handler is in (inside a `with-scratch`, through a
  failure: its last words see it). `rescue-all ( obj -- obj )` keeps one till `dispose`, its actor gone or not. Each
  takes what the object holds with it (a list's strings made where it was).
- **A message takes the objects it carries** made in a scratch region (a handler's, `with-scratch`'s): the sender
  gives them away. Delivered, each is the receiving handler's, freed as it ends unless it rescues it; one never
  delivered (a dead letter, its receiver killed first) is freed. A broadcast's goes to the last receiver, the others
  seeing it as it waits. One kept (rescued, the system's - made at load or outside an actor) goes as it is, by
  reference: the sender's still, the receiver's to read, not to keep past the sender's `dispose`. From another node
  every object comes as one made in a scratch region does.
- `actor-objects ( id -- n )` counts what an actor holds - what it rescued, and its running or waiting handler's;
  `.actor-objects ( id -- )` by kind; `.actors` shows the count. An actor whose count only grows is a leak to look at.

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

A state type defined again (`reload`) moves each live actor's state over: each field's value to where that field is
now, by its name (fields reordered, one added - zeroed - or one gone), as many bytes as both have room for. A word
compiled before that, using a field the state no longer has room for, is an error as it runs.

## Actors

```forth
listening listener-state  s" Debilitas" spawn  ( -- id )   \ its first message: `spawned`
id subscribe tick          \ (or inside a handler: self subscribe tick)
id kill                    \ its `killed` handler (its last words) first; killing itself, it goes after the handler
                           \ (its last words run there and then: what follows `self kill` finds them done)
self  ( -- id )            \ the actor being run
sender  ( -- id )          \ who sent the message being handled (-1: the engine)
id monitor                 \ this actor told `died ( id reason -- )` when that one is gone (unmonitor)
id link                    \ the two killed together: either gone, the other is killed (unlink)
```

`died`'s reason is 0 if the actor was killed, the record of the failure it died of (below: `failure-error` ...
read it), or `no-actor` (-1): there was none to watch, or its node ended (Nodes, below). One linked to it dies of the
same.

`actor-bytes ( id -- n )`: the bytes its objects take (what it rescued, its handler's) - its memory, near enough;
`.actors` shows it too. `alive? ( id -- flag )`, `busy? ( id -- flag )` (a handler of its is under way: waiting, or its slice spent - what it was doing is half done), `actor-slot ( id -- n )` (-1 for `nobody`), `actor-name ( id -- addr len )`, `actor-named ( addr len -- id | -1 )`, `actor-type ( id -- type )` (its state type; -1 none, -2 no such actor), `behaviour-of ( id
-- behaviour )`, `nobody` (-1), `actors-max`, `actors-alive`, `no-actor` (-1).

## Failures

A handler's error ends that handler, and is recorded. What becomes of the actor is its supervisor's to say:

- `id errors-to` names an **errors actor**: every handler's failure and every task's is told to it as `actor-failed
  ( record -- )` (not its own: those are only said). The actor that failed lives on; the errors actor asks it to put
  itself right (`start-over`), or kills it.
- With **no errors actor**, nobody would put it right: the actor **dies of it** - its last words (`killed`), those
  linked to it killed, those monitoring it told `died` with the failure's record for the reason.
- `errors-actor ( -- id )`: the errors actor named (nobody: none). The `supervisor` library is one: `supervise`
  starts it for the node - each failure a JSON line on stdout, the actor told `start-over` and living on; one failing
  more than `restart-max` (5) times in `restart-window-ms` (10 s) killed, unless `essential ( id -- )`. A server's
  listeners, SQL pools and watchers start it themselves, so a bug in one handler doesn't take the service down.
  `on-actor-failed ( xt -- )`: xt `( record -- )` told each failure too, after the line and before the actor is put
  right - another destination (an app's own log) without its own errors actor; an error in xt is said and passed over.

 The record by its number: `failure-actor ( r -- id
)` (nobody for a task's), `failure-who`, `failure-what` (the message), `failure-error`, `failure-chain` (the words
running) ( r -- addr len ), `failure-frame ( r -- n )`; the latest 32 are kept. `start-over ( -- )` is a kind for the
errors actor to ask one that failed to put itself right.

## The schedule and tools

- **Slices (preemption, as Erlang's reductions)**: a handler runs `slice-steps` steps (20000 in the `clankforth`
  tool - about a tenth of a millisecond; a host turns it on with `forth_set_slice`), then gives way: the node's other
  actors have their turns, and it goes on where it stopped in a later one - so a long handler (a report, a big
  parse) doesn't hold the node, nor is it stopped as a loop that never ends. `slice-steps! ( n -- )` (0: none - a
  handler runs till it ends or waits). A slice doesn't end inside code C runs (`atomically`, `map-each`'s, a sort's
  comparison): there the old limit holds, 20 million steps with no place to give way. What a handler keeps between
  its words is its own (What a handler may keep where).
- A turn that still holds its node - a long stretch that can't give way: a primitive's own work (a big sort, a
  PBKDF2), an `atomically` - past 200 ms is said: told to the errors actor, `actor-slow ( id kind ms -- )` (the
  supervisor's a JSON line, `"event":"actor-slow"`; an app's own errors actor puts it in its log), or with none a line
  on stdout, `slow: x's 'on msg' held its node 2700 ms`. `slow-handler-ms! ( ms -- )` sets when (0: never).
  `kind-name ( kind -- addr len )`: a message kind's name.

- `actors-frame ( -- )`: one frame - the busy actors whose waits are over go on, `tick` to its subscribers, deliver,
  `frame-end` to its subscribers, deliver. A host calls it each tick (after `forth_run_tasks`); tests call it.
- A held frame (the host's world held: a menu up): `held-tick` to its subscribers, then deliver. Messages still reach
  every actor.
- Between frames (`serve`, a node's loop) what came is delivered and the handlers that gave way go on, the loop not
  sleeping while one can; a frame - `tick`, the waits counted - every 16 ms.
- `actors-runnable? ( -- flag )`: a handler can go on now (it gave way, its job is done, a message waits). A loop of
  the program's own - deliver, then wait for what comes - delivers again at once while it is true, as `serve` does:
  waiting each round, a long handler would go a slice a wait.
- `deliver ( -- )`: (a slice more for each handler that gave way, then) deliver what is queued now, in rounds - what a round sends waits for the next - until the queue
  is empty (or 64 rounds: the rest waits for the next frame). First in, first out; broadcasts in spawn order.
- `trace-on` / `trace-off`: every message printed as it is delivered (`frame  from -> to  kind args`).
- `.actors`: every actor with its behaviour, its state type, its count of queued messages - and, busy, the handler
  waiting, what it awaits, and its mail.
- `.state ( id -- )`: every field of an actor's state, by name. `.recent ( n -- )`: the last n messages delivered
  (256 kept), oldest first - not the frames' own, nor one the same as its sender's last of that kind.
- `enter ( id -- )` / `leave-actor`: the prompt inside an actor - its fields, `self`, `become` and sends are that
  actor's. `attach ( id -- )` / `detach`: the prompt's lines (each line, while it runs) are that actor's till
  `detach` - `attached ( -- id )`. `queued ( -- n )`, `actors-frames ( -- n )`, `actors-reset`.
- `.actor ( id -- )`: one actor in full, one fact a line (its behaviour and what it handles, state, how busy, mail,
  objects, watchers, links, subscriptions, last failure). `.mail ( id -- )`: its messages kept and queued.
  `.failures`: the failures kept. `.behaviour ( behaviour -- )`: what it handles, and from which parent.
  `see-handler ( behaviour kind -- )`: a handler decompiled. docs/debugging.md puts them to work.

## Nodes: actors on other cores

A process may run many Forths - **nodes** - each on a thread of its own, so its actors use every core. Nothing is
shared between them; their actors send each other messages as they send their own (Erlang's way).

```forth
s" workers" start-node  ( -- node )   \ a new Forth on a thread of its own: the host's words, `USING: workers ;`
self s" boss" rot register-name         \ any node's `whereis` finds it ( addr len id -- )
s" boss" whereis  ( -- addr | nobody )
41 s" boss" whereis send ping            \ sent as to an actor here
on ping ( n -- )  1+ sender send pong ;  \ `sender` of a message from another node: its sender's address
```

- **Placement**: `spawn-on ( behaviour type addr len node -- addr )` makes the actor on that node - its behaviour and
  state type found there by name (the vocabulary loaded there too), its address given; nobody if that node hasn't
  them, or is gone. `spawn-anywhere ( behaviour type addr len -- addr )` makes it on the least busy node - this one
  unless another is a tenth of its time less busy (an actor elsewhere costs a copy each message) - and here if it
  can't be made there. Another node's: in a handler (the other's answer awaited). `node-load ( node -- n )`: how busy
  its handlers keep it, thousandths of its time (worked out each quarter second by `serve` and the nodes' loops; -1
  no such node); `least-busy-node ( -- node )`. A report, a big import: `spawn-anywhere` it, and send it the work.
- An actor on another node is known by its **address**: its node's number over 32 bits, its id under them. A plain
  id is an actor here, as ever; `address ( id -- addr )` is one of this node's as others reach it (`node ( -- n )`
  this node's number, `node-of ( addr -- n )` an address's). Sent to an address of this node's, a message is an
  ordinary one.
- A message to another node is **copied** there: its kind by name (each node declares it - `message ping ( n -- )`
  in a vocabulary both load - with as many arguments), its cells as they are, and every argument that is an object
  of the sender's (a string, a list, a tuple, a map, a byte buffer, with what they hold) made again on the other side - the receiving
  handler's, as one sent from a scratch region here (An actor's objects): freed as it ends, unless it rescues it. Only whole objects cross: any other address (an `s"` string, a variable, a
  slice of a string) arrives as a number that names nothing there. A tuple goes to the class of its name there, with
  as many slots - its slots taken in order, so both nodes should load the same definition.
- It comes in as the receiver's actors deliver (`deliver`, `actors-frame`): first in, first out from any one sender.
  One the receiver can't take - no actor of that id, no kind of that name and count, no class - is a dead letter,
  dropped and counted. So is one to a node that has ended.
- **Supervision** crosses too: `kill`, `monitor`, `unmonitor`, `link`, `unlink` take an address, and are asked of
  its node. A `died` from there carries the watched one's address; for a failure, its record is copied here (its
  number this node's, so `failure-error` ... read it; `failure-actor` the address). One already gone when asked is
  told at once - `died` with reason `no-actor`, or, linked, the linker killed of it. When a node ends (`bye`,
  `stop-node`, `actors-reset` there), whoever watches its actors is told `died ... no-actor` and whoever is linked to
  them is killed. Asked of a node that has ended already, they are an error, as for an actor gone here.
- `subscribe`, `alive?`, `actor-name` are this node's actors' (an address of another's is "no actor").
- A node started by `start-node` runs frames as messages come, and at least every 16 ms (its waits count those
  frames), until it runs `bye` or another says `node stop-node` (only one `start-node` made: a host's node runs the
  host's loop); then its names are gone. One whose vocabulary doesn't load (the error said) ends at once. `ms node-wait ( -- flag )`
  waits that long at most for a message from another node.
- `cores ( -- n )`: the cores the machine has - a server's nodes, one a core (one without threads).
- 64 nodes at once, 256 names; each node has as many actors as its creator may (`actors-max`). Without POSIX threads (a build without them) there is one node, this one: `node` is 0.

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

`forth_set_actors_max(f, n)` lets as many actors be alive at once (after `forth_actors`; up to
`FORTH_ACTOR_SLOTS_MAX`, 2^20, never under the slots already made), `forth_actors_max(f)` says how many: 256
(`FORTH_ACTORS_MAX`) unless the host says otherwise. The table grows as actors are spawned, so a high limit costs
nothing until it is used; a host with a table of its own by slot keeps the limit that table's size.

Nodes: `int forth_node(Forth *f)` this Forth's node number; `forth_set_node_setup(f, setup)` the host's words for a
Forth `start-node` makes (called on it before its prelude, as the host's own were - the tool's is its `bind`);
`int forth_node_wait(Forth *f, long ms)` waits that long at most for a message from another node (1 if one came):
where a host's loop with nothing to do sleeps.

`int forth_actor_slot(int id)` is an id's slot (0 .. `forth_actors_max(f) - 1`; -1 for nobody's): what a host's
table with an entry for each actor is indexed by. An id itself is past `FORTH_ACTOR_SLOTS_MAX` once its slot has been
taken again; `forth_actor_body(f, id)` is NULL for an id whose actor is gone.

`forth_actor_send` and `forth_actor_broadcast` send nothing (and say so) given a kind there isn't, or more than
`FORTH_MSG_ARGS` arguments.

The kernel's kinds come first (`FORTH_KIND_TICK` ...): `tick`, `frame-end`, `spawned`, `killed`, `held-tick`,
`actor-failed`, `start-over`, `died`. Tests: `tests/test_actors.fs`.

## Rules

- Actors may read facts (the host's world and data) directly, but never another actor's state.
- Replies are messages: to ask an actor something, send it a message - and `await` its answer, or handle it.

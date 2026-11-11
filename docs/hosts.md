# ClankForth in a host

What a host (the program that embeds ClankForth: OpenHuey, OpenMizzurna, ClankMUD) builds against is `src/forth.h`,
and `src/actors.h` for the actor kernel. A `Forth` is a handle there. The system's state itself is
`src/forth_internal.h`: `forth.c`'s and the actor kernel's, changed without notice.

Words (`Word`), vocabularies (`Vocab`) and tasks (`Task`) are structs in `forth.h`: a primitive is handed a `Word`
and may read its `body`; a host that leaves the stacks as it found them reads `forth_task(f)->sp`.

## What a host asks of the system

| | |
|---|---|
| `forth_compiling(f)` | inside a definition? (a prompt says ` ok` only outside one) |
| `forth_bye(f)` | `bye` was run: the host should stop |
| `forth_task(f)` | the task running now: its stacks (`sp` items on `ds`, `fp` on `fs`) |
| `forth_current(f)` | the vocabulary definitions go to (`forth_set_current` sets it) |
| `forth_find_vocab(f, name)` | the vocabulary of that name, NULL if there is none (`forth_vocab` would make it) |
| `forth_latest(f)` | the newest word |
| `forth_errors(f)`, `forth_warnings(f)` | errors raised outside `catch`; warnings given |
| `forth_last_error(f)`, `forth_last_error_chain(f)` | the last error's message; the words that were running |
| `forth_error_repeated(f)` | the last error was the one before again (so not printed) |
| `forth_forget_error(f)` | the next error is printed even if it is the last again (a prompt's line) |
| `forth_set_budget(f, steps)` | the steps the running code may still take; gives what it was, to put back |
| `forth_set_eval_budget(f, steps)` | the steps each `forth_eval` / `forth_include` may take (`< 0`: no limit) |
| `forth_get_output(f, &out, &ctx)` | where output goes now (`forth_set_output` to put it back) |
| `forth_set_reader(f, reader, ctx)` | source files from besides the disc (a zipped mod's) |
| `forth_set_task_failed(f, fn)` | told of a task stopped by an error (the actor kernel takes it: not with `forth_actors`) |

## A script that never ends

A task that never yields is stopped after its turn's steps. Text the host interprets had no limit:
`: spin begin again ; spin` in a file hung the host as it loaded. `forth_set_eval_budget(f, steps)` gives every
`forth_eval` and `forth_include` a limit (an error, "ran too long", the file's load failing as for any error); the
default is none, as before. `forth_set_budget` limits one call the host makes itself (a hook run with
`forth_call`): set it, call, put back what it gave.

## Moving a host from the open struct

Until this change `struct Forth` was in `forth.h` and hosts read its fields. Each has a function now:

| was | is |
|---|---|
| `f->compiling` | `forth_compiling(f)` |
| `f->bye` | `forth_bye(f)` |
| `f->t` (`f->t->sp`, `f->t->fp` ...) | `forth_task(f)` (`forth_task(f)->sp` ...) |
| `f->m.current` | `forth_current(f)` |
| `f->latest` | `forth_latest(f)` |
| `for (v = f->vocabs; ...)` looking for a name | `forth_find_vocab(f, name)` |
| `f->errors`, `f->warnings` | `forth_errors(f)`, `forth_warnings(f)` |
| `f->error`, `f->error_chain` | `forth_last_error(f)`, `forth_last_error_chain(f)` |
| `f->quiet` | `forth_error_repeated(f)` |
| `f->said[0] = 0; f->repeats = 0;` | `forth_forget_error(f)` |
| `was = f->budget; f->budget = n; ... f->budget = was;` | `was = forth_set_budget(f, n); ... forth_set_budget(f, was);` |
| `out = f->out; ctx = f->out_ctx;` ... `f->out = out; f->out_ctx = ctx;` | `forth_get_output(f, &out, &ctx);` ... `forth_set_output(f, out, ctx);` |
| `f->reader = r; f->reader_ctx = c;` | `forth_set_reader(f, r, c)` |
| `f->task_failed = fn` | `forth_set_task_failed(f, fn)` |

`Manifest`, `Source`, `Nesting`, `Guard` and `FORTH_LOCALS` went with the struct. A host that needs something of the
system this list lacks asks for a function (a pull request here) rather than including `forth_internal.h`.

Changes of behaviour a host's scripts may meet, all of them where a crash or an undefined result was:

- `;` warns of a `>r` not taken back before a word returns, and of an `exit` inside a `do` loop without `unloop`
  (counted in `warnings`: a host whose tests say its scripts load with none will hear of them).
- `execute`, `catch`, `jump`, `spawn` (now `start-task`) and a deferred word given an address that isn't a word's are
  an error; so are the compiler's own words (`(lit)`, `branch` ...) typed or executed.
- `f>s` and `3f>s` of a float too large for a cell give the largest (smallest) cell, of not-a-number 0.
- `base` holding what isn't a base counts as ten.

## Updating a host: ids, failures, the tasks' words, the lists' words

What a host changes to take the ClankForth with the actors' lasting ids, the tasks' words renamed and the lists' words
checked. `forth.h` is as it was; `actors.h` gained `forth_actor_slot`. Each step says how to find what it touches
(from the host's folder; leave out its copy of ClankForth) and what happens if it is skipped. Steps 1 and 2 must
be done before the host runs with this ClankForth; 3 to 5 are to be checked.

### 1. An actor's id is not under `FORTH_ACTORS_MAX` any more (must: skipped, it fails silently)

An id was 0 .. 255. It is now the actor's **slot** (0 .. 255) plus 256 for each actor that had the slot before, so
an id kept after its actor is gone names nobody. The first actor in a slot still has the slot for its id; the id is
past 255 as soon as a slot is taken a second time - soon, in a game that spawns and kills.

```sh
grep -rnE "ACTORS_MAX|actors-max" --include='*.c' --include='*.h' --include='*.fs' .
```

| was | is |
|---|---|
| `id >= 0 && id < FORTH_ACTORS_MAX ? forth_actor_body(f, id) : NULL` | `forth_actor_body(f, id)` (NULL for any id whose actor is gone; `forth_actor_name` gives "") |
| `table[id]` in C, the table `[FORTH_ACTORS_MAX]` | `table[forth_actor_slot(id)]`, after `forth_actor_slot(id) >= 0`; cleared when the actor goes (the slot's next actor is another) |
| `id >= 0 && id < FORTH_ACTORS_MAX` as "is this an actor's id" | `forth_actor_body(f, id) != NULL` |
| `create seen actors-max cells allot` ... `seen id cells +` | `seen id actor-slot cells +` (`actor-slot ( id -- n )`, -1 for `nobody`) |
| `id 0< id actors-max < 0= or` as "not an actor's id" | `id alive? 0=` |
| a test that expects a literal id (`self 3 =`) | still true for the first actor in a slot, and after `actors-reset` (the ids start from the slots again); compare ids kept in constants otherwise |

Skipped: a guard `id < FORTH_ACTORS_MAX` turns away every actor in a slot used before - it has no body, no camera
setup, no entry in the table - and nothing says so. A script's `table id cells +` writes past the table.

### 2. The tasks' `spawn`, `spawn-held` and `kill` are renamed (must: skipped, the script doesn't load)

`spawn` and `kill` are the actors' alone; they no longer hide core words of the same name.

```sh
grep -rnE "forth:spawn|forth:kill|spawn-held" --include='*.fs' .
grep -rnE "(^|[^a-z-])(spawn|kill)([^a-z-]|$)" --include='*.fs' . | grep -v "USING:.*actors"   # (then: files that don't use actors)
```

| was | is |
|---|---|
| `' word forth:spawn` (or `spawn` in a file that doesn't use `actors`) | `' word start-task` |
| `' word spawn-held` | `' word start-held-task` |
| `id forth:kill` (or `kill` in a file that doesn't use `actors`) | `id stop-task` |

`yield`, `wait`, `resume`, `restart`, `me`, `.tasks`, `tick-tasks`, `tasks-paused!` and C's `forth_spawn` are as they
were. Skipped: `unknown word forth:spawn` as the file loads.

### 3. A handler that fails with no errors actor kills its actor (check)

With an errors actor named (`id errors-to`) nothing changes: it is told (`actor-failed`), the actor that failed lives,
`start-over` works as it did. With none - `errors-to` never said, or the errors actor gone - the actor now dies of its
failure: its `killed` handler runs, those linked to it are killed, those monitoring it are told.

```sh
grep -rn "errors-to" --include='*.fs' .
```

To check: the host names its errors actor before any actor can fail (as it starts, and again after `actors-reset`,
which forgets it); a test that makes a handler fail and then expects its actor alive names one first. A host that
wants failed actors left alone needs only an errors actor that does nothing.

### 4. `died` carries a reason (check)

`died ( id -- )` is `died ( id reason -- )`: reason 0 for an actor killed, else the record of the failure it died of
(`failure-error`, `failure-chain` ... read it); one linked to it dies of the same.

```sh
grep -rn "on died" --include='*.fs' .
```

| was | is |
|---|---|
| `on died ( id -- )  ... ;` | `on died ( id reason -- )  drop ... ;` (or use it) |

Skipped: a warning as the handler compiles (`its message carries 2`), and a cell left on its stack each time.

### 5. `each map filter reduce find`: the stack their code sees, and new warnings (check)

Their code now runs on the stack as the caller left it, the item on top. Before, the list and the token (for `map`
and `filter` the new list too) lay under the item. And right after a quotation in a definition they are compiled as
the loop itself and checked like any code.

```sh
grep -rnE "\] (each|map|filter|reduce|find)([^a-z-]|$)" --include='*.fs' .
```

- Code that takes its item and leaves what the word wants (`( x -- )`, `( x -- y )`, `( x -- flag )`, `( acc x --
  acc' )`) runs as it did, faster.
- Code that reached under its item with `pick` to get at the list or the token finds the caller's stack there now.
- New warnings (counted in `warnings`: a host whose tests say its scripts load with none will hear of them): a
  definition whose comment its code doesn't keep (`: f ( l -- x ) [: drop ;] each ;`), a quotation of the wrong shape
  (`[: dup ;] map`). They were mistakes before; now they are said.
- A list made shorter, or disposed of, while it is walked is an error (it was for `each`; `map` and the rest too).

### The checker and tokens kept in slots: what a host does, and may meet

Nothing has to change. To have more checked (docs/forth.md, "Stack effects": words kept in slots):

- Say what each slot of words holds: `variable step  holds: ( -- )`, `cell field her-act  holds: ( -- )`, a record's
  `field: walk.xt  holds: ( -- )`. Words that fetch and run from it are followed; a word of another effect stored in
  it is a warning - through words that store their input there too (`: step! ( xt -- ) step ! ... ;` needs nothing
  said: it is found).
- `.checked` before and after shows the gain ("run a token" falls).

New warnings without anything declared (counted in `warnings`: a host whose tests say its scripts load with none
will hear of them). Each was a word nobody was checking:

- A word that runs a token fetched from memory (`table + @ execute`, an undeclared slot) is no longer "one that runs
  a token": its stack comment is its effect, and its callers are checked with it. Callers that didn't keep to it
  are said now - and theirs in turn, where the word had hidden them.
- A known word or quotation given to a word that runs its input (`['] w his`, `[: ... ;] with-him`, `['] . each`)
  is followed through that word's code: the caller's comment is checked against what really happens.
- `['] w is name`, name a deferred word with its effect said, is a warning if w's effect is another.

`send-after`, `curry`, `compose` and messages keeping the objects they carry (and `rescue-all` taking what an object
holds) were in this repository for a few hours and are out again: a host that took that commit and used them loses
them with this one.

### Also changed, where a crash or a wrong result was

- `id send kind` with a negative id (`nobody`: `sender` for the engine's message, a name `actor-named` didn't find)
  is a dead letter. It was a broadcast to the kind's subscribers; a script that meant that says `broadcast kind`.
- `actor-named` and the actors' `spawn` given a number for the name's address, and `abort"`'s text likewise, are
  errors; a `field` past a state's 64 KiB is an error where it is declared.
- An error caught by `catch` (or `forth_call`) inside `{ ... }` no longer leaves the list open: sixteen of them made
  every later `{` an error ("nested too deep").
- `fdepth` counts from the handler's floor, as `depth` does (an actor's last words, run over a waiting handler's
  stacks, see 0).
- `n wait` and `await-within` with a count past the last frame wait for good; the sum wrapped round to a frame long
  past.
- `forth_actor_send` / `forth_actor_broadcast` with a kind there isn't, or more than `FORTH_MSG_ARGS` arguments, send
  nothing and say so (they wrote past the message).

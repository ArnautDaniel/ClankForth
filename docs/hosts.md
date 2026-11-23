# ClankForth in a host

What a host (the program that embeds ClankForth: OpenHuey, OpenMizzurna, ClankMUD) builds against is `src/forth.h`,
and `src/actors.h` for the actor kernel. A `Forth` is a handle there. The system's state itself is
`src/forth_internal.h`: the system's own files', changed without notice.

The API has a version (`CLANKFORTH_VERSION` in `forth.h`; `forth_version()` the library's): before 1.0 its minor
number goes up when a host must change something, and CHANGELOG.md says what, pointing here for how. A host that
must build against more than one version tests `CLANKFORTH_VERSION_NUMBER` (`#if CLANKFORTH_VERSION_NUMBER >= 300`).

Words (`Word`), vocabularies (`Vocab`) and tasks (`Task`) are structs in `forth.h`: a primitive is handed a `Word`
and may read its `body`; a host that leaves the stacks as it found them reads `forth_task(f)->sp`.

## 0.5.3: `;` with cells left on the stack (check)

- `;` (and `;]`) now refuse a definition that leaves the data stack deeper than it began - an `if`, a loop or a `[:`
  not ended, which used to compile into a jump to address 0. A host's own defining word that keeps a value on the
  stack while the definition compiles, for its `;` to take, gets "`;` : an if, a begin or a do not ended": keep the
  value elsewhere (a variable, the word being defined) instead.

## 0.5.0: net's limits, a lingering close (nothing to do; check)

- `forth.h` and `actors.h` are as they were. A host whose scripts use `net` (a MUD's telnet sockets) gets two limits
  on each connection, 64 MiB each: what has come and not been taken (past it the socket isn't read till some is:
  `net-input-max!`) and what waits to go (a `net-write` past it ends the connection: `net-output-max!`). A
  connection that sends more than that before its script takes any, or a client that never reads, raises them.
- `net-close` lingers: sending shut, what still comes read and let go for up to 2 s before the socket is closed (so
  the other end gets the last bytes, not a reset). The handle names nothing from the close on, as before.

## 0.4.2: the compression vocabulary (nothing to do)

- Every host has a vocabulary more, `compression` (`src/compress.c`, built in as `crypto` is): DEFLATE and gzip, its
  own code - nothing to link. Its words are seen only by a script that says `USING: compression ;`.

## 0.4.0: on-shutdown (nothing to do)

- `system`'s `on-shutdown` sets SIGTERM's and SIGINT's handlers the first time a script says it; `serve` and a
  started node's loop then end as one comes, after the script's words. The check is inside those two loops: a host
  with a loop of its own (a game's) doesn't get it - and its scripts shouldn't say `on-shutdown`, which takes the
  signals' handling from the host.

## 0.4.0: the crypto vocabulary (nothing to do)

- Every host has a vocabulary more, `crypto` (`src/crypto.c`, built in like the core's own): its words are seen only
  by a script that says `USING: crypto ;`. `random-bytes` reads the system's secure random source (getrandom on
  Linux, arc4random on the BSDs and macOS, `rand_s` on Windows): nothing to link.

## 0.4.0: waiting inside catch (nothing to do)

- A task (an actor's handler) may wait inside `catch`. `Task` has a field more at its end, `sys`, the system's (made
  when a threaded task first needs it: the catches it may wait in) - nothing of it a host reads. A host that makes
  its tasks with `forth_task_new` and frees them with `forth_task_free`, as it must, has nothing to change.

## 0.3.0: attach, the repl (nothing to do)

- `forth_actor_attached(f)` and `forth_actors_attach(f, id)` (actors.h): the prompt's actor, the one `attach` sets -
  `forth_eval`'s lines run as it, each while it runs. A console with several users keeps one for each and sets it
  around each user's line (`forth_actors_attach` gives back the one it replaces). A line that `enter`s an actor stays
  in it on the next line, as before - and only while a line runs: a frame between them sees nobody.
- `repl-listen` (net): a host that gives `net` and calls `forth_net_poll` from its loop serves repl sessions there
  (from the interpreter's task, never inside a handler). A host that doesn't want one never says it: nothing listens.
- `>body` of a primitive or a colon definition is an error now (it gave the next word's header, and `to` wrote it).

## 0.3.0: reloading (check)

`forth_reload(f, "vocab")` (forth.h) reads a vocabulary's file again, as `reload` does (docs/forth.md, Reloading): a
host's console, a file watcher, a wizard's command. What a host may meet without calling it:

1. **Text given to `forth_eval` that defines a word again** in its vocabulary now forwards the old one to it: what was
   compiled for the old one runs the new. A host whose console text redefines words to try them gets what it meant;
   one that evaluated a file's text twice, counting on the first definitions staying with their callers, doesn't.
   Files (`forth_include`, `USING:`) are as they were: a word defined again there only hides the old.
2. **Machine code reads a word's code field as it runs** (it was taken as the word was compiled): a host that changes
   a Word's `code` after scripts are compiled against it now has them follow - as threaded code always did.

## 0.3.0: an actor's objects are freed for it (check)

`forth.h` and `actors.h` are as they were; what changes is when the objects an actor's handlers make are freed
(docs/actors.md, An actor's objects). Before, one made in a handler outside a `with-scratch` lasted till `dispose` -
a long-running world leaked whatever wasn't disposed. Now a handler runs in a scratch region of its own:

1. **An object a handler makes and keeps past its end must be rescued** (else it is freed as the handler ends; read
   later, it is an error naming it - `not a list: 94...`, `not a tuple` - not a crash). Find what a handler stores:
   in each `on` handler, a `list`, `{ ... }`, `<map>`, `<bytes>`, `>string`, `s+`, a tuple's `<name>` or `clone`
   whose result goes into a state field, a variable or another kept object. Kept for the actor: `rescue` (in the
   handler itself) or `rescue-actor` (from inside a `with-scratch` there, or to be seen by its last words after a
   failure). Kept past the actor: `rescue-all`, as before. Made outside a handler (as a file loads, in `spawned`'s
   spawner) nothing changes.
2. **Nothing to do for what already worked**: `rescue-all` before sending, and the receiver's `dispose`, still work
   (and an object `rescue-all`ed is still passed by reference). Such code may now be left to the system: an object a
   handler makes and sends is the receiving handler's, freed after it - `lasting`-style helpers and the receivers'
   `dispose` can go.
3. **An object from another node** is the receiving handler's (freed as it ends), not kept till `dispose`: one
   kept is rescued.
4. `.actors` has a column more (each actor's objects); `actor-objects`, `.actor-objects` and `rescue-actor` are new
   words in `actors` (a host's own of those names in a vocabulary its files use: its own still).

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

## Machine code and tasks' stacks

Built for x86-64 - ELF (Linux, the BSDs), or Windows with gcc or clang (MinGW) - the system translates colon
definitions to machine code
(`CLANKFORTH_NATIVE`, on by default; docs/forth.md, "The model") and runs every task - each actor's too - as a
coroutine on a native stack of its own: 1 MiB reserved, its pages taken as they are touched, a guard page below (on
Windows a fiber's: the thread that runs the tasks is made a fiber the first time, and back as the `Forth` is freed).
A task holds its stack only while its turn runs or it waits: idle tasks and actors - a million of them - hold none.
On Linux each waiting one takes two of the process's memory mappings (`vm.max_map_count`, 65530 by default): about
32,000 tasks waiting at once, then "out of memory for its stack" - raise it for more. Built with MSVC, every word stays threaded. Nothing changes in the API but a field at the end of `Task`, `co`, the system's. A host that runs scripts
from threads of its own still runs one `Forth` on one thread at a time. `CLANKFORTH_THREADED` in the environment turns
it off at run time, to tell whether a fault is the backend's.

## Nodes: Forths on threads of their own

A host may run many `Forth`s, each on one thread (docs/actors.md, "Nodes"): their actors message each other's, and
each runs alone, as one always has. A host that lets scripts `start-node` gives them its words with
`forth_set_node_setup(f, setup)` - the function it binds its own with - and a server's loop with nothing to do waits
in `forth_node_wait(f, ms)` for what other nodes send. The system links the platform's threads (POSIX; without them
there is one node). The state that was the process's - the crash report's Forth, `backtrace`'s buffer - is each
thread's now.

## Many actors, supervision across nodes: what changed for a host

`actors.h` gained `forth_set_actors_max`, `forth_actors_max`, `FORTH_ACTOR_SLOT_BITS`, `FORTH_ACTOR_SLOTS_MAX` and
`FORTH_NO_ACTOR`; nothing was taken away, and a host that changes nothing runs as it did:

- **The limit is the host's**: the actors' table grows as they are spawned, up to `forth_actors_max(f)` - still 256
  (`FORTH_ACTORS_MAX`) by default, so a host's own table by slot (`HACTOR_MAX FORTH_ACTORS_MAX`, `actors-max cells
  allot`) stays big enough. A host without such a table, or one that sizes it from `forth_actors_max`, may call
  `forth_set_actors_max(f, n)` after `forth_actors` for more (up to 2^20). `actors-max` in a script is now a word
  reading that limit (it was a constant 256): read at load time, as tables are sized, it gives the same.
- **An id's slot is its low 20 bits** (was: the id modulo 256). `forth_actor_slot` / `actor-slot` give it, as
  before; only a host that computed a slot itself (`id % FORTH_ACTORS_MAX`) must use them now (`grep -rn
  "ACTORS_MAX" .`).
- **`died` may carry `no-actor` (-1)** as its reason - only from another node (nothing local sends it). A handler that
  tests `reason 0=` for "killed" and anything else for "failed" reads -1 as a failure without a record
  (`failure-error` gives "").
- `failure-actor` of a record copied from another node is that actor's address (over `INT_MAX`).
- `Task` (forth.h): `wake` is a `Cell` (was `long`: 32 bits on Windows, a frame count that ran out), and a field at
  its end, `running`, is the system's. A host that reads `wake` gets the same number; nothing else to change.
- `forth_task_turn` on a task whose turn is under way further out (it resumed the one now running) gives it no
  other: `FORTH_TURN_WAITING`.

## The system vocabulary: files, arguments, the clock

`forth_system(f)` gives the scripts the vocabulary `system` (docs/forth.md): files, the program's arguments and
environment, other programs, the clock. A host leaves it out to keep its scripts off the disk (a game's mods); one
that runs programs - a server, a tool - calls it, and `forth_set_args(f, argc, argv)` for `args` (argv kept, so it
must last as long as f). The `clankforth` tool has both.

`forth_net(f)` (after `forth_actors`) gives them the `net` vocabulary: TCP sockets and timers, each an actor's
(docs/net.md). A host with a loop of its own waits in `forth_net_poll(f, ms)` - for the sockets, the timers and
other nodes' messages - then runs its frame. On Windows the library links Winsock (`ws2_32`).

Where the build finds OpenSSL (`CLANKFORTH_TLS`, on by default), `net` has TLS too (`tls-listen`, `tls-connect`,
`tls-ca!`: docs/net.md) and the library links OpenSSL (`OpenSSL::SSL`, public: a host's link gets it). A host that
doesn't want it configures with `-DCLANKFORTH_TLS=OFF`. A TLS context made ignores `SIGPIPE` for the process.

With `net` and `system`, the built-in `http` vocabulary and its parts (docs/http.md) work: a web server of actors.
The library's Forth is laid out as Factor's - `core/prelude.fs`, and `basis/<name>/` a library each
(docs/libraries.md); a host's scripts may lay their own vocabularies out so too (`a.b` in `a/b/b.fs`, as well as
`a/b.fs`). A node `start-node` makes
has the program's arguments too (argv is the process's).

Two changes a host's scripts may meet: `with-scratch` is the prelude's now, not a C word (the code in it may wait;
its errors end its region as before), and `s"` / `s\"` at the prompt longer than 255 bytes are an error where they
were cut short. New words a host's own might meet by name: `source-dir`, `list?`, `map?`, `bytes?`, and in `system`
`file-modified`, `dir?`; in `net`, `net-ended?`, `net-pending`.

## Under AddressSanitizer

The tasks' stacks are switched without telling the sanitizer, so its fake stacks (`detect_stack_use_after_return`,
on by default with clang) go wrong. A host's main file says `FORTH_SANITIZER_DEFAULTS` once, at file scope (forth.h:
nothing outside a sanitizer's build), as the tool and `tests/host_call.c` do; or runs with
`ASAN_OPTIONS=detect_stack_use_after_return=0`.

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

### The sources: four files

`src/forth.c` is the core now; the objects (lists, tuples, strings) are `src/objects.c`, the stack checker
`src/effects.c`, beside `src/actors.c`. A host that builds ClankForth with `add_subdirectory` changes nothing. One
that lists the sources itself adds the two files. `forth.h` and `actors.h` are as they were, and nothing behaves
otherwise.

### A build without the stack checker

`CLANKFORTH_CHECKER` (a CMake option, ON as ever) set OFF leaves `src/effects.c` out: `set(CLANKFORTH_CHECKER OFF
CACHE BOOL "" FORCE)` before `add_subdirectory`, or `-DCLANKFORTH_CHECKER=OFF`. What a host has then:

- The same scripts load: a stack comment is a comment, `holds: ( ... )` is read and let go, `is` stores.
  `forth_set_effect` and `forth_declare_effect` are there and do nothing, so the host's C needs no `#if`.
- No stack-effect warning is given, so nothing the checker would have caught is caught: build with it where the
  scripts are written and tested, without it (if at all) where they are only run.
- `effect.`, `stack-effect` and `.checked` are unknown words. A test file's checker tests go after `checker-tests`
  (the tester's: the file ends there in such a system); `effects-checked? ( -- flag )` tells a script which it is in.
- A little smaller, and scripts load faster (no code is followed at `;`).

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

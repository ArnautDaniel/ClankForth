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
- `execute`, `catch`, `jump`, `start-task` (then `spawn`) and a deferred word given an address that isn't a word's are an error; so are
  the compiler's own words (`(lit)`, `branch` ...) typed or executed.
- `f>s` and `3f>s` of a float too large for a cell give the largest (smallest) cell, of not-a-number 0.
- `base` holding what isn't a base counts as ten.
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
- The tasks' words have names of their own: `start-task ( xt -- id )` (was `spawn`), `start-held-task` (was
  `spawn-held`), `stop-task ( id -- )` (was `kill`). `spawn` and `kill` are the actors' alone, no longer hiding the
  core's: a script's `forth:spawn` / `forth:kill` (and a plain `spawn` / `kill` in a file without `USING: actors`)
  becomes `start-task` / `stop-task`. `forth_spawn` in C is as it was.
- `each map filter reduce find` run their code on the stack as their caller left it (the list and the token are no
  longer under the item), and right after a quotation in a definition they are checked: a comment their code doesn't
  keep is a warning now (counted in `warnings`), as is a quotation of the wrong shape (`[: dup ;] map`).
- `forth_actor_send` / `forth_actor_broadcast` with a kind there isn't, or more than `FORTH_MSG_ARGS` arguments, send
  nothing and say so (they wrote past the message).

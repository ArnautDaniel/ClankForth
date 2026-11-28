# ClankForth: notes for an AI assistant

A small Forth in C built around actors: Erlang's actors, Factor's vocabularies and stack effects, Forth's
directness. Embedded in a program through `forth.h`, or run on its own (`clankforth`) as a server. Read `README.md`,
then `docs/forth.md` (the language), `docs/actors.md` (actors), `docs/debugging.md` (a running system).

## The language, in brief

- **Stack effects are checked**: `: name ( a b -- c ) ... ;` - `;` compares the code with its comment and warns where
  they differ (`strict!` or `CLANKFORTH_STRICT=1` makes a warning an error). Write the comment first, and believe it.
- **Locals** `{: a b | c -- :}` (in a definition only - `|` names locals that start at 0), set with `to`. Quotations
  `[: ... ;]` see them. Prefer locals to `swap rot pick`; no `>r r>` shuffles.
- **Vocabularies** as Factor's: `IN: name`, `USING: a b ;`, `<PRIVATE ... PRIVATE>`, `vocab:word`. A file sees only
  what it asks for; a vocabulary is a file (`basis/a/b/b.fs` is `a.b`).
- **Objects** are known by their address: strings (`s" x"` is text; `>string drop` an object; `string` gives an
  object's `a n`), byte buffers, lists, maps (`s-at` / `s-at!` by text, `at` / `at!` by value), tuples (`TUPLE:`,
  `>>slot` / `slot>>`). Build text with `[: ... %s ... ;] build` or `format` (`"{} of {s}"`).
- **Memory by regions**: what a handler (or `with-scratch`) makes is freed when it ends; what must outlive it is
  stored in the actor's state (`state!`, a field) or rescued (`rescue-actor`, `rescue-all`), or freed by hand
  (`dispose`, `json-free`). `timed` and `.objects` show what a word leaves alive.
- **Errors**: `abort" text"`, `x throw`, `[: ... ;] catch` (0, or nonzero and `error-message`).
- Defined behaviour: sums wrap; every address a script reaches is checked (not a sandbox: a script can do anything
  its program can - docs/hosts.md).

## Actors

- `message name ( args -- )`, `behaviour b  on name ( args -- ) ... ;  end-behaviour`, `state: s  cell field f
  end-state`, `b s s" name" spawn ( -- id )`, `args id send name`. A handler handles one message at a time; it may
  `await kind` another actor's answer (others' messages are kept meanwhile). `sender` is who sent it.
- **No frames**: `serve` and a node's loop are rounds - what can run, runs; then a wait for what comes. A handler
  waits by the clock (`sleep-ms`, `await-within-ms`, `after`) or for a message; `yield` goes on at the next round.
  Frames (`wait`, `tick`, `actors-frame`) are the `frames` vocabulary's, a game host's whose loop runs them - never
  in `basis/`.
- **Preemption**: a handler runs a slice of steps (`slice-steps`), then the node's other actors have turns - so any
  two of a handler's words may have another actor's handler between them. **Never keep a handler's work in a plain
  `variable`** (every actor on the node shares it): use locals, the actor's state, a `task-variable` (each task its
  own), or a short `[: ... ;] atomically` for state the node's actors share (no waits inside it). The checker warns of
  a handler that writes one and reads it back; one meant to be shared is a `shared-variable`.
- **What blocks** (files, fsync, `shell`, `sleep-ms`, PBKDF2, a long text compressed) is done on worker threads
  while the handler waits; at the prompt it runs where it is. A word of C's is one step however long it works: one
  that works long counts it against the slice (`forth_slice_charge`), or is a job (`forth_job_new`, as PBKDF2's).
  A long job goes to its own actor (`spawn-anywhere` puts it on the least busy node).
- **Failures**: `supervise` gives a node an errors actor - a failed handler logged, its actor told `start-over`;
  `essential` for those never killed. Nodes (`start-node`) are Forths on threads; actors on others are addresses.
  Or processes: `n as-nodes` at a program's top (system's) - a supervisor runs the program again for each node and
  starts one again when it ends; every node the same program, sharing nothing (`node-index`; a jsondb folder each).
- Web: `http` (routes, handlers per connection actor), `http.live` (a live page per actor: state fields, events,
  regions - what is outside them changing sends the whole body), `jsondb` (records as maps: `db-record`, `db-save`;
  queries by `json.query`, never JSON's text put together by hand), `sql` - docs/http.md, docs/live.md,
  docs/jsondb.md, docs/sql.md.

## Practice

- **Simple first**: the smallest change that does the job; no new subsystem for a 10% win. Measure before
  optimising (`timed`, `.handler-times`, valgrind's callgrind); a performance change says what it measured.
- Forth in `core/` and `basis/` follows docs/forth.md's style: short words, a stack comment each, locals over
  juggling, comments that say why. C reads like the C around it; name every magic number.
- **Tests with every change** (`tests/test_*.fs`, `T{ ... -> ... }T`), a bug's test failing before its fix. Docs move
  with the code. Free what you keep: a test that makes objects checks `objects` comes back where it was.
- Preemption-safe library code: parser state in `task-variable`s; a cache read and swapped `atomically`, a copy
  taken before use if another may free it; never hold `net-input`'s address across a wait.

## The repository

- **Versions**: `forth.h`'s `CLANKFORTH_VERSION_*` (CMake reads them). Every change adds a line to CHANGELOG.md's
  Unreleased; a release bumps the version in the same pull request, then the merge is tagged `vX.Y.Z`. `forth.h` is
  the embedding API: a change to it is said in CHANGELOG and docs/hosts.md (how an embedder adapts).
- **A `Forth` is a handle**: the system's state is `src/forth_internal.h` (the system's own files'); what an embedder
  needs gets a function in `forth.h`, not a field made public.
- **The sources**: `src/forth.c` the core (interpreter, dictionary, compiler, tasks), `src/objects.c` (strings,
  lists, maps, tuples), `src/effects.c` (the stack checker; `CLANKFORTH_CHECKER=OFF` puts `src/effects_none.c` in
  its place - a checker test goes after `checker-tests`), `src/actors.c` (the actor kernel), `src/native.c` (machine
  code, tasks as coroutines), `src/jobs.c` (the worker pool), `src/nodes.c` (Forths on threads), `src/system.c`
  (files, arguments, the clock), `src/foreign.c` (C libraries loaded and called: the `c` vocabulary - the one place
  a script's mistake is a crash), `src/crypto.c`, `src/compress.c` (zlib's work inside `#ifdef CLANKFORTH_ZLIB`,
  its own code otherwise: both stay tested), `src/net.c` (sockets and timers on actors; TLS with OpenSSL, every
  TLS line inside `#ifdef CLANKFORTH_TLS`). What one file has of another is declared in `forth_internal.h` and
  named `forth_...`; the rest is `static`.
- **Defined behaviour in C**: arithmetic wraps through unsigned casts (no signed overflow); every address the
  scripts reach goes through `addr_n`.
- **The library's Forth is built in** (cmake/embed.cmake): `core/prelude.fs`, and `basis/<a>/<a>.fs` the vocabulary
  `a`, `basis/<a>/<b>/<b>.fs` the vocabulary `a.b`. One using `net` loads only where the embedder gives `net`, one
  using `system` where it gives `system`. A new library: its own folder and a `tests/test_<a>.fs`.

```sh
cmake -S . -B build -G Ninja && ninja -C build && (cd build && ctest)   # tests run from inside build/
cd build && ./clankforth ../tests/test_json.fs                         # one suite
```

CI is the full check - gcc and clang with warnings as errors, sanitizers, the minimal build, Windows, ARM64
(threaded throughout), a fuzzer, ThreadSanitizer (`tests/tsan_stress.fs`), SQL Server. Locally, run the suites a
change touches; push, and read CI.

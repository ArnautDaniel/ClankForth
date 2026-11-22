# Changelog

The version is `forth.h`'s (`CLANKFORTH_VERSION`, `CLANKFORTH_VERSION_NUMBER` for `#if`; `clankforth --version`,
`clankforth-version` in Forth). Before 1.0 the minor number goes up when a host must change something to build or run
with it - its entry says so, and [docs/hosts.md](docs/hosts.md) says how - and the patch number for what needs nothing
of a host. A release is a commit that moves the version, tagged `vX.Y.Z`.

Changes not yet released are listed under **Unreleased**; a release renames that heading to its version.

## Unreleased

- **A task may wait inside `catch`** (`wait`, `yield`, `await`): an actor's handler can `[: await reply ... ;]
  catch`, and an error after the wait is the catch's. With the native backend the catch's frames wait on the task's
  stack (what is the task's of the system's - its depth, the catch errors unwind to, its call chain - kept while it
  waits and put back for its next turn); threaded, a catch a task may wait in is the task's own frame (`Task`'s
  `catches`, the system's), up to 16 within each other. It was an error ("only a task can wait").
- `;]` with no `[:` before it is an error (it took what was on the stack for a quotation, and crashed - found by the
  fuzzer).
- A definition with a cell laid down by hand that is no word (`[ 41 , ]`) crashed the stack checker and `see` -
  found by the fuzzer. The checker leaves such a definition unchecked; `see` shows the cell as `[ 41 , ]`.
- A compiler's word that takes operands (`(local!)`, `(outer@)`, `branch` ...) named in a definition is an error:
  nothing laid its operands down, so the cells after it were read as them (the checker and the native backend
  crashed - found by the fuzzer). `postpone` lays one down, its operands then `,`, as before.
- **`http.client`** (docs/http.md, The client): `http-get`, `http-post`, `http-request` - each request a connection
  on an actor of its own, the asker awaiting its response (`res-status`, `res-body`, `res-header`, `res-headers`);
  no response is status 0 and `res-error` (not an error thrown, as Erlang's `{error, Reason}`). HTTPS with TLS.
  `http-timeout-ms`, `http-response-max`.
- The http server reads a request body in chunks (`Transfer-Encoding: chunked`: it was a 501), answers `Expect:
  100-continue`, and has `form-param` (an HTML form's body, as `query-param` the query string's). Both a
  `Transfer-Encoding` and a `Content-Length` is a 400. `header-of`, `parse-headers`, `dechunk` (the client's too).

## 0.3.0 - 2026-11-22

- **An actor's objects are freed for it** (a host checks its scripts: docs/hosts.md, 0.3.0). Each handler runs in a
  scratch region of its own, inside its actor's region: what it makes is freed as it ends, what it `rescue`s the
  actor keeps till it dies. A message takes the objects it carries from a scratch region: the receiving handler's
  (from another node too). `rescue` and `rescue-all` take what an object holds with them. New words
  `rescue-actor`, `actor-objects`, `.actor-objects`; `.actors` counts each actor's objects.
- A map or a byte buffer sent to another node is copied there (it crashed the process: packed as a tuple).
- **Looking into a running system** (docs/debugging.md): `repl-listen` (net) - a repl on 127.0.0.1, each session's
  lines run as the prompt's, answered with a last line (`ok`, `ok name#id`, `compiling`, `error`); `attach` / `detach`
  (the prompt's lines run as an actor; `forth_actor_attached`, `forth_actors_attach`); `.actor`, `.mail`, `.failures`,
  `.behaviour`, `see-handler`; `see-xt`, `with-output`; `listen-local`. Listings show an object as what it is.
  The tool's prompt says `ok name#id` while attached.
- `>body` of a primitive or a colon definition is an error (it gave the next word's header: `1 to .` corrupted the
  dictionary - found by the fuzzer).
- `see` of a word just before a quotation typed at the prompt read on past its end (and could crash).
- **Reloading** (docs/forth.md, Reloading): `reload name` and `forth_reload` read a vocabulary's file again, and each
  word it defines anew forwards the old one - every caller, threaded or machine code, an actor's handler, gets it from
  its next call. A word defined again at the prompt (`forth_eval`) does the same. A state type defined again moves
  its actors' states over by field name; a tuple class with the same slots and a generic defined again stay
  themselves. `reloading?`. A host's `forth_eval` text that defines a word again now forwards it (docs/hosts.md).

## 0.2.0 - 2026-11-22

- The version, in `forth.h` (`CLANKFORTH_VERSION_MAJOR` `_MINOR` `_PATCH`, `CLANKFORTH_VERSION`,
  `CLANKFORTH_VERSION_NUMBER`), `forth_version()`, the word `clankforth-version ( -- addr len )` and the tool's
  `--version`. CMake's project version is read from `forth.h`. Nothing for a host to change.

## 0.1.0 - 2026-11-21

Everything up to pull request #17 (commit `570b07a`), before there were versions: the interpreter and its native
backend (x86-64), vocabularies, stack effects and the checker, locals, quotations, lists, tuples and generic words,
strings, maps, tasks; the actor kernel, nodes; the `system` and `net` vocabularies; `basis/` (`http` with WebSockets,
server-sent events, static files, cookies and TLS; `json`, `base64`, `checksums.sha1`, `calendar`, `urls.encoding`).
What a host written before then changes is in docs/hosts.md.

# Changelog

The version is `forth.h`'s (`CLANKFORTH_VERSION`, `CLANKFORTH_VERSION_NUMBER` for `#if`; `clankforth --version`,
`clankforth-version` in Forth). Before 1.0 the minor number goes up when a host must change something to build or run
with it - its entry says so, and [docs/hosts.md](docs/hosts.md) says how - and the patch number for what needs nothing
of a host. A release is a commit that moves the version, tagged `vX.Y.Z`.

Changes not yet released are listed under **Unreleased**; a release renames that heading to its version.

## Unreleased

- `http.live`: `request:` - a view's word run in its page's GET (its user from its session into params, or the request
  answered itself). **`examples/work-queue.fs`** (`examples/queue/`): a work queue end to end - sign-in, a live queue
  (filters, claims), a task's form saved with its revision (a conflict said), new tasks; pages on every node.
- **Live views** (`http.live`, docs/live.md): each open page an actor - `live-view name mount: render: event:
  changed: info: layout: end-live-view`, `live-route`; its state (`state`, `state!`), `region`s re-rendered and sent
  when they change, `live-redirect`, `live-tell`; events from `live-click` / `live-submit` / `live-change`
  attributes; its socket joined with a secret, Origin checked; `live.js` served at `/live.js`. `http.websocket`: an
  owner word may give `nobody` (403).
- **jsondb queries and watching**: `db-query` (where: equality and `= != < <= > >= in contains exists`; sort by
  fields, descending with `-`; offset, limit; each record with its id and revision, and the total) - an equality on
  an indexed field uses the index. `db-watch` / `db-unwatch`: `db-changed ( c id rev json -- )` to an actor for each
  change once it lasts (a replica's watchers as its leader's lines come); a watcher that dies forgotten.
- **Pages** (docs/html.md): `html` - a word for each HTML element (`<div ... div>`), attribute words (`class=`,
  flag attributes taking a flag), `text` (escaped), `raw`, `html ( xt -- s )`, `%escaped`. `html.templates` - HTML
  files with holes (`{field}`, `{a.b}`, `{#each}`, `{#if}{#else}`, `<component-name/>`) compiled into words as they
  load (`templates dir`, `template path`, `template-text`), their errors naming file and line. `build-mark@`,
  `build-mark!`: a cell kept with the string being built.
- **Sequences** (Factor's protocol): lists, strings and bytes take the same words - `length nth nth! push pop clear
  like`, the walks (`each map filter reduce find`, now any kind: `map` over a string a string), `append append!
  subseq head tail reverse index subseq-index seq= starts? ends? concat join split replace trim first last empty?
  find-index each-index map-index`. `" text"`: a string object literal, with escapes. `build` with `% %s %n %c`: a
  string made a piece at a time (a scratch region of its own; each task's its own). `see` shows a string object
  literal as one. Strings keep their length (`push` on one is an error, as is a byte out of 0..255).
## 0.4.0 - 2026-11-22

- Fixed (a review of what came since 0.3.0): a wait inside a catch deeper than a threaded task's frames (16) let
  the code after it go unrun - it is an error there now, as said; a catch a task waited in, its error coming in a later
  turn, put back the interpreter's sources and definition as they were in the turn it began in (an `evaluate`d line
  cut short, a definition dropped) - it puts back this turn's; `client-address` behind a proxy takes the last of
  X-Forwarded-For (the first is the client's to forge: a rate limit got round); `log-open` makes its file's folder,
  and one that can't open leaves no logger (nor the interpreter in it); `csrf-ok?` takes an upload form's `csrf`
  part; `iso-date>` read past a cut-short offset; a body in chunks is read once as it comes (it was read again from
  its start each time: a long one took time on the square); a rate limiter flooded with keys looks for the old ones
  only as their number doubles; a second SIGINT or SIGTERM after `on-shutdown` ends the process at once. `Task`'s
  catch fields are one, `sys`.
- `json`: `json-string@`, `json-int@`, `json-bool@`, `json-list@`, `json-map@` (a field of the kind wanted, or false).
  **`html`**: `html-escape`. **`http.json`**: `req-json`, `respond-value`, `respond-error`.
- **Stopping when told to**: `system`'s `on-shutdown` (SIGTERM, SIGINT: words run from the loop, then `serve` returns;
  nodes with words of their own run them, the main loop waits for them), `shutting-down?`, `shutdown-grace!`.
  `http-drain` (each connection closed after its answer, the loop run till none is being answered),
  `http-in-flight`, `http-draining`.
- **`http.uploads`**: multipart/form-data - `req-parts`, `req-part`, `part-text`, `part-name`, `part-filename`,
  `part-type`, `part-data` (binary as it came).
- **`logging`**: a log file that is an actor - `log-open`, `log` (each line with its ISO 8601 time), batched writes,
  rotation (`log-max-bytes`, `log-keep`). **`http.logging`**: `access-log`.
- `calendar`: `>iso-date`, `iso-date>` (ISO 8601 / RFC 3339).
- `http`: hooks round every handler - `before-each`, `before-under` (a path and those under it), `after-each`, and
  their `-with` forms giving a hook data of its own; `client-address` (`http-trust-proxy`: X-Forwarded-For);
  `req-status`. **`http.security`**: `security-headers`, `security-headers-with` (CSP, HSTS); `allow-origin` (CORS,
  preflights). **`http.limits`**: `rate-limit-under` (429, Retry-After), `<rate-limit>`, `rate-ok?`, `retry-after`.
  `http.sessions`: `require-user` (401), `require-csrf` (403).
- **`http.sessions`**: sessions in a jsondb collection (by the id's SHA-256) - a random id in an HttpOnly, SameSite=Lax,
  Secure cookie; `session`, `session@`, `session!`, `session-forget`; `log-in` (a new id), `log-out`, `session-user`;
  `csrf-token`, `csrf-ok?`; sliding expiry, `sweep-sessions`. `http`: `req-attr`, `req-attr!` (a request's own values).
- **`crypto`** (built in, `src/crypto.c`): `sha256`, `hmac-sha256`, `pbkdf2-sha256`, `random-bytes` (the system's
  secure random source), `same-bytes?` (constant time). On it, `crypto.passwords` (`hash-password`, `check-password`,
  `rehash?`: PBKDF2, a salt each, 600,000 iterations) and `crypto.tokens` (`random-token`, `random-hex`, `uuid4`,
  `sign` / `unsign`). `base64`'s `>base64url`.
- **`jsondb`** (docs/jsondb.md): a JSON database that is an actor - collections of JSON values by id in memory, each
  change a CRC-32'd line of a log, fsynced before it is answered, writes that come together synced together (group
  commit); the log folded into a snapshot (written, synced, renamed, the folder synced). Revisions (`db-put-if`),
  unique and plain indexes, atomic batches, counters (`db-add`), finds, lists, `db-stats`. A torn last line is left
  out, a damaged one refuses to open; a failed disk write stops its writes. Asked from handlers by messages; from the
  interpreter, served there and then. Replicas (`db-replica`): a copy on each web node its reads read, the leader's
  lines streamed to it once they last, its writes sent to the leader - each answered once its lines are in the copy.
- `system`: files held open - `file-open-append`, `file-write`, `file-sync` (fsync), `file-close` - and `dir-sync`;
  `rename-file` replaces a file there on Windows too (MoveFileEx), as on POSIX.
- `checksums.crc32`: `crc32`, `crc32-hex`, `crc32-more`.
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

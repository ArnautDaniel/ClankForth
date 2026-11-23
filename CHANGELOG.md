# Changelog

The version is `forth.h`'s (`CLANKFORTH_VERSION`, `CLANKFORTH_VERSION_NUMBER` for `#if`; `clankforth --version`,
`clankforth-version` in Forth). Before 1.0 the minor number goes up when a host must change something to build or run
with it - its entry says so, and [docs/hosts.md](docs/hosts.md) says how - and the patch number for what needs nothing
of a host. A release is a commit that moves the version, tagged `vX.Y.Z`.

Changes not yet released are listed under **Unreleased**; a release renames that heading to its version.

## Unreleased

- Small devices: docs/small-devices.md (a 64-bit Raspberry Pi: building, a smaller and lighter build, idle CPU,
  hardware through a host's words); CI runs the suite on ARM64, and a small board's build (no checker, MinSizeRel, a
  4 MB dictionary). `CLANKFORTH_DICTIONARY_MB` sets the tool's dictionary (16 MB at first; a node it starts has as
  much). A 32-bit build is refused as it is configured (and by a static assertion): actors' addresses need 64-bit
  cells.
## 0.5.5 - 2026-11-23

Hosts: nothing (`forth.h` unchanged). Apps: a binary column reads as a bytes object (R6-1); SQL with a `USE` statement is refused.

- R6-1: binary through `sql` as bytes. A bytes object as a parameter goes as varbinary (varbinary(8000), longer in
  PLP pieces as varbinary(max)) - a file into a varbinary column without base64 (~2.7 times its size on the wire
  before); a binary column reads back as a bytes object, not `0x` hex text (half the memory) - `>json` writes it as
  the `"0x..."` it was. Parameters are copied for their request deeply (not by their JSON), so bytes stay bytes.
  **An app reading a binary column's value as a string adapts**: it is a bytes object now (`b-data`).
- R6-2: a use of a word with another of its name in sight, of a different stack effect - a program's `build` hiding
  the core's in a vocabulary using both - is warned of where it is used, naming both (`write vocabulary:word for the
  one meant`), not only where the hiding one was defined.
- A data sweep (jsondb, sql, sql.model, sql.watch, azure.identity), each with a test:
  - sql's lock: `USE prod; ...; USE staging` in one batch ran its middle in prod - only the last change was looked
    at. A `USE` statement is refused before it is sent now, and a reply that changed the database at all is dropped.
  - sql's pool: a connection whose handler failed lived on, never back in the pool (a hang) - it goes now; an asker
    gone mid-query left its connection lent - it comes back of itself; `sql-query-all` with a connection dying sent
    to it and lost the others; a waiter was told another connection's error; the count could go down twice; a
    connection lent again kept its borrower's session (SET, temp tables, session context) - reset now
    (RESETCONNECTION); a reply it can't read failed after sql-timeout-ms - at once, saying why.
  - sql.model: a delete without a version checked the key only - the old values now; a text longer than its column
    was cut and "saved" - refused; 2.5 compared with an int column went as 2; `{">": null}` matched the null rows -
    an error; `limit 0` sent `FETCH NEXT 0` (no SQL); a form's whole number took `$10` and 300 for a tinyint, and
    threw on one too large - a sign and digits in the type's range now; its text length counted characters - UTF-16
    units for an nvarchar, bytes for a varchar; an inserted row's key in another case gave null.
  - azure.identity: a token's refresh that failed left a freed token kept - every call after failed.
  - jsondb: a record deleted and made again began at revision 1, so a stale put-if was taken (a lost update); an
    index found 1 but not 1.0 where a scan found both; booleans sorted last (a comment ate their checks); a negative
    offset gave the wrong records; a unique index clashed on null; `sweep-sessions` could delete a session refreshed meanwhile - deleted at the revision read now.
## 0.5.4 - 2026-11-23

Hosts: nothing (`forth.h` unchanged). Apps: `permanent`; CORS, cookies, sessions and live.js behave as their docs say.

- Fixed (R5-1): `state-flag:` (and anything putting json's true, false or null in an actor's keeping) handed the
  shared constant to the actor, freed when it went - "not a tuple" everywhere after. `permanent ( obj -- obj )`: an
  object no region, rescue or message moves and `dispose` refuses; json's three constants are.
- A web sweep (http, the client, files, uploads, SSE, live, WebSockets, sessions, auth), each with a test:
  - a guard went round by %-escapes: hooks matched the path still encoded (`/static/%73ub`, `/admin%2Fx`) - now
    decoded, as the routes see it;
  - an answer given, then the handler failed: dropped with the connection - sent now; a client that shuts its
    sending side after its request (`nc -N`) got nothing - answered now (net.c: a clean end of input still sent to);
  - a header set in another case (`content-encoding`, `content-type`) gzipped again or doubled; `gzip;q=0.8` taken
    for a refusal; a 204 sent a length and a type; HTTP/1.0 kept alive didn't say so;
  - SSE data split on LF only: a CR in it began a field of its own (`event:` injected); the client sent a URL's
    `#fragment`; a multipart filename with a `;` inside its quotes was cut;
  - live: `live-redirect` let `/\host` and control characters through (another site); a push before the socket
    joined waited for the next event; live.js reloaded once a second for ever when its socket was refused (three
    tries now); a form field named `constructor` and the like mis-read; a WebSocket frame larger than the HTTP input
    cap hung (the cap is the message's now);
  - sessions: one refreshed in the database kept the cookie's first Max-Age (the browser dropped it) - the cookie set
    again; `sweep-sessions` outside a scratch region leaked what it read; CORS with `"*"` and a named origin doubled
    `Access-Control-Allow-Origin` and lost credentials on the preflight - one hook per router now, `Vary: Origin`
    always; several `Cookie` headers lost cookies; `require-role` over `/auth` redirected sign-in to itself; the way
    to sign in dropped the page's query; a JWT without a `kid` checked against the first of several keys - refused.

## 0.5.3 - 2026-11-23

Hosts: `forth.h` unchanged; a host defining word that leaves cells on the stack until `;` checks it (docs/hosts.md).

- A bug sweep (the native backend fuzzed against threaded code; objects.c, actors.c and forth.c reviewed), each with a
  test:
  - objects: `subseq-index-from` from past the end read out of bounds (a crash); a map's key rescued
    (`rescue`, `rescue-actor`) or sent was freed with its rescuer, the map then holding a dead key; an object carried
    by two messages was freed (or moved) with the first's; `(like-done)` given what isn't an object crashed.
  - actors: `await` didn't take a message of its kind already kept (it came while the handler ran) - it waited for
    ever; a monitor stayed on the watched actor's list after its watcher died, so the id given again was told of a
    stranger's death; a field word compiled before its state was defined smaller wrote past it; `end-behaviour`
    took any number as a handler (a crash as it ran). An actor of no node no longer joins the nodes' table as the
    actors reset.
  - core: deferred words set to each other recursed in C (a hang, or the C stack) - an error now; `;]` trusted the
    stack it compiled with (`[: [ 99 ] ;]` crashed, a swap there wrote anywhere) - checked; a negative `allot` could
    give back the primitives and the prelude - no further than them now; `;` took a definition with an `if` or a
    `[:` not ended (it ran to a null address) - an error. A host whose own defining words leave cells on the data
    stack until `;` gets that error there: docs/hosts.md.
- Fixed: a do loop ended early as `index - limit` wrapped from the largest number to the smallest (`0 MAX do`,
  `+loop` by 2^62) - the sign changed without crossing limit-1 to limit. `loop_step` adds the standard's test
  (`(d ^ d') & (d ^ n)`); the native `(loop)` checks `index + 1 = limit` (three instructions where it had eight).
- `tools/native-diff.py`: the native backend against threaded code - random definitions of the words it writes in
  line, run three ways, compared. In CI.
## 0.5.2 - 2026-11-23

Hosts and apps: nothing - words added.

- `crypto`: `sha1`, `hmac-sha1`, `pbkdf2-sha1` (C, by FIPS 180's, RFC 2202's and RFC 6070's vectors) - for another
  system's hashes, not new secrets; SHA-256's code and SHA-1's share their blocks, HMAC and PBKDF2.
  `crypto.passwords`: `aspnet-v2-ok? ( password n hash n -- flag )` checks an ASP.NET Identity 2 ("V2") hash.
## 0.5.1 - 2026-11-23

Hosts: nothing. An app: a live form now sends `"off"` for a checkbox not ticked (it sent nothing).

- Fixed (0.5.0): a live page's socket that landed on another node than its page was refused after a 5 s wait - the
  page's `live-check` was sent the asker's id, not its address, so its answer went to another actor on the page's
  node. A test with the page on one node and its socket upgraded by another's. `sql.watch` registered a watching
  actor by its id too (a page and its watcher on two nodes: its changes never came): its address now.
- `model-form` after a real app's forms (each a workaround it had): text trimmed; an empty text field NULL (`not-null`
  after a column: ""); dates (date, datetime, datetime2 ...) checked and compared as the moment they say, not as
  text; an empty field over NULL, and "off" over a NULL bit, nothing changed - a form saved unchanged writes nothing.
  `http.live`'s forms send `"off"` for a checkbox not ticked. `state-int:`, `state-text:`, `state-flag:`: page state
  that reads 0, "", false till set (a `state-field:`'s json-null is true to `if`). `calendar`'s `iso-date>` reads a
  time without seconds (`2026-11-23T10:00`, a form's datetime-local).
- Docs: the README rewritten for what ClankForth is now (actors, the web stack, data, sign-in); docs/hosts.md's
  sections under the versions they shipped in, and one for 0.5.0's net limits; `managed-identity-token`,
  `live-mount-ms`, `actor-bytes` documented; http.md's parts table with `http.oidc` and `http.ops`, its stale "no
  compression" gone.

## 0.5.0 - 2026-11-23

A minor release: apps adapt where they read a CSRF token's length (86 now, masked) or build `/live?p=` URLs
themselves (`&s=<secret>`); a site of several nodes signing in by OIDC sets `oidc-key`. Hosts: nothing (`forth.h`'s
API unchanged).

- **A fuzzer for what a server reads** (`tests/fuzz_http.c`): requests (chunked, parameters, cookies, multipart),
  JSON, percent-decoding, gzip and DEFLATE, JWTs - seeds in `tests/fuzz-http/`, run over in CI's fuzz job.
- **A security review of the web stack, and its fixes**:
  - `before-under` hooks match the path as the routes see it: `//admin/x` went round `/admin`'s guard.
  - Request smuggling: a header name must be a token, `Content-Length` decimal digits, CR and LF only as CR LF, no
    NUL; at most `http-header-lines-max` (100) header lines; a chunked body's size lines and trailer bounded.
  - `header!` refuses CR, LF and NUL (header injection); `http.oidc`'s `next` is a strict path (no `\`, no `//`).
  - Memory: `net-input-max!`, `net-output-max!` (64 MiB each at first; http sets a request's worth); answers wait
    while `http-pending-max` (1 MiB) waits to go; `http-connections-max` (10000) a node, more answered 503.
  - `net-close` lingers: sending shut, what still comes read and let go (2 s at most), so the answer isn't lost to
    a reset (a 413, a 503 before the request was read).
  - `http.live`: `/live?p=<page>&s=<secret>` - the page asked (`live-check`) before the upgrade, so a socket can't be
    bound to an actor that isn't the page; the join secret compared with `same-bytes?`; `live-redirect` takes a path
    or an http(s) URL only; route parameters win over the query's; the page's GET `Cache-Control: no-store`.
  - `http.oidc`: the sign-in under way is a signed cookie (`oidc_flow`), not a session - `/auth/login` writes
    nothing; `oidc-key ( a n cfg -- cfg )` for a site of several nodes. The provider's keys fetched before the old
    are let go. `/auth/` exempt from `require-signed-in` only for its three routes.
  - `http.sessions`: CSRF tokens masked per response (BREACH); `log-in` forgets the token from before.
  - `http.websocket`: pinged every `ws-ping-ms` (30 s), closed (1001) if nothing came since the last ping.
  - `crypto.jwt`: a token without `exp` refused. `http.ops`: a client's `X-Request-Id` taken only if 1-64 of
    `[A-Za-z0-9._-]`.
  - A host or app reading `/live?p=` itself, or a CSRF token's length (now 86, masked), adapts.
- **SQL logins with Entra ID**: `sql-token ( xt config -- config )` - TDS's FEDAUTH login (an access token in
  LOGIN7's FeatureExt, FEDAUTHREQUIRED in PRELOGIN). `azure.identity`: `managed-identity-token`,
  `managed-identity-sql-token` - from a VM's metadata endpoint or App Service's IDENTITY_ENDPOINT, kept till five
  minutes before they expire. `sql.tds`: `prelogin-with`, `prelogin-option`, `prelogin-fedauth`; `login-info`
  takes a token and the server's echo.
- **Model-driven forms** (`sql.model`): `model-form ( form row m -- changes errors )` - a form's text read by each
  column's SQL type, checked (whole numbers, decimals' precision and scale, bits, text's length, `required`), only
  what changed kept; `validate:` (a model's own rules, `field-error`); `on-insert:` / `on-update:` / `on-delete:`
  hooks after a write (an audit row, in the same transaction). Live views: `state-field:` - declared page state
  (`name@`, `name!`).
- **Signing in with Entra ID** (`http.oidc`): OpenID Connect's code flow with PKCE - `oidc-routes` (/auth/login,
  /auth/callback, /auth/logout), the ID token checked, the user and their app roles in the session;
  `require-signed-in`, `require-role`, `has-role?`. Live views: `guard:` - every event checked first (an event is an
  endpoint). `crypto`: `rsa-sha256-verify`, `rsa-sha256-sign` (RS256, 512-4096 bits; checked against OpenSSL).
  `crypto.jwt`: `jwt-verify`, `jwt-check`, `jwt-sign`, `jwks-of`.
- **Supervision by default**: `supervisor` - an errors actor for the node (`supervise`): a handler's failure a JSON
  line on stdout, its actor told `start-over` and living on; one failing too often killed, unless `essential`.
  `http-listen`, `sql-pool` and `sql-watcher` start it (and mark their actors essential). An http connection whose
  handler failed answers 500 and goes, as before. Kernel: `errors-actor`, `actor-bytes` (an actor's objects'
  bytes; `.actors` shows them).
- **Operating a server**: `http.ops` - `/health`, `/ready` (`ready-check`), `/metrics` (`metric`), request ids and
  a JSON line per request (`request-log`). `logging`: `log-json`, `log-event` (JSON lines to stdout). A
  `Dockerfile` (Debian, the tool, OpenSSL), built and run in CI.
## 0.4.2 - 2026-11-23

- Windows: a listener's backlog asked for in full (`SOMAXCONN_HINT(65535)`; `SOMAXCONN` there means some 200) - one
  node with 200 clients at once refused some. `compression`: what a call works in sized to its input (a short
  WebSocket message's a few KB, not the 400 KB a page's takes) - the heap kept it as each open page's memory.
- **Compression**: `compression` (built in, C, no library) - `gzip` / `gunzip`, `deflate` / `inflate`,
  `deflate-sync`. HTTP answers of `http-gzip-min` bytes or more (text, JSON, JavaScript, XML, SVG) gzipped to a
  client that takes them; WebSocket messages compressed (permessage-deflate) with a client that offers it - a
  live view's regions an eighth of their size on the wire. A live page keeps a SHA-256 of each region's last HTML,
  not the HTML (a big page's memory most of it).
- `net`: **one port, several nodes, on Windows too** - where the system can't spread connections (no
  `SO_REUSEPORT`), the node that bound the port hands them round the nodes listening on it, in turn. A listener's
  backlog is the system's most (`SOMAXCONN`, was 128: a burst of 200 clients was refused in part). On Windows a
  port is the process's alone (`SO_EXCLUSIVEADDRUSE`: a second server is refused, not answered beside the first).
  `cores ( -- n )` (actors): the machine's cores - `examples/work-queue.fs` starts a node a core unless told.
- `sql`, TLS first (Azure): **a request past one TDS packet closed the connection** - its packets went in one TLS
  record, and SQL Server reads a TDS packet a record; `tls-record-max! ( n conn -- )` (net) caps a connection's
  records, sql sets it to the packet size. **`map-each` refused a map whose entries had all been taken away**
  (taken for one changed as it was walked): a pool whose free connections had all closed died at its next timer.
  A pool's timer mistakes are said, not its end.
- `sql-query-all` / `sql-also`: several queries out at once on the pool's connections, their rows awaited
  together. `quiet-message` (actors): a kind dropped without a word where unhandled - `sql-answer`, late after its
  asker stopped waiting. A pool passes over an asker that died while it waited (the connection not lost). A live
  event that changes nothing is answered (`{"r":[]}`).
- `sql`: **a caller's parameters were freed with its request** - the map and the values in it went with the
  message to the connection, freed after; a copy goes now. **A connection could be lent with a transaction
  open** (a rollback that failed, a BEGIN of the caller's): a connection given back is rolled back first, or
  closed if that fails.
- `sql`: a connection free for `sql-idle-ms` (60 s) is closed - a serverless database pauses only with no session
  open. An empty list parameter is `SELECT NULL WHERE 1 = 0` (NOT IN finds every row); names in comments, brackets
  and double quotes left; more than 2100 parameters said, and a request that can't be made answered (its connection
  kept). Logins refused for now too many times: everyone waiting told.
- `sql`, for Azure serverless: logins refused for now (40613 - a paused database resuming - and the like) tried
  again after a doubling wait (`sql-retry-ms`, `sql-retry-max-ms`, `sql-retries`); `sql-connect-ms` 60 s. A list
  parameter is several (`IN (@ids)`). `sql.watch`: `sql-watch-where` / `model-watch-where` - only the rows a WHERE
  finds; a watch nobody watches dropped (a database no one looks at may pause). A failed connection no longer
  tells its pool twice (its count went wrong, a waiting asker told of the second).
- **SQL Server** (`sql`, docs/sql.md): Azure SQL Database, Managed Instance and SQL Server 2022 from actors, in
  Forth over `net` - TDS 8.0 (TLS first, the certificate checked), Azure's redirects followed. `sql-pool`,
  `sql-query` / `sql-one` / `sql-exec` / `sql-reply` with parameters (sp_executesql; `sql-reply-typed` declares
  them), `sql-transaction`. Locked to its database: the login fails without it, the server must say it, a session
  whose database changes is closed; `sql-forbid` refuses SQL naming a database; `sql-dry-run` rolls every request
  back. `sql.model`: a table's words from its declaration (get, find and count in jsondb's query shape, insert,
  update and delete checked by a rowversion or the old values). `sql.describe`: a table as a model, the databases
  a login may open. `sql.watch`: tables watched, `db-changed` to an actor (live views). `sql.tds`: the protocol.
  `tools/sql-check.fs`: a first look that writes nothing and refuses anything but a staging database.
- `net`: `tls-connect-as` - the certificate checked against a name of its own, ALPN offered; `net-why` - why an
  error ended a connection (refused, a TLS handshake's failure and its reason).
## 0.4.1 - 2026-11-22

- A code review's fixes. `" text"` in a definition gives a new string each run: one compiled once and shared could be
  moved into a region (`state!`, `rescue`) or freed (`json-free`) and its word then read freed memory. `html`: no
  `<u` (its `u>` hid the core's). `http.live`: `timer:` (a view's timers were taken for its join's, the page killed
  or the tick lost); live.js waits for the server by `/live.js` (a HEAD of the page made a page); a page with no
  regions rendered once, not twice. `html.templates`: a template's code names its words whole (it failed without
  `html` used, and a file's own `raw` or `each` changed it). jsondb: a record without the sorted field sorts as null
  (first), not as 0; a put's text copied only when a watcher is told. `see`: code after a quotation shown (inlined
  walks); a string literal shown with its escapes.
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

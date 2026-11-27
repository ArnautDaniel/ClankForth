# Changelog

The version is `forth.h`'s (`CLANKFORTH_VERSION`, `CLANKFORTH_VERSION_NUMBER` for `#if`; `clankforth --version`,
`clankforth-version` in Forth). Before 1.0 the minor number goes up when a host must change something to build or run
with it - its entry says so, and [docs/hosts.md](docs/hosts.md) says how - and the patch number for what needs nothing
of a host. A release is a commit that moves the version, tagged `vX.Y.Z`.

Changes not yet released are listed under **Unreleased**; a release renames that heading to its version.

## Unreleased

- A program's nodes as processes: `n as-nodes` (system's). The process that says it becomes a supervisor: it starts
  the program again n times, each a node - its own process, Forth, actors and memory - starts again one that ends
  (a crash in the system's C took every thread-node with it: it takes one process now), tells them to stop when it
  is told, and ends them if it is killed. `node-index`, `node-count`, `node-life`: which node this is. Every node
  listens on the program's port and the system spreads the connections (Linux, the BSDs; Windows: to come). Seen:
  three nodes on one port, one killed by a signal as it answered - the other two answering all the while, the third
  back in 200 ms. `forth_set_program` (forth.h, new) for a host whose scripts say it; the tool does
  (docs/hosts.md). src/supervisor.c.

- jsondb keeps its indexes. An index was only in memory: a program made each one again every time it opened the
  database, and one it forgot - or made after its first writes - was a unique rule quietly not there. An index is
  now a line of the log, as a record is: made to last before `db-index` / `db-unique` answers, in the snapshot after
  a compaction, sent to the replicas with the leader's other lines, and there when the database is opened. Said
  again as it is, nothing is written - so a program that makes its indexes at each start (every one did) goes on as
  it is, and has them kept from its first start with this. `db-drop-index ( field n c n db -- flag )`, new: one let
  go (kept too); an index there already another way - unique or not - is refused till it is dropped. A database an
  index was made in is still read by 0.8.0 (it knew the line, from its replicas); one an index was dropped in is
  not, till it is compacted.

- `http.files` sends a long file a piece at a time, and a part of a file when asked. Every file was read whole and
  written to its connection at once: a file's size in memory for each client taking it, and one past 64 MB not
  served at all (a connection with that much waiting to go is ended). Now a file over `http-file-whole-max` (1 MiB)
  is read 64 KiB at a time and sent as its client takes it: a 300 MB download through a client held to 40 MB a
  second, the server at 7 MB throughout, the file whole at the other end. And `Range: bytes=` is answered - 206
  with `Content-Range` (from a place to another, from a place on, the last so many), 416 past the file's end,
  `If-Range` by the file's date, `Accept-Ranges: bytes` on every file - so a download goes on where it stopped and
  a video is sought in (curl's `-C -` and `-r`: the same bytes). A shorter file is read whole and gzipped, as before.
- `respond-start-sized ( length status req -- )`, new (http): a streamed answer whose length is known - its head says
  `Content-Length`, its pieces go as they are, the connection kept after, to HTTP/1.0 too. `respond-drained ( req --
  )`, new: a handler streaming waits here while more than `http-stream-ahead` (256 KiB) of its answer is still to
  go, so what waits for a slow client is bounded. `read-file-part ( path n from count -- s len )`, new (system): so
  many of a file's bytes from a place. 416 has its reason.

- jsondb by values, and queries written in Forth. Its words took and gave JSON's text: a query was a string of
  escaped JSON in the source (`s\" {\"where\":{\"status\":\"open\"}}"`), a value to look for went into that text
  by `format` - where a quote in what a user typed ends the string - and every answer was parsed by its caller.
  New, beside them: `db-record` `db-record-rev` `db-save` `db-save-if` `db-create` `db-where` `db-first` (a record a
  map), `db-select ( q c n db -- records total )`, and the vocabulary `json.query` - `[: s" open" s" status"
  where$=  2 s" >=" s" priority" where  s" -priority" sorted-by  0 50 page ;] query` - whose map `sql.model` takes
  too (`name-select`, `name-count-of`, `model-select`, `model-count-of`). `json-string! ( a n key n m -- )`, new: a
  text field written. The words that take text are as they were (a body sent on as it came needs no parse);
  examples/queue uses the new ones. Not changed: a query without an index still reads each record as JSON.

- A handler that keeps its work in a plain variable is warned of. A variable is every actor's on the node, and a
  handler gives way after a slice of steps and wherever it waits: what it wrote may be another's by the time it
  reads it - the oldest idiom of Forth a race here, and nothing said so. The checker now notes, for each definition,
  the variables and values it writes (`x !`, `to x`) and reads, through the words it calls and the quotations it
  has; a behaviour's handler that does both to one is warned of as the behaviour ends, and so is a route's word, a
  hook's and a live view's as it is given (`in-handler ( xt -- xt )`, new, for a library that keeps words to run in
  handlers). Not: one only written, only read, counted in (`+!`), or used inside `atomically`. `shared-variable
  name` and `x shared-value name`, new: one every actor is meant to share, never warned of. In the library it found
  http's `Date` of this second, made by whichever connection asked first with nothing keeping the others out (now
  `atomically`); the rest were tests' own records, now said shared. Apps: a warning where a handler does this - an
  error under `strict!` / `CLANKFORTH_STRICT`.

- `http.live`: a page shows what its render says, whatever is in a region. A region written only sometimes, and text
  outside every region, never reached the browser once the page had regions - it was a rule to remember ("render
  every region every time"), and nothing said when it was broken. The page now keeps a SHA-256 of what is round its
  regions' contents (the text outside them, and which regions it has), and sends the whole body when that changed;
  a region's contents alone changing sends the region, as before. A page costs 32 bytes more. `build-at ( -- b n )`,
  new: the string being built - which, and how long so far.

## 0.8.0 - 2026-11-27

Frames are a host's, not the actors'; a node of thousands of quiet actors and sockets costs next to nothing (epoll on
Linux, a completion port on Windows); a faster Forth (the native backend: runs, small definitions written into
their callers, the stack's top and a loop's index in registers). Hosts: rebuild; nothing to change in C -
`forth_round` is new in `forth.h`. A host's scripts, and apps: a script that times things in frames says `USING:
frames ;` or uses the clock, and a handler's `base` and task-variables are its own
([docs/hosts.md](docs/hosts.md)).

Faster where a profile of a request said the time went (the JSON route, a browser's headers: 57,000 -> 68,000
requests a second on one node; text written out escaped, 23 KB of it: HTML 8 times faster, JSON 29 times).

- `format` and `%n` write whole numbers themselves. Each `{}` built a printf format, read it back and ran the C
  library's machinery - three calls a number, twice over (measured, then written): 6% of a request.
- `s-marked ( a n table -- i )`, new: where a string's first byte marked in a table of 256 is. json's strings and
  html's text find the next byte to escape with it; each looked at every byte in Forth, some 100 instructions a
  byte.
- `crlf-lines? ( a n -- flag )`, new: http's check that a request's head has lines ended CR LF and nothing else, in
  one primitive (it was a word run for each line's end).
- The native backend: words written in line that follow one another are a run - the stack's depth kept in a
  register from one to the next, what they need of the stack checked once as the run begins (each read the depth
  from the task, checked it had its items and its room, and wrote the depth back: a dozen instructions round a
  word's two or three). A word of nine operations ran 120 instructions and runs 70. And a call of another
  definition pushes where it goes on in line, not by a call of C. Loops of Forth alone are 11% to 29% faster (a
  word called for each of 8.8 MB of bytes: 108 -> 77 ms; `30 fib`: 15 -> 13 ms; ten million rounds of arithmetic on
  locals: 150 -> 130 ms); a request, a few per cent. Checked against threaded code: 240,000 random definitions run
  three ways, the same.
- The native backend writes a small definition into its callers: one straight through to its end, every word of it
  written in line (a predicate, an arithmetic helper - with its locals). No call, and its words part of the
  caller's run. The return stack is as a call leaves it, so errors name the same callers; a word defined again has
  the callers it was written into translated again. `native-inlined ( xt -- n )`, and `.native` counts them. A
  helper called for each byte: 152 -> 127 instructions a byte (250 before the runs above).
- The native backend keeps the top of the stack in registers: through a run the top two items are in `rsi` and
  `rdi` (a third pushed, the deepest goes to its cell), the stack's depth is counted as the code is written and not
  by the code (no `inc` and `dec` of it a word), the locals frame is read once, and a comparison's flag is made in
  the register it stays in. `x 3 * 7 + 1023 and to y` is four instructions on a register between a read and a write
  of the locals; `swap`, `drop` and `nip` of what is in registers are no instruction. All of it is written back
  where a run ends (a call, a branch, a word's primitive run for its error), so nothing else sees a difference.
  Instructions counted exactly (callgrind), a round of each loop: arithmetic on locals 44 -> 26 (98 before any of
  this), a helper called for each byte 127 -> 87 (250), `i i * +` 39 -> 30 (51), a call of `fib` 50 -> 46 (69).
  By the clock, on a shared machine: the locals loop 130-166 -> 38-49 ms, the helper a byte 76-91 -> 46-52 ms; a JSON
  request as a browser sends it, some 5% (most of a request is C). x86-64 only, as the backend is. Checked four
  ways now - `CLANKFORTH_STUBS_ONLY` sends each word of a run round by its primitive, the registers written back
  and read again at every one (ctest runs three suites so; `tools/native-diff.py`, 104,000 random definitions) -
  which found, before it shipped, a run's stack pointer left wrong after a primitive had run in the middle of it.
- The native backend keeps a `do` loop's index and limit in registers (`r13`, `r14`): `i` is a register, `loop`
  counts one and compares two, and `do`, `?do` and `j` are written in line (they were calls of C). The return
  stack's three cells are still written, so `j`, `leave`, `unloop`, an error's report and a word called find what
  they always did; the registers are read again after anything that may have changed them - a word called (it may
  have a loop of its own), `>r` `r>`, a locals frame, code come to by a jump from where they weren't known - so
  `i` after `>r` is still what was pushed. Callee-saved registers: the C a word calls leaves them, a task's switch
  keeps them, and a word's entry from C saves them (two pushes and two pops more for each call of a word from C;
  a word's code 16 bytes longer). Instructions a round, counted exactly: `i i * +` 30 -> 21, a helper called for
  each byte 87 -> 81, arithmetic on locals 26 -> 23; by the clock the difference is inside this machine's noise.
  Starting the tool translates the library with 3% more instructions. 60,000 random definitions, four ways, the
  same.

A review of the three changes below (rounds, frames, the completion port) found these in them:

- jsondb: a request that failed left its reason (`db-error`) kept by the asking actor for good - a string a failure
  for as long as the actor lived. It was held in a task-variable and kept with the actor; a task's own is a
  handler's now, so nothing named the last one to let it go. It is kept as long as the handler:
  `rescue-handler ( obj -- obj )`, new - out of a `with-scratch`, no further than the handler's own region.
- A script's own message named `tick` (or `frame-end`, `held-tick`) was the frame's kind under another word: not
  kept for a busy actor, as the frame's aren't. It is a kind of its own; the frame's are named apart
  (`0 kind-name` is `frames:tick`).
- A handler or task that began a wait counted in frames where frames might have come - run by `deliver` at a
  program's top, before `serve` - waited for ever under `serve`, unsaid. The first round stops it with the error
  `wait` there gives.
- `http-drain` ran the actors but no longer the tasks: a request in flight that waited on one was cut off when the
  drain's time ran out. It runs rounds, and the last of them after its last wait: what that told of is heard
  before the program goes on.
- Told to stop (SIGTERM, Ctrl-C), a node asleep with nothing to do looked up only when its wait ran out, up to a
  second on - after the main loop had gone, if `shutdown-grace!` gave it less. Whichever loop sees the signal
  wakes the rest (on Windows, the signal's own thread does).
- jsondb: `db-replica`'s wait for its leader's records was held open by anything on the node that could run (a
  handler that yields, now) and never timed out. It goes by its own handler being half way.
- net: a wait that couldn't be set up (no epoll, no completion port) left the vocabulary half made behind its
  error; it is let go first.

Frames are a host's, not the actors'. Hosts: nothing to change in C; a script that times things in frames says
`USING: frames ;` (docs/hosts.md). A server's scripts: `wait` and `await-within` are no longer in sight - use the
clock (`sleep-ms`, `await-within-ms`, `after`).

- The system's own loops - `serve`, a node's - are rounds: what can run, runs (the tasks' turns, the messages
  delivered, the handlers whose waits are over gone on), then the loop waits for what comes. Nothing in them counts
  frames or passes the time. `forth_round(f)` (forth.h) is a round for a host that serves with its own loop; `round
  ( -- ms )` and `rounds ( -- n )` for a script. `yield` there goes on at the next round (it waited for a frame, up
  to 16 ms, and kept the node waking 60 times a second).
- The words that count a host's frames are the `frames` vocabulary: `wait`, `frame`, `tick-tasks`, `tasks-paused!`,
  `actors-frame`, `actors-frames`, `await-within`, `failure-frame`, and the messages `tick`, `frame-end`, `held-tick`.
  A game host drives them as before (`forth_run_tasks`, `forth_actors_frame` each frame). Where nothing counts
  frames, `wait` and `await-within` are errors that say so - not a handler waiting for ever, nor a server quietly
  put back on a 60 Hz loop by one stray `subscribe tick`. A message of a script's own may be called `tick`.
- `deliver` goes on with a handler that yielded (it is the actors' part of a round). `http-drain` and
  `http.live.test` no longer run frames. The example web app's clock slept 60 frames between events: a second, by
  the clock, now.
- `trace-on`, `.recent`, `.failures` and `.actor` say when by the clock (ms since the actors began), not a frame
  number; `failure-ms ( n -- ms )`.

Windows waits as Linux does: for what happened, not on every socket held.

- net (Windows): the wait is a completion port's (`-DCLANKFORTH_IOCP=OFF`: `WSAPoll`, as it was). `WSAPoll` looked
  at every socket at each wake - a walk of thousands of quiet connections to learn that one had bytes. Each
  connection's read is now asked of the port (a read of no bytes, which the system ends when there is something to
  read), and the port tells those that ended. The sockets it can't tell of that way - listeners, those connecting,
  those whose bytes didn't all go - are the few still polled, an event of theirs waking the port. Not measured on
  Windows by its author (no Windows machine): Atlas's Round 16 reproducer says what it gains.
- net: `net-waits ( -- s len )` says how a build waits for its sockets ("epoll", "iocp", "poll"), and the build
  says it as it is configured; the tests check the two agree.
- net: what a handler wrote is sent at the next round's start, and only a socket that didn't take it all is asked
  about (told when it has room). An answer cost a wake to be told its socket could be written to, though it nearly
  always could. `serve`, stopping, delivers what its last wait told of before the program goes on.

A node holding thousands of idle pages (Atlas's Round 16): what an idle node does, and what a busy one does for the
idle. Measured on Linux with 5000 quiet websockets on one node: idle CPU 23.4% -> 0.3% (0.9% with `poll`, as on
Windows); requests a second on another route meanwhile 11,450 -> 59,500 (16,000 with `poll`) - what it serves with
none held.

- The loop (`serve`, a node's) sleeps when there is nothing to run: it woke every 16 ms, whatever there was to do,
  and each wake looked at every actor several times. It waits now for what comes (a socket, a timer, another
  node's message, a job done, a handler's sleep over). What a round and a delivery walk are the actors mid-handler
  and those with mail kept - two sets kept as they change - not every actor; a broadcast of a kind nobody
  subscribes to asks nobody.
- net: a round is for the connections something happened to (a word named them, bytes came, their actor died), not
  every connection six times over; a connection's slot is found without a search; an actor's connections are a list,
  closed as it dies without looking at the rest. The sockets polled are kept from round to round; on Linux the wait
  is `epoll`'s (`-DCLANKFORTH_EPOLL=OFF`: `poll`), so a wake costs what happened whatever the number held. With
  `poll` (Windows) the system still looks at every socket each wake - the rest of a round no longer does.
- An actor's task - 11 KB of stacks, 15 with what the system keeps beside it, and on Windows a fiber's committed
  stack - is its handler's now, lent as one begins and taken back as it ends (64 kept for the next): an idle actor
  is about 1 KB. 5000 websockets held (two actors each): 145 MB -> 21 MB on Linux; 2000 idle actors: 24 MB -> 2 MB.
  So a task's own - `base`, `error-message`, task-variables - is a handler's, new each time, not kept from one of an
  actor's handlers to its next (check: docs/hosts.md).
- sql.watch: the actors a watcher tells may be on other nodes (one watcher for a server's nodes). It asked whether
  each was alive as if it were its own node's - so another node's was dropped at the next round, or taken for a
  local actor with its number; one on another node is now monitored, and taken off every watch as it dies or its
  node ends. A node's db tells such a watcher of its writes (`watcher-address db >>watcher`).
- sql.watch: a round of several requests sends them at once, on as many of the pool's connections as it has - one
  after another they cost a round trip each - and is over when all have answered. sql: `sql-reply-all ( xt db --
  replies )`, `sql-query-all` with each request's whole reply and 0 for one that failed, nothing thrown.
- A node asleep is woken by `stop-node`, and `bye` ends its loop at once (with the loop no longer waking every
  16 ms, they would have waited for it to look up).
- net: timers are a heap - set, cancelled and found due in the same time whether ten or a hundred thousand are set
  (each was a search of all of them, and every round read them all twice). They fire the soonest first, two due
  together in the order set.
- net: `after-together ( ms step -- timer )` - `after`, at the clock's next multiple of `step`: timers so set fire
  together. `http.websocket` pings with it: a node's sockets are pinged a thirty-second of them at a time, so 5000
  held wake it some once a second for pings, not 167 times.

## 0.7.1 - 2026-11-25

C libraries called from Forth; a sweep for bugs; a name two vocabularies have. Hosts: nothing to change - `forth_c`
is new in `forth.h`, for a host that wants it. Hosts and apps: check a bare name that two vocabularies a file uses
both have (below; [docs/hosts.md](docs/hosts.md)).

- Two vocabularies may each define a word of one name: a file defining one named as a used vocabulary's is no longer
  warned (`x hides vocab:x` - an error in strict mode, so a library that gained a word broke a program that had one
  of that name). Its own is the one its bare name means, as before. What is warned of now is ambiguity: a bare name
  that two vocabularies the file uses both have, at each use (`split is ambiguous here: strings:split and
  text:split are both in sight (text:split taken, the later in USING:)`) - whatever their stack effects (it was said
  only when they differed). `vocab:word` says which, and is never warned of. Hosts and apps: check - a file relying
  on "the later in USING: wins" for a bare name is now told at each use.

A sweep for bugs, with the tools: gcc's analyzer, valgrind, the suites under the sanitizers threaded and with every
word called, and with slices of a few steps.

- jsondb: a database asked from the interpreter (the prompt, a repl's line, a word run as the program stops) while
  a handler of its own was half way - a write waiting for its fsync, its slice spent - was served through it: both
  wrote what was pending (each line in the log twice, from two threads at once) and answered the same waiters. It
  is served once that handler is over (`busy? ( id -- flag )`, new in actors, says one is under way).
- Memory a burst of objects took is given back: the objects' table and a region's list of what it made stayed as
  big as the most objects ever alive at once (two million made and freed in one handler: 180 MB held for good; now
  the 4 MB of before). A table past 65,536 slots with under an eighth of them in use is made smaller as a region
  ends, a region's list past 4,096 freed.
- `dir-list` of an empty folder handed `qsort` no address (undefined in C: a sanitizer's build stopped there).
- A job's bytes that couldn't be copied (no memory) named the job after freeing it.
- The two empty files at the repository's top (l1.txt, l2.txt) are gone.
- `bench/http`: the web benchmark as it was measured for 0.7.0 - the routes for ClankForth (any number of nodes),
  Node and Bun, and `run.sh`, which asks with a bare request and with a browser's headers.
- `tests/tsan_stress.fs` calls a C library's function on the worker pool too (`SLOW-FUNCTION:`).

- The `c` vocabulary: a C library loaded and its functions called from Forth - `LIBRARY: zlib libz.so.1`, `zlib
  FUNCTION: crc32 ( crc bytes n -- uint )`. A function's arguments are the cells its stack comment counts (whole
  numbers and addresses, eight at most), its result `int`, `uint`, `cell` or none; `SLOW-FUNCTION:` makes the call
  on a worker thread while the handler waits; `c-library`, `c-symbol`, `c-function`, `z>string`. No floats,
  structures by value, variadic functions or callbacks yet. Unlike the rest of the system a mistake here is a
  crash, not an error: the tool has it, a host gives it with `forth_c(f)` (new in forth.h) to scripts it trusts
  (docs/c.md, docs/hosts.md). Hosts: nothing to change.


## 0.7.0 - 2026-11-25

A server that gives its memory back, and answers a browser's request three times as fast; sql.watch's rounds as one
request. Hosts: rebuild - `forth.h` is as it was ([docs/hosts.md](docs/hosts.md) says what is new around it). Apps:
nothing to change, but one that calls http's `parse-headers` itself (it takes the head's text now: below).

- sql.watch: a round's looks go as one request, a statement and a result set each (`sql-watch-batch`, 100, at
  most a request), not a request each awaited in turn - with many pages open a round ran past its interval and kept
  one of the pool's connections busy throughout (Atlas R15-1: a company page's 11 looks 313 ms one at a time, 28 ms
  as one, from a machine 22 ms from the server). Each watch's parameters are renamed for the request
  (`sql-suffixed`, in sql: `@id` to `@id_w2`). A look that fails fails its request: that request's looks are made
  one at a time, so only the failing one goes without - `sql-watch-retries` counts them. A look after a write
  through the db stays a request of its own. Apps: nothing to change.

A pass on speed, by what a profile of the web server said (callgrind, the machine code's cost given to its words).
`examples`-like routes on one node, `wrk` with 64 kept connections: a request as a browser sends it (eleven headers,
a cookie) 11,000 a second before, 34,700 after; a bare one 31,800, then 42,800. Hosts: nothing to change. Apps:
nothing, but one that calls http's `parse-headers` itself (below).

- http: a header name's bytes are looked up in a table, not worked out (`token-char?` walked fifteen marks for every
  byte: with `clean-head?`, most of a browser's request); a head's and a header value's CR, LF and NUL are found by
  `s-under`, not a loop over every byte; no request is made to find there is nothing more to read (one was, after
  every answer); a request's text not yet read is one shared string, not five new ones. json: a string's bytes are
  written a run at a time.
- `fields>map ( a n max m -- flag )`: a message's header fields (`name: value` lines) read into a map, in C - what
  http's `parse-headers` did in Forth for every request, and after the rest of this pass still a fifth of a
  browser's: 28,400 a second without it, 34,700 with - level with Node's `http` on the same machine (35,500). The
  same answers as before on 6,000 random request heads (status, each header's value). `parse-headers` now takes the head's text after its first line
  (`first-line`, `rest-lines`), not a list of its lines: an app calling it itself passes `a n rest-lines`.
- An object's block comes from, and goes back to, short lists of blocks by size (16 to 256 bytes, 64 kept of each
  at most: a few hundred KB a node), not the allocator each time: most objects are small and live for one handler,
  and `calloc` and `free` for each were a sixth of a request. A browser's request 33,500 a second to 38,500, a bare
  one 42,100 to 48,500 - past Node's `http` on the same machine (32,100 and 44,400). Tried first and left out: the
  object looked up last remembered (it is the next one asked for only 37 times in 100: no gain).
- `s-under ( a n c -- i )`: where a string's first byte under c is, -1 if none.
- `s-find` and `s-split` look for the first byte, then compare (they compared at every byte).
- The native backend writes in line: `@` `!` `c@` `c!` (their checks as the primitives': what is no address, the
  dictionary's last bytes, memory a host lent - the primitive's then), a word's frame of locals made and dropped,
  `within`, `true`, `false`, `0<=`, `0>=`. In a loop each was a call into C: `@` 7 ns to 4, `within` 9 to 4, a call
  of a word with locals 14 to 11. `tools/native-diff.py` runs them the three ways.
- `CLANKFORTH_CODE_MAP=file`: each definition's machine code listed there (where, how long, its name - the form
  `perf` reads), so a profile of a running system names the Forth words its time went to; `tools/word-costs.py` does
  so from callgrind's output (docs/debugging.md). How this pass found what it did.

- http: a kept connection (keep-alive - every browser's) held each request's strings and maps till it closed: 21
  objects a request, a connection's actor growing with every request it answered (64 connections, 600,000 requests:
  5 MB to 400 MB, and the objects' table with it - never given back). The request itself is still the actor's (its
  last words see it); what it holds is its scratch region's, freed as the request is answered. Hosts and apps:
  nothing to change.

- A server gives memory back: what handlers freed stayed with C's allocator, so a process hit hard stayed as large
  as its busiest moment (examples/web-app.fs after 30,000 requests, 3,000 at once, on Linux: 51 MB held with 6 MB
  in use; now 21 MB once idle). `serve` and the nodes' loops trim the allocator's free memory when they have nothing
  to run - after 4,096 objects freed, at most every 2 s (`malloc_trim` with glibc, `HeapCompact` on Windows). In
  `system`: `trim-memory ( -- )` does it at once, `process-bytes ( -- n )` is what the process holds as the system
  counts it, `memory-trims ( -- n )` the times this node has trimmed (docs/debugging.md). Hosts: nothing to change -
  a host's own loop is never trimmed for it (docs/hosts.md).

## 0.6.8 - 2026-11-25

- `ms await-within-ms kind ( -- args true | false )`: an await given up on after so many of the clock's
  milliseconds - a server's timeout, where `await-within`'s frames (16 ms each in `serve`, a game's own elsewhere)
  are a game's. The libraries' waits are the clock's now: a live page's first render, its socket's check
  (`live-check-ms`, 4 s), a jsondb request (the time left till `db-timeout-ms`). Hosts and apps: nothing to change.

## 0.6.7 - 2026-11-25

A review of 0.6.4-0.6.6. Hosts and apps: nothing to change.

- The stack checker sees a task-variable's word again (0.6.5 ran it in C, and the checker took it for a word of no
  known effect: a definition using one went unchecked) - an address, as a variable's. `see` names it.
- `.handler-times`: an actor spawned into a slot one killed mid-handler held starts with none of its time.
- CLAUDE.md: the language, actors and practice in brief, for an assistant; test_system's clock test allows for
  Windows' 16 ms clock.

## 0.6.6 - 2026-11-25

- `http.live` (Atlas's R15-1): a link followed off a live page no longer lands back on it. Its socket closes as the
  page is left; live.js took that for a lost connection and reloaded - the old URL, cancelling the navigation when
  the new page was slow to come. A page being left (beforeunload, pagehide, a redirect) isn't reloaded.
- Looking into a running system: `.handler-times` - each handler that has run (behaviour, message): how often, its
  time on the node all told, on average, at most - the most first; `handler-times-reset`. `timed ( xt -- )`: a
  word's time, and the objects it left alive. Hosts and apps: nothing to change.

## 0.6.5 - 2026-11-25

A performance and memory sweep - simple wins, measured. Hosts and apps: nothing to change.

- `json>` 3x faster (a 50 KB document: 9.1 ms, now 3.1): its strings, blanks and digits read a run at a time
  (`scan-plain`, `skip-blanks`, `scan-digits`, C), and -
- `constant`, `value` and `task-variable` run in C: their `does>` code (`@`, `@ (task-var)`) recognised as it is
  made, so machine code calls C at once instead of a trip through the inner interpreter at each use.
- A connection's buffers begin at 4 KB and double as they need (were 64 KB), and are let go once empty: an idle
  keep-alive connection 18 KB, was 35 KB.
- Measured and left: an actor that has run a handler about 12 KB (its stacks); HTTP about 50,000 requests a second
  on one node, the load generator the limit.

## 0.6.4 - 2026-11-24

- `http.live` (Atlas's R14-1): a focused field the page fills from what was typed - a search box redrawn as its
  region comes back - keeps what is in it and its cursor. live.js remembers what each field sent; a value drawn that
  is one of those (an echo, a step behind) or what is in the field now leaves it alone. One the client never sent -
  the app clearing it, setting it - still wins. (0.6.3 kept a field only if drawn as before: an echoing field lost
  its cursor, and letters typed faster than the answers came.) Hosts and apps: nothing to change.

## 0.6.3 - 2026-11-24

A review of the web libraries. Hosts and apps: nothing to change - but an app on 0.6.2 or before was leaking (below).

- **`json>` leaked**: it rescued the tree it read out of its scratch region at the root and again at each level -
  each going a region further out - so in a handler what was three levels down (two in a plain actor's) was never
  freed, the actor's death included: a signed-in user's every request (the session's record), a JSON body, a live
  page's events and payloads. It rescues the root, once. (Atlas's suspected leak.)
- **A live page leaked each message its browser sent** (the socket's string, the page's to free): let go.
- `json`: floats written with every digit they need (`f>exact`: 1700000000.123, not %g's 1.7e+09); a whole number
  of 19 digits read as one if it fits a cell; a string not UTF-8 written with U+FFFD (`utf8-clean`) - what comes out
  is JSON a browser takes. A template's hole of a float writes it so too.
- `html.templates`: every hole before a URL value's first literal byte checked as a URL (one in `{#else}`, or after
  a hole that gave nothing, got past: `javascript:`).
- `http.live`: live.js keeps what was typed in the focused field only if the server drew it as before - a field
  the server changed (cleared after a submit) keeps the server's. docs/live.md: render every region every time.
- `http.sessions`: a session written sends its cookie again (its time moved on, in the browser too - it could end
  there while still in use); a session signed out by one request isn't brought back by another's write.
- `http.websocket`: a client's close answered as RFC 6455 says (1002 for codes no peer sends or one byte, 1007 for
  a reason not UTF-8); text not UTF-8 sent with U+FFFD (a browser fails the socket for one bad byte).
- `http.oidc`: the provider's discovery and keys kept per config (two sites, two providers, on one node); a path
  to come back to may hold `: @ ! $ ' ( ) *` (`?at=10:00` no longer lost).
- `http.limits`: `rate-ok?` atomically (two handlers' counts interleaved let a burst of tries through).
- `http`: a 304 answered with no Content-Type nor Content-Length (RFC 9110). `http.client`: a Content-Length of
  decimal digits only.

## 0.6.2 - 2026-11-24

Hosts and apps: nothing to change; words added (`actors-runnable?`, compression's `crc32-more`).

- **A replica's copy, at full speed** (Atlas's R13-1): `db-replica` waited 10 ms between each slice of reading its
  leader's records - about 0.5 MB/s whatever the machine, and a store of some megabytes ran past its 10 s timeout
  ("its leader sent no records"). It delivers at once while the replica can go on (5.5 MB: 0.46 s), its timeout
  counted from the last slice. `actors-runnable? ( -- flag )` for a program's own loop.
- A leader serving a big store's copy: its lines made in slices (walked with `each`, not `map-each`, which C runs and
  no slice ends in - 3 MB of records, threaded, ran past the 20 million steps a stretch may), and their CRCs in C:
  `checksums.crc32` is compression's new `crc32-more` (gzip's own).
- `checksums.sha1` is crypto's SHA-1 (C) - no state of the node's, no `atomically` needed; the same digests.
- CI: a ThreadSanitizer job - `tests/tsan_stress.fs` runs the worker pool, nodes' messages, placement and the load
  table hard, threaded (docs/forth.md, Tests).
- README: the concurrency story as it is since 0.6 - slices, the worker pool, placement.

## 0.6.1 - 2026-11-24

Hosts: rebuild (a new kernel kind, actor-slow). Apps: nothing to change; words added.

From holt's notes (R10):
- `http.live`: `in-live-page? ( -- flag )` - the running actor is a page; false, not an error, from an actor of
  another state (it read a page's field first). `actors`: `actor-type ( id -- type )`, `kind-name ( kind -- addr len )`.
- `supervisor`: `on-actor-failed ( xt -- )` - each failure told to xt too (an app's log) without copying the
  restart policy.
- A handler that held its node (`slow:`) is told to the errors actor, `actor-slow ( id kind ms -- )` - the
  supervisor logs a JSON line, an app's own errors actor its own; stdout's line only with none. Hosts: a new kernel
  kind - rebuild.

## 0.6.0 - 2026-11-24

A minor release: Erlang's concurrency for actors - handlers sliced and taking turns, what blocks done off the node,
actors placed on the least busy node. Hosts rebuild (actors.h's kernel kinds moved by one) and opt in to slices
(`forth_set_slice`); apps check that no handler keeps its work in a plain `variable` across its words (docs/actors.md,
What a handler may keep where) - `task-variable` or `atomically` instead.

- Review fixes: a job only from the actor's own handler task (a plain task run while an actor is entered used a job
  freed under it); threaded, a job's primitive called through `execute` works where it is (it ran again as
  `execute`); `sleep-ms` a timed wait, not a worker held (sleepers held up every file read); a handler's `yield`
  waits a frame again (it spun the node); a `task-variable` defined again keeps its number (reloads used them up);
  jobs and sleeps only where handlers are sliced (a game's frame loop unchanged); the load table read under its lock.
- **Placement**: `spawn-on ( behaviour type addr len node -- addr )` - an actor made on another node (its behaviour
  and state type found there by name), and `spawn-anywhere` - on the least busy node, or here. `node-load`,
  `least-busy-node`: each node's handlers' share of its time, worked out as its loop runs. Actors.h: the kernel kind
  `placed`.

- **Work that blocks, off the node** (`src/jobs.c`): in a handler, `read-file`, `read-bytes`, `write-file`,
  `append-file`, `write-bytes`, `file-write`, `file-sync`, `dir-sync`, `shell`, `shell-output`, `sleep-ms` and
  PBKDF2 run on a pool of worker threads while the handler waits (as on an await) and the node's other actors go on -
  a jsondb fsync, a log's write, a password's hash no longer hold up a node. The same words; at the prompt and in
  plain tasks they work where they are, as before. A test that drives actors with `deliver` alone gives the workers
  time (a `1 sleep-ms` a round).

Preemption: an actor's handler runs a slice of steps, then another's - Erlang's fairness - and what it works on is
its own, so another may run between any two of its words.

- **Slices**: `forth_set_slice(f, steps)` / `slice-steps!` - a handler gives way when its steps are up and goes on in
  a later turn. The `clankforth` tool slices at `FORTH_SLICE_STEPS` (20000); off in a host that doesn't ask. A long
  handler no longer holds its node (a fast request answered in 0.5 ms while one computes, was 48 ms) nor is stopped
  at 20 million steps; about 1.5% slower alone. No slice ends in code C runs (`atomically`, `map-each`'s ...).
- `serve` and a node's loop: a frame every 16 ms; between, what came delivered and the handlers that gave way going
  on - no sleep while one can. `forth_actors_go_on`, `forth_actors_runnable` (actors.h) for a host's own loop.
  `deliver` gives each handler that gave way a slice more first. `slow:` says a turn that held its node in a stretch
  that can't give way.

- `task-variable name`: a variable each task (each actor's handlers) has its own cell of. `[: ... ;] atomically`:
  code with no other task between its words (no wait inside), for what the node's actors share. The checker follows
  both.
- Each task's own, no longer the node's: `base` (an actor's `hex` changes no one else's numbers), `error-message`
  (another's error since a `catch` isn't its), `n>str`'s digits and `$clear $+ $@`'s string.
- The libraries' state per task: `json`'s parser, `http.websocket`'s frame reader, `sql.tds`'s reply reader,
  `sql.model`'s query being built, `sql`'s `sql-also`, `http.client`'s URL, `http.live`'s render capture;
  atomically: `checksums.sha1`, `sql.tds`'s decimals, `html.templates`' compiling, `jsondb`'s request tags,
  `http.ops`' slowest, `http.oidc`'s keys and `azure.identity`'s tokens read (a copy: another may free the kept one).
- `jsondb`'s `db-error` is the asking handler's own (another's failure since isn't it). A test reading it at the
  prompt after a handler's request reads it in the handler instead.

## 0.5.10 - 2026-11-24

A security sweep: the runtime's own surface (the repl, machine code's memory, crypto) and what the libraries read
from others (a SQL server's replies, an HTTP server's, a browser's sockets and uploads, templates).

- **The repl and web pages**: a page open in the machine's browser could POST to the repl's port (`text/plain`
  needs no preflight) and its body ran, line by line, as Forth. A session sending a line of HTTP (`... HTTP/1.1`, a
  `Host:` header) is ended before that line runs. docs/debugging.md says what the repl's lack of a password means.
- **Machine code W^X**: the native backend's memory is never writable and runnable at once - run and read only,
  made writable for the moment a word (or reload's jump) is written. A system refusing that leaves the word
  threaded.
- **What a script can do** (docs/hosts.md): a script has the whole power of its program - leaving out `system`
  takes away words, not that; scripts from strangers (a game's mods) need a process of their own.
- `html.templates`: a hole's place read as a browser reads the HTML. A URL attribute's hole is checked however it
  is written (`href = "{u}"`, `href=" {u}"`, `data=`, `cite=` ...); a hole escaping can't make safe - an unquoted
  value, a tag, `on...=`, `style=`, `srcdoc=`, inside `<script>` or `<style>` - is refused as the template compiles.
  A template with one of those no longer compiles: quote the value, or give a script its data in a `data-` attribute.
- `http.websocket`: a handshake whose `Origin` is another site's refused (403) - a page elsewhere opened sockets
  with the user's cookies. `ws-allow-origin ( a n -- )` allows one. A site whose sockets other sites' pages open
  adds `ws-allow-origin`. A socket closing itself in a message's last frame (bad UTF-8) no longer fails after.
- `http.client`: a URL with a space, control byte or DEL refused (a CR LF in one wrote headers, or a second
  request, of its own); a method of letters only; the caller's headers with a CR or LF not ending a line refused.
  At most 16 informational (1xx) answers before the answer.
- `sql.tds`: a value shorter than its type (a guid of 1 byte) refused - it was read past the reply; a time's scale
  past 7 refused; a row of no columns refused; a parameter's name letters, digits and `_` only. `sql`: a server
  that routes again, once routed, refused.
- `http.files`: a hidden file or folder (`.git/`, `.env`) not served; `.well-known/` is.
- `http.uploads`: a body with one byte before its first delimiter no longer read a byte before the body.
- `azure.identity`: tokens kept per identity (`identity-client-id`), not one for all; the token given a copy (a
  refresh no longer frees what a caller holds).
- **A return stack of its own size**: `RSTACK_CELLS` (1024; the data stack's `STACK_CELLS` stays 256) - JSON 32
  deep is read, copied, written (`>json` ran out at 10, `json-copy` at 13) and stored, threaded or machine code.
  `json-depth-max` 32 (was 128, never reached: the return stack, and objects inside objects past 32, ran out
  first). A task is 6 KB bigger. Hosts: `forth.h`'s `Task` is bigger - rebuild against it; nothing else changes.
- `crypto`: `rsa-sha256-verify` refuses a signature not less than the modulus (it did, by the arithmetic - now said).

## 0.5.9 - 2026-11-24

Hosts and apps: nothing to change (`forth.h` unchanged); words added.

- json: `json-text`, `json-int`, `json-flag` - a field read with a default, no flag to test (what an app reads of a
  row or a form most).
- http.live: `state-map:` and `state-list:` - typed page state for a row and a list (0 / an empty list till set),
  each `name!` keeping a copy of what it is given (no `json-copy` before it). `http.live.test`: a view tested without
  a browser - `live-open`, `live-do`, `live-look`, `live-region`, `live-body`, `tested-state`, `live-redirected`.
- actors: a handler's turn that holds its node past 200 ms is said (`slow: x's 'on msg' held its node n ms`);
  `slow-handler-ms!` sets when (0 never).
## 0.5.8 - 2026-11-23

Hosts and apps: nothing to change (`forth.h` unchanged).

- R8-1: `/live.js` under its hash (`/live.js?v=<crc32>`, the page's script tag) - kept a year, immutable; a new build's
  script is a new URL, so no browser runs an old one. Without it (or an old one): `no-cache`, an ETag, a 304.
- R8-2: the sql pool sent to a connection gone before its `sql-gone` came (`send: no actor` in `on sql-done-with`)
  - the pool failed, and its waiters with it. A gone connection is forgotten there now (and an idle one before it is
  lent, a waiter gone before it is told).
- R7-2: sql (and http.client) found their TLS words by name in the caller's search order - a strict-TLS connection
  failed from code without `USING: net`. By their qualified names now (`net:tls-connect-as`).
## 0.5.7 - 2026-11-23

Hosts: nothing (`forth.h` unchanged). Apps: nothing to change - strict mode is opt-in (`CLANKFORTH_STRICT=1`).

- An unknown word says the vocabularies it is in (loaded, or the library's not loaded yet): `unknown word sql-query
  (in vocabulary sql: add it to USING:)` - one fix, not a round each.
- Strict mode: `CLANKFORTH_STRICT=1` or `true strict!` - every warning an error (a stack comment the code doesn't fit
  stops the load); `strict?`. The library loads clean under it.
- live.js (R7-1): a region drawn again keeps the focused field found by its id, else its name, else its place (only
  fields with an id kept focus before). CI checks the served script parses (node --check).
- json: `json-copy ( v -- v' )` - a deep copy (bytes stay bytes), for what `>json json>` was used for; sql uses it.
## 0.5.6 - 2026-11-23

Hosts: nothing (`forth.h` unchanged). Apps: an unsafe URL in `href=` / `src=` / a template's URL hole is written `#`.

- html: `href= src= action= formaction=` check their URL (`url-attr=`, `safe-url?`): `javascript:`, `data:`,
  `vbscript:` (however spaced or cased) written `#`; the site's own, http(s), mailto, tel as they are. A template's
  hole that begins a URL attribute's value is checked the same.
- http.oidc: Entra's `common` / `organizations` - the provider's issuer with `{tenantid}` in it, the token's `tid`
  put there before it is checked (every sign-in failed "another issuer" before); the user's `tenant` kept. A token
  the callback can't take is answered 400, saying why (a 500 before).
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

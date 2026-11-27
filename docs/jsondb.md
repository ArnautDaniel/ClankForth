# jsondb: a JSON database that is an actor

A database for a website's data - users, sessions, posts - written in ClankForth on the actor kernel, the `json`
library and the `system` vocabulary's files (`basis/jsondb/`, `USING: jsondb ;`, a host that gives `system`).
Collections of JSON values by id, held in memory; every change written to a log on the disk and made to last
(fsync) before it is answered; the log folded into a snapshot as it grows.

```forth
USING: actors json json.query jsondb ;
s" data" db-open constant db                        \ the folder: made if it isn't there, read if it is
s" email" s" users" db db-unique drop               \ an index: by a field, one record a value

: sign-up ( req -- )   \ a web handler (docs/http.md): it waits for the answer, other connections go on
    {: r :}  r req-body s" users" db db-insert if  ( id n )  201 r respond
    else  db-error 409 r respond  then ;
: open-tasks ( who n -- records total )   \ a query written in Forth; what comes back, values to read
    {: wa wn :}
    [:  s" open" s" status" where$=  wa wn s" assignee" where$=  s" -priority" sorted-by  0 50 page ;] query
    s" tasks" db db-select ;
: claim ( who n id n -- claimed? )   \ a record read, changed and written back - unless someone was first
    {: wa wn ia in | rev :}
    s" tasks" ia in db db-record-rev 0= if  false exit  then  to rev  {: task :}
    wa wn s" assignee" task json-string!
    task rev s" tasks" ia in db db-save-if dup if  nip  then ;
```

Two ways to say a record, a query and an answer. **As values** (below, By values): a record is a map, read with
`json-string@` and its kin and written with `s-at!` and `json-string!`; a query is written with `json.query`'s
words; what comes back is ready to read. This is the way for a handler that works on its data. **As JSON's text**
(Asking, next): what the words underneath take and give - for a request's body sent on as it came, a record written
back as it was read, an answer sent to a browser as it is.

## What it promises

- **A write is answered once it is on the disk**: its line written to the log and `file-sync`ed (fdatasync; on macOS
  F_FULLFSYNC, past the drive's cache). A crash, of the program or of the machine, loses no write that was answered.
- **Writes that come together are synced together** (group commit): the first write after a sync sends the actor a
  `db-flush`, which comes after every request already in its mailbox - so all of those go to the disk in one write
  and one fsync. A hundred writers waiting at once cost one fsync, not a hundred.
- **One change at a time**: one actor owns the data, and handles one request at a time - no locks, no torn reads.
  A read sees every write applied before it (one not yet synced, too); a write's answer comes after the sync.
- **All or nothing**: a batch's changes, or one change's (its record and its indexes), are made together or not at
  all - a failure partway takes back what it had done - and are one line of the log, so read back together.
- **A torn write is found**: every line has its CRC-32. A last line that isn't whole - no newline, or its CRC wrong:
  the write a crash cut short - is left out as it is read (said), and the log written clean again. A line that isn't
  whole anywhere else, or in the snapshot, means the file is damaged: `db-open` fails, saying the line, rather than
  open less than was written.
- **A write to the disk that fails** (the disk full, an I/O error) leaves the disk behind what is held: the database
  takes no more writes from then, each saying why (`db-error`, the `broken` of `db-stats`); its reads go on. Opened
  again, it reads what the disk has.

## Asking

Each word sends the database a request (`db-ask`) and awaits its answer (`db-answer`), so it is used **from a
handler** - an actor's, a web request's - which waits while the rest of the system goes on, on this node or asking
one on another (`whereis`). From the interpreter - a program's top, the repl, a test - a database on this node is
served there and then (`enter`ed: no message); that is how a program opens it and makes its indexes.

A request that fails gives `false`, and `db-error ( -- a n )` says why. One that gets no answer in `db-timeout-ms`
(10 s) fails with "db: no answer" (its answer, if it comes later, is dropped: each request has a tag).

Records:

- `db-get ( c n id n db -- json n true | false )`, `db-get-rev ( c n id n db -- json n rev true | false )`.
- `db-put ( json n c n id n db -- rev true | false )`: made or replaced; its revision now (1 the first time). A
  record deleted and made again goes on from the revision it had (till the next compaction): a `db-put-if` that read
  it before the delete is refused, not taken.
- `db-put-if ( json n rev c n id n db -- rev' true | false )`: only if the record is at that revision (0: there is
  none yet) - read it with its revision, change it, write it back: if anyone else wrote it between, this fails and
  nothing is lost (read again, try again).
- `db-insert ( json n c n db -- id n true | false )`: a new record, its id the collection's next number ("1", "2"
  ...; one an id already in use passed over).
- `db-delete ( c n id n db -- flag )` (true if there was one), `db-delete-if ( rev c n id n db -- flag )`.
- `db-add ( n field n c n id n db -- n' true | false )`: a whole number field of the record added to (made 0 if it
  isn't there): its value now. A count - views, likes - that no one else's change can lose.

Many at once:

- `db-find ( value n field n c n db -- json n )`: the records whose field has the value - given as JSON:
  `s\" \"ann@x.org\""`, `42`, `true` - as one JSON object, their ids its keys (`{}` for none). By the field's
  index if it has one; else each record looked at.
- `db-find-one ( value n field n c n db -- id n json n true | false )`: the first such (a sign-in by email).
- `db-list ( offset limit c n db -- json n )`: so many records from the offset, in the order they were first
  written; `db-newest` the same from the last written back (a page of the newest posts). `db-count ( c n db -- n )`.
- `db-batch ( ops n db -- flag )`: changes all made or none, as a JSON array of ops - `["put", c, id, value]`,
  `["put-if", c, id, rev, value]`, `["delete", c, id]`, `["delete-if", c, id, rev]` - checked in order, each seeing
  those before it.

```forth
\ a draft published: the post written and the draft deleted, together
s\" [[\"put\",\"posts\",\"p9\",{\"by\":\"7\",\"text\":\"hi\"}],[\"delete\",\"drafts\",\"d4\"]]" db db-batch
```

Queries - records by several fields, sorted, a page of them; in the database, so a filtered queue never ships every
record:

```forth
s\" {\"where\":{\"status\":\"open\",\"priority\":{\">=\":2},\"assignee\":null},\"sort\":[\"-priority\",\"title\"],\"limit\":50}"
s" tasks" db db-query   ( -- json n )   \ {"total": 7, "records": [{"id": "t4", "rev": 3, "value": {...}}, ...]}
```

- `db-query ( query n c n db -- json n )`: the query a JSON object -
  - `where`: conditions on top-level fields, all of them true. A value: equal to it (`null`: the field null or not
    there). Or an object of operators, all of them true: `=` `!=` `<` `<=` `>` `>=` (numbers with numbers, strings
    with strings by their bytes), `in` (a list: equal to one of them), `contains` (a string field holding the text,
    ASCII case ignored), `exists` (true: there and not null; false: not).
  - `sort`: fields, `"-field"` descending; ties by the next, then the order the records were first written. Kinds
    sort none and null, false, true, numbers, strings, lists, objects.
  - `offset`, `limit`: a page of them.
- The answer: `total` (all that match, for paging), and `records` - each its `id`, its `rev` (for `db-put-if`: an
  edit that can't overwrite someone else's) and its `value`. An equality on an indexed field uses the index; the rest
  looks at each record (read as JSON) - make an index for a field most queries name.

Watching - for screens that update as the data does (docs/design/full-stack.md, live views):

- `db-watch ( c n db -- flag )`: the asking actor is told of every change to the collection from then -
  `db-changed ( c id rev json -- )` (json 0: deleted; strings its handler's), once it lasts (after the fsync), after the
  writer's answer. A batch undone tells nothing. `db-unwatch ( c n db -- flag )`. A watcher that dies is forgotten.
  From a handler.
- A replica tells its own watchers as the leader's lines come: an actor on a web node watches its node's replica, so a
  change reaches every node's watchers with one message to each node.

Indexes:

- `db-index ( field n c n db -- flag )`, `db-unique ( field n c n db -- flag )`: an index of the collection by a
  top-level field: `db-find` by that field looks it up (a field null, or not there, isn't in it: two nulls don't
  clash). Unique: no two records with one value - a write that would
  make a second fails (`db: users: another record has that email (a unique index)`); making one where two records
  share a value already fails. A record without the field isn't in it.
- **An index is kept**: made, it is a line of the log, as a record is - written and made to last before `db-index`
  answers, in the snapshot after a compaction, and there again when the database is opened: a unique one refuses
  from the first write, whether or not the program says it again. Saying it again as it is - a program's start may
  - writes nothing. `db-drop-index ( field n c n db -- flag )` lets one go (true if there was one), kept too: an
  index is made another way - unique where it wasn't - by dropping it first (`db: users: an index of email is there
  already, not unique (db-drop-index, then make it as wanted)`). `db-stats` lists each collection's.

The rest: `db-stats ( db -- json n )` (its folder, its records and indexes by collection, the log's and the
snapshot's bytes, writes, reads, syncs, the last sync's time, what waits, why it is broken) and `.db ( db -- )`
printing it; `db-compact ( db -- flag )`; `db-close ( db -- )` (what is pending synced, the log closed: no more
writes). Killed, the database's last words do the same.

Names: a collection's name or an id is 1 to 255 bytes, each shown ASCII (`!` to `~`) but `"` and `\` - so it goes
in a log line and a JSON key as it is.

## By values

A record as a map, an answer ready to read - the same requests as above, said without JSON's text. What these give
is the handler's (docs/forth.md, regions): gone when it ends, unless kept (`state!`, `rescue-actor`).

- `db-record ( c n id n db -- v true | false )`, `db-record-rev ( c n id n db -- v rev true | false )`: the record.
- `db-save ( v c n id n db -- rev true | false )`: made or replaced. `db-save-if ( v rev c n id n db -- rev' true |
  false )`: only if it is at that revision (0: there is none) - read with `db-record-rev`, change, save: someone
  else's save in between makes this one fail, and nothing is lost.
- `db-create ( v c n db -- id n true | false )`: a new record, its id the collection's next number.
- `db-where ( v field n c n db -- m )`: the records whose field has the value (a number, a string - `s" ann"
  >string drop` - `json-true` ...): a map, their ids its keys. `db-first ( v field n c n db -- id n v' true | false
  )`: the first such.
- `db-select ( q c n db -- records total )`: a query's records - a list, each a map of `"id"`, `"rev"` and
  `"value"` - and how many match in all (for paging).

**A query written in Forth** (`USING: json.query ;`): `[: ... ;] query ( -- q )` runs the words that say it and gives
the map `db-select` takes (and `sql.model`'s `name-select`: one shape for both).

- `where$= ( a n field n -- )`: the field equal to the text. `where= ( v field n -- )`: to a value (a number,
  `json-true`). `where-null ( field n -- )`: null, or not there. `where-contains ( a n field n -- )`: a string field
  holding the text (ASCII case ignored). `where-in ( l field n -- )`: equal to one of the list's (`{ " done" "
  review" }`).
- `where ( v op n field n -- )`: any operator - `=` `!=` `<` `<=` `>` `>=` `in` `contains` `exists`: `2 s" >="
  s" priority" where`. Several on one field all hold (`2 s" >=" ...  5 s" <" ...`).
- `sorted-by ( field n -- )`: by the field, `"-field"` descending; said again, ties by the next.
- `page ( offset limit -- )`.

Each value goes in as a value: text a user typed is one string in the query whatever is in it - a quote, a brace -
where JSON's text put together by hand (`format`) would let it end the string and say a condition of its own.

## Where it runs

**A folder is one opener's at a time.** `db-open` takes a lock on it (a file `lock` in it, held by the system: gone
when the database is closed or its process ends, however it ends - a crash leaves none behind). Opened again while
it is open - in this process, or by another - it is refused: `db: data: another has this database open ...`. So a
program whose nodes are processes (`as-nodes`: docs/actors.md) gives each node a folder of its own, `node-index s"
data/node-{}" format db-open`, or opens one on one node and has the others ask it.

`db-open ( dir n -- db )` makes its actor on the node that opens it, and reads its files there. An fsync waits for
the disk - a millisecond or more - and holds up that node's other actors meanwhile; a website gives the database
**a node of its own**, and its handlers on the other nodes ask it there:

```forth
\ app/db.fs - the database's node
IN: app.db
USING: actors jsondb ;
s" data" db-open constant db
s" email" s" users" db db-unique drop
db s" db" rot register-name

\ the program
s" app.db" start-node drop          \ (then, in a handler: s" db" whereis  ... db-get)
```

A message between nodes is copied (docs/actors.md, Nodes): what a handler sends and gets is its own.

## Replicas: a copy on each web node

The leader (the database `db-open` made) does every write; a **replica** on each web node holds a copy its handlers
read there - no message to another node for a read, and the reads of every node spread over every core.

```forth
\ app/web.fs - each web node's vocabulary
IN: app.web
USING: actors http jsondb ;
s" db" db-replica constant db       \ (it waits for the leader's name, then for its records)
\ ... the routes, as before: db-get, db-find, db-insert ... on db
```

- `db-replica ( name n -- db )`: a replica, here, of the database registered by that name (on any node). From the
  interpreter; it returns once it has every record the leader had - sent it whole (what was pending synced first) -
  and its indexes. The same words ask it as a leader.
- **Its reads are its copy's**; **its writes go to the leader**, and come back through it: each answered once the
  write's own lines are in the copy. So a handler that writes through its node's replica and then reads there sees its
  write (a write's answer carries the leader's line count, `lsn`; the replica holds the answer till it has that line).
- **It is sent only what lasts**: the leader streams each line to its replicas after its fsync, before it answers the
  writer. A replica never shows a write a crash could take back. Another node's replica may show a write a moment
  (the time a message takes between nodes) after the writer's answer.
- An index the leader makes after, or lets go, its replicas do too: its line comes to them as a record's does.
- Each watches the other (`monitor`): a replica that is gone the leader stops sending to; a leader gone (its node
  ended), its replicas take no more writes (`db: its leader is gone`), their reads giving what they had. There is no
  failover: start the leader again, and the web nodes (their replicas made again).
- What it costs: each replica holds every record (memory times the nodes), and its first copy is the whole database
  in one message. From the interpreter, a replica serves its reads there and then; its writes it refuses there (a
  handler's, or the leader's own words).

Measured (on the machine it was written on): four web nodes each reading 10,000 records - from the leader on its own
node, 237 ms each (the leader the bottleneck, ~170,000 reads/s in all); from a replica each, ~45 ms each (~870,000/s).
Writes through a replica ~14,600/s.

## On the disk

The folder has `snapshot.jsonl` (every record when it was last compacted) and `log.jsonl` (each change since, a line
each). A line is its CRC-32 (8 hex digits, of what follows the space), a space, and its ops, a tab between them:

```
d735bfba S users 2	P users 1 1 {"name":"ann","email":"a@x.org"}
91bd654f P users 1 2 {"name":"ann","email":"a@x.org","visits":6}
f8b9a939 P posts p1 1 {"t":"hi"}	D users 2
```

`P c id rev json` a record written (its JSON canonical: `>json` of what `json>` read - one line), `D c id` one
deleted, `S c n` a collection's next id, `V clankdb 1` (a snapshot's first line) the format. Opening reads the
snapshot, then the log - each line's ops done again, which gives the same records whether or not a line was read
before (so a crash between writing a snapshot and emptying the log loses nothing and doubles nothing).

**Compaction**: once the log is longer than `db-compact-min` (4 MiB) and than the snapshot, every record is written to
`snapshot.tmp`, synced, renamed over `snapshot.jsonl` (at once, as one step: POSIX's rename, Windows' MoveFileEx),
the folder synced (`dir-sync`), and the log emptied. The database waits while it does (a few MB: tens of ms).

The files are text a person or a model reads: `grep` the log for what happened to a record.

## How much

Measured (one node, 100 actors asking at once, on the machine it was written on): durable inserts ~12,000/s (100
writes to each fsync), reads by id ~270,000/s, finds by a unique index ~120,000/s. Opening 5,000 records (0.6 MB)
~45 ms, an index of them ~60 ms; about 1 s for each 100,000.

What it isn't: the data is in memory (it is for thousands to hundreds of thousands of records, not billions); a find
by a field without an index looks at every record; there are no queries but by one field's value (filter, sort and
join what comes back in the handler); one process opens a folder at a time (nothing stops a second: don't).

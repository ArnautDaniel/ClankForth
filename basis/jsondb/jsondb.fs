\ jsondb.fs - a JSON database that is an actor: collections of JSON values by id, held in memory, every change
\ written to a log on the disk - made to last (fsync) before it is answered - and the log folded into a snapshot as
\ it grows. docs/jsondb.md.
\
\   s" data" db-open constant db                      \ (its folder: at a program's top, or a node's vocabulary's)
\   s" email" s" users" db db-unique drop             \ an index: by a field, one record a value
\   ... in a handler:
\   s\" {\"name\":\"ann\",\"email\":\"a@x.org\"}" s" users" db db-insert  ( -- id len true | false )
\   s" users" s" 1" db db-get  ( -- json len true | false )
\
\ One actor owns it, so its changes come one at a time - no locks. Each request is a message, its answer awaited
\ (the asker's other messages kept meanwhile). Writes that come together are made to last together: one fsync for
\ all of them (group commit). A read sees every write applied; a write is answered once it is on the disk.
\
\ Replicas: `s" db" db-replica` on each web node - a copy its handlers read there, the leader's lines streamed to it
\ once they last; its writes go to the leader, each answered once its own lines are in the copy.
IN: jsondb
USING: actors system json accessors checksums.crc32 ;

4194304 value db-compact-min      \ the log folded into the snapshot once it is this long, and longer than the snapshot
10000 value db-timeout-ms          \ a request with no answer in this long fails ("db: no answer")

message db-ask ( op a b c n m tag -- )    \ to the database: a request - its op, its strings (0 none), its numbers
message db-answer ( tag ok s n lsn -- )   \ to the asker: whether it was done, a string (0 none), a number, and the
                                          \ leader's lines so far (a write: up to its own)
message db-flush ( -- )                   \ to itself: the lines written since made to last, their writers answered
message db-follow ( -- )                  \ to a leader: the sender is a replica from now
message db-records ( text lsn -- )        \ to a replica: every record, as lines - the leader's, as of line lsn
message db-lines ( text lsn -- )          \ to a replica: lines the leader made to last, the last of them lsn

<PRIVATE
1 constant op-get        2 constant op-put        3 constant op-insert     4 constant op-delete
5 constant op-add        6 constant op-find       7 constant op-find-one   8 constant op-list
9 constant op-newest     10 constant op-count     11 constant op-batch     12 constant op-index
13 constant op-compact   14 constant op-stats     15 constant op-close     16 constant op-query
17 constant op-watch     18 constant op-unwatch   19 constant op-drop-index
-1 constant any-rev      \ a write's condition: none (else the revision it must find there: 0 for no record)
-2 constant direct       \ an answer's receiver: the interpreter, served there and then (no message)
-1 constant no-limit
1048576 constant write-chunk      \ a snapshot written this much at a time
256 constant name-max    \ a collection's name or an id: 1 to 255 bytes
33 constant first-shown  127 constant past-shown   34 constant quote-char   92 constant backslash-char
32 constant space-char   9 constant tab-char       10 constant newline-char
8 constant crc-digits    \ a line: its CRC-32 in 8 hex digits, a space, its ops (a tab between them), a newline
char P constant put-kind    char D constant delete-kind    char S constant seq-kind    char V constant format-kind
char I constant index-kind  \ an index made: its line, written as a record's is - read back, it is there again
char X constant unindex-kind   \ ... and one let go
s" snapshot.jsonl" >string drop constant snapshot-name
s" snapshot.tmp" >string drop constant snapshot-temp
s" log.jsonl" >string drop constant log-name
s" lock" >string drop constant lock-name   \ (held by the one process that has the database open)
s" V clankdb 1" >string drop constant format-op    \ a snapshot's first line: the format it is in

\ ---- what it keeps ----
\ a collection: texts id -> its JSON (canonical: one line); revs id -> its revision (1 the first write); seq the next
\ id `insert` tries; indexes a list of db-idx
TUPLE: db-coll  name texts revs seq indexes ;
\ an index: by-key a value (its JSON) -> the id (unique), or a map of the ids; by-id an id -> its value's JSON
TUPLE: db-idx  field unique by-key by-id ;
\ an asker to answer later: a writer once its lines are on the disk; a replica's writer once its lines are in the copy
TUPLE: db-waiter  to tag ok s n lsn ;
\ what a change took away, put back if the batch it is in fails: its record's text (0 none) and revision
TUPLE: db-undo  coll id text rev ;
\ a change asked for: put-kind or delete-kind, the collection's name and the id (strings), the revision it must
\ find (any-rev: none), the value (put: what json> gave)
TUPLE: db-op  kind coll id rev val ;

state: db-state
    cell field my-dir          \ the folder (a string)
    cell field my-log          \ the log, open to write at its end (a system file); 0 while it isn't
    cell field my-lock         \ its folder's lock file, held (file-lock): no other process opens the folder meanwhile
    cell field my-colls        \ name -> db-coll
    cell field my-pending      \ lines written since the last sync, not yet on the disk (bytes)
    cell field my-waiting      \ their writers (a list of db-waiter)
    cell field my-flushing     \ a db-flush sent itself, not yet come
    cell field my-log-bytes
    cell field my-snap-bytes
    cell field my-broken       \ why it takes no more writes (a string): one to the disk failed, or it is closed
    cell field my-writes       cell field my-reads       cell field my-syncs
    cell field my-sync-us      \ the last sync's time
    cell field my-who          \ the request being served: its asker (direct: the interpreter), its tag
    cell field my-tag
    cell field my-why          \ why it failed (a string)
    cell field my-last         \ what the last change gave: a revision, a count
    cell field my-lsn          \ lines written (a leader's), or taken in (a replica's), since it was opened
    cell field my-followers    \ a leader's replicas (a list of addresses)
    cell field my-replica?     \ a replica: its writes the leader's, its records the leader's lines
    cell field my-leader       \ a replica's leader
    cell field my-ready        \ a replica has its leader's records
    cell field my-forwarded    \ a replica's writes sent on: its tag -> the db-waiter to answer
    cell field my-held         \ their answers waiting for their lines (a list of db-waiter)
    cell field my-next         \ the last tag it sent on
    cell field my-watchers     \ a collection's name -> the actors told of its changes (a list of addresses)
    cell field my-changes      \ the changes made since they were last told (a list of db-change)
    cell field my-recording    \ changes being made are kept, to tell (a request's, a replica's lines)
end-state

\ a change to tell its collection's watchers: the collection's name, the id, the revision, the JSON (0: deleted)
TUPLE: db-change  coll id rev text ;
message db-changed ( c id rev json -- )   \ to a watcher: a change to a collection it watches (json 0: deleted)

: keep ( obj -- obj )  rescue-actor ;
: kept-string ( a n -- s )  >string drop keep ;
: forget ( obj -- )  ?dup if  dispose  then ;
: new-string ( a n -- s )  >string drop ;

: fail ( a n -- false )  new-string my-why !  false ;   \ the request fails: why
: here? ( addr -- flag )  {: a :}  a 32 rshift 0=  a node-of node =  or ;   \ an actor of this node
\ a message can go to it: one here alive (a send to one gone here is an error); one elsewhere taken to be (one gone
\ there: a dead letter)
: reachable? ( addr -- flag )  {: a :}  a here? if  a $ffffffff and alive?  else  true  then ;
: ?copy ( s -- s' )  dup if  string new-string  then ;   \ (0 stays 0)
: why ( -- a n )  my-why @ ?dup if  string  else  s" db: failed"  then ;

\ ---- answers ----
\ an answer to the interpreter (asked from no handler, the database is served there and then: these are that way's
\ alone - no handler's asking comes by them, so said shared)
shared-variable direct-ok   shared-variable direct-s   shared-variable direct-n
: reply-to ( tag ok s n lsn who -- )   \ (an asker gone: nothing)
    {: tag ok s n lsn who :}
    who direct = if  ok direct-ok !  s if  s string new-string rescue-all  else  0  then direct-s !  n direct-n !  exit  then
    who reachable? 0= if  exit  then
    tag ok s n lsn who send db-answer ;
: answer ( ok s n -- )  {: ok s n :}  my-tag @ ok s n my-lsn @ my-who @ reply-to ;   \ s a new string, or 0
: refuse ( -- )  false why new-string 0 answer ;
\ the writer answered once what it wrote is on the disk (the db-flush that does it sent, if it isn't yet)
: answer-later ( s n -- )
    {: s n :}
    my-who @ my-tag @ true  s ?dup if  string kept-string  else  0  then  n my-lsn @  <db-waiter> keep  my-waiting @ push
    my-flushing @  my-who @ direct =  or if  exit  then
    true my-flushing !  self send db-flush ;
: answer-waiters ( ok -- )
    {: ok :}
    my-waiting @ [: {: w | s :}
        ok if  w s>> ?dup if  string new-string to s  then
        else  my-broken @ string new-string to s  then
        w tag>> ok s w n>> w lsn>> w to>> reply-to
        w s>> forget  w dispose ;] each
    my-waiting @ list-free  list keep my-waiting ! ;

\ ---- names ----
: name-ok? ( a n -- flag )   \ a collection's name or an id: 1 to 255 bytes, each shown ASCII but " and \
    {: a n :}
    n 1 name-max within 0= if  false exit  then
    n 0 ?do
        a i + c@ {: c :}
        c first-shown past-shown within 0=  c quote-char = or  c backslash-char = or if  false unloop exit  then
    loop
    true ;
: ?names ( c n id n -- flag )   \ both good (else the request fails, saying why)
    {: ca cn ia in :}
    ca cn name-ok? 0= if  ca cn s\" db: \"{s}\" is no collection's name (1 to 255 bytes, shown ASCII but \" and \\)" format fail exit  then
    ia in name-ok? 0= if  ia in s\" db: \"{s}\" is no id (1 to 255 bytes, shown ASCII but \" and \\)" format fail exit  then
    true ;

\ ---- collections, records, indexes ----
: coll? ( c n -- coll true | false )  my-colls @ s-at ;
: coll-of ( c n -- coll )   \ made if there is none
    {: a n :}
    a n coll? if  exit  then
    a n kept-string  <map> keep  <map> keep  1  list keep  <db-coll> keep  {: c :}
    c a n my-colls @ s-at!  c ;
: text? ( id n coll -- a n true | false )  texts>> s-at  dup if  drop string true  then ;
\ a record's revision, 0 for none; a deleted one's is kept negative (its tombstone) till the next compaction, so the
\ id made again goes on from it - a stale db-put-if (one that read it before the delete) is refused, not taken
: rev-of ( id n coll -- rev )  revs>> s-at 0= if  0  then  0 max ;
: rev-floor ( id n coll -- rev )  revs>> s-at 0= if  0  then  abs ;   \ the highest it has had

\ a record's value of the field as JSON (a new string)
: key-of ( text n field n -- key n true | false )
    {: ta tn fa fn | v r :}
    ta tn json> to v   \ (a field null: not in the index, as one not there - two nulls clash with no one)
    v map? if  fa fn v s-at if  dup json-null = if  drop  else  >json new-string to r  then  then  then
    v json-free
    r if  r string true  else  false  then ;
: index? ( field n coll -- index true | false )
    {: fa fn c | found :}
    c indexes>> [: {: x :}  x field>> string fa fn s= if  x to found  then ;] each
    found if  found true  else  false  then ;

: unindex ( id n coll -- )   \ the record's values taken out of the collection's indexes
    {: ia in c :}
    c indexes>> [: {: x | ids :}
        ia in x by-id>> s-at if  {: key :}
            x unique>> if  key string x by-key>> s-at if  dispose  then  key string x by-key>> s-delete-at
            else
                key string x by-key>> s-at if  to ids  ia in ids s-delete-at
                    ids map-size 0= if  key string x by-key>> s-delete-at  ids dispose  then
                then
            then
            ia in x by-id>> s-delete-at  key dispose
        then ;] each ;
: reindex ( id n text n coll -- )   \ the record's values put in the collection's indexes
    {: ia in ta tn c :}
    c indexes>> [: {: x | ids :}
        ta tn x field>> string key-of if  {: ka kn :}
            ka kn kept-string ia in x by-id>> s-at!
            x unique>> if  ia in kept-string ka kn x by-key>> s-at!
            else
                ka kn x by-key>> s-at if  to ids  else  <map> keep to ids  ids ka kn x by-key>> s-at!  then
                0 ia in ids s-at!
            then
        then ;] each ;
\ a unique index of the collection that has the value for another record: the field
: clash? ( id n text n coll -- field n true | false )
    {: ia in ta tn c | found :}
    c indexes>> [: {: x :}
        found 0=  x unique>> and if
            ta tn x field>> string key-of if  {: ka kn :}
                ka kn x by-key>> s-at if  string ia in s= 0= if  x to found  then  then
            then
        then ;] each
    found if  found field>> string true  else  false  then ;

: watched? ( coll -- flag )   \ an actor watches it
    name>> string my-watchers @ s-at if  length 0<>  else  false  then ;
: note-change ( text n rev id n coll -- )   \ kept to tell, if changes are being kept and anyone watches (text 0: gone)
    {: ta tn rev ia in c :}
    my-recording @  c watched? and 0= if  exit  then
    c name>> string kept-string  ia in kept-string  rev  ta if  ta tn kept-string  else  0  then
    <db-change> keep  my-changes @ push ;
: changes-back-to ( n -- )   \ the changes kept past the first n let go (a batch undone tells nothing)
    {: n :}
    begin  my-changes @ length n >  while
        my-changes @ pop {: x :}  x coll>> forget  x id>> forget  x text>> forget  x dispose
    repeat ;
: apply-put ( text n rev id n coll -- )   \ (checked already: a request's change, or a line read back)
    {: ta tn rev ia in c :}
    ta tn rev ia in c note-change
    ia in c unindex
    ia in c texts>> s-at if  dispose  then
    ta tn kept-string ia in c texts>> s-at!
    rev ia in c revs>> s-at!
    ia in ta tn c reindex ;
: apply-delete ( id n coll -- )
    {: ia in c :}
    ia in c texts>> s-at 0= if  exit  then  dispose
    0 0 0 ia in c note-change
    ia in c unindex
    ia in c texts>> s-delete-at
    ia in c revs>> s-at if  abs negate ia in c revs>> s-at!  then ;   \ (its tombstone: the revision it had)

\ an index let go, all it holds with it
: free-index ( x -- )
    {: x :}
    x by-key>> [: nip dispose ;] map-each   \ (an id, or a map of them)
    x by-id>> [: nip dispose ;] map-each
    x by-key>> dispose  x by-id>> dispose  x field>> dispose  x dispose ;
\ the collection's index by the field let go (false: it had none)
: forget-index ( c n field n -- found? )
    {: ca cn fa fn | co l at :}
    ca cn coll? 0= if  false exit  then  to co
    co indexes>> to l  -1 to at
    l length 0 ?do  i l nth field>> string fa fn s= if  i to at  then  loop
    at 0< if  false exit  then
    at l nth free-index
    l pop  at l length < if  at l nth!  else  drop  then   \ (the last in its place: their order is no one's)
    true ;
\ an index of the collection by the field made (there already: as unique or not as asked, or it fails)
: make-index ( c n field n unique? -- ok )
    {: ca cn fa fn unique? | co x clash :}
    ca cn coll-of to co
    fa fn co index? if
        unique>> unique? 0<> = if  true exit  then
        ca cn fa fn unique? if  s" not unique"  else  s" unique"  then
        s" db: {s}: an index of {s} is there already, {s} (db-drop-index, then make it as wanted)" format fail
        exit
    then
    fa fn kept-string  unique? 0<>  <map> keep  <map> keep  <db-idx> keep  to x
    co texts>> [: {: ia t | ids :}
        t string x field>> string key-of if  {: ka kn :}
            x unique>> if
                ka kn x by-key>> s-key? if  true to clash  else  ia string kept-string ka kn x by-key>> s-at!  then
            else
                ka kn x by-key>> s-at if  to ids  else  <map> keep to ids  ids ka kn x by-key>> s-at!  then
                0 ia string ids s-at!
            then
            ka kn kept-string ia string x by-id>> s-at!
        then ;] map-each
    clash if
        x free-index
        ca cn fa fn s" db: {s}: two records have the same {s} (a unique index can't be made)" format fail exit
    then
    x co indexes>> push  true ;

\ ---- lines ----
\ a line of the ops into b: their CRC, a space, the ops, a newline
: line-into ( a n b -- )
    {: a n b :}
    a n crc32-hex {: ha hn :}  ha hn b b-append  ha dispose
    space-char b b-push  a n b b-append  newline-char b b-push ;
: op-start ( b -- )  dup b-length if  tab-char swap b-push  else  drop  then ;   \ (a tab between ops)
: put-op ( text n rev c n id n b -- )
    {: ta tn rev ca cn ia in b :}  b op-start
    ca cn ia in rev ta tn s" P {s} {s} {} {s}" format {: s sn :}  s sn b b-append  s dispose ;
: delete-op ( c n id n b -- )
    {: ca cn ia in b :}  b op-start
    ca cn ia in s" D {s} {s}" format {: s sn :}  s sn b b-append  s dispose ;
: seq-op ( n c n b -- )
    {: n ca cn b :}  b op-start
    ca cn n s" S {s} {}" format {: s sn :}  s sn b b-append  s dispose ;
: index-op ( c n field n unique? b -- )
    {: ca cn fa fn u b :}  b op-start
    ca cn fa fn  u 0<> 1 and  s" I {s} {s} {}" format {: s sn :}  s sn b b-append  s dispose ;
: unindex-op ( c n field n b -- )
    {: ca cn fa fn b :}  b op-start
    ca cn fa fn s" X {s} {s}" format {: s sn :}  s sn b b-append  s dispose ;

\ the text up to the first space, and the rest after it ("" if there is no space)
: cut ( a n -- f fn rest rn )
    {: a n :}  a n s"  " s-find {: at :}
    at 0< if  a n  a n + 0  exit  then
    a at  a at 1+ +  n at - 1- ;
: number-of ( a n -- n )  string>number 0= abort" db: a number in its file isn't one" ;

: replay-op ( a n -- )   \ an op read back: done again as it was (no checks: they were made before it was written)
    {: a n :}
    n 0= abort" db: an empty op in its file"
    a c@ {: kind :}
    kind format-kind = if  a n format-op string s= 0= abort" db: a format this doesn't read"  exit  then
    a 2 + n 2 - 0 max cut {: ca cn ra rn :}
    kind seq-kind = if  ra rn number-of  ca cn coll-of {: c :}  c seq>> max  c swap >>seq drop  exit  then
    ra rn cut {: ia in ra2 rn2 :}
    kind delete-kind = if  ia in ca cn coll-of apply-delete  exit  then
    kind index-kind = if  ca cn ia in ra2 rn2 number-of make-index 0= abort" db: an index its file (or its leader) has can't be made"  exit  then
    kind unindex-kind = if  ca cn ia in forget-index drop  exit  then
    kind put-kind = if  ra2 rn2 cut {: va vn ta tn :}  ta tn  va vn number-of  ia in  ca cn coll-of apply-put  exit  then
    true abort" db: an op in its file of no kind this knows" ;
: replay-line ( a n -- )   \ a line's ops
    {: a n | from :}
    n 0 ?do
        a i + c@ tab-char = if  a from + i from - replay-op  i 1+ to from  then
    loop
    a from + n from - replay-op ;
\ a line whole: its CRC there, and matching what follows it
: whole? ( a n -- flag )
    {: a n :}
    n crc-digits 1+ > 0= if  false exit  then
    a crc-digits + c@ space-char <> if  false exit  then
    a crc-digits 1+ +  n crc-digits 1+ -  crc32-hex  a crc-digits s= ;

\ a file's lines done again. A last line not whole (no newline, or its CRC wrong) is a write a crash cut short: true,
\ in the log; anywhere else, or in a snapshot, the file is damaged - an error.
: replay ( a n path n log? -- torn? )
    {: a n pa pn log? | pos line# :}
    begin  pos n <  while
        line# 1+ to line#
        a pos + n pos - s\" \n" s-find {: eol :}
        eol 0< if
            log? if  true exit  then
            pa pn line# s" db: {s}: line {} has no end: the file is damaged" format  true -rot (abort")
        then
        a pos + eol whole? 0= if
            log?  pos eol + 1+ n =  and if  true exit  then
            pa pn line# s" db: {s}: line {} is spoiled (its CRC-32 doesn't match): the file is damaged" format
            true -rot (abort")
        then
        a pos + crc-digits 1+ +  eol crc-digits 1+ -  replay-line
        pos eol + 1+ to pos
    repeat
    false ;

: path-of ( name n -- s len )  {: a n :}  my-dir @ string s" /" s+  a n s+ ;

\ ---- the snapshot: every record in it, the log emptied after ----
\ every record as lines into b - the format, each collection's next id, its records, then its indexes (made in one
\ go as they are read back) - written to the file fh as they grow (fh 0: all kept in b)
: state-lines ( fh b -- )   \ (walked by keys, with each - not map-each, which C runs: a big store's lines
    {: fh b | o :}  <bytes> to o   \  made in slices, the node's other actors going on between)
    format-op string b line-into
    my-colls @ keys [: {: ca :}  ca string my-colls @ s-at drop {: c :}
        c seq>> 1 > if  o b-clear  c seq>> ca string o seq-op  o b-data b line-into  then
        c texts>> keys [: {: ia :}  ia string c texts>> s-at drop {: t :}
            o b-clear  t string  ia string c rev-of  ca string  ia string  o put-op  o b-data b line-into
            fh  b b-length write-chunk >  and if  b b-data fh file-write  b b-clear  then ;] each
        c indexes>> [: {: x :}  o b-clear  ca string x field>> string x unique>> o index-op  o b-data b line-into ;] each ;] each
    fh if  b b-data fh file-write  then  o dispose ;
\ the deleted records' revisions let go (the snapshot keeps none: opened again, they would be gone too)
: drop-tombstones ( -- )
    my-colls @ [: {: ca c | gone :}  list to gone
        c revs>> [: {: ia r :}  r 0< if  ia gone push  then ;] map-each
        gone [: string c revs>> s-delete-at ;] each  gone list-free ;] map-each ;
: compact ( -- )
    {: | fh b :}
    snapshot-temp string path-of {: tpa tpn :}
    tpa tpn file-exists? if  tpa tpn delete-file  then
    tpa tpn file-open-append to fh
    <bytes> to b  fh b state-lines  b dispose  fh file-sync  fh file-close
    tpa tpn file-size my-snap-bytes !
    tpa tpn  snapshot-name string path-of  rename-file
    my-dir @ string dir-sync   \ (the snapshot is the one there from here: the log's lines are in it)
    my-log @ file-close  0 my-log !
    log-name string path-of {: lpa lpn :}
    s" " lpa lpn write-file
    lpa lpn file-open-append my-log !
    0 my-log-bytes !
    drop-tombstones ;

\ ---- writing to the disk ----
\ what was written since the last sync written and made to last, its writers answered. A write that fails leaves the
\ disk behind what is held: it takes no more writes from then (the error said to each) - started again, it reads
\ what the disk has.
\ lines to each replica (the leader's: each line once it lasts)
\ the changes kept told each watcher of their collections (then let go)
: tell-watchers ( -- )
    my-changes @ [: {: x :}
        x coll>> string my-watchers @ s-at if
            [: {: who :}
                who reachable? if
                    x coll>> string new-string  x id>> string new-string  x rev>>
                    x text>> if  x text>> string new-string  else  0  then
                    who send db-changed
                then ;] each
        then ;] each
    0 changes-back-to ;
: to-followers ( a n -- )
    {: a n :}
    my-followers @ [: {: who :}  who reachable? if  a n new-string  my-lsn @  who send db-lines  then ;] each ;
: flush ( -- )
    {: | t0 n :}
    false my-flushing !
    my-pending @ b-length to n
    n 0= if  true answer-waiters  exit  then
    us-now to t0
    [: my-pending @ b-data my-log @ file-write  my-log @ file-sync ;] catch if
        error-message s" db: writing to the disk failed, so it takes no more writes (restart it): {s}" format
        kept-string my-broken !
        false answer-waiters
        0 changes-back-to   \ (nothing lasted: nothing to tell)
    else
        n my-log-bytes +!  1 my-syncs +!  us-now t0 - my-sync-us !
        my-pending @ b-data to-followers   \ (before the answers: a replica's writer's answer comes after its lines)
        true answer-waiters
        tell-watchers   \ (once it lasts)
    then
    my-pending @ b-clear ;
: maybe-compact ( -- )
    my-broken @ if  exit  then
    my-log-bytes @ db-compact-min >  my-log-bytes @ my-snap-bytes @ >  and 0= if  exit  then
    ['] compact catch if
        ." db: compacting failed: " error-message type cr
        my-log @ 0= if  s" db: compacting failed, the log closed: restart it" kept-string my-broken !  then
    then ;

\ ---- changes ----
: ?writable ( -- flag )  my-broken @ ?dup if  string fail exit  then  true ;
\ the text read as JSON (false, why said, if it isn't)
: parse ( a n -- v true | false )
    {: a n | v :}
    [: a n json> to v ;] catch if  error-message s" db: not JSON: {s}" format fail exit  then
    v true ;

\ a change checked and done: its line's op added to o, what it took away to u (false: not done, why said)
: run-op ( op o u -- ok )
    {: x o u | c cur :}
    x coll>> string x id>> string ?names 0= if  false exit  then
    x coll>> string coll-of to c
    x id>> string c rev-of to cur
    x rev>> any-rev <>  x rev>> cur <>  and if
        x coll>> string x id>> string cur x rev>> s" db: {s} {s} is at revision {}, not {}" format fail exit
    then
    x kind>> put-kind = if
        x val>> >json {: ta tn :}
        x id>> string ta tn c clash? if  {: fa fn :}
            x coll>> string fa fn s" db: {s}: another record has that {s} (a unique index)" format fail exit
        then
        c  x id>>  x id>> string c text? if  new-string  else  0  then  cur  <db-undo> u push
        x id>> string c rev-floor 1+ {: new :}   \ (past a deleted one's: never a revision it had before)
        ta tn new x id>> string c apply-put
        ta tn new x coll>> string x id>> string o put-op
        new my-last !
    else
        cur 0= if  true exit  then   \ (none there: nothing to do)
        c  x id>>  x id>> string c text? drop new-string  cur  <db-undo> u push
        x id>> string c apply-delete
        x coll>> string x id>> string o delete-op
        1 my-last !
    then
    true ;
: undo-all ( u -- )   \ the changes taken back, the last first
    {: u :}
    u length 0 ?do
        u length 1- i - u nth {: d :}
        d text>> if  d text>> string d rev>> d id>> string d coll>> apply-put
        else  d id>> string d coll>> apply-delete  then
    loop ;
\ the changes, all or none: their line (after what o has already) to be written. True if any was made.
: run-ops ( ops o -- ok )
    {: ops o | u mark :}  list to u  0 my-last !
    ?writable 0= if  false exit  then
    my-changes @ length to mark
    true my-recording !
    ops length 0 ?do
        i ops nth o u run-op 0= if  false my-recording !  u undo-all  mark changes-back-to  false unloop exit  then
    loop
    false my-recording !
    o b-length if  o b-data my-pending @ line-into  1 my-writes +!  1 my-lsn +!  then
    true ;
: run-one ( op -- ok )  {: x | ops :}  list to ops  x ops push  ops <bytes> run-ops ;
\ answered once on the disk; or now, if nothing was written
: done-write ( s n -- )
    {: s n :}  my-waiting @ length  my-pending @ b-length  or if  s n answer-later  else  true s n answer  then ;

: do-put ( c id text rev -- )
    {: c i t rev | v :}
    t string parse 0= if  refuse exit  then  to v
    put-kind c i rev v <db-op> run-one 0= if  refuse exit  then
    0 my-last @ done-write ;
: do-delete ( c id rev -- )
    {: c i rev :}  0 my-last !
    c string i string ?names 0= if  refuse exit  then
    c string coll? if  drop  delete-kind c i rev 0 <db-op> run-one 0= if  refuse exit  then  then
    0 my-last @ done-write ;
: free-id ( coll -- id n )   \ the next id insert gives: the collection's next number no record has
    {: c :}
    begin  c seq>> s" {}" format {: s sn :}  s sn c texts>> s-key?  while  s dispose  c c seq>> 1+ >>seq drop  repeat
    s sn ;
: do-insert ( c text -- )
    {: c t | v co o :}
    c string s" 1" ?names 0= if  refuse exit  then
    t string parse 0= if  refuse exit  then  to v
    c string coll-of to co
    co free-id {: ia in :}
    <bytes> to o  co seq>> 1+  c string  o seq-op
    put-kind c  ia in new-string  0 v <db-op> {: x :}  list {: ops :}  x ops push
    ops o run-ops 0= if  refuse exit  then
    co co seq>> 1+ >>seq drop
    ia in new-string 0 done-write ;
: integer? ( v -- flag )  object? 0= ;   \ (json> gives a whole number as a cell, all else as objects)
: do-add ( c id field delta -- )
    {: c i fl delta | co v old :}
    c string i string ?names 0= if  refuse exit  then
    c string coll? 0= if  s" db: no such record" fail drop refuse exit  then  to co
    i string co text? 0= if  s" db: no such record" fail drop refuse exit  then
    parse 0= if  refuse exit  then  to v
    v map? 0= if  s" db: the record isn't a JSON object" fail drop refuse exit  then
    fl string v s-at if  to old  else  0 to old  then
    old integer? 0= if  fl string s" db: its {s} isn't a whole number" format fail drop refuse exit  then
    old delta +  fl string v s-at!
    put-kind c i  i string co rev-of  v <db-op> run-one 0= if  refuse exit  then
    0  old delta +  done-write ;

\ ---- a batch: [["put", c, id, value], ["put-if", c, id, rev, value], ["delete", c, id], ["delete-if", c, id, rev]]
: batch-op ( e -- op true | false )
    {: e | kind n :}
    e list? 0= if  s" db: a batch's op isn't a list" fail exit  then
    e length to n
    n 3 < if  s" db: a batch's op too short" fail exit  then
    0 e nth string? 0=  1 e nth string? 0= or  2 e nth string? 0= or if
        s" db: a batch's op: its name, collection and id strings" fail exit
    then
    0 e nth string {: ka kn :}
    ka kn s" put" s=  n 4 = and if  put-kind 1 e nth 2 e nth any-rev 3 e nth <db-op> true exit  then
    ka kn s" put-if" s=  n 5 = and if  put-kind 1 e nth 2 e nth 3 e nth 4 e nth <db-op> true exit  then
    ka kn s" delete" s=  n 3 = and if  delete-kind 1 e nth 2 e nth any-rev 0 <db-op> true exit  then
    ka kn s" delete-if" s=  n 4 = and if  delete-kind 1 e nth 2 e nth 3 e nth 0 <db-op> true exit  then
    ka kn s\" db: a batch's op \"{s}\": put, put-if, delete or delete-if (with as many parts)" format fail ;
: do-batch ( text -- )
    {: t | v ops :}
    t string parse 0= if  refuse exit  then  to v
    v list? 0= if  s" db: a batch is a JSON array of ops" fail drop refuse exit  then
    list to ops
    v length 0 ?do  i v nth batch-op 0= if  refuse unloop exit  then  ops push  loop
    ops <bytes> run-ops 0= if  refuse exit  then
    0 0 done-write ;

\ ---- reads ----
: do-get ( c id -- )
    {: c i :}  1 my-reads +!
    c string i string ?names 0= if  refuse exit  then
    c string coll? 0= if  false s" db: no such record" new-string 0 answer exit  then  {: co :}
    i string co text? 0= if  false s" db: no such record" new-string 0 answer exit  then
    new-string  i string co rev-of  {: s r :}  true s r answer ;
\ the records' ids to give: those the index has for the value, or (none) those whose field has it
: matching ( key n field n coll -- l )
    {: ka kn fa fn c | l :}  list to l
    fa fn c index? if  {: x :}
        ka kn x by-key>> s-at if  {: ids :}
            x unique>> if  ids l push  else  ids [: drop l push ;] map-each  then
        then
        l exit
    then
    c texts>> [: {: ia t :}  t string fa fn key-of if  ka kn s= if  ia l push  then  then ;] map-each
    l ;
\ records as one JSON object, its keys their ids: those of ids (strings) from `from`, so many (no-limit: all) - from
\ the last back, newest?
: object-of ( ids from count newest? coll -- s len )
    {: ids from0 count newest? c | b k from :}  <bytes> to b  [char] { b b-push  from0 0 max to from
    ids length from - 0 max  count 0< if  else  count min  then  to k
    k 0 ?do
        newest? if  ids length 1- from - i -  else  from i +  then  ids nth string {: ia in :}
        ia in c text? if  {: ta tn :}
            b b-length 1 > if  [char] , b b-push  then
            quote-char b b-push  ia in b b-append  quote-char b b-push  [char] : b b-push  ta tn b b-append
        then
    loop
    [char] } b b-push  b b>string  b dispose ;
: do-find ( c field value limit one? -- )
    {: c fl val limit one? | co v key ids :}  1 my-reads +!
    c string fl string ?names 0= if  refuse exit  then
    val string parse 0= if  refuse exit  then  to v
    v >json new-string to key
    c string coll? 0= if
        one? if  false s" db: no such record" new-string 0 answer  else  true s" {}" new-string 0 answer  then  exit
    then  to co
    key string fl string co matching to ids
    one? if
        ids length 0= if  false s" db: no such record" new-string 0 answer exit  then
        0 ids nth string {: ia in :}  ia in co text? drop {: ta tn :}
        ia in ta tn s\" {s}\t{s}" format drop  ia in co rev-of  {: s r :}  true s r answer exit
    then
    ids 0 limit false co object-of drop  ids length  {: res k :}  true res k answer ;
: do-list ( c offset limit newest? -- )
    {: c from limit newest? | co ids :}  1 my-reads +!
    c string coll? 0= if  true s" {}" new-string 0 answer exit  then  to co
    co texts>> keys to ids
    ids from limit newest? co object-of drop  co texts>> map-size  {: s k :}  true s k answer ;
: do-count ( c -- )
    {: c :}  1 my-reads +!
    c string coll? if  texts>> map-size  else  0  then  {: k :}  true 0 k answer ;

\ ---- queries: records by several fields' conditions, sorted, a page of them ----
\ {"where": {field: value | {"=" "!=" "<" "<=" ">" ">=": v, "in": [..], "contains": "text", "exists": flag}, ...},
\  "sort": ["field", "-field" ...], "offset": n, "limit": n}  ->  {"total": n, "records": [{"id", "rev", "value"}]}
TUPLE: db-hit  id rec text ;   \ (rec: the record, as json> read it)
: number? ( x -- flag )  {: x :}  x object? 0=  x json-float instance? or ;
: rank ( x -- n )   \ the order kinds sort in: none and null, false, true, numbers, strings, lists, objects
    {: x :}
    x json-null = if  0 exit  then   \ (a field not there: given as null)
    x json-false = if  1 exit  then  x json-true = if  2 exit  then
    x number? if  3 exit  then  x string? if  4 exit  then  x list? if  5 exit  then  6 ;
: to-float ( x -- ) ( F: -- r )  {: x :}  x object? if  x value>>  else  x s>f  then ;
: compare-values ( x y -- -1|0|1 )
    {: x y | rx ry :}  x rank to rx  y rank to ry
    rx ry <> if  rx ry < if  -1  else  1  then  exit  then
    rx 3 = if
        x object? 0=  y object? 0= and if  x y < if  -1  else  x y > if  1  else  0  then then  exit  then
        x to-float  y to-float  fover fover f< if  fdrop fdrop -1 exit  then  f> if  1  else  0  then  exit
    then
    rx 4 = if  x string y string compare exit  then
    rx 5 >= if  x >json y >json compare exit  then
    0 ;
: same? ( x y -- flag )  compare-values 0= ;
: contains-text? ( x text -- flag )   \ (strings both; ASCII case ignored)
    {: x t :}  x string? t string? and 0= if  false exit  then
    x string s-lower  t string s-lower  s-find 0>= ;
: operator? ( spec -- flag )   \ an object of operators: {"<": 3, ...}
    {: spec | ok :}  spec map? 0= if  false exit  then  spec map-size 0= if  false exit  then  true to ok
    spec [: drop string {: ka kn :}
        ka kn s" =" s=  ka kn s" !=" s= or  ka kn s" <" s= or  ka kn s" <=" s= or  ka kn s" >" s= or
        ka kn s" >=" s= or  ka kn s" in" s= or  ka kn s" contains" s= or  ka kn s" exists" s= or
        0= if  false to ok  then ;] map-each
    ok ;
: equal-cond? ( x there? spec -- flag )   \ a plain value: equal (null: none or null)
    {: x there? spec :}
    spec json-null = if  there? 0=  x json-null = or  exit  then
    there? if  x spec same?  else  false  then ;
: ordered? ( x there? y -- flag )   \ comparable: both numbers, or both strings
    {: x there? y :}  there? 0= if  false exit  then
    x number? y number? and  x string? y string? and or ;
: op-holds? ( x there? op n v -- flag )
    {: x there? oa on v :}
    oa on s" =" s= if  x there? v equal-cond? exit  then
    oa on s" !=" s= if  x there? v equal-cond? 0= exit  then
    oa on s" in" s= if  v list? 0= if  false exit  then
        v [: {: e :}  x there? e equal-cond? ;] any? exit  then
    oa on s" contains" s= if  there? if  x v contains-text?  else  false  then  exit  then
    oa on s" exists" s= if  there?  x json-null <> and  v json-true = = exit  then
    x there? v ordered? 0= if  false exit  then
    x v compare-values {: c :}
    oa on s" <" s= if  c 0< exit  then
    oa on s" <=" s= if  c 0<= exit  then
    oa on s" >" s= if  c 0> exit  then
    c 0>= ;
: cond-holds? ( record field n spec -- flag )
    {: r fa fn spec | x there? ok :}
    r map? if  fa fn r s-at if  to x  true to there?  then  then
    spec operator? 0= if  x there? spec equal-cond? exit  then
    true to ok
    spec [: {: k v :}  ok if  x there? k string v op-holds? to ok  then ;] map-each
    ok ;
: matches? ( record where -- flag )
    {: r w | ok :}  true to ok
    w 0= if  true exit  then
    w [: {: k spec :}  ok if  r k string spec cond-holds? to ok  then ;] map-each
    ok ;
: candidates ( where coll -- ids )   \ by an equality on an indexed field, if the where has one; else every id
    {: w co | found :}
    w map? if
        w [: {: k spec :}
            found 0=  spec operator? 0= and  spec json-null <> and  spec number? 0= and if   \ (a number: 1 is 1.0 to
                                                                                      \ a scan, not to the index's text)
                k string co index? if  drop  spec >json  k string co matching to found  then
            then ;] map-each
    then
    found if  found  else  co texts>> keys  then ;
: sort-keys-cmp ( a b keys -- -1|0|1 )   \ the first key they differ by; "-field" descending
    {: a b ks | c :}
    ks [: {: k :}
        c 0= if
            k string {: ka kn :}  ka c@ [char] - = {: down :}
            down if  ka 1+ kn 1- to kn to ka  then
            a rec>> map? if  ka kn a rec>> s-at 0= if  json-null  then  else  json-null  then
            b rec>> map? if  ka kn b rec>> s-at 0= if  json-null  then  else  json-null  then
            compare-values  down if  negate  then  to c
        then ;] each
    c ;
: do-query ( c query -- )
    {: c q | qv w ks from limit co ids hits :}  1 my-reads +!
    c string s" x" ?names 0= if  refuse exit  then
    q string parse 0= if  refuse exit  then  to qv
    qv map? 0= if  s" db: a query is a JSON object" fail drop refuse exit  then
    s" where" qv s-at if  to w  w map? 0= if  s" db: a query's where is an object" fail drop refuse exit  then  then
    s" sort" qv s-at if  to ks  ks list? 0= if  s" db: a query's sort is a list of fields" fail drop refuse exit  then  then
    s" offset" qv json-int@ if  0 max  else  0  then  to from
    s" limit" qv json-int@ if  0 max  else  no-limit  then  to limit
    list to hits
    c string coll? if  to co
        w co candidates to ids
        ids [: {: id | v :}
            id string co text? if  {: ta tn :}  ta tn json> to v
                v w matches? if  id v ta tn new-string <db-hit> hits push  else  v json-free  then
            then ;] each
    then
    ks if  ks length if  hits [: ks sort-keys-cmp 0< ;] sort-by  then  then
    [:  s\" {\"total\":" %s  hits length %n  s\" ,\"records\":[" %s
        hits length from - 0 max  limit 0< if  else  limit min  then  0 ?do
            i from + hits nth {: h :}
            i if  [char] , %c  then
            s\" {\"id\":\"" %s  h id>> %  s\" \",\"rev\":" %s  h id>> string co rev-of %n
            s\" ,\"value\":" %s  h text>> %  [char] } %c
        loop
        s" ]}" %s ;] build
    true swap  hits length  answer ;

\ ---- watching: an actor told of a collection's changes ----
: do-watch ( c on? -- )
    {: c on? | ws :}
    my-who @ direct = if  s" db: a watcher is an actor (watch from a handler)" fail drop refuse exit  then
    c string s" x" ?names 0= if  refuse exit  then
    c string my-watchers @ s-at if  to ws  else  list keep to ws  ws c string my-watchers @ s-at!  then
    on? if
        my-who @ ws contains? 0= if  my-who @ ws push  my-who @ monitor  then
    else
        list {: still :}  ws [: {: who :}  who my-who @ <> if  who still push  then ;] each
        ws clear  still [: ws push ;] each
    then
    true 0 0 answer ;

\ ---- indexes, and the rest ----
\ an op's line to be written with what else is pending: made to last, and sent to the replicas, as a record's is
: line-pending ( o -- )  b-data my-pending @ line-into  1 my-writes +!  1 my-lsn +! ;
\ An index is a line of the log, as a record is: opened again, the database has it - a program that says it again
\ (as each start, before indexes were kept) is answered at once, nothing written.
: do-index ( c field unique? -- )
    {: c fl unique? | o there :}
    c string fl string ?names 0= if  refuse exit  then
    c string coll? if  fl string rot index? if  drop  true to there  then  then
    there 0= if  ?writable 0= if  refuse exit  then  then
    c string fl string unique? make-index 0= if  refuse exit  then
    there if  true 0 0 answer exit  then   \ (as asked, already)
    <bytes> to o  c string fl string unique? o index-op  o line-pending  o dispose
    0 0 done-write ;
: do-drop-index ( c field -- )   \ answered with 1 if there was one
    {: c fl | o :}
    c string fl string ?names 0= if  refuse exit  then
    ?writable 0= if  refuse exit  then
    c string fl string forget-index 0= if  true 0 0 answer exit  then
    <bytes> to o  c string fl string o unindex-op  o line-pending  o dispose
    0 1 done-write ;
: stats ( -- s len )   \ what it holds and has done, as JSON
    {: | m cs :}  <map> to m  <map> to cs
    my-dir @ string new-string s" dir" m s-at!
    my-log-bytes @ s" log_bytes" m s-at!   my-snap-bytes @ s" snapshot_bytes" m s-at!
    my-pending @ b-length s" pending_bytes" m s-at!   my-waiting @ length s" waiting" m s-at!
    my-writes @ s" writes" m s-at!   my-reads @ s" reads" m s-at!   my-syncs @ s" syncs" m s-at!
    my-sync-us @ s" last_sync_us" m s-at!
    my-broken @ ?dup if  string new-string  else  json-null  then  s" broken" m s-at!
    my-replica? @ if  s" replica"  else  s" leader"  then  new-string s" role" m s-at!
    my-lsn @ s" lsn" m s-at!   my-followers @ length s" replicas" m s-at!
    my-colls @ [: {: ca c | cm xs :}  <map> to cm  list to xs
        c texts>> map-size s" records" cm s-at!   c seq>> s" next_id" cm s-at!
        c indexes>> [: {: x :}  x field>> string  x unique>> if  s"  (unique)" s+  else  >string  then  drop xs push ;] each
        xs s" indexes" cm s-at!
        cm ca string cs s-at! ;] map-each
    cs s" collections" m s-at!
    m >json ;
: do-close ( -- )
    flush
    my-log @ ?dup if  file-close  0 my-log !  then
    my-lock @ ?dup if  file-close  0 my-lock !  then
    my-broken @ 0= if  s" db: closed" kept-string my-broken !  then
    true 0 0 answer ;
: do-compact ( -- )
    flush
    ?writable 0= if  refuse exit  then
    ['] compact catch if  error-message s" db: compacting failed: {s}" format fail drop
        my-log @ 0= if  s" db: compacting failed, the log closed: restart it" kept-string my-broken !  then
        refuse exit
    then
    true 0 0 answer ;

\ a request served: its answer sent (or, a write, sent once it is on the disk)
: serve ( op a b c n m -- )
    {: op a b c n m :}
    op case
        op-get of  a b do-get  endof
        op-put of  a b c n do-put  endof
        op-insert of  a c do-insert  endof
        op-delete of  a b n do-delete  endof
        op-add of  a b c n do-add  endof
        op-find of  a b c n false do-find  endof
        op-find-one of  a b c no-limit true do-find  endof
        op-list of  a n m false do-list  endof
        op-newest of  a n m true do-list  endof
        op-count of  a do-count  endof
        op-batch of  c do-batch  endof
        op-index of  a b n do-index  endof
        op-drop-index of  a b do-drop-index  endof
        op-compact of  do-compact  endof
        op-stats of  1 my-reads +!  true stats new-string 0 answer  endof
        op-close of  do-close  endof
        op-query of  a c do-query  endof
        op-watch of  a true do-watch  endof
        op-unwatch of  a false do-watch  endof
        s" db: no such request" fail drop refuse
    endcase ;
\ ---- a replica: its records the leader's, its writes sent to the leader ----
: write-op? ( op -- flag )
    {: op :}  op op-put =  op op-insert = or  op op-delete = or  op op-add = or  op op-batch = or
    op op-index = or  op op-drop-index = or  op op-compact = or ;
: forward ( op a b c n m -- )   \ a write sent on to the leader, its answer brought back (relay)
    {: op a b c n m | t :}
    my-who @ direct = if
        s" db: a replica sends its writes to its leader from a handler (from the interpreter: write to the leader)" fail drop
        refuse exit
    then
    ?writable 0= if  refuse exit  then
    my-leader @ reachable? 0= if  s" db: its leader is gone" fail drop refuse exit  then
    1 my-next +!  my-next @ to t
    my-who @ my-tag @ 0 0 0 0 <db-waiter> keep  t my-forwarded @ at!
    op a b c n m t my-leader @ send db-ask ;
: release ( -- )   \ the answers whose lines are in the copy now given
    {: | still :}  list to still
    my-held @ [: {: w :}
        w lsn>> my-lsn @ <= if
            w tag>> w ok>> w s>> ?copy w n>> w lsn>> w to>> reply-to  w s>> forget  w dispose
        else  w still push  then ;] each
    my-held @ list-free  still keep my-held ! ;
: relay ( tag ok s n lsn -- )   \ the leader's answer to a write sent on: to its asker, once its lines are here
    {: t ok s n l | w :}
    t my-forwarded @ at 0= if  exit  then  to w   \ (none: an answer to nothing asked)
    t my-forwarded @ delete-at
    ok  l my-lsn @ >  and if
        w ok >>ok drop  w s ?copy keep >>s drop  w n >>n drop  w l >>lsn drop  w my-held @ push  exit
    then
    w tag>> ok s n l w to>> reply-to  w dispose ;
: take-lines ( text lsn -- )   \ the leader's lines done in the copy, its watchers told
    {: t l :}
    true my-recording !  t string s" its leader's lines" false replay drop  false my-recording !
    l my-lsn !  tell-watchers  release ;
: take-state ( text lsn -- )
    {: t l :}  t string s" its leader's records" false replay drop  l my-lsn !  true my-ready !  release ;
: leader-gone ( -- )   \ writes refused from here; what was sent on answered (those held: done, though not here)
    s" db: its leader is gone: it takes no writes (its reads give what it had)" kept-string my-broken !
    my-forwarded @ values {: ws :}  my-forwarded @ map-clear
    ws [: {: w :}  w tag>> false my-broken @ string new-string 0 my-lsn @ w to>> reply-to  w dispose ;] each
    -1 1 rshift my-lsn !  release ;

\ ---- a leader's replicas ----
: follow ( who -- )   \ a replica from now: sent every record (what lasts: what is pending synced first)
    {: who | b :}
    my-replica? @ if  exit  then
    who reachable? 0= if  exit  then
    flush
    <bytes> to b  0 b state-lines
    b b-data new-string my-lsn @ who send db-records  b dispose
    who my-followers @ push  who monitor ;
: unwatch-all ( id -- )   \ the actor no longer told of any collection's changes
    {: id :}
    my-watchers @ [: nip {: ws | still :}  list to still
        ws [: {: who :}  who id <> if  who still push  then ;] each
        ws clear  still [: ws push ;] each ;] map-each ;
: gone ( id -- )   \ one it watched is gone: its leader (a replica's), one of its replicas (a leader's), a watcher
    {: id | still :}
    id unwatch-all
    my-replica? @ if  id my-leader @ = if  leader-gone  then  exit  then
    list to still
    my-followers @ [: {: who :}  who id <> if  who still push  then ;] each
    my-followers @ list-free  still keep my-followers ! ;

: serve-caught ( op a b c n m -- )   \ (a mistake of its own answered, not the database's end)
    {: op a b c n m :}  0 my-why !
    my-replica? @  op write-op? and if  op a b c n m forward exit  then
    my-replica? @  my-ready @ 0= and if  s" db: a replica with no records yet" fail drop refuse exit  then
    [: op a b c n m serve ;] catch if  false  error-message s" db: {s}" format new-string  0 answer  then ;

\ its last words, killed: what was written made to last (its writers answered), the log closed
: last-words ( -- )
    my-pending @ if  ['] flush catch drop  then
    my-log @ ?dup if  file-close  0 my-log !  then
    my-lock @ ?dup if  file-close  0 my-lock !  then ;
PRIVATE>

behaviour jsondb
    on db-ask ( op a b c n m tag -- )  my-tag !  sender my-who !  serve-caught ;
    on db-flush ( -- )  flush  maybe-compact ;
    on db-follow ( -- )  sender follow ;
    on db-records ( text lsn -- )  take-state ;
    on db-lines ( text lsn -- )  take-lines ;
    on db-answer ( tag ok s n lsn -- )  relay ;
    on died ( id reason -- )  drop gone ;
    on killed ( -- )  last-words ;
end-behaviour

<PRIVATE
\ ---- opening ----
: init-state ( -- )   \ what a leader and a replica both hold, empty
    <map> keep my-colls !  <bytes> keep my-pending !  list keep my-waiting !
    list keep my-followers !  <map> keep my-forwarded !  list keep my-held !
    <map> keep my-watchers !  list keep my-changes ! ;
: load ( dir n -- )
    {: a n | torn :}
    a n kept-string my-dir !  a n make-dir
    \ one process's at a time: two writing one log would each write over what the other thought it had (a program's
    \ nodes as processes, each opening "data": the second is told here, not found out later)
    lock-name string path-of file-open-append my-lock !
    my-lock @ file-lock 0= if
        my-lock @ file-close  0 my-lock !
        true a n s" db: {s}: another has this database open - here, or another process (each node of a program: a folder of its own)" format (abort")
    then
    init-state
    snapshot-name string path-of {: pa pn :}
    pa pn file-exists? if  pa pn read-file {: ta tn :}  ta tn pa pn false replay drop  tn my-snap-bytes !  ta dispose  then
    log-name string path-of {: la ln :}
    la ln file-exists? if  la ln read-file {: xa xn :}  xa xn la ln true replay to torn  xn my-log-bytes !  xa dispose  then
    la ln file-open-append my-log !
    torn if   \ (written again without it: the log emptied, so what comes after it isn't after a broken line)
        ." db: " a n type ." : its log's last line was cut short (a write a crash stopped) - left out" cr
        compact
    then ;

\ ---- asking ----
variable tags   \ (the node's: each request's tag taken atomically)
\ the last failed request's reason - each handler its own, kept as long as the handler (a task-variable is a
\ handler's: a string kept with the actor would be there still when nothing named it); the interpreter's, till the next
task-variable error-s
: note ( a n -- )
    error-s @ ?dup if  dispose  then
    >string drop  self 0< if  rescue-all  else  rescue-handler  then  error-s ! ;
\ From the interpreter a database is served there and then, as its actor - but not through a handler of its own
\ that is half way (a write waiting for its fsync, a slice spent): that one's flush and this one's would each write
\ what is pending, and answer the same waiters. The actors' turns are run till it is over.
: unhurried ( db -- )
    {: db | deadline :}  ms-now db-timeout-ms + to deadline
    begin  db busy?  while
        ms-now deadline > abort" db: its own handler didn't finish (from the interpreter: served after it)"
        deliver  1 sleep-ms
    repeat ;
: ask ( op a b c n m db -- ok s n )
    {: op a b c n m db | tag deadline :}
    self 0< if   \ (the interpreter: a database here served there and then)
        db here? 0= abort" db: from the interpreter, only a database on this node (an answer from another is a message)"
        db unhurried
        db enter
        [:  direct my-who !  0 my-tag !  op a b c n m serve-caught  my-waiting @ length if  flush  then ;] with-scratch
        leave-actor
        direct-ok @ direct-s @ direct-n @  exit
    then
    [: 1 tags +!  tags @ ;] atomically to tag
    op a b c n m tag db send db-ask
    ms-now db-timeout-ms + to deadline
    begin   \ (the time left: an answer to a request given up on, another's tag, dropped - waited on)
        deadline ms-now - {: left :}  left 0 <= if  false s" db: no answer" new-string 0 exit  then
        left await-within-ms db-answer if  {: t ok s r l :}  t tag = if  ok s r exit  then  then
    again ;
: str ( a n -- s )  new-string ;
: ?failed ( ok s -- ok )   \ false: why noted (db-error)
    {: ok s :}  ok 0= if  s if  s string  else  s" db: failed"  then  note  then  ok ;
PRIVATE>

: db-error ( -- a n )  error-s @ ?dup if  string  else  s" "  then ;   \ why the last request that gave false failed

\ a database: its folder (made if it isn't there), read - the snapshot, then the log - into an actor of its own here;
\ the actor. From the interpreter: a program's top, or a node's vocabulary as it loads (a database on a node of its
\ own: its fsyncs hold up no one else).
: db-open ( dir n -- db )
    {: a n | id :}
    self 0< 0= abort" db-open: from the interpreter (a program's top, a node's vocabulary)"
    jsondb db-state s" db" spawn to id
    id enter  [: [: a n load ;] with-scratch ;] catch  leave-actor
    if  error-message new-string {: e :}  id kill  true e string (abort")  then
    id ;

10 constant wait-ms   \ db-replica's waits for its leader, a piece at a time
\ a replica of the database registered by that name (`register-name`, on any node), here: its handlers' reads read
\ its copy, their writes go to the leader. From the interpreter (a web node's vocabulary); it returns once it has the
\ leader's records.
: db-replica ( name n -- db )
    {: a n | leader id t0 ready :}
    self 0< 0= abort" db-replica: from the interpreter (a program's top, a node's vocabulary)"
    ms-now to t0
    begin  a n whereis to leader  leader nobody =  while
        ms-now t0 - db-timeout-ms > if  true a n s" db-replica: no database named {s}" format (abort")  then
        wait-ms node-wait drop
    repeat
    jsondb db-state s" db-replica" spawn to id
    id enter
        init-state  a n s" (a replica of {s})" format kept-string my-dir !
        true my-replica? !  leader my-leader !  leader monitor  leader send db-follow
    leave-actor
    begin  id enter  my-ready @ to ready  leave-actor  ready 0=  while
        ms-now t0 - db-timeout-ms > if  id kill  true s" db-replica: its leader sent no records" (abort")  then
        id busy? if  ms-now to t0           \ (the records come, being read a slice at a time: at once - and its
        else  wait-ms node-wait drop  then  \  time counted from the last slice: a big store isn't "no records".
                                            \  Its own handler half way, not anything on the node that could run)
        deliver
    repeat
    id ;

: db-get ( c n id n db -- json n true | false )
    {: ca cn ia in db :}  op-get ca cn str ia in str 0 0 0 db ask {: ok s r :}
    ok s ?failed if  s string true  else  false  then ;
: db-get-rev ( c n id n db -- json n rev true | false )   \ with its revision (db-put-if's)
    {: ca cn ia in db :}  op-get ca cn str ia in str 0 0 0 db ask {: ok s r :}
    ok s ?failed if  s string r true  else  false  then ;
: db-put ( json n c n id n db -- rev true | false )   \ the record made or replaced: its revision now
    {: ja jn ca cn ia in db :}  op-put ca cn str ia in str ja jn str any-rev 0 db ask {: ok s r :}
    ok s ?failed if  r true  else  false  then ;
\ only if the record is at that revision (0: there is none): a read, a change, a write - no one else's between
: db-put-if ( json n rev c n id n db -- rev' true | false )
    {: ja jn rev ca cn ia in db :}  op-put ca cn str ia in str ja jn str rev 0 db ask {: ok s r :}
    ok s ?failed if  r true  else  false  then ;
: db-insert ( json n c n db -- id n true | false )   \ a record with a new id (the collection's next number)
    {: ja jn ca cn db :}  op-insert ca cn str 0 ja jn str 0 0 db ask {: ok s r :}
    ok s ?failed if  s string true  else  false  then ;
: db-delete ( c n id n db -- flag )   \ true if there was one
    {: ca cn ia in db :}  op-delete ca cn str ia in str 0 any-rev 0 db ask {: ok s r :}
    ok s ?failed  r 0<> and ;
: db-delete-if ( rev c n id n db -- flag )   \ only if it is at that revision
    {: rev ca cn ia in db :}  op-delete ca cn str ia in str 0 rev 0 db ask {: ok s r :}
    ok s ?failed  r 0<> and ;
\ a whole number field of the record added to (made 0 if it isn't there): its value now - a count no one else's
\ change can lose
: db-add ( n field n c n id n db -- n' true | false )
    {: d fa fn ca cn ia in db :}  op-add ca cn str ia in str fa fn str d 0 db ask {: ok s r :}
    ok s ?failed if  r true  else  false  then ;
\ the records whose field has the value (JSON: s\" \"ann\"" or 42) - by its index if it has one - as one JSON object,
\ their ids its keys
: db-find ( value n field n c n db -- json n )
    {: va vn fa fn ca cn db :}  op-find ca cn str fa fn str va vn str no-limit 0 db ask {: ok s r :}
    ok s ?failed if  s string  else  s" {}"  then ;
: db-find-one ( value n field n c n db -- id n json n true | false )   \ the first such
    {: va vn fa fn ca cn db :}  op-find-one ca cn str fa fn str va vn str 0 0 db ask {: ok s r :}
    ok s ?failed 0= if  false exit  then
    s string  2dup s\" \t" s-find {: tab :}  {: sa sn :}
    sa tab  sa tab 1+ +  sn tab - 1-  true ;
\ so many records (from the offset) as one JSON object - the first written first, or the last
: db-list ( offset limit c n db -- json n )
    {: from k ca cn db :}  op-list ca cn str 0 0 from k db ask {: ok s r :}
    ok s ?failed if  s string  else  s" {}"  then ;
: db-newest ( offset limit c n db -- json n )
    {: from k ca cn db :}  op-newest ca cn str 0 0 from k db ask {: ok s r :}
    ok s ?failed if  s string  else  s" {}"  then ;
: db-count ( c n db -- n )
    {: ca cn db :}  op-count ca cn str 0 0 0 0 db ask {: ok s r :}  ok s ?failed drop  r ;
\ changes all made or none: a JSON array of [put c id value] [put-if c id rev value] [delete c id] [delete-if c id rev]
: db-batch ( ops n db -- flag )
    {: oa on db :}  op-batch 0 0 oa on str 0 0 db ask {: ok s r :}  ok s ?failed ;
\ an index of the collection by a field: its finds by it look it up. Unique: no two records with one value (a write
\ that would make two fails). Kept: written as a record is, there again when the database is opened (said again, as
\ a program's start may: nothing to do).
: db-index ( field n c n db -- flag )
    {: fa fn ca cn db :}  op-index ca cn str fa fn str 0 false 0 db ask {: ok s r :}  ok s ?failed ;
: db-unique ( field n c n db -- flag )
    {: fa fn ca cn db :}  op-index ca cn str fa fn str 0 true 0 db ask {: ok s r :}  ok s ?failed ;
\ the collection's index by the field let go (true if it had one): to make it again another way, unique or not
: db-drop-index ( field n c n db -- flag )
    {: fa fn ca cn db :}  op-drop-index ca cn str fa fn str 0 0 0 db ask {: ok s r :}  ok s ?failed  r 0<> and ;
: db-compact ( db -- flag )   \ the log folded into the snapshot now
    {: db :}  op-compact 0 0 0 0 0 db ask {: ok s r :}  ok s ?failed ;
: db-stats ( db -- json n )   \ what it holds and has done (records, indexes, bytes, syncs, why broken)
    {: db :}  op-stats 0 0 0 0 0 db ask {: ok s r :}
    ok s ?failed if  s string  else  s" {}"  then ;
: db-close ( db -- )   \ what is written made to last, the log closed: it takes no more writes
    {: db :}  op-close 0 0 0 0 0 db ask {: ok s r :}  ok s ?failed drop ;
: .db ( db -- )  db-stats type cr ;

\ records by their fields: a query (JSON: where, sort, offset, limit - docs/jsondb.md, Queries) in the collection;
\ {"total": n, "records": [{"id": ..., "rev": ..., "value": {...}}, ...]}
: db-query ( query n c n db -- json n )
    {: qa qn ca cn db :}  op-query ca cn str 0 qa qn str 0 0 db ask {: ok s r :}
    ok s ?failed if  s string  else  s\" {\"total\":0,\"records\":[]}"  then ;

\ ---- the same by values: a record a map (json's - read with json-string@, written with s-at!, json-string!), a
\ query written in Forth (json.query). What they give is the handler's: gone as it ends, unless kept (state!,
\ rescue-actor). The words above take and give JSON's text: for a body sent on as it came, a record written as read ----
\ (the text an answer came as is let go once read: from the interpreter it would be kept)
: db-record ( c n id n db -- v true | false )
    {: | ja :}  db-get if  over to ja  json>  ja dispose  true  else  false  then ;
: db-record-rev ( c n id n db -- v rev true | false )   \ with its revision (db-save-if's)
    {: | ja rev :}  db-get-rev if  to rev  over to ja  json>  ja dispose  rev true  else  false  then ;
: db-save ( v c n id n db -- rev true | false )   \ the record made or replaced: its revision now
    {: v ca cn ia in db :}  v >json {: ja jn :}  ja jn ca cn ia in db db-put  ja dispose ;
: db-save-if ( v rev c n id n db -- rev' true | false )   \ only if it is at that revision (0: there is none)
    {: v rev ca cn ia in db :}  v >json {: ja jn :}  ja jn rev ca cn ia in db db-put-if  ja dispose ;
: db-create ( v c n db -- id n true | false )   \ a record with a new id
    {: v ca cn db :}  v >json {: ja jn :}  ja jn ca cn db db-insert  ja dispose ;
\ the records whose field has the value (a number, a string, json-true ...): a map, their ids its keys
: db-where ( v field n c n db -- m )
    {: v fa fn ca cn db :}  v >json {: ja jn :}  ja jn fa fn ca cn db db-find {: ra rn :}  ra rn json>
    ja dispose  ra object? if  ra dispose  then ;   \ (none: a literal's text, no one's to let go)
: db-first ( v field n c n db -- id n v' true | false )   \ the first such: its id, the record
    {: v fa fn ca cn db :}  v >json {: ja jn :}  ja jn fa fn ca cn db db-find-one
    if  json> true  else  false  then  ja dispose ;
\ a query's records (json.query: where, sorted-by, page) - a list, each a map of "id", "rev" and "value" - and how
\ many match in all (for paging)
: db-select ( q c n db -- records total )
    {: q ca cn db :}  q >json {: qa qn :}  qa qn ca cn db db-query {: ra rn :}  ra rn json> {: r :}
    qa dispose  ra object? if  ra dispose  then
    s" records" r json-list@ 0= if  list  then  s" total" r json-int ;

\ this actor told of every change to the collection from now - db-changed ( c id rev json -- ) (json 0: deleted),
\ once it lasts; from its node's replica, as the leader's lines come. From a handler.
: db-watch ( c n db -- flag )  {: ca cn db :}  op-watch ca cn str 0 0 0 0 db ask {: ok s r :}  ok s ?failed ;
: db-unwatch ( c n db -- flag )  {: ca cn db :}  op-unwatch ca cn str 0 0 0 0 db ask {: ok s r :}  ok s ?failed ;

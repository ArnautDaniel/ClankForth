\ jsondb (basis/jsondb): a JSON database that is an actor - its records, revisions, indexes and batches; what it
\ writes read back (the log, the snapshot, a torn last line, a damaged one); asked by actors, here and on another node
IN: test-jsondb
USING: tester actors system json jsondb ;

\ ---- a folder of its own (in the build's), emptied first ----
: wipe ( dir n -- )
    {: a n :}
    a n dir? 0= if  exit  then
    a n dir-list {: l :}
    l length 0 ?do  a n s" /" s+  i l nth string s+  delete-file  loop
    a n delete-file ;
s" jsondb-test-data" >string drop constant dir
dir string wipe
: in-dir ( name n -- s len )  {: a n :}  dir string s" /" s+  a n s+ ;
dir string db-open constant db

testing records: inserted with the next id, read back, their revisions counted
T{ s\" {\"name\":\"ann\",\"email\":\"a@x.org\"}" s" users" db db-insert  -rot s" 1" s= -> true true }T
T{ s\" {\"name\":\"bob\",\"email\":\"b@x.org\"}" s" users" db db-insert  -rot s" 2" s= -> true true }T
T{ s" users" s" 1" db db-get  -rot s\" {\"name\":\"ann\",\"email\":\"a@x.org\"}" s= -> true true }T
T{ s" users" s" 9" db db-get -> false }T
T{ db-error s" db: no such record" s= -> true }T
T{ s\"  {  \"x\" : [1, 2] } " s" things" s" t1" db db-put -> 1 true }T
T{ s" things" s" t1" db db-get-rev  2swap s\" {\"x\":[1,2]}" s= -> 1 true true }T   \ (kept as canonical JSON)
T{ s" [3]" s" things" s" t1" db db-put -> 2 true }T
testing put-if: only at the revision given (0: none there yet)
T{ s" [4]" 2 s" things" s" t1" db db-put-if -> 3 true }T
T{ s" [5]" 2 s" things" s" t1" db db-put-if -> false }T
T{ db-error s" db: things t1 is at revision 3, not 2" s= -> true }T
T{ s" [6]" 0 s" things" s" t2" db db-put-if -> 1 true }T
T{ s" [7]" 0 s" things" s" t2" db db-put-if -> false }T
testing delete, delete-if
T{ 5 s" things" s" t2" db db-delete-if -> false }T
T{ 1 s" things" s" t2" db db-delete-if -> true }T
testing deleted, then made again: on from its last revision - a put-if that read it before the delete is refused
T{ s" [8]" s" things" s" t2" db db-put -> 2 true }T
T{ s" [9]" 1 s" things" s" t2" db db-put-if -> false }T   \ (read at 1, before the delete: stale)
T{ s" [9]" 2 s" things" s" t2" db db-put-if -> 3 true }T
s" things" s" t2" db db-delete drop   \ (gone again, as the test after it expects)
T{ s" things" s" t2" db db-delete -> false }T
T{ s" things" s" t2" db db-get -> false }T
testing what isn't taken: JSON that isn't, a name or an id that isn't one
T{ s" {nope" s" things" s" t3" db db-put -> false }T
T{ db-error s" db: not JSON" s-starts? -> true }T
T{ s" 1" s" has space" s" t3" db db-put -> false }T
T{ s" 1" s" things" s\" a\"b" db db-put -> false }T
T{ s" things" db db-count -> 1 }T
testing add: a whole number field counted up (made if it isn't there), not a number refused
T{ 1 s" visits" s" users" s" 1" db db-add -> 1 true }T
T{ 41 s" visits" s" users" s" 1" db db-add -> 42 true }T
T{ 1 s" name" s" users" s" 1" db db-add -> false }T
T{ 1 s" visits" s" users" s" 77" db db-add -> false }T
testing a unique index: a second record with the value refused, by insert, put and batch
T{ s" email" s" users" db db-unique -> true }T
T{ s\" {\"name\":\"eve\",\"email\":\"a@x.org\"}" s" users" db db-insert -> false }T
T{ db-error s" db: users: another record has that email (a unique index)" s= -> true }T
T{ s\" {\"email\":\"a@x.org\"}" s" users" s" 2" db db-put -> false }T
T{ s\" {\"name\":\"ann\",\"email\":\"a@x.org\",\"visits\":42}" s" users" s" 1" db db-put -> 4 true }T   \ (its own value: fine)
T{ s" name" s" users" db db-unique -> true }T
T{ s\" {\"name\":\"bob\"}" s" users" db db-insert -> false }T
T{ s\" {\"email\":null}" s" users" s" n1" db db-put nip  s\" {\"email\":null}" s" users" s" n2" db db-put nip -> true true }T   \ (null: not in the index)
T{ s" users" s" n1" db db-delete  s" users" s" n2" db db-delete -> true true }T
testing find: by an index, or by looking through; find-one
T{ s\" \"b@x.org\"" s" email" s" users" db db-find  s\" {\"2\":{\"name\":\"bob\",\"email\":\"b@x.org\"}}" s= -> true }T
T{ s\" \"ann\"" s" name" s" users" db db-find-one  drop 2swap s" 1" s=  -rot s\" \"ann\"" s-find 0> -> true true }T
T{ s" 42" s" visits" s" users" db db-find  s" {" s-starts? -> true }T
T{ s" 7" s" visits" s" users" db db-find  s" {}" s= -> true }T
T{ s\" \"x\"" s" email" s" nothing" db db-find  s" {}" s= -> true }T
T{ s\" \"z@x\"" s" email" s" users" db db-find-one -> false }T
testing list and newest: so many from an offset, oldest first or newest
variable the-db   db the-db !
: post ( i -- )  {: i :}  i s\" {{\"n\":{}}}" format  s" posts"  i s" p{}" format  the-db @ db-put 2drop ;
: posts ( n -- )  0 ?do  i post  loop ;
5 posts
T{ s" posts" db db-count -> 5 }T
T{ 1 2 s" posts" db db-list  s\" {\"p1\":{\"n\":1},\"p2\":{\"n\":2}}" s= -> true }T
T{ 0 2 s" posts" db db-newest  s\" {\"p4\":{\"n\":4},\"p3\":{\"n\":3}}" s= -> true }T
T{ 4 9 s" posts" db db-newest  s\" {\"p0\":{\"n\":0}}" s= -> true }T
T{ 9 9 s" posts" db db-list  s" {}" s= -> true }T
T{ -1 2 s" posts" db db-list  s\" {\"p0\":{\"n\":0},\"p1\":{\"n\":1}}" s=  -1 1 s" posts" db db-newest  s\" {\"p4\":{\"n\":4}}" s= -> true true }T   \ (a negative offset: 0)
testing a batch: all its changes or none (one fails: those before it taken back, indexes too)
T{ s\" [[\"put\",\"users\",\"3\",{\"name\":\"cy\",\"email\":\"c@x.org\"}],[\"delete\",\"posts\",\"p0\"]]" db db-batch -> true }T
T{ s" posts" db db-count  s" users" db db-count -> 4 3 }T
T{ s\" [[\"put\",\"users\",\"3\",{\"name\":\"cy\",\"email\":\"d@x.org\"}],[\"delete\",\"posts\",\"p1\"],[\"put-if\",\"users\",\"2\",9,{}]]" db db-batch -> false }T
T{ s" posts" db db-count -> 4 }T
T{ s\" \"c@x.org\"" s" email" s" users" db db-find-one nip nip nip nip -> true }T   \ (its index as it was)
T{ s\" \"d@x.org\"" s" email" s" users" db db-find-one -> false }T
T{ s\" [[\"frob\",\"a\",\"b\"]]" db db-batch -> false }T
T{ s" {}" db db-batch -> false }T

testing a batch that fails, what it took back as it was: a record it deleted in its place among the others (it went
testing  to the end, till the database was opened again), an id it made at no revision spent (its first real put
testing  the second)
s\" {\"k\":1}" s" order" s" x1" db db-put 2drop   s\" {\"k\":2}" s" order" s" x2" db db-put 2drop
s\" {\"k\":3}" s" order" s" x3" db db-put 2drop
: order-ids ( -- a n )  0 9 s" order" db db-list json> keys " ,"  join string ;
T{ order-ids s" x1,x2,x3" s= -> true }T
T{ s\" [[\"delete\",\"order\",\"x2\"],[\"delete\",\"order\",\"x1\"],[\"put\",\"order\",\"new\",{}],[\"put-if\",\"order\",\"zz\",99,{}]]" db db-batch -> false }T
T{ order-ids s" x1,x2,x3" s= -> true }T
T{ s" {}" s" order" s" new" db db-put -> 1 true }T
T{ s" order" s" x1" db db-get-rev drop nip nip -> 1 }T   \ (their revisions as they were)
s" order" s" new" db db-delete drop

testing what it wrote read back: opened again, the same records, revisions and next id
db db-close
T{ s" [1]" s" things" s" t9" db db-put -> false }T   \ (closed: no more writes)
dir string db-open constant db2
T{ s" users" s" 1" db2 db-get-rev  2swap 2drop -> 4 true }T
T{ s" things" s" t1" db2 db-get  -rot s" [4]" s= -> true true }T
T{ s" posts" db2 db-count  s" things" db2 db-count -> 4 1 }T
T{ s" {}" s" users" db2 db-insert  -rot s" 4" s= -> true true }T
testing an index is kept: opened again, there without being made - a unique one still refuses; said again, nothing
testing  written; made another way only once it is let go (db-drop-index: kept too)
: has-index? ( a n db -- flag )  db-stats 2swap s-find 0>= ;   \ (its name among the collections' indexes)
: log-length ( db -- n )  db-stats json> {: m :}  s" log_bytes" m json-int ;
T{ s\" \"email (unique)\"" db2 has-index?  s\" \"name (unique)\"" db2 has-index? -> true true }T
T{ s\" {\"email\":\"b@x.org\"}" s" users" db2 db-insert -> false }T
T{ db-error s" db: users: another record has that email (a unique index)" s= -> true }T
T{ db2 log-length  s" email" s" users" db2 db-unique  db2 log-length rot - -> true 0 }T
T{ s" email" s" users" db2 db-index  db-error s" there already, unique" s-find 0>= -> false true }T
T{ s" email" s" users" db2 db-drop-index  s" email" s" users" db2 db-drop-index  s\" \"email" db2 has-index? -> true false false }T
T{ s" email" s" users" db2 db-index  s\" {\"email\":\"b@x.org\"}" s" users" s" dup" db2 db-put nip -> true true }T   \ (not unique now)
T{ s" users" s" dup" db2 db-delete  s" email" s" users" db2 db-drop-index  s" email" s" users" db2 db-unique -> true true true }T

testing compaction: the log folded into the snapshot; opened again, all there
T{ db2 db-compact -> true }T
T{ s\" \"email (unique)\"" db2 has-index? -> true }T
T{ s" log.jsonl" in-dir file-size  s" snapshot.jsonl" in-dir file-size 0> -> 0 true }T
200 to db-compact-min
: more-posts ( -- )  20 5 do  i post  loop ;
db2 the-db !
more-posts
4194304 to db-compact-min
T{ s" log.jsonl" in-dir file-size 4000 < -> true }T
db2 db-close
dir string db-open constant db3
T{ s" posts" db3 db-count -> 19 }T
T{ s" users" s" 4" db3 db-get  -rot s" {}" s= -> true true }T
T{ s\" \"email (unique)\"" db3 has-index?  s\" {\"email\":\"a@x.org\"}" s" users" db3 db-insert -> true false }T   \ (from the snapshot)

testing a crash once the snapshot is written and before the log is emptied: opened again, the log's lines read over
testing  a snapshot that holds them already - nothing lost, nothing doubled, and its indexes right (a record put
testing  and deleted in the log took another's place in a unique index; an index made another way stopped the open)
s" jsondb-crash-data" >string drop constant crash-dir
: crash-file ( name n -- a n )  crash-dir string s" /" s+ 2swap s+ ;
: crashed-compacting ( db -- )   \ compacted, closed - and its log put back as it was before
    {: db :}  s" log.jsonl" crash-file read-file {: la ln :}
    db db-compact drop  db db-close  la ln s" log.jsonl" crash-file write-file  la dispose ;
crash-dir string wipe  crash-dir string db-open constant db-c1
s\" {\"email\":\"a\"}" s" users" s" 1" db-c1 db-put 2drop
s\" {\"email\":\"a\"}" s" users" s" 2" db-c1 db-put 2drop
s" users" s" 2" db-c1 db-delete drop
s" email" s" users" db-c1 db-unique drop
db-c1 crashed-compacting
crash-dir string db-open constant db-c2
: a-is-1? ( db -- flag )  {: db :}  s\" \"a\"" s" email" s" users" db db-find-one if  2drop s" 1" s=  else  false  then ;
T{ db-c2 a-is-1? -> true }T
T{ s\" {\"email\":\"a\"}" s" users" s" 3" db-c2 db-put -> false }T   \ (still unique)
db-c2 db-close  crash-dir string wipe
crash-dir string db-open constant db-c3
s" email" s" users" db-c3 db-index drop  s" email" s" users" db-c3 db-drop-index drop  s" email" s" users" db-c3 db-unique drop
db-c3 crashed-compacting
: opens? ( -- flag )  [: crash-dir string db-open db-close ;] catch 0= ;
T{ opens? -> true }T
crash-dir string wipe

testing a last line a crash cut short: left out (said), the rest read; the log written clean again
s" 1" s" things" s" t5" db3 db-put 2drop
db3 db-close
s" 0badc0de P things t6 1 {" s" log.jsonl" in-dir append-file
dir string db-open constant db4
T{ s" things" s" t5" db4 db-get nip nip -> true }T
T{ s" things" s" t6" db4 db-get -> false }T
T{ s" [2]" s" things" s" t6" db4 db-put -> 1 true }T
db4 db-close
dir string db-open constant db5
T{ s" things" s" t6" db5 db-get  -rot s" [2]" s= -> true true }T
db5 db-close

testing a damaged line not the last: the database isn't opened (the error says where)
: damage ( -- )   \ a good line, then the same changed, then a good one
    {: | b :}  <bytes> to b
    s" 00000000 P things t7 1 [1]" b b-append  10 b b-push  s" more" b b-append  10 b b-push
    b b-data s" log.jsonl" in-dir write-file  b dispose ;
damage
T{ dir string ' db-open catch nip nip -> -1 }T
T{ error-message s" line 1 is spoiled" s-find 0> -> true }T
s" " s" log.jsonl" in-dir write-file

testing a database's folder is one opener's at a time (a lock the system keeps): opened again while it is open,
testing  refused - saying so; closed, it opens (every test above that opened one again did, after db-close)
: opened-twice ( -- failed? said? )
    [: dir string db-open drop ;] catch 0<>  error-message s" has this database open" s-find 0>= ;
T{ dir string db-open constant held-db  opened-twice -> true true }T
T{ held-db db-close  dir string db-open db-close -> }T

testing a write to the disk that fails: refused from then (the error said), reads still answered
USING: jsondb.private ;
dir string db-open constant db6
T{ s" [1]" s" things" s" w1" db6 db-put -> 1 true }T
db6 enter  my-log @ file-close  leave-actor   \ (its log taken away from under it)
T{ s" [2]" s" things" s" w2" db6 db-put -> false }T
T{ db-error s" db: writing to the disk failed" s-starts? -> true }T
T{ s" [3]" s" things" s" w3" db6 db-put -> false }T
T{ s" things" s" w1" db6 db-get nip nip -> true }T
db6 enter  0 my-log !  leave-actor  db6 db-close   \ (its folder's lock let go: Windows deletes no file that is open)

testing asked by actors: answers awaited, writes that come together made to last with one fsync
message go ( xt -- )
variable got   variable done
behaviour asker  on go ( xt -- )  execute got !  1 done +! ;  end-behaviour
create asking 10 cells allot
: askers ( n -- )  0 ?do  asker -1 s" asker" spawn  asking i cells + !  loop ;
: ask-all ( xt n -- )  {: xt n :}  0 done !  n 0 ?do  xt  asking i cells + @ send go  loop ;
: settle ( n -- )  {: n :}  2000 0 do  deliver  done @ n >= if  leave  then  1 sleep-ms  loop ;   \ (a write's
                                                                                       \  fsync done on a worker: time to)
actors-reset  dir string wipe  dir string db-open constant db8
10 askers
: write-one ( -- n )  s" {}" s" hits" db8 db-insert if  2drop 1  else  0  then ;
T{ ' write-one 10 ask-all  10 settle  done @  s" hits" db8 db-count -> 10 10 }T
: stat ( name n -- v )  db8 db-stats json> s-at drop ;
T{ s" syncs" stat  s" writes" stat -> 1 10 }T   \ (ten writes, one fsync)
: read-one ( -- n )  s" hits" s" 3" db8 db-get if  2drop 1  else  0  then ;
objects constant objects-before
T{ ' read-one 10 ask-all  10 settle  done @ -> 10 }T
T{ objects objects-before - -> 0 }T   \ (each answer the asker's handler's: freed as it ends)
testing a request that fails: why (db-error) is its handler's too - an actor that asks and fails a hundred times
testing  keeps none of the reasons, and one read after a with-scratch round the request is still there
: miss-one ( -- n )  s" hits" s" none-such" db8 db-get if  2drop 0 exit  then  db-error nip 0> if  1  else  0  then ;
: misses ( times -- )  0 do  ['] miss-one 10 ask-all  10 settle  loop ;
T{ objects  10 misses  objects - -> 0 }T
: miss-in-scratch ( -- n )  [: s" hits" s" none-such" db8 db-get if  2drop  then ;] with-scratch  db-error nip 0> if  1  else  0  then ;
T{ ' miss-in-scratch 10 ask-all  10 settle  got @ -> 1 }T
testing asked from the interpreter while a write waits for its fsync: served after it, not through it - each write
testing  in the log once (the handler's flush and one from here each wrote what was pending)
s" jsondb-busy-data" wipe
s" jsondb-busy-data" db-open constant db-busy
: busy-write ( -- n )  s" {}" s" hits" db-busy db-insert if  2drop 1  else  0  then ;
: log-lines ( -- n )  s" jsondb-busy-data/log.jsonl" read-file {: s n :}  s n s\" \n" s-split {: l :}
    l length 1-  l [: dispose ;] each  l list-free  s dispose ;
: from-here-meanwhile ( -- n busy? )   \ ten writes asked, their flush under way: a count and a write from here
    ['] busy-write 10 ask-all  deliver deliver  db-busy busy?
    s" hits" db-busy db-count  s" {}" s" hits" db-busy db-insert drop 2drop  swap ;
T{ from-here-meanwhile  10 settle  s" hits" db-busy db-count  log-lines -> 10 true 11 11 }T
T{ db-busy busy? -> false }T
behaviour deaf  on db-ask ( op a b c n m tag -- )  2drop 2drop 2drop drop ;  end-behaviour   \ (asked, it never answers)
deaf -1 s" deaf" spawn constant not-a-db
\ (db-error is each handler's own: one that failed keeps it here for the test to read)
create seen-error 256 allot   variable seen-error-n
: keep-error ( -- -1 )  db-error 255 min dup seen-error-n !  seen-error swap move  -1 ;
: seen ( -- a n )  seen-error seen-error-n @ ;
: asks-dead ( -- n )  s" hits" s" 3" not-a-db db-get if  2drop 1  else  keep-error  then ;
100 to db-timeout-ms
\ (a wait for an answer goes by the clock: rounds run, time goes by)
: settle-slowly ( n -- )  {: n :}  500 0 do  1 sleep-ms  round drop  done @ n >= if  leave  then  loop ;
T{ ' asks-dead 1 ask-all  1 settle-slowly  got @ -> -1 }T
T{ seen s" db: no answer" s= -> true }T
10000 to db-timeout-ms

testing a database on a node of its own, asked from here
s" jsondb-node-data" wipe
s" vocabs.dbnode" start-node constant db-node
: found ( a n -- addr )  {: a n :}  nobody  500 0 do  drop  a n whereis  dup nobody <> if leave then  10 node-wait drop  loop ;
s" db" found constant far-db
: far-write ( -- n )  s\" {\"far\":true}" s" things" far-db db-insert if  2drop 1  else  0  then ;
: far-read ( -- n )  s" things" s" 1" far-db db-get if  s" far" s-find 0>  else  0  then ;
: far-settle ( n -- )  {: n :}  500 0 do  10 node-wait drop  deliver  done @ n >= if  leave  then  loop ;
T{ ' far-write 1 ask-all  1 far-settle  got @ -> 1 }T
T{ ' far-read 1 ask-all  1 far-settle  got @ -> -1 }T
testing a replica here of the database on that node: its writes through it, read back from it at once
s" db" db-replica constant near-db
: through ( -- n )   \ a write by the replica, then a read of it there: its own write seen
    s\" {\"via\":\"replica\"}" s" things" s" r1" near-db db-put if  drop  else  0 exit  then
    s" things" s" r1" near-db db-get if  s" replica" s-find 0>  else  0  then ;
T{ ' through 1 ask-all  1 far-settle  got @ -> -1 }T
: far-sees ( -- n )  s" things" s" r1" far-db db-get if  2drop -1  else  0  then ;
T{ ' far-sees 1 ask-all  1 far-settle  got @ -> -1 }T
db-node stop-node

testing replicas here: every record the leader had, each of its lines once it lasts, its indexes
db8 db-close  dir string wipe
dir string db-open constant leader
s\" {\"name\":\"ann\",\"email\":\"a@x.org\"}" s" users" leader db-insert 2drop drop
leader s" main-db" rot register-name
s" main-db" db-replica constant copy1
s" main-db" db-replica constant copy2
T{ s" users" s" 1" copy1 db-get  -rot s" ann" s-find 0> -> true true }T
T{ s\" {\"name\":\"bob\",\"email\":\"b@x.org\"}" s" users" leader db-insert nip nip -> true }T
deliver
T{ s" users" copy1 db-count  s" users" copy2 db-count -> 2 2 }T
T{ s" email" s" users" leader db-unique -> true }T
deliver
T{ s\" \"b@x.org\"" s" email" s" users" copy2 db-find-one nip nip nip nip -> true }T
: delivered ( -- )  20 0 do  deliver  loop ;
T{ s\" \"email (unique)\"" copy2 has-index? -> true }T                                  \ (the leader's line come to it)
T{ s" email" s" users" leader db-drop-index  delivered  s\" \"email" copy2 has-index? -> true false }T   \ (... let go, too)
T{ s" email" s" users" leader db-unique  delivered  s\" \"email (unique)\"" copy2 has-index? -> true true }T
: role-of ( db -- a n )  db-stats json> s" role" rot s-at drop string ;
T{ copy1 role-of s" replica" s=  leader role-of s" leader" s= -> true true }T
testing a replica's writes: sent to the leader (a unique index's refusal brought back); from the interpreter, refused
: via-copy ( -- n )   \ written by one replica, read at once from it
    {: | ia in :}
    s\" {\"name\":\"cy\",\"email\":\"c@x.org\"}" s" users" copy1 db-insert 0= if  0 exit  then  to in  to ia
    s" users" ia in copy1 db-get if  s" cy" s-find 0>  else  0  then ;
asker -1 s" asker" spawn  asking !
T{ ' via-copy 1 ask-all  1 settle  got @ -> -1 }T
T{ s" users" leader db-count  s" users" copy2 db-count -> 3 3 }T
: clashes ( -- n )  s\" {\"email\":\"a@x.org\"}" s" users" copy2 db-insert if  2drop 1  else  keep-error  then ;
T{ ' clashes 1 ask-all  1 settle  got @ -> -1 }T
T{ seen s" unique" s-find 0> -> true }T
T{ s" {}" s" users" s" 9" copy1 db-put -> false }T
T{ db-error s" from a handler" s-find 0> -> true }T
testing an index said again through a replica (each start says its indexes) while a write of the leader's isn't in
testing  the copy yet: answered once it is - the replica died of that answer (no text in it to keep), every request
testing  through it after refused
3 askers
: compacts ( -- n )  leader db-compact drop  1 ;                                     \ (the leader busy a moment)
: writes ( -- n )  s" {}" s" hits" leader db-insert if  2drop 1  else  0  then ;     \ (a write come meanwhile)
: says-again ( -- n )  s" email" s" users" copy1 db-unique ;
: all-three ( -- )
    0 done !  ['] compacts asking @ send go  ['] writes asking cell+ @ send go  ['] says-again asking 2 cells + @ send go ;
T{ all-three  3 settle  done @  copy1 alive? -> 3 true }T
T{ s" users" copy1 db-count -> 3 }T
testing a replica whose leader is gone: its writes refused, its reads give what it had
leader kill  deliver
: after-leader ( -- n )  s" {}" s" users" copy1 db-insert if  2drop 1  else  keep-error  then ;
T{ ' after-leader 1 ask-all  1 settle  got @ -> -1 }T
T{ seen s" leader is gone" s-find 0> -> true }T
T{ s" users" copy1 db-count -> 3 }T

testing a replica of a store of some megabytes: ready at once (its records read a slice at a time, delivered as fast
testing  as they can be - not one slice each wait for nothing)
s" jsondb-test-big" >string drop constant big-dir
big-dir string wipe
big-dir string db-open constant big
: big-record ( i -- a n )   \ (about 10 KB: {"i":n,"pad":"xxx..."})
    {: i :}  [:  s\" {\"i\":" %s  i n>str %s  s\" ,\"pad\":\"" %s  10000 0 do  [char] x %c  loop  s\" \"}" %s ;] build string ;
: fill-big ( -- )  300 0 do  i big-record  s" things" i n>str big db-put 2drop  loop ;
fill-big
big s" big-db" rot register-name
ms-now  s" big-db" db-replica constant big-copy  ms-now swap -  constant big-copy-ms
T{ big-copy-ms 2000 <  s" things" big-copy db-count -> true 300 }T
big-copy kill  big db-close  big-dir string wipe

test-summary

\ jsondb's queries (where, sort, offset, limit) and watching (db-changed to an actor for each change, once it lasts)
IN: test-jsondb-query
USING: tester actors system json json.query jsondb ;

: wipe ( dir n -- )
    {: a n :}  a n dir? 0= if  exit  then
    a n dir-list {: l :}  l length 0 ?do  a n s" /" s+  i l nth string s+  delete-file  loop  a n delete-file ;
s" jsondb-query-data" wipe
s" jsondb-query-data" db-open constant db

: task ( title n status n priority assignee|0 n id n -- )   \ (assignee "" for none)
    {: ta tn sa sn p aa an ia in | m :}  <map> to m
    ta tn >string drop s" title" m s-at!  sa sn >string drop s" status" m s-at!  p s" priority" m s-at!
    an if  aa an >string drop s" assignee" m s-at!  then
    m >json  s" tasks" ia in db db-put 2drop ;
s" Fix login"    s" open"   3 s" ann" s" t1" task
s" Write docs"   s" open"   1 s" "    s" t2" task
s" Ship release" s" done"   5 s" bob" s" t3" task
s" Fix LOGOUT"   s" open"   5 s" "    s" t4" task
s" Plan week"    s" review" 2 s" ann" s" t5" task

: ids ( query n -- a n )   \ the ids a query gives, in order, joined by spaces
    s" tasks" db db-query json> {: r :}
    s" records" r json-list@ drop [: s" id" rot json-string@ drop >string drop ;] map  " ,"  join  string ;
: total ( query n -- n )  s" tasks" db db-query json> {: r :}  s" total" r json-int@ drop ;

: loose ( priority|-99 id n -- )   \ (-99: no priority field)
    {: p ia in | m :}  <map> to m  p -99 <> if  p s" priority" m s-at!  then
    m >json  s" loose" ia in db db-put 2drop ;
2 s" l1" loose  -99 s" l2" loose  -1 s" l3" loose  0 s" l4" loose
: loose-ids ( query n -- a n )
    s" loose" db db-query json> {: r :}
    s" records" r json-list@ drop [: s" id" rot json-string@ drop >string drop ;] map  " ,"  join  string ;

testing a query written in Forth (json.query): the same answers - each value put in as a value, no JSON by hand
: picked ( q -- a n )   \ the ids it gives, in order
    s" tasks" db db-select drop  [: s" id" rot json-string@ drop >string drop ;] map  " ,"  join  string ;
T{ [: s" open" s" status" where$=  s" title" sorted-by ;] query picked s" t4,t1,t2" s= -> true }T
T{ [: 3 s" >=" s" priority" where  s" title" sorted-by ;] query picked s" t4,t1,t3" s= -> true }T
T{ [: s" assignee" where-null  s" title" sorted-by ;] query picked s" t4,t2" s= -> true }T
T{ [: { " done" " review" } s" status" where-in  s" title" sorted-by ;] query picked s" t5,t3" s= -> true }T
T{ [: s" log" s" title" where-contains  s" title" sorted-by ;] query picked s" t4,t1" s= -> true }T
T{ [: 2 s" >=" s" priority" where  5 s" <" s" priority" where  s" -priority" sorted-by ;] query picked s" t1,t5" s= -> true }T
T{ [: 3 s" priority" where=  1 s" >" s" priority" where ;] query picked s" t1" s= -> true }T   \ (equal, then more of the field)
T{ [: s" open" s" status" where$=  s" title" sorted-by  1 1 page ;] query s" tasks" db db-select  swap length -> 3 1 }T
T{ [: ;] query picked s" t1,t2,t3,t4,t5" s= -> true }T                                         \ (nothing said: all)
\ (text that would end a string and add a field, written by hand: a value here - it is looked for, and found nowhere)
T{ [: s\" open\",\"x\":\"" s" status" where$= ;] query picked s" " s= -> true }T
T{ [: s" open" s" status" where$=  [: s" x" sorted-by ;] query drop  s" title" sorted-by ;] query picked s" t4,t1,t2" s= -> true }T
T{ [: ;] query s" no such" db db-select  swap length  db-error nip 0> -> 0 0 true }T   \ (a name that isn't one: none, and why)
T{ 7 s" priority" s" no such" db db-where keys length -> 0 }T
T{ [: s" title" sorted-by ;] catch 0<>  error-message s" outside [: ... ;] query" s-find 0>= -> true true }T
T{ [: [: true abort" stopped" ;] query ;] catch 0<>  [: 0 1 page ;] catch 0<> -> true true }T   \ (one that fails: none being written after)

testing records as values: made, read (with a revision), saved (if at a revision), found by a field
: new-task ( -- v )
    <map> {: m :}  s" Val" s" title" m json-string!  7 s" priority" m s-at!  s\" a \"q\"" s" notes" m json-string!  m ;
: title-of ( v -- a n )  s" title" rot json-text ;
: retitled ( a n -- v )  {: a n :}  s" vals" s" 1" db db-record drop {: v :}  a n s" title" v json-string!  v ;
: made-first? ( -- flag )  new-task s" vals" db db-create if  s" 1" s=  else  false  then ;
T{ made-first? -> true }T
T{ s" vals" s" 1" db db-record drop title-of s" Val" s=  s" vals" s" 1" db db-record-rev drop nip -> true 1 }T
T{ s" vals" s" 1" db db-record drop  s" notes" rot json-text  s\" a \"q\"" s= -> true }T
T{ s" Two" retitled s" vals" s" 1" db db-save -> 2 true }T
T{ s" Three" retitled 2 s" vals" s" 1" db db-save-if -> 3 true }T
T{ s" Stale" retitled 2 s" vals" s" 1" db db-save-if  s" vals" s" 1" db db-record drop title-of s" Three" s= -> false true }T
: with-7 ( -- there? n )  7 s" priority" s" vals" db db-where {: m :}  s" 1" m s-at nip  m keys length ;
T{ with-7 -> true 1 }T
: first-three ( -- title? id? )
    s" Three" >string drop s" title" s" vals" db db-first if  title-of s" Three" s=  -rot s" 1" s=  else  false false  then ;
T{ first-three -> true true }T
T{ 99 s" priority" s" vals" db db-first  s" vals" s" none" db db-record -> false false }T
: a-round ( -- )   \ (in a scratch region, as a handler's: what the words made goes with it)
    [:  [: s" open" s" status" where$=  0 2 page ;] query s" tasks" db db-select 2drop
        s" Four" retitled s" vals" s" 1" db db-save 2drop  7 s" priority" s" vals" db db-where drop ;] with-scratch ;
T{ a-round  objects  a-round a-round  objects swap - -> 0 }T

testing where: equality, a comparison, null (none or null), !=, in, contains (case ignored), exists; all of them
T{ s\" {\"where\":{\"status\":\"open\"},\"sort\":[\"title\"]}" ids s" t4,t1,t2" s= -> true }T   \ (strings sorted by their bytes: L before l)
T{ s\" {\"where\":{\"priority\":{\">=\":3}},\"sort\":[\"title\"]}" ids s" t4,t1,t3" s= -> true }T
T{ s\" {\"where\":{\"assignee\":null},\"sort\":[\"title\"]}" ids s" t4,t2" s= -> true }T
T{ s\" {\"where\":{\"status\":{\"!=\":\"open\"}},\"sort\":[\"title\"]}" ids s" t5,t3" s= -> true }T
T{ s\" {\"where\":{\"status\":{\"in\":[\"done\",\"review\"]}},\"sort\":[\"title\"]}" ids s" t5,t3" s= -> true }T
T{ s\" {\"where\":{\"title\":{\"contains\":\"fix\"}},\"sort\":[\"title\"]}" ids s" t4,t1" s= -> true }T
T{ s\" {\"where\":{\"assignee\":{\"exists\":true}},\"sort\":[\"title\"]}" ids s" t1,t5,t3" s= -> true }T
T{ s\" {\"where\":{\"status\":\"open\",\"priority\":{\">\":1,\"<\":5}}}" ids s" t1" s= -> true }T
T{ s\" {\"where\":{\"status\":\"nope\"}}" ids nip -> 0 }T
testing sort: a record without the field as null - first (before every number, zero too)
T{ s\" {\"sort\":[\"priority\"]}" loose-ids s" l2,l3,l4,l1" s= -> true }T
T{ s\" {\"sort\":[\"-priority\"]}" loose-ids s" l1,l4,l3,l2" s= -> true }T

testing sort: by fields, - descending, ties by the next; offset and limit; the total all that match
T{ s\" {\"sort\":[\"-priority\",\"title\"]}" ids s" t4,t3,t1,t5,t2" s= -> true }T
T{ s\" {\"sort\":[\"-priority\",\"title\"],\"offset\":1,\"limit\":2}" ids s" t3,t1" s= -> true }T
T{ s\" {\"where\":{\"status\":\"open\"},\"limit\":1}" total -> 3 }T
T{ s\" {}" total -> 5 }T
testing each record with its id, revision and value
s\" {\"title\":\"Fix login\",\"status\":\"open\",\"priority\":4}" s" tasks" s" t1" db db-put 2drop
: first-record ( -- r )  s\" {\"where\":{\"priority\":4}}" s" tasks" db db-query json> {: q :}  s" records" q json-list@ drop first ;
: record-parts ( r -- id? rev priority )
    {: r :}  s" id" r json-string@ drop s" t1" s=  s" rev" r json-int@ drop
    s" value" r json-map@ drop {: v :}  s" priority" v json-int@ drop ;
T{ first-record record-parts -> true 2 4 }T
testing with an index: the same answers
T{ s" status" s" tasks" db db-index -> true }T
T{ s\" {\"where\":{\"status\":\"open\"},\"sort\":[\"title\"]}" ids s" t4,t1,t2" s= -> true }T
T{ s\" {\"where\":{\"status\":\"open\",\"title\":{\"contains\":\"docs\"}}}" ids s" t2" s= -> true }T
testing a query said wrong: none, and why
T{ s" {nope" total -> 0 }T
T{ db-error s" db: not JSON" s-starts? -> true }T
T{ s\" {\"where\":[1]}" total -> 0 }T

\ ---- watching ----
message go ( xt -- )
variable seen   variable last-id   variable last-rev   variable last-gone   variable done
create last-id-text 32 allot
behaviour watcher
    on go ( xt -- )  execute drop  1 done +! ;
    on db-changed ( c id rev json -- )
        {: c id rev j :}  1 seen +!  id string last-id-text swap move  rev last-rev !  j 0= last-gone ! ;
end-behaviour
watcher -1 s" watcher" spawn constant w
: as-watcher ( xt -- )  0 done !  w send go  20 0 do  deliver  loop ;
: settle ( -- )  20 0 do  deliver  loop ;
testing booleans sort before numbers (none, null, false, true, numbers, strings, lists, objects); an index finds what
testing  a scan finds (1 is 1.0)
s\" {\"b\":5}" s" kinds" s" k1" db db-put 2drop   s\" {\"b\":\"s\"}" s" kinds" s" k2" db db-put 2drop
s\" {\"b\":true}" s" kinds" s" k3" db db-put 2drop   s\" {\"b\":false}" s" kinds" s" k4" db db-put 2drop
: kind-ids ( query n -- a n )
    s" kinds" db db-query json> {: r :}
    s" records" r json-list@ drop [: s" id" rot json-string@ drop >string drop ;] map  " ,"  join  string ;
T{ s\" {\"sort\":[\"b\"]}" kind-ids s" k4,k3,k1,k2" s= -> true }T
s\" {\"n\":1.0}" s" nums" s" a" db db-put 2drop   s\" {\"n\":1}" s" nums" s" b" db db-put 2drop
: nums-total ( -- n )  s\" {\"where\":{\"n\":1}}" s" nums" db db-query json> {: r :}  s" total" r json-int@ drop ;
T{ nums-total  s" n" s" nums" db db-index drop  nums-total -> 2 2 }T

testing watching: each change told once it lasts - a write, a delete (json 0); a batch undone, none
T{ [: s" tasks" db db-watch ;] as-watcher  done @ -> 1 }T
0 seen !
T{ s" {}" s" tasks" s" t9" db db-put nip  settle  seen @  last-rev @  last-gone @ -> true 1 1 false }T
T{ last-id-text 2 s" t9" s= -> true }T
T{ s" tasks" s" t9" db db-delete drop  settle  seen @  last-gone @ -> 2 true }T
T{ s\" [[\"put\",\"tasks\",\"t8\",{}],[\"put-if\",\"tasks\",\"t1\",99,{}]]" db db-batch  settle  seen @ -> false 2 }T
T{ s" {}" s" other" s" x" db db-put 2drop  settle  seen @ -> 2 }T   \ (another collection: not told)
testing unwatching; a watcher gone, forgotten (writes go on)
T{ [: s" tasks" db db-unwatch ;] as-watcher  s" {}" s" tasks" s" t7" db db-put 2drop  settle  seen @ -> 2 }T
T{ [: s" tasks" db db-watch ;] as-watcher  w kill  settle  s" {}" s" tasks" s" t6" db db-put nip -> true }T
testing watching a replica: told as the leader's lines come
watcher -1 s" watcher2" spawn constant w2
db s" query-db" rot register-name
s" query-db" db-replica constant copy
0 seen !
: watch-copy ( -- )  0 done !  [: s" tasks" copy db-watch ;] w2 send go  20 0 do  deliver  loop ;
watch-copy
T{ s" {}" s" tasks" s" t5b" db db-put 2drop  settle  seen @ -> 1 }T

test-summary

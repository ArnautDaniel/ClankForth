\ jsondb's queries (where, sort, offset, limit) and watching (db-changed to an actor for each change, once it lasts)
IN: test-jsondb-query
USING: tester actors system json jsondb ;

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

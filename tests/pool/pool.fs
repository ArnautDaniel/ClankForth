\ a program of several nodes, each a process (as-nodes), for tests/pool_driver.fs: its arguments say what it does
\ and where it writes what its nodes did - a file a node, so the driver sees each
\   clankforth tests/pool/pool.fs -- hello out-dir
USING: actors net system json jsondb http http.client ;

: scenario ( -- a n )  0 args nth string ;
: out-dir ( -- a n )  1 args nth string ;
: is? ( a n -- flag )  scenario s= ;
\ a file named for this node: what it says of itself in it
: note ( name n -- )
    {: a n :}  node-index node-count node-life s" {} {} {}" format {: t tn :}
    t tn  out-dir a n node-index s" {s}/{s}-{}" format  write-file ;

\ a file named for this node with that text in it
: note-text ( text n name n -- )
    {: t tn a n :}  t tn  out-dir a n node-index s" {s}/{s}-{}" format  write-file ;
: noted? ( name n i -- flag )  {: a n i :}  out-dir a n i s" {s}/{s}-{}" format file-exists? ;
: nodes-wanted ( -- n )  s" wrong" is? if  1  else  s" hello" is? if  3  else  2  then  then ;
nodes-wanted as-nodes

\ hello: each node says who it is, and is done
: hello ( -- )  s" hello" note  0 exit-with ;
\ again: node 2 ends badly the first time (its status 3): started again, it says so in its second life
: again ( -- )
    node-index 2 =  node-life 1 = and if  3 exit-with  then
    s" again" note  0 exit-with ;
\ crash: node 1's process killed outright in its first life (a signal: no word of its own ran)
: crash ( -- )
    node-index 1 =  node-life 1 = and if  s" kill -SEGV $PPID" shell drop  10000 sleep-ms  then
    s" crash" note  0 exit-with ;
\ wrong: the one node ends badly every time - a program that can't run
: wrong ( -- )  s" wrong" note  1 exit-with ;
\ stop: each node up and serving till the pool is told to stop - then its on-shutdown word, and it ends
: stopped ( -- )  s" stopped" note ;
: stop ( -- )  ['] stopped on-shutdown  s" up" note  serve  0 exit-with ;

\ ---- actors across the nodes: messages, names, a node's end told to those who watched its actors ----
message ask ( n -- )        \ answered `told` with 100 more
message told ( n -- )
message done ( -- )         \ the node that hears it is done
message begin-it ( -- )
message crash-now ( -- )    \ the node that hears it is killed outright
: found ( name n -- addr )   \ whoever registered it, once anyone has (10 s at most: then nobody)
    {: a n | who :}  nobody to who
    500 0 do  a n whereis to who  who nobody <> if  leave  then  20 sleep-ms  loop  who ;
: other ( -- i )  3 node-index - ;   \ (of two nodes: the other's index)
: index-of ( addr -- i )  node-of 64 mod ;   \ (a node's number: its life over its index)
behaviour echo
  on ask ( n -- )  100 + sender send told ;
  on done ( -- )  0 exit-with ;
  on crash-now ( -- )  s" kill -SEGV $PPID" shell drop ;
end-behaviour

\ talk: node 1's actor finds node 2's by its name, asks it, is answered
behaviour talker
  on begin-it ( -- )
      s" echo-2" found {: who :}
      7 who send ask
      5000 await-within-ms told if  s" {}" format  else  s" no answer"  then  s" talk" note-text
      who send done  0 exit-with ;
end-behaviour
: talk ( -- )
    echo -1 s" echo" spawn {: e :}  node-index s" echo-{}" format e register-name
    node-index 1 = if  talker -1 s" talker" spawn send begin-it  then
    serve ;

\ names: a name every node registers is each node's own; one only the other node has is found there
behaviour namer
  on begin-it ( -- )
      other s" only-{}" format found {: theirs :}
      s" db" whereis node-of node =  theirs index-of  s" {} {}" format s" names" note-text
      begin  s" names" other noted? 0=  while  20 sleep-ms  repeat   \ (the other has looked too: both done)
      0 exit-with ;
end-behaviour
: names ( -- )
    echo -1 s" db" spawn {: d :}  s" db" d register-name
    echo -1 s" only" spawn {: o :}  node-index s" only-{}" format o register-name
    namer -1 s" namer" spawn send begin-it
    serve ;

\ watch: node 1 watches an actor of node 2's, and has one linked to it; node 2 killed, the watcher is told it died
\ and the linked one dies with it; a message to its old address goes nowhere; started again, it is found again
behaviour linked  on begin-it ( -- )  s" victim" found link ;  end-behaviour
behaviour watcher
  on begin-it ( -- )
      {: | v l saw-v saw-l v2 :}
      s" victim" found to v  v monitor
      linked -1 s" linked" spawn to l  l monitor  l send begin-it
      200 sleep-ms   \ (the link asked of node 2, and noted there)
      v send crash-now
      2 0 do  10000 await-within-ms died if  drop  dup v = if  true to saw-v  then  l = if  true to saw-l  then  then  loop
      saw-v saw-l s" {} {}" format s" watch" note-text
      7 v send ask  300 await-within-ms told if  drop s" answered"  else  s" dropped"  then  s" stale" note-text
      begin  s" victim" found to v2  v2 v =  v2 nobody = or  while  20 sleep-ms  repeat
      7 v2 send ask  5000 await-within-ms told if  s" {}" format  else  s" no answer"  then  s" again" note-text
      v2 send done  0 exit-with ;
end-behaviour
: watch ( -- )
    node-index 2 = if  s" victim"  echo -1 s" victim" spawn  register-name  then
    node-index 1 = if  watcher -1 s" watcher" spawn send begin-it  then
    serve ;

\ replica: a library that is nothing but actors and messages, across processes as it is across threads - node 1 a
\ database, node 2 a copy of it (its records come as one message of some hundreds of kilobytes), a write through
\ the copy made by node 1 and read back from the copy
: big-text ( -- a n )  [: 400 0 do  s" 0123456789" %s  loop ;] build string ;
: seed ( db -- )  {: db :}  100 0 do  i big-text s\" {{\"n\":{},\"text\":\"{s}\"}}" format  s" things" db db-insert 2drop drop  loop ;
0 value the-copy   \ (node 2's replica: made at its top, as a program makes one)
behaviour copier
  on begin-it ( -- )
      s\" {\"n\":-1}" s" things" s" mine" the-copy db-put 2drop
      s" things" the-copy db-count  s" things" s" mine" the-copy db-get if  nip  else  0  then
      s" {} {}" format s" replica" note-text
      s" keeper" found send done  0 exit-with ;
end-behaviour
: keep-db ( -- )
    out-dir db-open {: db :}  db seed  s" the-db" db register-name
    s" keeper"  echo -1 s" keeper" spawn  register-name ;
: replica ( -- )
    node-index 1 = if  keep-db  then
    node-index 2 = if  s" the-db" db-replica to the-copy  copier -1 s" copier" spawn send begin-it  then
    serve ;

\ serve: both nodes listen on one port (the driver's third argument); node 1 asks it twenty times, a connection
\ each, and notes how many were answered and whether both nodes answered some - the system spreading them, or
\ (Windows) the supervisor handing them round
: the-port ( -- n )  2 args nth string string>number drop ;
: which ( req -- )  {: r :}  node-index s" {}" format 200 r respond ;
behaviour asker
  on begin-it ( -- )
      {: | ok one two :}
      s" serving-2" found drop
      20 0 do
          the-port s" http://127.0.0.1:{}/" format http-get {: res :}
          res res-status 200 = if
              ok 1+ to ok
              res res-body s" 1" s= if  true to one  then
              res res-body s" 2" s= if  true to two  then
          then
      loop
      ok one two and s" {} {}" format s" serve" note-text
      s" serving-2" found send done  0 exit-with ;
end-behaviour
: serve-port ( -- )
    <router> {: site :}  s" /" ['] which site on-get  site the-port http-listen drop
    s" serving"  echo -1 s" serving" spawn  node-index s" serving-{}" format rot register-name 2drop
    node-index 1 = if  asker -1 s" asker" spawn send begin-it  then
    serve ;

: go ( -- )
    s" serve" is? if  serve-port  then
    s" replica" is? if  replica  then
    s" talk" is? if  talk  then
    s" names" is? if  names  then
    s" watch" is? if  watch  then
    s" hello" is? if  hello  then
    s" again" is? if  again  then
    s" crash" is? if  crash  then
    s" wrong" is? if  wrong  then
    s" stop" is? if  stop  then
    s" pool.fs: no such scenario" type cr  2 exit-with ;
go

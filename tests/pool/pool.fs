\ a program of several nodes, each a process (as-nodes), for tests/pool_driver.fs: its arguments say what it does
\ and where it writes what its nodes did - a file a node, so the driver sees each
\   clankforth tests/pool/pool.fs -- hello out-dir
USING: actors net system ;

: scenario ( -- a n )  0 args nth string ;
: out-dir ( -- a n )  1 args nth string ;
: is? ( a n -- flag )  scenario s= ;
\ a file named for this node: what it says of itself in it
: note ( name n -- )
    {: a n :}  node-index node-count node-life s" {} {} {}" format {: t tn :}
    t tn  out-dir a n node-index s" {s}/{s}-{}" format  write-file ;

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

: go ( -- )
    s" hello" is? if  hello  then
    s" again" is? if  again  then
    s" crash" is? if  crash  then
    s" wrong" is? if  wrong  then
    s" stop" is? if  stop  then
    s" pool.fs: no such scenario" type cr  2 exit-with ;
go

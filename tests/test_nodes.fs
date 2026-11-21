\ nodes: Forths on threads of their own, their actors messaging each other's (a system without threads skips it)
IN: test-nodes
USING: tester actors accessors vocabs.nodemsgs ;

T{ node 0 >= -> true }T
: without-threads ( -- )  node 0= if  test-summary skip-file  then ;
without-threads

variable got
behaviour asker
  on pong ( n -- )  got +! ;
  on carried ( n -- )  got ! ;
end-behaviour
asker -1 s" asker" spawn constant me

\ the frames run, waiting for a message from another node between them, till `got` is n (or a while)
: settle ( n -- )  {: n :}  500 0 do  actors-frame  got @ n = if leave then  10 node-wait drop  loop ;
: found ( addr len -- addr )  {: a n :}  nobody  500 0 do  drop  a n whereis  dup nobody <> if leave then  10 node-wait drop  loop ;

testing a node started, its actor found by name
s" vocabs.nodeecho" start-node constant worker
T{ worker node <> -> true }T
s" echo" found constant echo
T{ echo nobody <> -> true }T
T{ echo node-of worker = -> true }T

testing a message there, the answer back to its sender's address
T{ 0 got !  me enter  41 echo send ping  leave-actor  42 settle  got @ -> 42 }T
T{ 0 got !  me enter  1 echo send ping  2 echo send ping  leave-actor  5 settle  got @ -> 5 }T   \ (2 + 3)

testing objects carried: copied to the other node, its own there
: parcel-of ( -- p )  s" box" >string drop  7 <parcel> ;
: send-carry ( -- )  me enter  s" hello" >string drop  { 1 2 3 }  parcel-of  echo send carry  leave-actor ;
T{ 0 got !  send-carry  18 settle  got @ -> 18 }T   \ (5 + 3 + 3 + 7)

testing this node's own address: an id here
T{ 0 got !  me enter  5 me address send pong  leave-actor  deliver  got @ -> 5 }T
T{ me address node-of node = -> true }T

testing a node that stops: its names gone, a message to it a dead letter
: gone? ( -- flag )  500 0 do  s" echo" whereis nobody = if -1 unloop exit then  10 node-wait drop  loop  0 ;
T{ me enter  echo send halt  leave-actor  gone? -> true }T
T{ me enter  1 echo send ping  leave-actor  0 -> 0 }T

testing many nodes at once
: start-echoes ( n -- )  0 ?do  s" vocabs.nodeecho" start-node drop  loop ;
4 start-echoes
T{ s" echo" found nobody <> -> true }T   \ (one of them: the last registered)
T{ 0 got !  me enter  99 s" echo" found send ping  leave-actor  100 settle  got @ -> 100 }T

test-summary

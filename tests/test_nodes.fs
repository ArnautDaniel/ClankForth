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

testing objects carried: the receiving handler's there, freed after it
: settle-any ( -- )  500 0 do  actors-frame  got @ -1 <> if leave then  10 node-wait drop  loop ;
: objects-there ( -- n )  -1 got !  me enter  echo send count-objects  leave-actor  settle-any  got @ ;
: carry-on ( -- )  me enter  s" x" >string drop  { 4 5 }  parcel-of  echo send carry-on  leave-actor ;
T{ objects-there  carry-on  objects-there - -> 0 }T

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

testing supervision across nodes: an actor there watched, linked to, killed from here
variable dead-id  variable dead-why
behaviour watcher
  on died ( id reason -- )  dead-why !  dead-id ! ;
  on made ( addr -- )  got ! ;
end-behaviour
behaviour tied-here  end-behaviour
watcher -1 s" watcher" spawn constant wat
: settle-on ( var -- )  {: v :}  500 0 do  actors-frame  v @ if leave then  10 node-wait drop  loop ;
: settle-dead ( id -- )  {: id :}  500 0 do  actors-frame  id alive? 0= if leave then  10 node-wait drop  loop ;
s" vocabs.nodekeeper" start-node drop
s" keeper" found constant keeper
: make-one ( -- addr )  0 got !  wat enter  keeper send make  leave-actor  got settle-on  got @ ;
: watch ( addr -- )  0 dead-id !  wat enter  monitor  leave-actor ;
make-one constant v1
T{ v1 node-of keeper node-of = -> true }T
T{ v1 watch  v1 kill  dead-id settle-on  dead-id @ v1 =  dead-why @ -> true 0 }T   \ (killed: reason 0)
make-one constant v2
T{ v2 watch  wat enter  v2 send fail  leave-actor  dead-id settle-on  dead-id @ v2 =  dead-why @ 0> -> true true }T
T{ dead-why @ failure-error s" null address" s=  dead-why @ failure-actor v2 = -> true true }T   \ (its record, here)
testing a link across nodes: either gone, the other killed
make-one constant v3
tied-here -1 s" tied" spawn constant t3
T{ t3 enter  v3 link  leave-actor  v3 kill  t3 settle-dead  t3 alive? -> false }T
make-one constant v4
tied-here -1 s" tied" spawn constant t4
T{ v4 watch  t4 enter  v4 link  leave-actor  t4 kill  dead-id settle-on  dead-id @ v4 = -> true }T
make-one constant v5
tied-here -1 s" tied" spawn constant t5
T{ t5 enter  v5 link  v5 unlink  leave-actor  v5 kill  v5 watch  dead-id settle-on  t5 alive? -> true }T
testing one gone already, watched: `died` at once, no actor
T{ v1 watch  dead-id settle-on  dead-id @ v1 =  dead-why @ -> true no-actor }T
testing a node that ends: those watching its actors told they are gone
make-one constant v6
T{ v6 watch  wat enter  keeper send halt  leave-actor  dead-id settle-on  dead-id @ v6 =  dead-why @ -> true no-actor }T
: keeper-gone? ( -- flag )  500 0 do  s" keeper" whereis nobody = if -1 unloop exit then  10 node-wait drop  loop  0 ;
: watch-gone ( -- )  wat enter  v6 monitor ;
T{ keeper-gone?  ' watch-gone catch  leave-actor -> true -1 }T   \ (its node ended: an error, as one gone here is)
T{ error-message drop 17 s" monitor: no actor" s= -> true }T

testing stop-node of a node start-node didn't make: an error (its loop is its host's)
: stop-mine ( -- )  node stop-node ;
T{ ' stop-mine catch -> -1 }T
testing a node whose vocabulary doesn't load ends (the error said), its place free again
T{ s" vocabs.no-such-vocabulary" start-node 0> -> true }T

test-summary

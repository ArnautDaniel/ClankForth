\ nodes: Forths on threads of their own, their actors messaging each other's (a system without threads skips it)
IN: test-nodes
USING: tester actors accessors vocabs.nodemsgs ;

T{ node 0 >= -> true }T
T{ cores 0> -> true }T
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

testing a map and bytes carried: copied, a string key by its bytes
: map-of ( -- m )  <map> {: m :}  10 s" n" m s-at!  20 7 m at!  m ;
: bytes-of ( -- b )  <bytes> {: b :}  1 b b-push  2 b b-push  b ;
: send-map ( -- )  me enter  map-of bytes-of echo send carry-map  leave-actor ;
T{ 0 got !  send-map  32 settle  got @ -> 32 }T   \ (10 + 20 + 2)

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

testing one port, several nodes: each listening on it gets some of its connections (the system spreads them - or,
testing  without SO_REUSEPORT, Windows, the node that bound it hands them round)
USING: net vocabs.listenmsgs ;
variable port   variable here-got   variable there-got   variable answers
message spread-start ( -- )
message dial ( n -- )
behaviour spreader
  on spread-start ( -- )   \ (a free port found, then listened on again: its number named, so the nodes share it)
      0 listen {: l :}  l listener-port port !  l net-close  port @ listen drop ;
  on net-accepted ( listener conn -- )  nip net-close  1 here-got +! ;
  on dial ( n -- )  0 ?do  s" 127.0.0.1" port @ connect drop  loop ;
  on net-connected ( conn -- )  net-close ;
  on counted ( n -- )  there-got +!  1 answers +! ;
end-behaviour
spreader -1 s" spreader" spawn constant spreading
: pump ( n -- )  0 ?do  actors-frame  10 net-poll drop  loop ;
spreading send spread-start  5 pump
s" vocabs.nodelisten" start-node constant n1
s" vocabs.nodelisten" start-node constant n2
: node-listener ( node -- addr )  s" listener{}" format found ;
n1 node-listener constant l1   n2 node-listener constant l2
me enter  port @ l1 send listen-on  port @ l2 send listen-on  leave-actor
20 pump
30 spreading send dial
100 pump
: ask-counts ( -- )  0 answers !  0 there-got !  spreading enter  l1 send how-many  l2 send how-many  leave-actor ;
: counts-in ( -- n )  ask-counts  100 0 do  actors-frame  answers @ 2 = if leave then  10 net-poll drop  loop  answers @ ;
T{ counts-in -> 2 }T
T{ here-got @ there-got @ + -> 30 }T
T{ here-got @ 0>  there-got @ 0> -> true true }T   \ (both this node and the others got some)

testing a node idle after its handlers freed much: the free memory handed back to the system (memory-trims counts)
s" vocabs.nodechurn" start-node drop
s" churner" found constant churner
: answered ( -- )  500 0 do  actors-frame  got @ 0< 0= if leave then  10 node-wait drop  loop ;
: trims ( -- n )  -1 got !  me enter  churner send count-trims  leave-actor  answered  got @ ;
: trims-soon ( -- n )  0  100 0 do  drop trims  dup if leave then  10 node-wait drop  loop ;   \ (once it is idle)
T{ trims -> 0 }T
: a-while ( -- )  5 0 do  20 node-wait drop  loop ;
T{ me enter  100 churner send churn  leave-actor  a-while  trims -> 0 }T   \ (too few to be worth it)
T{ me enter  10000 churner send churn  leave-actor  trims-soon -> 1 }T
T{ me enter  churner send halt  leave-actor -> }T

test-summary

\ placement: an actor made on another node (spawn-on), or on the least busy (spawn-anywhere) - its behaviour and
\ state type found there by name; each node's load (node-load) its handlers' share of its time
IN: test-placement
USING: tester actors system vocabs.placework ;

s" vocabs.placework" start-node constant worker
s" vocabs.nodeecho" start-node constant stranger   \ (it hasn't the behaviour: what is made there is nobody)
200 sleep-ms

message try ( xt -- )
message got ( x -- )
variable answer   variable answered
behaviour asker
  on try ( xt -- )  execute answer !  true answered ! ;
  on crunched ( sum node -- )  answer !  answer 1 cells + !  true answered ! ;   \ (answer: the node; +1 cell: the sum)
end-behaviour
asker -1 s" asker" spawn constant me
create sum-cell 2 cells allot
: settle ( -- )  3000 0 do  deliver  answered @ if  leave  then  1 sleep-ms  loop ;
: ask ( xt -- x )  false answered !  me send try  settle  answer @ ;

testing spawn-on: a cruncher made on the worker node - its address that node's, its answer from there
variable far
: on-worker ( -- addr )  cruncher work-state s" far-cruncher" worker spawn-on ;
T{ ' on-worker ask dup far !  node-of -> worker }T
: crunch-there ( -- 0 )  1000 far @ send crunch  0 ;
false answered !  ' crunch-there me send try  settle  false answered !  settle
T{ answer @  answer 1 cells + @ -> worker 499500 }T

testing spawn-on a node without the behaviour: nobody; a node not there: nobody; this node: made here, at once
: on-stranger ( -- addr )  cruncher work-state s" lost" stranger spawn-on ;
T{ ' on-stranger ask -> nobody }T
: on-none ( -- addr )  cruncher -1 s" lost" 999999 spawn-on ;
T{ ' on-none ask -> nobody }T
: on-here ( -- addr )  cruncher -1 s" near" node spawn-on ;
T{ ' on-here ask node-of -> node }T
testing at the prompt, another node's: an error (it awaits the answer: a handler's)
: prompt-try ( -- )  cruncher -1 s" x" worker spawn-on drop ;
T{ ' prompt-try catch -> -1 }T

testing spawn-anywhere: made where the load is least - a live actor, here or there
: anywhere ( -- addr )  cruncher work-state s" anywhere" spawn-anywhere ;
T{ ' anywhere ask nobody <> -> true }T

testing a node's load: its handlers' share of its time, in thousandths - a busy node's high, then least-busy-node
testing  another
: busy-worker ( -- 0 )  cruncher -1 s" busy" worker spawn-on {: w :}  1500 w send hold-busy  0 ;
' busy-worker ask drop
800 sleep-ms
T{ worker node-load 500 > -> true }T
T{ least-busy-node worker <> -> true }T
T{ 999999 node-load -> -1 }T

worker stop-node  stranger stop-node
test-summary

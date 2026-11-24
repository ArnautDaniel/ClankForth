\ (test_placement.fs) a behaviour both the test and a node of its own have: an actor made on either by spawn-on
IN: vocabs.placework
USING: actors system ;
message crunch ( n -- )              \ sum 0..n-1, answered to the sender
message crunched ( sum node -- )
message hold-busy ( ms -- )          \ (keeps its node's handlers busy that long: its load)
state: work-state  cell field crunches  end-state
behaviour cruncher
  on crunch ( n -- )  {: n | s :}  n 0 do  i s + to s  loop  1 crunches +!  s node sender send crunched ;
  on hold-busy ( ms -- )  {: ms :}  ms-now ms + {: till :}  begin  ms-now till <  while  repeat ;
end-behaviour

\ (test_placement.fs) a behaviour both the test and a node of its own have: an actor made on either by spawn-on
IN: vocabs.placework
USING: actors system compression ;
message crunch ( n -- )              \ sum 0..n-1, answered to the sender
message crunched ( sum node -- )
message squeeze ( n -- )             \ n rows gzipped and back (tsan_stress.fs: every node compressing at once)
message squeezed ( same? node -- )
message hold-busy ( ms -- )          \ (keeps its node's handlers busy that long: its load)
state: work-state  cell field crunches  end-state
: rows ( n -- a len )  {: n :}  [: n 0 ?do  s" <tr><td>row " %s  i %n  s" </td></tr>" %s  loop ;] build string ;
: squeezes? ( n -- flag )  rows {: a n :}  a n gzip n gunzip if  a n s=  else  false  then ;
behaviour cruncher
  on crunch ( n -- )  {: n | s :}  n 0 do  i s + to s  loop  1 crunches +!  s node sender send crunched ;
  on squeeze ( n -- )  squeezes? node sender send squeezed ;
  on hold-busy ( ms -- )  {: ms :}  ms-now ms + {: till :}  begin  ms-now till <  while  repeat ;
end-behaviour

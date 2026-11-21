\ (nodes.fs) a worker node: each job a fib, its answer back
IN: benchworker
USING: actors benchmsgs ;
: fib ( n -- f )  dup 2 < if exit then  dup 1- recurse  swap 2 - recurse + ;
behaviour worker
  on job ( n -- )  fib sender send done ;
end-behaviour
worker -1 s" worker" spawn  drop

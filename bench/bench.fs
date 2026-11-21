\ A benchmark of the inner interpreter: calls, arithmetic, loops, locals, lists, tuples.
USING: accessors ;

: fib ( n -- f )  dup 2 < if exit then  dup 1- recurse  swap 2 - recurse + ;
: sum-loop ( n -- s )  0 swap 0 do i + loop ;
: collatz-len ( n -- len )  0 swap begin dup 1 > while
      dup 1 and if 3 * 1+ else 2/ then  swap 1+ swap  repeat drop ;
: collatz ( n -- )  0 do i 1+ collatz-len drop loop ;
: locals-mix ( a b c -- r )  {: a b c :}  a b * c + a - ;
: locals-loop ( n -- )  0 do i i 3 locals-mix drop loop ;
TUPLE: pt  x y ;
GENERIC: norm1 ( p -- n )
M: pt norm1 ( p -- n )  dup x>> swap y>> + ;
: tuples ( n -- )  1 2 <pt> {: p :}  0 do p norm1 drop loop  p dispose ;
: lists ( n -- )  0 do  { 1 2 3 4 5 6 7 8 } dup [: dup * ;] map  dup list-free drop list-free  loop ;


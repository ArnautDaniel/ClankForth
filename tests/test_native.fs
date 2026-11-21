\ the native backend: colon definitions translated to machine code, and what must run the same as threaded code.
\ (A system without it - another machine, CLANKFORTH_THREADED set - runs these too: `native?` is false there, so
\ those checks say what it is; the rest must hold either way.)
IN: test-native
USING: tester ;

: native-here? ( -- flag )  [: ;] native? ;   \ (has this system the backend at all?)

testing which definitions are translated
: sq ( n -- n*n )  dup * ;
T{ ' sq native? -> native-here? }T
T{ [: 1+ ;] native? -> native-here? }T
: konst ( n "name" -- )  create , does> @ ;   \ (does>: left threaded)
T{ ' konst native? -> false }T
7 konst seven
T{ seven -> 7 }T
: hops ( n -- n*n )  ['] sq jump ;   \ (jump: left threaded)
T{ ' hops native? -> false }T
T{ 5 hops -> 25 }T

testing control flow
: fib ( n -- f )  dup 2 < if exit then  dup 1- recurse  swap 2 - recurse + ;
T{ 20 fib -> 6765 }T
: sum-to ( n -- s )  0 swap 0 do i + loop ;
T{ 100 sum-to -> 4950 }T
: none-to ( n -- s )  0 swap 0 ?do i + loop ;
T{ 0 none-to 4 none-to -> 0 6 }T
: until-big ( -- n )  0 begin 7 + dup 50 > until ;
T{ until-big -> 56 }T
: first-over ( limit -- i )  100 0 do  i dup * over > if drop i unloop exit then  loop  drop -1 ;
T{ 50 first-over -> 8 }T
: leaves ( -- n )  0  10 0 do  5 0 do  i 3 = if leave then  1+  loop  loop ;
T{ leaves -> 30 }T
: by-twos ( -- s )  0  10 0 do i + 2 +loop ;
T{ by-twos -> 20 }T
: classify ( n -- c )  case 1 of 10 endof 2 of 20 endof drop 0 0 endcase ;
T{ 1 classify 2 classify 9 classify -> 10 20 0 }T

testing locals, quotations, closures, walks
: mix ( a b c -- r )  {: a b c :}  a b * c + ;
T{ 2 3 4 mix -> 10 }T
: add-to ( l n -- l' )  {: n :}  [: n + ;] map ;
T{ { 1 2 3 } 10 add-to dup 2 swap nth swap list-free -> 13 }T
: total ( l -- n )  {: l | sum :}  l [: sum + to sum ;] each  sum ;
T{ { 1 2 3 4 } dup total swap list-free -> 10 }T
: kept ( x -- x x )  [: 1+ ;] keep ;
T{ 5 kept -> 6 5 }T
: evens ( l -- l' )  [: 1 and 0= ;] filter ;
T{ { 1 2 3 4 } dup evens dup length swap list-free swap list-free -> 2 }T

testing native and threaded calling each other
: thrice ( xt -- )  {: xt :}  xt execute  xt execute  xt execute ;
: incr ( n -- n+1 )  1+ ;
T{ 0 ' incr thrice -> 3 }T
T{ 1 ' seven thrice -> 1 7 7 7 }T   \ (a word made by does>: threaded, run from native code)
defer later
: via-defer ( -- n )  later later + ;
' seven is later
T{ via-defer -> 14 }T
' native-here? is later
T{ via-defer -> native-here? native-here? + }T

testing errors from inside machine code: caught, the stacks put back, the words named
: deep-fail  5 pick drop ;   \ (out of range: no comment, it would be warned of)
: middle  deep-fail ;
T{ ' middle catch -> -1 }T
T{ error-message s" pick: out of range" s= -> true }T
: count-on ( n -- n' )  dup 0= if  drop -1 exit  then  1- recurse ;
T{ 5 count-on -> -1 }T
: too-deep ( -- )  recurse ;
T{ ' too-deep catch -> -1 }T
T{ error-message s" return stack overflow" s= -> true }T

testing a task that waits deep inside machine code goes on where it was
variable seen
: note ( n -- )  seen @ 10 * + seen ! ;
: inner ( -- )  2 note  yield  3 note ;
: outer ( -- )  1 note  inner  4 note  2 wait  5 note ;
0 seen !  ' outer start-task drop
T{ 1 tick-tasks seen @ -> 12 }T
T{ 1 tick-tasks seen @ -> 1234 }T
T{ 2 tick-tasks seen @ -> 12345 }T
: loop-yields ( -- )  3 0 do  i 1+ note  yield  loop ;
0 seen !  ' loop-yields start-task drop
T{ 5 tick-tasks seen @ -> 123 }T
: with-locals ( a b -- )  {: a b :}  a note  yield  b note ;
: start-locals ( -- )  7 8 with-locals ;
0 seen !  ' start-locals start-task drop
T{ 1 tick-tasks seen @  1 tick-tasks seen @ -> 7 78 }T

testing restart from machine code: its frames dropped, the task in the other word
: after ( -- )  9 note ;
: restarts ( -- )  1 note  ['] after restart  2 note ;
: nests ( -- )  restarts  3 note ;
0 seen !  ' nests start-task drop
T{ 1 tick-tasks seen @ -> 19 }T

testing a task stopping itself from machine code
variable me
: quits ( -- )  1 note  me @ stop-task  2 note ;
0 seen !  ' quits start-task me !
T{ 2 tick-tasks seen @ -> 1 }T

test-summary

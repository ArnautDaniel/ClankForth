\ the native backend: colon definitions translated to machine code, and what must run the same as threaded code.
\ (A system without it - another machine, CLANKFORTH_THREADED set - runs these too: `native?` is false there, so
\ those checks say what it is; the rest must hold either way.)
IN: test-native
USING: tester system ;

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

testing the words written in line: their edges and errors as their C primitives'
: wraps ( -- a b c )  $7fffffffffffffff 1+  -1 1 u<  -1 1 <  ;
T{ wraps -> $8000000000000000 false true }T
: folded ( n -- n a b c d )  dup -5 +  over 3 *  2 pick 7 and  3 pick -2 xor ;
T{ 10 folded -> 10 5 30 2 -12 }T
: big-literal ( n -- n' )  $100000000 + ;   \ (past 32 bits: not folded in)
T{ 1 big-literal -> $100000001 }T
: compares ( a b -- f1 f2 f3 f4 f5 f6 )  {: a b :}  a b =  a b <>  a b <  a b >  a b <=  a b >= ;
T{ 3 4 compares -> false true true false true false }T
T{ 4 4 compares -> true false false false true true }T
: zeros ( n -- f1 f2 f3 f4 )  {: n :}  n 0=  n 0<>  n 0<  n 0> ;
T{ -2 zeros 0 zeros -> false true true false  true false false false }T
: branches ( n -- r )  dup 10 < if drop 1 exit then  dup 0= if drop 2 exit then  20 > if 3 else 4 then ;
T{ 5 branches 15 branches 25 branches -> 1 4 3 }T
: rstack ( a b -- b a )  >r >r  r@ drop  r> r> ;
T{ 1 2 rstack -> 1 2 }T
: misc ( n -- n a b c d )  dup invert  over negate  2 pick cells  3 pick 1- ;
T{ 5 misc -> 5 -6 -5 40 4 }T
: under-plus  + ;   \ (no comment: the checker would warn of the underflow)
T{ : one-plus  1 under-plus ;  ' one-plus catch  error-message s" stack underflow" s= -> -1 true }T
: under-rfrom  r> r> r> r> r> r> ;
T{ ' under-rfrom catch -> -1 }T
: no-loop-i  i ;
T{ ' no-loop-i catch  error-message s" i: not in a do loop" s= -> -1 true }T
: fills ( -- )  begin 1 again ;
T{ ' fills catch  error-message s" stack overflow" s= -> -1 true }T
: big-loop ( -- n )  0  -3 -6 do  1+  loop ;
T{ big-loop -> 3 }T
: to-max ( -- n )  0  $7fffffffffffffff dup 3 - do  1+  loop ;   \ (the index up to the largest cell)
T{ to-max -> 3 }T

testing memory, written in line: @ ! c@ c! as their primitives - a number or nothing where an address belongs, the
testing  dictionary's last bytes, each an error (the same words said)
create cells-of 3 cells allot
: fills-cells ( -- a b c )  1 cells-of !  -2 cells-of cell+ !  cells-of @  cells-of cell+ @  cells-of 2 cells + @ ;
T{ 0 cells-of 2 cells + !  fills-cells -> 1 -2 0 }T
: bytes-of ( -- a b )  $1ff cells-of c!  cells-of c@  cells-of 1+ c@ ;   \ (a byte: the low one, no sign)
T{ 0 cells-of !  bytes-of -> $ff 0 }T
: unaligned ( -- x )  $0102030405060708 cells-of 3 + !  cells-of 3 + @ ;
T{ unaligned -> $0102030405060708 }T
: fetch-it  @ ;   : cfetch-it  c@ ;   : store-it  ! ;   : cstore-it  c! ;
T{ 0 ' fetch-it catch nip  error-message s" null address" s= -> -1 true }T
T{ 0 ' cfetch-it catch nip  error-message s" null address" s= -> -1 true }T
T{ 7 0 ' store-it catch nip nip  error-message s" null address" s= -> -1 true }T
T{ 7 0 ' cstore-it catch nip nip  error-message s" null address" s= -> -1 true }T
T{ 42 ' fetch-it catch nip  error-message s" not an address" s-starts? -> -1 true }T
T{ -8 ' cfetch-it catch nip  error-message s" not an address" s-starts? -> -1 true }T
T{ ' fetch-it catch  error-message s" stack underflow" s= -> -1 true }T
T{ 1 ' store-it catch nip  error-message s" stack underflow" s= -> -1 true }T
: at-end ( n -- addr )  unused here + swap - ;   \ (n bytes before the dictionary's end)
T{ 8 at-end fetch-it drop  1 at-end cfetch-it drop -> }T
T{ 7 at-end ' fetch-it catch nip  error-message s" past the dictionary's end" s-starts? -> -1 true }T
T{ 0 3 at-end ' store-it catch nip nip  error-message s" past the dictionary's end" s-starts? -> -1 true }T

testing a locals frame, written in line: zeroed each time, the frame before back as the word ends; too deep, the
testing  primitive's error
: zeroed ( n -- a b )  {: n | a b :}  a b  n to a  n to b ;   \ (a b: 0 each call, whatever the last left there)
T{ 5 zeroed 6 zeroed -> 0 0 0 0 }T
: in-frame ( x -- y )  {: x | t :}  x 1+ to t  t ;
: out-frame ( x -- a b )  {: x | k :}  x in-frame to k  x k ;   \ (its own frame as it was, after inner's)
T{ 4 out-frame -> 4 5 }T
: deeper ( n -- )  {: n | a b c :}  n 1+ recurse ;
T{ 0 ' deeper catch nip  error-message s" return stack overflow" s= -> -1 true }T

testing within, true, false, 0<=, 0>= written in line: as the prelude's
: withins ( x -- f1 f2 f3 )  {: x :}  x 3 7 within  x -2 2 within  x 7 3 within ;
T{ 3 withins 6 withins 7 withins -> true false false  true false false  false false true }T
T{ -1 withins 0 withins -> false true true  false true true }T
: flags ( -- t f )  true false ;
T{ flags -> -1 0 }T
: zero-sides ( n -- f1 f2 )  {: n :}  n 0<=  n 0>= ;
T{ -3 zero-sides 0 zero-sides 3 zero-sides -> true false  true true  false true }T
: sides ( n -- r )  dup 0<= if drop 1 exit then  0>= if 2 else 3 then ;
T{ -1 sides 0 sides 5 sides -> 1 1 2 }T
: under-within  within ;
T{ 1 2 ' under-within catch nip nip  error-message s" stack underflow" s= -> -1 true }T
T{ ' fills-cells native?  ' withins native?  ' zeroed native? -> native-here? native-here? native-here? }T

testing W^X: no memory writable and runnable at once - machine code read and run only, made writable just to be
testing  written (where the system lists its mappings: Linux's /proc/self/maps)
: rwx-mappings ( -- n )   \ (-1: no list to read)
    s" /proc/self/maps" file-exists? 0= if  -1 exit  then
    s" /proc/self/maps" read-file {: a n | k :}
    begin  a n s"  rwx" s-find dup 0>= while  1+ {: i :}  k 1+ to k  a i + to a  n i - to n  repeat  drop k ;
: later-word ( n -- n )  dup + ;   \ (one more translated after the check's own: still none)
T{ rwx-mappings 0>  3 later-word -> false 6 }T

test-summary

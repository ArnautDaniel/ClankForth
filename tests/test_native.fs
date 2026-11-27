\ the native backend: colon definitions translated to machine code, and what must run the same as threaded code.
\ (A system without it - another machine, CLANKFORTH_THREADED set - runs these too: `native?` is false there, so
\ those checks say what it is; the rest must hold either way.)
IN: test-native
USING: tester system frames actors ;

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

testing CLANKFORTH_CODE_MAP: each definition's machine code listed in the file it names - where, how long, its name
testing  (set by ctest's native_code_map; not set, or no backend, there is no list to look in)
: in-the-map ( -- n )  42 ;
: listed? ( path n -- flag )  read-file {: s n :}  s n s"  in-the-map" s-find 0>=  s dispose ;
: map-lists-it? ( -- flag )
    s" CLANKFORTH_CODE_MAP" getenv 0= if  true exit  then
    {: p pn :}  native-here? if  p pn listed?  else  true  then  p dispose ;
T{ in-the-map  map-lists-it? -> 42 true }T

testing a run of words written in line - the stack's depth kept in a register from one to the next, what they need
testing  of it checked once as the run begins: the same as each alone, whatever goes wrong and wherever
variable poked   create cell-buf 4 cells allot
\ (no comments on those meant to fail: the checker would say so)
: poke-then-under  7 poked !  drop drop ;                    \ the store is done before the word that hasn't its item
T{ 0 poked !  ' poke-then-under catch  error-message s" stack underflow" s=  poked @ -> -1 true 7 }T
: under-late  dup 1+ swap drop nip nip nip ;                 \ fine for four words, then not
T{ 5 ' under-late catch nip  error-message s" stack underflow" s= -> -1 true }T
: push-eight ( -- a b c d e f g h )  1 2 3 4 5 6 7 8 ;
: nearly-full ( n -- )  0 do  0  loop ;
: over-the-top ( -- flag )   \ six cells of room, eight wanted: the overflow said, as each push alone says it
    [: 250 depth - nearly-full  push-eight ;] catch 0<>  error-message s" stack overflow" s= and ;
T{ over-the-top -> true }T
: to-the-top ( -- code )   \ exactly the room there is: no error (thrown out of, to empty the stack: 1 if it was full)
    [: 249 depth - nearly-full  push-eight  2drop  depth 254 = if  1 throw  then  2 throw ;] catch ;
T{ to-the-top -> 1 }T
: fetch-bad-mid  1 poked !  2 0 @ 3 poked ! ;         \ a word's own check failing in a run: the words after not run
T{ ' fetch-bad-mid catch  error-message s" null address" s=  poked @ -> -1 true 1 }T
: mixed ( a b -- x )  {: a b | t :}  a b + to t  t t * a -  b 3 and if  1+  else  1-  then  t xor ;
T{ 3 4 mixed  10 5 mixed  -2 9 mixed -> 42 215 51 }T
: scan ( a n c -- i )  {: a n c | i :}  begin  i n < if  a i + c@ c <>  else  false  then  while  i 1+ to i  repeat  i ;
T{ s" a run of bytes" 2dup 98 scan -rot 122 scan -> 9 14 }T   \ (a b; no z)
: cells-sum ( -- n )  4 0 do  i 10 * cell-buf i cells + !  loop  0  4 0 do  cell-buf i cells + @ +  loop ;
T{ cells-sum -> 60 }T
: into-run ( n -- m )  begin  1+ dup 2* drop  dup 10 <  while  repeat  dup + ;   \ (a branch lands inside what would be a run)
T{ 0 into-run  20 into-run -> 20 42 }T
: calls-between ( n -- m )  1+ dup sq swap 1- sq + 2 * ;     \ (runs either side of calls: sp the task's across them)
T{ 3 calls-between -> 50 }T

testing a word's call: where it goes on pushed in line - as deep as the return stack goes, then the same error
: down ( n -- n )  dup 0> if  1- recurse  then ;
T{ 300 down -> 0 }T
: down-for-ever  recurse ;
T{ ' down-for-ever catch  error-message s" return stack overflow" s= -> -1 true }T
T{ 5 down -> 0 }T   \ (and calls as they were, after)

testing a small definition written into its callers (no call: its words part of the caller's run) - the same answers,
testing  the same errors and the same callers named; defined again, its callers have the new one
: calls-only? ( -- flag )  s" CLANKFORTH_CALLS_ONLY" getenv dup if  -rot drop dispose  then ;
: inlines ( xt -- n )   \ (-1: a system that writes nothing in line - no backend, or told to call every word)
    native-here?  calls-only? 0= and if  native-inlined  else  drop -1  then ;
: was-inlined? ( xt n -- flag )  swap inlines dup 0< if  2drop true  else  =  then ;
: tripled ( n -- m )  dup dup + + ;
: tripled-plus ( n -- m )  tripled 1+ ;
T{ 5 tripled-plus  -7 tripled-plus  ' tripled-plus 1 was-inlined? -> 16 -20 true }T
: blend ( a b -- c )  {: a b :}  a b +  2 *  a - ;                \ (with locals: its frame made and let go in line)
: blends ( a b -- c d )  {: a b :}  a b blend  b a blend ;
T{ 3 10 blends  ' blends 2 was-inlined? -> 23 16 true }T
: nine-times ( n -- m )  tripled tripled ;                          \ (one written into another, written into a third)
: nine-plus ( n -- m )  nine-times 1+ ;
T{ 2 nine-plus  ' nine-plus inlines 0<> -> 19 true }T
: recursive ( n -- m )  dup 0> if  1- recurse  then ;               \ (not: a branch in it)
: uses-recursive ( n -- m )  recursive 1+ ;
T{ 4 uses-recursive  ' uses-recursive 0 was-inlined? -> 1 true }T
: stacked ( a -- a )  >r r> ;                                       \ (not: the return stack's words in it)
: uses-stacked ( a -- a )  stacked 1+ ;
T{ 4 uses-stacked  ' uses-stacked 0 was-inlined? -> 5 true }T
: fetches  @ ;                                                      \ (written in line: what is wrong in it said the same,
: fetch-nothing  7 poked !  0 fetches  8 poked ! ;                  \  and the words after it not run)
T{ ' fetch-nothing catch  error-message s" null address" s=  poked @ -> -1 true 7 }T
: named-in ( -- )  0 fetches drop ;                                  \ (... its callers named, as called it would name them)
message go ( -- )
variable failed
behaviour chained  on go ( -- )  named-in ;  end-behaviour
behaviour noting  on actor-failed ( n -- )  failed ! ;  end-behaviour
chained -1 s" chained" spawn constant in-chain
noting -1 s" noting" spawn constant took-note   took-note errors-to
T{ in-chain send go  deliver deliver  failed @ failure-chain s" fetches <- named-in <- chained:on go" s-starts? -> true }T
T{ ' named-in 1 was-inlined? -> true }T
in-chain kill  took-note kill
: deep-with ( n -- n )  tripled-plus recurse ;                      \ (the return stack full as its words begin: said so)
T{ 1 ' deep-with catch nip  error-message s" return stack overflow" s= -> -1 true }T
\ (one defined again: its callers follow - at the prompt, where a word is defined again: tests/host_call.c)

testing the stack's top in registers through a run (two: a third item pushed, the deeper goes to its cell) - each
testing  word of the stack with none of its items there, with one, with both; written back where anything else looks
\ (and again, each word sent round by its stub - the registers written to the cells and read back: ctest's
\  test_native_stubs_only)
: swap-none ( a b -- n )  swap - ;                 : swap-one ( a -- n )  10 swap - ;       : swap-both ( -- n )  3 10 swap - ;
T{ 3 10 swap-none  3 swap-one  swap-both -> 7 7 7 }T
: over-none ( a b -- n )  over + + ;               : over-one ( a -- n )  10 over + + ;     : over-both ( -- n )  3 10 over + + ;
T{ 3 10 over-none  3 over-one  over-both -> 16 16 16 }T
: nip-none ( a b -- n )  nip 1+ ;                  : nip-one ( a -- n )  10 nip 1+ ;        : nip-both ( -- n )  3 10 nip 1+ ;
T{ 3 10 nip-none  3 nip-one  nip-both -> 11 11 11 }T
: dup-none ( a -- n )  dup 1+ * ;                  : dup-one ( -- n )  3 dup 1+ * ;         : dup-both ( a -- n )  3 dup 1+ * + ;
T{ 3 dup-none  dup-one  100 dup-both -> 12 12 112 }T
: drops ( a -- a )  1 2 drop drop  3 drop ;
T{ 9 drops -> 9 }T
: five ( -- a b c d e )  1 2 3 4 5 ;                                   \ (more than the registers hold)
: summed ( -- n )  1 2 3 4 5 + + + + ;
: weighed ( -- n )  1 2 3 4 5  swap - swap - swap - swap - ;           \ (5-4, 1-3, -2-2, -4-1: each the order `-` has)
T{ five  summed  weighed -> 1 2 3 4 5  15  -5 }T
: less-top ( a b c -- n )  nip - ;                 : more-top ( a b c -- n )  nip + ;       \ (the top in a register, the
: less-fetched ( a -- n )  poked @ - ;             : times-top ( a b c -- n )  nip * ;      \  item under it in its cell)
T{ 10 99 3 less-top  10 99 3 more-top  10 99 3 times-top   4 poked !  10 less-fetched -> 7 13 30 6 }T
: under? ( a b c -- flag )  nip < ;                : both-under? ( -- flag flag )  1 2 <  2 1 < ;
: zero-top? ( a -- flag flag )  dup 0=  swap 0<> ; : between? ( x -- flag )  1 10 within ;
T{ 1 9 2 under?  2 9 1 under?  both-under?  0 zero-top?  5 between?  10 between? -> true false  true false  true false  true false }T
: stores ( -- )  42 poked !  poked @ 1+ poked ! ;  : store-byte ( -- c )  0 poked !  300 poked c!  poked @ ;
T{ stores poked @  store-byte -> 43 44 }T
: aside ( a b -- b a+1 )  >r 1+ r> swap ;          : asides ( -- a b )  2 1 >r >r r> r> ;
T{ 3 10 aside  asides -> 10 4  2 1 }T
: flagged ( a -- n )  1+ dup if  10 +  else  20 +  then ;              \ (a flag tested in its register)
: kept-under ( -- n )  7 2 dup if  +  else  -  then ;                  \ (... an item under it in another: to its cell
: kept-over ( a -- n )  7 swap dup 5 < if  +  else  -  then ;          \  before the jump, whichever way it goes)
T{ 4 flagged  -1 flagged  kept-under  2 kept-over  9 kept-over -> 15 20  9  9 -2 }T
: turned-fetch  swap 0 @ ;                         \ (an error after a swap that was no instruction: the cells as swapped)
T{ 1 2 ' turned-fetch catch 0<> -> 2 1 true }T
: summed-fetch  5 + dup 0 @ ;                      \ (... a sum that was in a register: in its cell)
T{ 1 ' summed-fetch catch 0<> -> 6 true }T
: framed ( a b -- n )  {: a b :}  a b blend  a +  b a blend  b + + ;   \ (locals read round a word written in line with
T{ 3 10 framed -> 52 }T                                                \  a frame of its own: the caller's frame found again)

testing a do loop's index and limit in registers (the return stack's cells kept right all the same): read again
testing  after anything that may have changed them - a word called with a loop of its own, >r, a jump from where
testing  they weren't known; (do) (?do) i j (loop) written in line
: squares ( n -- s )  0 swap 0 do  i i * +  loop ;
: none-or ( n -- s )  -1 swap 0 ?do  drop i  loop ;                   \ ((?do): no round at all, or the last index)
T{ 5 squares  0 none-or  4 none-or  1 squares -> 30  -1 3  0 }T
: grid ( w h -- s )  {: w h | s :}  h 0 do  w 0 do  j 10 * i + s + to s  loop  loop  s ;   \ (one in another: i, j)
: after-inner ( n -- s )  0 swap 0 do  3 0 do  i +  loop  i 100 * +  loop ;   \ (the outer's i after the inner loop)
T{ 3 2 grid  2 after-inner -> 36 106 }T
: inner-sum ( n -- s )  0 swap 0 ?do  i +  loop ;
: with-call ( n -- s )  0 swap 0 do  i inner-sum +  i +  loop ;       \ (a word called, with a loop of its own)
: with-execute ( n -- s )  0 swap 0 do  i ['] inner-sum execute +  i +  loop ;   \ (... by way of C)
: with-quotation ( n -- s )  0 swap 0 do  [: 0 4 0 do  i +  loop ;] execute  i * +  loop ;
T{ 5 with-call  5 with-execute  4 with-quotation -> 20 20 36 }T
: nested-self ( n -- s )  dup 0= if  exit  then  {: n :}  0  n 0 do  i +  n 1- recurse +  i +  loop ;   \ (itself)
T{ 3 nested-self  0 nested-self -> 12 0 }T
: found-at ( c a n -- i )  {: c a n :}  -1  n 0 do  a i + c@ c = if  drop i leave  then  loop ;   \ (leave)
: found-early ( c a n -- i )  {: c a n :}  n 0 do  a i + c@ c = if  i unloop exit  then  loop  -1 ;   \ (unloop exit)
T{ 'c' s" abcabc" found-at  'z' s" abc" found-at  'b' s" abcabc" found-early  'z' s" abc" found-early -> 2 -1 1 -1 }T
: by-threes ( n -- s )  0 swap 0 do  i +  3 +loop ;                   \ (+loop: its index from the cell each time)
: downward ( -- a b c )  0 2 do  i  -1 +loop ;
T{ 10 by-threes  downward -> 18  2 1 0 }T
: top-pushed ( -- s )  0  3 0 do  100 >r  i +  r> drop  i +  loop ;   \ (`i` is the return stack's top: after >r,
T{ top-pushed -> 303 }T                                               \  what was pushed - as it always was)
: counted-down ( n -- s )  0 swap 0 do  i  begin  dup 0> while  1- swap 1+ swap  repeat  drop  i +  loop ;
T{ 4 counted-down -> 12 }T                                            \ (a begin loop inside: jumped back to)
: one-way ( n -- s )  0 swap 0 do  i 1 and if  i inner-sum +  else  i +  then  i +  loop ;   \ (a call down one way of
T{ 6 one-way -> 34 }T                                                 \  an if: after it the registers read again)
: limits ( -- a b )  0  9223372036854775807 dup 2 - do  i 0> +  loop   0  -9223372036854775806 -9223372036854775808 do  i 0< +  loop ;
T{ limits -> -2 -2 }T                                                 \ (at the largest number, and the smallest)
: i-alone ( -- n )  i ;          : j-alone ( -- n )  3 0 do  j  loop ;     \ (no loop, or not two: said as ever)
T{ ' i-alone catch 0<>  error-message s" i: not in a do loop" s=
   ' j-alone catch 0<>  error-message s" j: not in a nested do loop" s= -> true true true true }T
: deep-loops ( n -- )  dup 0= if  drop exit  then  3 0 do  dup 1- recurse  loop  drop ;   \ (the return stack full
T{ 2 deep-loops  depth -> 0 }T                                        \  as a loop begins: the primitive's error)
: loops-forever ( -- )  1 0 do  recurse  loop ;
T{ ' loops-forever catch 0<>  error-message s" return stack overflow" s= -> true true }T

testing W^X: no memory writable and runnable at once - machine code read and run only, made writable just to be
testing  written (where the system lists its mappings: Linux's /proc/self/maps)
: rwx-mappings ( -- n )   \ (-1: no list to read)
    s" /proc/self/maps" file-exists? 0= if  -1 exit  then
    s" /proc/self/maps" read-file {: a n | k :}
    begin  a n s"  rwx" s-find dup 0>= while  1+ {: i :}  k 1+ to k  a i + to a  n i - to n  repeat  drop k ;
: later-word ( n -- n )  dup + ;   \ (one more translated after the check's own: still none)
T{ rwx-mappings 0>  3 later-word -> false 6 }T

testing within on a full stack: an overflow, as the prelude's own (its `over` has no room) - in line it had none
: fill-up ( -- )  253 0 do  0  loop ;
: within-full ( -- flag )  fill-up  5 0 9 within ;
: overflowed? ( -- flag )  depth {: d :}  ['] within-full catch 0<>  begin  depth d 1+ >  while  nip  repeat ;
T{ overflowed?  error-message s" stack overflow" s-find 0>= -> true true }T

testing a variable's address, a constant's and a value's number, a string's text - in line, no call: as called,
testing  through a run, written into a caller, a value set between, and on a full stack (an overflow, as the call's)
variable nv   12 constant nc   3 value nq   create nbuf 16 allot
: data-mix ( -- n )  7 nv !  nv @ nc + nq + ;
T{ data-mix -> 22 }T
: set-q ( n -- m )  to nq  nq 1+ ;
T{ 40 set-q  nq -> 41 40 }T
: text-of ( -- a n )  s" in line" ;
: text-lens ( -- n )  s" four" nip  s" xy" nip + ;
T{ text-of s" in line" compare  text-lens -> 0 6 }T
: inner-data ( -- n )  nc nq + ;
: outer-data ( -- n )  inner-data inner-data + ;
T{ 3 to nq  outer-data  5 to nq  outer-data -> 30 34 }T
: buf-use ( -- n )  65 nbuf c!  66 nbuf 15 + c!  nbuf c@  nbuf 15 + c@ + ;
T{ buf-use -> 131 }T
: const-full ( -- n )  fill-up  0 0 0  nc ;
: var-full ( -- a )  fill-up  0 0 0  nv ;
: text-full ( -- a n )  fill-up  0 0  s" x" ;
: overflows? ( xt -- flag )  depth {: d :}  catch 0<>  begin  depth d >  while  nip  repeat ;
T{ ' const-full overflows?  ' var-full overflows?  ' text-full overflows? -> true true true }T
task-variable ntv   \ (a task-variable is not such a word: each task's own cell, found as it runs)
: task-cell ( n -- m )  ntv !  ntv @ 1+ ;
T{ 8 task-cell  ntv @  ntv ' ntv >body <> -> 9 8 true }T

testing numbers worked out as the code is written (a literal and the words that work on it alone): what the
testing  words give - sums that wrap, flags, one after another - and no more than that: a number a branch comes
testing  to, one of the stack's, are left; a full stack overflows as the words would have made it
: f-sum ( -- n )  2 3 + ;                : f-chain ( -- n )  2 3 + 4 * 1- ;
: f-cells ( -- n )  3 cells ;            : f-index ( a -- a' )  2 cells + ;
: f-wrap ( -- n )  -1 1 rshift 1+ ;      : f-wrap2 ( -- n )  9223372036854775807 1 + ;
: f-flags ( -- a b c d )  2 3 <  3 2 <  -1 1 u<  5 5 = ;
: f-ones ( -- a b c d e )  5 negate  0 invert  0 0=  -3 0<  7 1- 1+ ;
: f-bits ( -- a b c )  12 10 and  12 10 or  12 10 xor ;
: f-zero ( -- a b c d )  0 0<>  1 0>  0 0<=  -1 0>= ;
: f-under ( n -- m )  10 3 - * ;         \ (the 7 worked out, the * the stack's)
: f-branch ( flag -- n )  if  2  else  3  then  4 + ;   \ (4 + is of either: not worked out)
: f-cmp ( n -- flag )  2 3 + < ;
T{ f-sum  f-chain  f-cells  10 f-index -> 5 19 3 cells 10 2 cells + }T
T{ f-wrap  f-wrap2  f-wrap f-wrap2 = -> -1 1 rshift 1+  dup  true }T
T{ f-flags -> true false false true }T
T{ f-ones -> -5 -1 true true 7 }T
T{ f-bits  f-zero -> 8 14 6  false true true false }T
T{ 6 f-under  true f-branch  false f-branch  4 f-cmp  5 f-cmp -> 42 6 7 true false }T
: f-full ( -- n )  fill-up  0 0  2 3 + ;      \ (two literals: one cell more than there is)
: f-one-full ( -- n )  fill-up  0 0 0  3 cells ;   \ (a full stack, and one)
T{ ' f-full overflows?  ' f-one-full overflows? -> true true }T

test-summary

\ the c vocabulary: a C library loaded, its functions called - each kind of argument and result it knows, and what it
\ refuses. (The library is tests/foreign/clanktest.c, built beside the tool; a build without a loader skips the file.)
IN: test-c
USING: tester actors system c ;

\ the test library, by the name this system gives one (the tests run where it was built)
: opens? ( a n -- lib true | false )  {: a n :}  [: a n c-library ;] catch if  false  else  true  then ;
: test-library ( -- lib | 0 )
    s" ./libclanktest.so" opens? if  exit  then
    s" ./libclanktest.dylib" opens? if  exit  then
    s" libclanktest.dll" opens? if  exit  then
    s" clanktest.dll" opens? if  exit  then
    0 ;
test-library constant lib
: without-it ( -- )  lib 0= if  test-summary skip-file  then ;
without-it

testing a function called: whole numbers in, an int out - its 32 bits with their sign
lib FUNCTION: add2 ( a b -- int )
T{ 3 4 add2  -7 2 add2  $80000000 0 add2 -> 7 -5 -2147483648 }T   \ (an int argument: the cell's low 32 bits)
: in-a-definition ( n -- n' )  1 add2 2 add2 ;   \ (the checker has its effect: two in, one out)
T{ 10 in-a-definition -> 13 }T
testing each kind of result: uint (no sign), cell (all 64 bits), none
lib FUNCTION: high_bit ( -- uint )
lib FUNCTION: big ( -- cell )
lib FUNCTION: set_flag ( x -- )
lib FUNCTION: get_flag ( -- int )
T{ high_bit  big -> $fffffff0 $123456789abcdef0 }T
T{ 77 set_flag  get_flag  -1 set_flag  get_flag -> 77 -1 }T
testing eight arguments, each in its own place
lib FUNCTION: sum8 ( a b c d e f g h -- cell )
T{ 1 1 1 1 1 1 1 1 sum8  1 0 0 0 0 0 0 0 sum8  0 0 0 0 0 0 0 1 sum8  0 0 1 0 0 0 0 0 sum8 -> 36 1 8 3 }T

testing text: a string object is C's text as it is (a 0 after it); C's text comes back by z>string
lib FUNCTION: length_of ( z -- int )
lib FUNCTION: greeting ( -- cell )
: length-of ( a n -- n' )  >string drop {: s :}  s length_of  s dispose ;
T{ s" hello" length-of  s" " length-of -> 5 0 }T
: greets? ( -- flag )  greeting z>string {: s n :}  s n s" hello from C" s=  s dispose ;
T{ objects  greets?  swap objects = -> true true }T
T{ 0 ' z>string catch nip  error-message s" null address" s= -> -1 true }T

testing memory C writes: a cell it is given the address of, bytes filled, a structure read
lib FUNCTION: divide ( a b quotient* remainder* -- int )
variable quotient   variable remainder
T{ 17 5 quotient remainder divide  quotient @  remainder @ -> 0 3 2 }T
T{ 1 0 quotient remainder divide -> -1 }T
lib FUNCTION: fill_bytes ( addr n value -- )
create filled 8 allot   filled 8 0 fill
T{ filled 1+ 3 $ab fill_bytes  filled c@  filled 1+ c@  filled 3 + c@  filled 4 + c@ -> 0 $ab $ab 0 }T
lib FUNCTION: point_sum ( point* -- int )
create point  2 4 * allot   \ (two 32-bit fields)
T{ 30 point l!  -8 point 4 + l!  point point_sum -> 22 }T

testing c-symbol and c-function: a function by its address under a name of one's own; a variable's address
s" add2" lib c-symbol c-function plus ( a b -- int )
T{ 20 22 plus -> 42 }T
T{ s" answer" lib c-symbol sl@ -> 42 }T

testing LIBRARY: the same library opened by its path, under a name
: its-path ( -- a n )   \ (as test-library found it)
    s" ./libclanktest.so" opens? if  drop s" ./libclanktest.so" exit  then
    s" ./libclanktest.dylib" opens? if  drop s" ./libclanktest.dylib" exit  then
    s" libclanktest.dll" opens? if  drop s" libclanktest.dll" exit  then
    s" clanktest.dll" ;
: open-again ( -- )  its-path s" LIBRARY: again {s}" format {: s n :}  s n evaluate  s dispose ;
open-again
T{ again lib <>  s" add2" again c-symbol  s" add2" lib c-symbol = -> true true }T   \ (another number, the same library)

testing what is refused: no such library, no such function, a number that is no library's, a result of no kind, nine
testing  arguments, a comment not there
: says? ( a n -- flag )  error-message 2swap s-find 0>= ;
T{ s" ./no-such-library-here.so" ' c-library catch nip nip  s" c-library: can't open ./no-such-library-here.so" says? -> -1 true }T
T{ s" lib FUNCTION: no_such_function ( -- int )" ' evaluate catch nip nip  s" the library has no no_such_function" says? -> -1 true }T
T{ s" 999 FUNCTION: add2 ( a b -- int )" ' evaluate catch nip nip  s" not a library" says? -> -1 true }T
T{ s" add2" 0 ' c-symbol catch nip nip nip  s" not a library" says? -> -1 true }T
T{ s" lib FUNCTION: add2 ( a b -- float )" ' evaluate catch nip nip  s" its result is int, uint" says? -> -1 true }T
T{ s" lib FUNCTION: add2 ( a b -- int int )" ' evaluate catch nip nip  s" its result is int, uint" says? -> -1 true }T
T{ s" lib FUNCTION: sum8 ( a b c d e f g h i -- cell )" ' evaluate catch nip nip  s" 9 arguments (8 at most)" says? -> -1 true }T
T{ s" lib FUNCTION: add2 a b" ' evaluate catch nip nip  s" its stack comment expected" says? -> -1 true }T
T{ s" lib FUNCTION: add2 ( a b )" ' evaluate catch nip nip  s" needs -- and )" says? -> -1 true }T

testing SLOW-FUNCTION: called where it stands at the prompt; in a handler on a worker thread - the actor waits, the
testing  node's other actors go on meanwhile (a node without threads calls it there too: nothing to see)
lib SLOW-FUNCTION: slow_add ( a b -- int )
T{ 2 3 slow_add -> 5 }T
message work ( -- )
message ping ( -- )
variable result   variable worked   variable pinged   variable busy-when-pinged
behaviour worker  on work ( -- )  40 2 slow_add result !  true worked ! ;  end-behaviour
behaviour pinger  on ping ( -- )  true pinged !  worked @ 0= busy-when-pinged ! ;  end-behaviour
worker -1 s" worker" spawn constant w
pinger -1 s" pinger" spawn constant p
: settle ( -- )  3000 0 do  deliver  worked @ if  leave  then  1 sleep-ms  loop ;
w send work  deliver  p send ping  deliver
: with-threads? ( -- flag )  node 0<> ;
T{ pinged @  busy-when-pinged @ with-threads? = -> true true }T
settle
T{ worked @  result @ -> true 42 }T

test-summary

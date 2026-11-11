\ a script's mistakes: errors it can catch, never a crash or a dictionary spoiled
IN: test-errors
USING: tester ;

-1 1 rshift constant largest   largest negate 1- constant smallest

testing a word defined while another is compiled: an error (its header would split the code)
: inside  s" : outer [ 5 constant inner ] 1 ;" evaluate ;
T{ ' inside catch -> -1 }T
T{ s" outer" find-word  s" inner" find-word -> 0 0 }T
: after-it 2 ;
T{ after-it -> 2 }T

testing allot: back down to the dictionary's start, no further; up to its end, no further
T{ here  16 allot  -16 allot  here = -> true }T
: below-start  here negate allot ;
T{ ' below-start catch -> -1 }T
: past-end  unused 1+ allot ;
T{ ' past-end catch -> -1 }T
T{ unused 0> -> true }T

testing a fetch reaching past the dictionary's end
: last-cell  here unused + 1- @ ;
T{ ' last-cell catch -> -1 }T
T{ here unused + 1- c@ drop -> }T

testing strings: a number for the address, or a negative length, is an error
: type-null  0 5 type ;
T{ ' type-null catch -> -1 }T
: eval-negative  s" 1" drop -1 evaluate ;
T{ ' eval-negative catch -> -1 }T
: compare-negative  s" ab" drop -1 s" ab" compare ;
T{ ' compare-negative catch -> -1 }T
: compare-null  0 3 s" abc" compare ;
T{ ' compare-null catch -> -1 }T
: find-negative  s" dup" drop -2 find-word ;
T{ ' find-negative catch -> -1 }T
T{ 0 0 type  s" " s" " compare -> 0 }T

testing an execution token that is a number
T{ 0 ' execute catch -> 0 -1 }T
T{ 5 ' execute catch -> 5 -1 }T
T{ 7 ' catch catch -> 7 -1 }T

testing an execution token that is an address, but not a word's
: here-run  here execute ;
T{ ' here-run catch -> -1 }T
: mid-word  ['] dup cell+ execute ;
T{ ' mid-word catch -> -1 }T
create not-code  4 cells allot
: data-run  not-code execute ;
T{ ' data-run catch -> -1 }T
T{ 3 ' dup execute  :noname 4 ; execute  [: 5 ;] execute -> 3 3 4 5 }T

testing a word given back with its space is a word no more
: gone-run ( -- )  here {: was :}  s" : gone 1 ;" evaluate  s" gone" find-word  was here - allot  execute ;
T{ ' gone-run catch -> -1 }T

testing a deferred word set to what isn't a word
defer hook
: hook-number  5 ['] hook >body !  hook ;
T{ ' hook-number catch -> -1 }T
: hook-data  not-code ['] hook >body !  hook ;
T{ ' hook-data catch -> -1 }T
T{ ' noop is hook  hook -> }T

testing the compiler's own words: not typed, nor executed (they read the cells after them)
: typed-lit  s" (lit)" evaluate ;
T{ ' typed-lit catch -> -1 }T
: run-branch  ['] branch execute ;
T{ ' run-branch catch -> -1 }T
: uses-them ( flag -- n )  if 1 else 2 then ;
T{ true uses-them  false uses-them -> 1 2 }T

testing a float too large for a cell: the largest, the smallest; not a number: 0
T{ 1e30 f>s  -1e30 f>s -> largest smallest }T
T{ 1e 0e f/ f>s  -1e 0e f/ f>s  0e 0e f/ f>s -> largest smallest 0 }T
T{ 1.9e f>s  -1.9e f>s  9.2e18 f>s 9200000000000000000 = -> 1 -1 true }T
T{ 1e30 0e 0e f/ -2.5e 3f>s -> largest 0 -2 }T

testing a base that isn't one counts as ten
T{ 1 base !  10 base !  base @ -> 10 }T
T{ 99 base !  255 n>s  s" 255" s=  10 base ! -> true }T
T{ 0 base !  s" 12" string>number  10 base ! -> 12 true }T

testing shifts by the cell's width or more: every bit goes; arshift keeps the sign
T{ 1 64 lshift  1 64 rshift  1 -1 lshift -> 0 0 0 }T
T{ -8 1 arshift  -1 64 arshift  largest 64 arshift  -1 1 rshift largest = -> -4 -1 0 true }T

testing division by -1: the smallest wraps round to itself, as negate does
T{ smallest -1 /  smallest -1 mod  7 -1 / -> smallest 0 -7 }T
T{ smallest -1 /mod -> 0 smallest }T

testing */ with a product past a cell
T{ 1000000000000 1000000000000 1000000 */ -> 1000000000000000000 }T
T{ largest 2 2 */ -> largest }T

testing literals: any cell's bits, but not more
T{ $ffffffffffffffff  -9223372036854775808  9223372036854775807 -> -1 smallest largest }T
: too-large  s" 99999999999999999999" evaluate ;
T{ ' too-large catch -> -1 }T

testing a definition an error leaves unfinished is dropped, its space given back
: unfinished  s" : half 1 nosuchword ;" evaluate ;
T{ here latest  ' unfinished catch  -rot  latest =  swap here = -> -1 true true }T
T{ s" half" find-word -> 0 }T

testing jump from a word with locals: an error as it compiles
: jump-locals  s" : jl {: a :} ['] noop jump ;" evaluate ;
T{ ' jump-locals catch -> -1 }T

testing a vocabulary's name keeps its file in the scripts' folders
: using-slash  s" USING: ../tests/vocabs/va ;" evaluate ;
T{ ' using-slash catch -> -1 }T
: using-empty  s" USING: vocabs..va ;" evaluate ;
T{ ' using-empty catch -> -1 }T

testing cells and floats wrap round, as products do; a slice's address too
T{ largest cells  largest floats -> -8 -8 }T
T{ largest largest 5 5 s-slice nip -> 5 }T

testing a wait longer than there are frames: the task waits, the count doesn't wrap round to the past
variable woke
: waits-for-ever ( -- )  largest wait  true woke ! ;
T{ ' waits-for-ever start-task  3 tick-tasks  stop-task  woke @ -> false }T

testing an error caught inside { ... }: the list it had begun isn't left open
: half-list ( -- )  { 1 0 @ } drop ;
: half-lists ( -- )  20 0 do  ['] half-list catch drop  loop ;
T{ half-lists  { 1 2 } length -> 2 }T

testing abort" given a number for its text
: abort-number ( -- )  true 5 5 (abort") ;
T{ ' abort-number catch -> -1 }T
: aborts ( -- )  true abort" stop here" ;
T{ ' aborts catch  error-message s" stop here" s= -> -1 true }T

testing a folder included: an error, as a file that isn't there
: include-folder ( -- )  s" ." included ;
T{ ' include-folder catch -> -1 }T

test-summary

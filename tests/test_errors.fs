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

test-summary

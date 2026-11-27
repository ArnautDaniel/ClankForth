\ a script's mistakes: errors it can catch, never a crash or a dictionary spoiled
IN: test-errors
USING: tester frames ;

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

testing >body of a word with no data - a primitive, a colon definition - is an error (to, is: not the next header)
T{ ' dup ' >body catch nip -> -1 }T
T{ 1 s" to ." ' evaluate catch nip nip nip -> -1 }T
: a-colon ( -- )  ;
T{ ' a-colon ' >body catch nip -> -1 }T
5 value a-value
T{ 7 to a-value  a-value -> 7 }T

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

testing a task's failing evaluate: its text gone with it, not the interpreter's to go on with
variable after-eval
: evals-and-fails ( -- )  s" 1 0 / 1 after-eval !" evaluate ;
T{ 0 after-eval !  ' evals-and-fails start-task drop  1 tick-tasks  after-eval @ -> 0 }T
testing resume: a task whose turn is under way further out isn't run again inside it
variable a-task  variable b-task  variable a-turns
: b-body ( -- )  a-task @ resume drop ;
: a-body ( -- )  1 a-turns +!  b-task @ resume drop ;
' b-body start-held-task b-task !
T{ 0 a-turns !  ' a-body start-task a-task !  1 tick-tasks  a-turns @ -> 1 }T
testing stop-task by a task it resumed: it stays stopped, a wait after doesn't bring it back
variable c-task  variable d-task  variable c-after
: d-body ( -- )  c-task @ stop-task ;
: c-body ( -- )  d-task @ resume drop  1 c-after !  1 wait  2 c-after ! ;
' d-body start-held-task d-task !
T{ 0 c-after !  ' c-body start-task c-task !  3 tick-tasks  c-after @ -> 0 }T
testing a task stopping itself inside with-scratch: its region ended
: stops-in-scratch ( -- )  [: { 1 } drop  me stop-task ;] with-scratch ;
T{ objects  ' stops-in-scratch start-task drop  2 tick-tasks  objects - -> 0 }T
testing a task waiting inside with-scratch: what it made kept while it waits, freed once it goes on to the end
variable scratch-list
: waits-in-scratch ( -- )  [: { 1 2 } scratch-list !  2 wait  scratch-list @ length drop ;] with-scratch ;
T{ objects  ' waits-in-scratch start-task drop  1 tick-tasks  objects swap - -> 1 }T
T{ objects  3 tick-tasks  objects - -> 1 }T
testing a task stopped while it waits inside with-scratch: its region ended
variable long-task
: waits-long ( -- )  [: { 1 } drop  100 wait ;] with-scratch ;
T{ objects  ' waits-long start-task long-task !  1 tick-tasks  long-task @ stop-task  1 tick-tasks  objects - -> 0 }T
testing an error inside with-scratch caught: the regions inside the catch ended, the one round it kept
: catch-in-scratch ( -- n )
    [: objects {: before :}  { 1 } drop  [: [: { 2 } drop  0 @ drop ;] with-scratch ;] catch drop  objects before - ;]
    with-scratch ;
T{ objects  catch-in-scratch  objects rot - -> 1 0 }T

testing throw: catch gives its value; 0 throws nothing; a string object is the message
: throws-42 ( -- )  42 throw ;
T{ ' throws-42 catch  error-message s" thrown: 42" compare -> 42 0 }T
: throws-text ( -- )  s" no such file" >string drop throw ;
T{ ' throws-text catch  drop  error-message s" no such file" compare -> 0 }T
: throws-none ( -- n )  0 throw 7 ;
T{ ' throws-none catch -> 7 0 }T
: fails ( -- )  0 @ drop ;
T{ ' fails catch -> -1 }T   \ (the system's own errors: -1, as ever)
testing ;] with no [: before it: an error (it took what was on the stack for a quotation - found by the fuzzer)
T{ s" 0 0 : semi-alone ;] ;" ' evaluate catch nip nip -> -1 }T
T{ error-message s" ;]: no [: to end" s= -> true }T
T{ s" semi-alone" find-word -> 0 }T
testing a definition with a cell laid down by hand that is no word: checked, shown (found by the fuzzer)
: by-hand ( -- )  [ 41 , ] ;
T{ [: ['] by-hand see-xt ;] with-output string  s" [ 41 , ]" s-find 0>= -> true }T
testing a compiler's word that takes operands named in a definition: an error (nothing lays its operands down)
T{ s" : named-local! 1 (local!) ;" ' evaluate catch nip nip -> -1 }T
T{ s" : named-branch branch ;" ' evaluate catch nip nip -> -1 }T
T{ s" named-local!" find-word -> 0 }T

testing exit and leave given to execute: an error, the same in threaded code and machine code (they ended the
testing  word that ran them in the one, and went nowhere - a return stack underflow - in the other)
: runs-exit ( -- n )  1 ['] exit execute 2 ;
: runs-leave ( -- n )  5 0 do  i 2 = if  ['] leave execute  then  loop  99 ;
T{ ' runs-exit catch 0<>  error-message s" ends the definition" s-find 0>= -> true true }T
T{ ' runs-leave catch 0<>  error-message s" ends the definition" s-find 0>= -> true true }T
testing a word that takes its caller's return address (r> ... >r, r> drop): left threaded - run from machine code
testing  it is an error said, or right; it was a wrong answer (the change passed over), or a crash
: skip-one ( -- )  r> cell+ >r ;
: skipper ( -- n )  5 skip-one negate 10 + ;
: said-or-right? ( xt n -- flag )  {: xt n :}  xt catch if  error-message s" return address" s-find 0>=  else  n =  then ;
T{ ' skipper 15 said-or-right? -> true }T
: lit-after ( -- n )  r> dup cell+ >r @ ;
: lit-user ( -- n )  lit-after [ 42 , ] 1+ ;
T{ ' lit-user 43 said-or-right? -> true }T

testing a definition with something open: an error at ; or ;] (it ran to a null address, or wrote where the stack said)
: refused? ( a n -- flag )  ['] evaluate catch dup if nip nip then 0<> ;
T{ s" : open-if 0 if 2 ;" refused? -> true }T
T{ s" : open-quote [: 1 ;" refused? -> true }T
T{ s" : quote-stack [: [ 99 ] ;] ;" refused? -> true }T
T{ s" : quote-swap [: [ swap drop 70000 swap ] ;] ;" refused? -> true }T
T{ s" : fine [: 1 ;] drop ;" refused? -> false }T
T{ s" open-if" find-word  s" quote-swap" find-word -> 0 0 }T

testing deferred words set to each other: an error, not a C stack's worth of calls (nor a hang)
defer looping   ' looping is looping
defer ping-d  defer pong-d   ' pong-d is ping-d  ' ping-d is pong-d
T{ ' looping catch  ' ping-d catch -> -1 -1 }T

testing allot gives back no further than the system's own words
: wipe ( -- )  ['] dip here - allot ;   \ (back to a word of the prelude's)
T{ here  ' wipe catch  here rot = -> -1 true }T

testing an unknown word: the vocabularies it is in said (loaded, or the library's not yet) - USING: one of them
: unknown-says ( text n -- a n )  ['] evaluate catch drop  2drop  error-message ;
T{ s" : uses-sql ( -- )  0 0 0 0 sql-query drop ;" unknown-says  s" in vocabulary sql: add it to USING:" s-find 0>= -> true }T
T{ s" : uses-nothing ( -- )  no-such-word-anywhere ;" unknown-says  s" in vocabulary" s-find -> -1 }T

test-summary

\ stack effects: `;` checks a definition's code against its stack comment, and warns where they differ
IN: test-effects
USING: tester ;

\ the warnings the text gives as it is read
: warns ( addr len -- n )  warnings {: before :}  evaluate  warnings before - ;

testing code that does what its comment says: no warning
T{ s" : sq ( n -- n*n ) dup * ;" warns -> 0 }T
T{ s" : pick-if ( flag -- n ) if 1 else 2 then ;" warns -> 0 }T
T{ s" : sum-to ( n -- sum ) 0 swap 0 ?do i + loop ;" warns -> 0 }T
T{ s" : early ( a -- b ) dup 0< if exit then 1+ ;" warns -> 0 }T
T{ s" : leaves ( n -- ) 0 ?do i 3 = if leave then loop ;" warns -> 0 }T
T{ s" : fact ( n -- n! ) dup 1 > if dup 1- recurse * then ;" warns -> 0 }T
T{ s" : choose ( n -- m ) case 1 of 10 endof 2 of 20 endof 0 swap endcase ;" warns -> 0 }T
T{ s" : with-locals ( a b -- c ) {: a b :} a b + ;" warns -> 0 }T
T{ s" : under ( a b -- a b a ) 1 pick ;" warns -> 0 }T
T{ s" : floats-too ( F: a b -- c ) f+ ;" warns -> 0 }T
T{ s" : both ( n -- flag ) ( F: x -- ) s>f f< ;" warns -> 0 }T
T{ warnings  : named ( "name" -- ) create ;  warnings swap - -> 0 }T
T{ s" : tail-call ( a -- b ) ['] 1+ jump ;" warns -> 0 }T
T{ s" : quoted ( a -- b ) [: 1+ ;] execute ;" warns -> 0 }T
T{ s" : caught ( -- flag ) [: ;] catch ;" warns -> 0 }T

testing code that doesn't: a warning, the word still made
T{ s" : takes-more ( a b -- c ) + + ;" warns -> 1 }T
T{ s" : leaves-more ( a -- ) dup ;" warns -> 1 }T
T{ s" : uneven-if ( a -- b ) if 1 else 1 2 then ;" warns -> 1 }T
T{ s" : reaches-under ( a -- a x ) 1 pick ;" warns -> 1 }T
T{ s" : float-left ( F: a -- ) fdup ;" warns -> 1 }T
T{ s" : quotation-off ( a -- ) [: 1+ ;] execute ;" warns -> 1 }T
T{ 2 3 4 takes-more -> 9 }T

testing the return stack: what a word puts there it takes back before it returns
T{ s" : kept ( a b -- b a ) >r 1+ r> swap ;" warns -> 0 }T
T{ s" : peeked ( a -- a a ) >r r@ r> ;" warns -> 0 }T
T{ s" : dropped ( a -- ) >r rdrop ;" warns -> 0 }T
T{ s" : both-ways ( a f -- ) swap >r if r> drop exit then r> drop ;" warns -> 0 }T
T{ s" : stranded ( a -- ) >r ;" warns -> 1 }T
T{ s" : stranded-bare 5 >r ;" warns -> 1 }T
T{ s" : one-way ( a f -- ) swap >r if exit then r> drop ;" warns -> 1 }T
T{ s" : in-quotation ( -- xt ) [: 5 >r ;] ;" warns -> 1 }T
T{ s" : framed ( a -- ) {: a :} a >r ;" warns -> 0 }T   \ (its frame dropped, and what was over it)
T{ 7 framed -> }T

testing an exit inside a do loop: unloop first
T{ s" : found ( n -- flag ) 0 ?do i 3 = if unloop true exit then loop false ;" warns -> 0 }T
T{ s" : lost ( n -- flag ) 0 ?do i 3 = if true exit then loop false ;" warns -> 1 }T
T{ s" : lost-bare 10 0 do exit loop ;" warns -> 1 }T
T{ s" : framed-loop ( n -- flag ) {: n :} n 0 ?do i 3 = if true exit then loop false ;" warns -> 0 }T
T{ 9 found  2 found  9 framed-loop  2 framed-loop -> true false true false }T

testing ( -- x true | false ): its two ways, and a caller's if taking them apart
T{ s" : maybe ( x -- x true | false ) dup 0> if true else drop false then ;" warns -> 0 }T
T{ s" : maybe-or-0 ( x -- y ) maybe if else 0 then ;" warns -> 0 }T
T{ s" : maybe-bad ( x -- y ) maybe if then ;" warns -> 1 }T
T{ s" : quiet-dup ( x -- ) ?dup if drop then ;" warns -> 0 }T
T{ 5 maybe-or-0  -5 maybe-or-0 -> 5 0 }T

testing a word without a comment: its effect worked out, its callers checked with it
: doubled  dup + ;
T{ ' doubled stack-effect -> 1 1 0 0 1 }T
T{ s" : doubled-wrong ( a -- ) doubled ;" warns -> 1 }T

testing words made by a defining word: their DOES> code's effect
T{ warnings  : konst ( n "name" -- ) create , does> @ ;  warnings swap - -> 0 }T
5 konst five
T{ ' five stack-effect -> 0 1 0 0 1 }T
T{ s" : five-dropped ( -- ) five ;" warns -> 1 }T
T{ s" : five-used ( -- n ) five 1+ ;" warns -> 0 }T

testing ( F: x -- flag ): the inputs floats, the result on whichever stack the code has it
T{ s" : positive? ( F: x -- flag ) f0< 0= ;" warns -> 0 }T
T{ ' positive? stack-effect -> 0 1 1 0 1 }T
T{ s" : halved ( F: x -- y ) 0.5e f* ;" warns -> 0 }T
T{ ' halved stack-effect -> 0 0 1 1 1 }T
T{ s" : two-left ( F: x -- flag ) f0< 0 ;" warns -> 1 }T

testing what isn't checked: loops' turns taking items, immediate words, tokens from the stack
T{ s" : stores ( a b c addr -- ) 3 0 do tuck ! cell+ loop drop ;" warns -> 0 }T
T{ s" : grows-each-turn ( n -- ) 0 ?do i loop ;" warns -> 0 }T
T{ s" : takes-each-turn ( a b c flag -- ) if 3 0 do drop loop else 2drop drop then ;" warns -> 0 }T
T{ s" : compiles-drop ( x -- ) postpone drop ; immediate" warns -> 0 }T
T{ s" : runs-it ( x xt -- y ) execute ;" warns -> 0 }T
T{ ' runs-it stack-effect nip nip nip nip -> 4 }T
T{ s" : each-token ( l xt -- x ) each ;" warns -> 0 }T
T{ s" : varies ( x -- .. ) dup ;" warns -> 0 }T

testing a deferred word's stack comment, right after its name: its callers checked with it
defer hook-a ( x -- y )
defer hook-b   \ (no comment: not checked)
defer hook-c  ' noop is hook-c  ( x -- y y )   \ (not right after its name: a comment only)
T{ s" : calls-a ( x -- y ) hook-a ;" warns -> 0 }T
T{ s" : calls-a-wrong ( x -- ) hook-a ;" warns -> 1 }T
T{ s" : calls-b ( x -- ) hook-b ;" warns -> 0 }T
T{ s" : calls-c ( x -- ) hook-c ;" warns -> 0 }T
T{ ' hook-a stack-effect drop 2drop -> 1 1 }T

testing a C word's effect, as a host says it
T{ ' dup stack-effect -> 1 2 0 0 1 }T
T{ ' f@ stack-effect -> 1 0 0 1 1 }T
T{ ' restart stack-effect nip nip nip nip -> 5 }T

testing a list's words right after their quotation: checked, as the walk they are
T{ s" : each-dropped ( l -- x ) [: drop ;] each ;" warns -> 1 }T
T{ s" : summed ( l -- n ) 0 swap [: + ;] each ;" warns -> 0 }T
T{ ' summed stack-effect -> 1 1 0 0 1 }T
T{ s" : squares ( l -- l' ) [: dup * ;] map ;" warns -> 0 }T
T{ s" : map-two ( l -- l' ) [: dup ;] map ;" warns -> 1 }T
T{ s" : evens ( l -- l' ) [: 2 mod 0= ;] filter ;" warns -> 0 }T
T{ s" : filter-none ( l -- l' ) [: drop ;] filter ;" warns -> 1 }T
T{ s" : total ( l -- n ) 0 [: + ;] reduce ;" warns -> 0 }T
T{ s" : total-short ( l -- n ) [: + ;] reduce ;" warns -> 1 }T
T{ s" : first-big ( l -- x true | false ) [: 3 > ;] find ;" warns -> 0 }T
T{ s" : has-big ( l -- flag ) [: 3 > ;] find if drop true else false then ;" warns -> 0 }T
T{ s" : has-big-leaky ( l -- flag ) [: 3 > ;] find if true else false then ;" warns -> 1 }T
T{ s" : spills ( l -- .. ) [: ;] each ;" warns -> 0 }T

testing a slot that holds words of a declared effect: a word fetched from it and run is followed
variable step  holds: ( -- )
variable on-hit  holds: ( damage -- )
: idle ( -- ) ;
: leaky ( -- n )  1 ;
: hurt ( damage -- )  drop ;
T{ s" : frame ( -- ) step @ execute ;" warns  ' frame stack-effect -> 0  0 0 0 0 1 }T
T{ s" : frame? ( -- ) step @ ?dup if  execute  then ;" warns  ' frame? stack-effect -> 0  0 0 0 0 1 }T
T{ s" : hit ( damage -- ) on-hit @ execute ;" warns  ' hit stack-effect -> 0  1 0 0 0 1 }T
T{ s" : hit-short ( -- ) on-hit @ execute ;" warns -> 1 }T
T{ s" : frame-local ( -- ) step @ {: xt :}  xt execute ;" warns  ' frame-local stack-effect -> 0  0 0 0 0 1 }T
T{ ' idle step !  frame  frame? -> }T
testing a record's field and a deferred word: slots too
begin-structure /walk  field: walk.xt  holds: ( -- )  end-structure
create a-walk /walk allot
T{ s" : run-walk ( -- ) a-walk walk.xt @ execute ;" warns  ' run-walk stack-effect -> 0  0 0 0 0 1 }T
defer on-step ( n -- )
T{ s" : sets-hook ( -- ) ['] hurt is on-step ;" warns -> 0 }T
T{ s" : sets-hook-wrong ( -- ) ['] leaky is on-step ;" warns -> 1 }T
T{ s" ' hurt is on-step" warns  s" ' idle is on-step" warns  5 on-step -> 0 1 5 }T   \ (said; set all the same)

testing a word stored in a slot: one of another effect is a warning, whatever the definition's comment
T{ s" : store-ok ( -- ) ['] idle step ! ;" warns -> 0 }T
T{ s" : store-leaky ( -- ) ['] leaky step ! ;" warns -> 1 }T
T{ s" : store-quoted ( -- ) [: 1 ;] step ! ;" warns -> 1 }T
T{ s" : store-quoted-ok ( -- ) [: 1 drop ;] step ! ;" warns -> 0 }T
T{ s" : store-walk ( -- ) ['] leaky a-walk walk.xt ! ;" warns -> 1 }T
T{ s" : store-number ( -- ) 0 step ! ;" warns -> 0 }T   \ (not a word's: not the checker's to say)
T{ s" : store-from-stack ( xt -- ) step ! ;" warns -> 0 }T

testing a word that stores its input in a slot: its callers' words are checked - through another such word, and locals
: step! ( xt -- )  step !  0 on-hit ! ;
: act! ( xt n -- )  {: xt n :}  n drop  xt step! ;
T{ s" : via ( -- ) ['] idle step! ;" warns -> 0 }T
T{ s" : via-leaky ( -- ) ['] leaky step! ;" warns -> 1 }T
T{ s" : via-two ( -- ) ['] hurt 3 act! ;" warns -> 1 }T
T{ s" : via-two-ok ( -- ) ['] idle 3 act! ;" warns -> 0 }T
T{ s" : passes-on ( xt -- ) step! ;  : far ( -- ) ['] leaky passes-on ;" warns -> 1 }T
testing a local stored twice isn't known for what it first held
T{ s" : twice ( xt -- ) {: xt :}  ['] idle to xt  xt step ! ;  : uses-twice ( -- ) ['] leaky twice ;" warns -> 0 }T

testing a token fetched from memory and run: the word doesn't run its caller's - its comment is its callers' to go by
create acts-table 4 cells allot
: dispatch ( i -- )  cells acts-table + @ execute ;
T{ ' dispatch stack-effect nip nip nip nip -> 1 }T
T{ s" : dispatches ( -- ) 2 dispatch ;" warns  s" : dispatches-wrong ( -- ) dispatch ;" warns -> 0 1 }T

testing a word that runs a token it is given, given a known word: followed with it
: with-three ( xt -- )  3 swap execute ;
: through ( xt -- )  with-three ;
T{ ' with-three stack-effect nip nip nip nip -> 4 }T
T{ s" : shows ( -- ) ['] hurt with-three ;" warns  ' shows stack-effect -> 0  0 0 0 0 1 }T
T{ s" : shows-far ( -- ) [: drop ;] through ;" warns -> 0 }T
T{ s" : leaves-one ( -- ) ['] 1+ with-three ;" warns -> 1 }T
T{ s" : each-shown ( l -- ) ['] hurt each ;" warns  s" : each-kept ( l -- ) ['] 1+ each ;" warns -> 0 0 }T
T{ s" : summed-by-token ( l -- n ) 0 swap ['] + each ;" warns  ' summed-by-token stack-effect -> 0  1 1 0 0 1 }T
T{ s" : kept-by-token ( x -- x ) ['] hurt keep ;" warns -> 0 }T
T{ s" : given-on ( xt -- ) with-three ;" warns  ' given-on stack-effect nip nip nip nip -> 0 4 }T   \ (its own caller's)

testing holds: says what it can't take
: holds-nothing  s" variable v1  holds: 5" evaluate ;
: holds-varying  s" variable v2  holds: ( x -- .. )" evaluate ;
T{ ' holds-nothing catch  ' holds-varying catch -> -1 -1 }T

test-summary

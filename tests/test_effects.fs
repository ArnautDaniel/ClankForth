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
T{ s" : each-dropped ( l -- x ) [: drop ;] each ;" warns -> 0 }T
T{ s" : varies ( x -- .. ) dup ;" warns -> 0 }T

testing a C word's effect, as a host says it
T{ ' dup stack-effect -> 1 2 0 0 1 }T
T{ ' f@ stack-effect -> 1 0 0 1 1 }T
T{ ' restart stack-effect nip nip nip nip -> 5 }T

test-summary

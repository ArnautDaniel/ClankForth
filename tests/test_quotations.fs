\ quotations that use their word's locals (closures), and the combinators compiled inline after quotations
IN: test-quotations
USING: tester ;

: .list-sum ( l -- n )  {: l | sum :}  l [: sum + to sum ;] each  sum ;   \ (the list's items summed)

testing a quotation reads its word's locals
: add-to ( l n -- l' )  {: n :}  [: n + ;] map ;
T{ { 1 2 3 } 10 add-to .list-sum -> 36 }T

testing a quotation stores into them (`to`): a total gathered
T{ { 4 5 6 } .list-sum -> 15 }T
testing a token that is a number near a word: an error, not a crash
: off-by-one  [: 1+ ;] 1+ execute ;
T{ ' off-by-one catch -> -1 }T

testing its own locals, and its word's, together
: scaled ( l k -- l' )  {: k :}  [: {: x :} x k * ;] map ;
T{ { 1 2 3 } 3 scaled .list-sum -> 18 }T

testing two quotations deep, the word's locals reached through both
: nested ( n -- m )  {: n :}  [: [: n 1+ ;] execute 2 * ;] execute ;
T{ 5 nested -> 12 }T

testing float locals
: f-scaled ( l -- l' ) ( F: s -- )  {: l F: s :}  l [: s>f s f* f>s ;] map ;
T{ { 2 4 } 1.5e f-scaled .list-sum -> 9 }T

testing a closure's word's frame: each call its own
: counter ( n -- a b )  {: n :}  [: n 1+ ;] execute  [: n 2 + ;] execute ;
T{ 1 counter 10 counter -> 2 3 11 12 }T

testing a closure after its word returned: an error, not a crash
: make-one ( n -- xt )  {: n :}  [: n ;] ;
: run-stale  7 make-one execute ;
T{ ' run-stale catch -> -1 }T
: spawn-one ( n -- )  {: n :}  [: n drop ;] spawn drop ;
: spawn-it  1 spawn-one ;
T{ ' spawn-it catch -> -1 }T

testing quotations without locals stay plain: their exits too
: early-out ( n -- m )  [: dup 0< if drop 0 exit then 1+ ;] execute ;
T{ -5 early-out 5 early-out -> 0 6 }T
: early-local ( n k -- m )  {: k :}  [: dup 0< if drop k exit then k + ;] execute ;
T{ -5 3 early-local 5 3 early-local -> 3 8 }T

testing combinators after literal quotations, compiled inline
: d1 ( a b -- a+1 b )  [: 1+ ;] dip ;
T{ 1 2 d1 -> 2 2 }T
: d2 ( a b c -- a+1 b c )  [: 1+ ;] 2dip ;
T{ 1 2 3 d2 -> 2 2 3 }T
: k1 ( a -- a*2 a )  [: 2 * ;] keep ;
T{ 5 k1 -> 10 5 }T
: k2 ( a b -- a+b a b )  [: + ;] 2keep ;
T{ 2 3 k2 -> 5 2 3 }T
: b1 ( a -- a+1 a*2 )  [: 1+ ;] [: 2 * ;] bi ;
T{ 5 b1 -> 6 10 }T
: t1 ( a -- a+1 a*2 a-1 )  [: 1+ ;] [: 2 * ;] [: 1- ;] tri ;
T{ 5 t1 -> 6 10 4 }T
: ba ( a b -- a*a b*b )  [: dup * ;] bi@ ;
T{ 3 4 ba -> 9 16 }T
: w1 ( n flag -- n' )  [: 1+ ;] when ;
T{ 5 true w1 5 false w1 -> 6 5 }T
: u1 ( n flag -- n' )  [: 1+ ;] unless ;
T{ 5 true u1 5 false u1 -> 5 6 }T
: with-local ( n k -- n+k )  {: k :}  [: k + ;] keep drop ;
T{ 5 3 with-local -> 8 }T
: sum-when ( l -- n )  {: l | sum :}  l [: dup 2 mod 0= [: sum + to sum ;] [: drop ;] ? execute ;] each  sum ;
T{ { 1 2 3 4 } sum-when -> 6 }T
testing ... and their code is the quotation's, called where it stands
T{ ' d1 stack-effect -> 2 2 0 0 1 }T
: plain-dip ( a xt -- a )  dip ;   \ (not after a quotation: the word itself, run as it is)
T{ 1 2 [: 1+ ;] plain-dip -> 2 2 }T

testing stack effects through them: a quotation's effect counted inline
: warns ( addr len -- n )  warnings {: before :}  evaluate  warnings before - ;
T{ s" : dip-ok ( a b -- a b ) [: 1+ ;] dip ;" warns -> 0 }T
T{ s" : dip-off ( a b -- a ) [: 1+ ;] dip ;" warns -> 1 }T
T{ s" : when-uneven ( n flag -- n ) [: 1 ;] when ;" warns -> 1 }T

test-summary

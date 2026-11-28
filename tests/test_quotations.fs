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
: spawn-one ( n -- )  {: n :}  [: n drop ;] start-task drop ;
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

testing a quotation run where it lies: a walk or a combinator laid round its code - its locals cells of the frame
testing  around it, its word's locals that frame's own (no call, no frame of its own for each item)
: own-each ( l -- n )  0 swap [: {: x | twice :}  x 2* to twice  twice + ;] each ;   \ (a word with no frame: one made)
T{ { 1 2 3 } own-each -> 12 }T
: zeroed ( l -- l' )  [: {: x | acc :}  acc x + to acc  acc ;] map ;   \ (a local after | begins at 0 each item)
T{ { 1 2 3 } zeroed dup 0 swap nth swap dup 2 swap nth swap list-free -> 1 3 }T
: reads-writes ( l k -- n )  {: l k | t :}  l [: k * t + to t ;] each  t ;
T{ { 1 2 3 } 10 reads-writes -> 60 }T
: walk-in-walk ( -- n )  {: | t :}  { 1 2 } [: {: a :}  { 10 20 } [: a * t + to t ;] each ;] each  t ;
T{ walk-in-walk -> 90 }T
: sifted ( l lim -- l' )  {: lim :}  [: lim > ;] filter ;
T{ { 10 20 30 } 15 sifted dup length swap list-free -> 2 }T
: found ( l lim -- x flag )  {: lim :}  [: lim > ;] find ;
T{ { 10 20 30 } 15 found  { 10 20 30 } 99 found -> 20 true false }T
: folded ( l k -- n )  {: k :}  0 [: k * + ;] reduce ;
T{ { 1 2 3 } 2 folded -> 12 }T
: text-mapped ( s d -- s' )  {: d :}  [: d + ;] map ;
T{ " abc" 1 text-mapped dup string s" bcd" s= swap dispose -> true }T
: floats ( l -- n )  {: | F: sum :}  [: {: x | F: half :}  x s>f 2e f/ to half  sum half f+ to sum ;] each  sum f>s ;
T{ { 2 4 6 } floats -> 6 }T
: looped ( l -- n )  {: | t :}  [: {: x :}  x 0 ?do  t 1+ to t  loop ;] each  t ;   \ (a loop of its own in it)
T{ { 1 2 3 } looped -> 6 }T
: recursive ( n -- sum )  {: n | t :}  n 0= if  0 exit  then  { 1 } [: n + to t ;] each  t  n 1- recurse + ;
T{ 3 recursive -> 9 }T   \ (each call of the word its own frame: 4 + 3 + 2)
: in-called ( l -- n )  {: l | t :}  [: l [: t + to t ;] each ;] execute  t ;   \ (in a quotation that is called)
T{ { 1 2 3 } in-called -> 6 }T
: twice-each ( l -- n )  {: l | t :}  l [: t + to t ;] each  l [: {: x :}  x x * t + to t ;] each  t ;
T{ { 1 2 3 } twice-each -> 20 }T

testing ... every walk of each's kin: any? all? count each-index map-index find-index - and all? stops at the first
: some ( l k -- flag )  {: k :}  [: k = ;] any? ;
T{ { 1 2 3 } 2 some  { 1 2 3 } 9 some -> true false }T
: every ( l -- flag )  [: 1 and 0= ;] all? ;
T{ { 2 4 6 } every  { 2 5 6 } every  { } every -> true false true }T
: stops ( l -- looked )  {: | n :}  [: n 1+ to n  0> ;] all? drop  n ;
T{ { 1 -1 1 1 } stops -> 2 }T
: stops-called ( l xt -- looked )  {: l xt | n :}  l [: n 1+ to n  xt execute ;] dup drop all? drop  n ;   \ (the word itself)
T{ { 1 -1 1 1 } [: 0> ;] stops-called -> 2 }T
: letters ( s c -- n )  {: c :}  [: c = ;] count ;
T{ " banana" char a letters -> 3 }T
: count-under ( -- n n )  { 5 1 5 } {: l :}  5  l [: over = ;] count  l list-free ;   \ (its code sees the caller's stack)
T{ count-under -> 5 2 }T
: placed ( l -- n )  {: | t :}  [: * t + to t ;] each-index  t ;
T{ { 10 20 30 } placed -> 80 }T
: place-added ( l -- l' )  [: + ;] map-index ;
T{ { 10 20 30 } place-added dup 2 swap nth swap list-free -> 32 }T
: place-of ( s c -- i )  {: c :}  [: c = ;] find-index ;
T{ " hello" char l place-of  " hello" char z place-of  " hello" char h place-of  " " char h place-of -> 2 -1 0 -1 }T
: places-nested ( -- n )  {: | t :}  { 1 2 } [: {: a i :}  { 5 6 } [: {: b j :}  a b * i j + * t + to t ;] each-index ;] each-index  t ;
T{ places-nested -> 40 }T   \ (1*5*0 + 1*6*1 + 2*5*1 + 2*6*2: each walk its own count)

testing ... the combinators: dip keep when unless, with the word's locals
: dipped ( a b k -- a+k b )  {: k :}  [: k + ;] dip ;
T{ 1 2 7 dipped -> 8 2 }T
: kept ( a k -- a*k a )  {: k :}  [: k * ;] keep ;
T{ 5 3 kept -> 15 5 }T
: chosen ( flag -- n )  {: fl | k :}  fl [: k 5 + to k ;] when  fl [: k 100 + to k ;] unless  k ;
T{ true chosen false chosen -> 5 100 }T

testing ... with-scratch and build: their quotation's code in the word's frame, what it made freed as ever
: scratch-sum ( l -- n )  {: l | t :}  [: l [: {: x :}  x s" {}" format 2drop  x t + to t ;] each ;] with-scratch  t ;
T{ objects  { 1 2 3 } dup scratch-sum swap list-free  swap objects = -> 6 true }T
: built ( n -- s )  {: n :}  [: n 0 ?do  i [char] a + %c  loop ;] build ;
T{ 3 built dup string s" abc" s= swap dispose -> true }T
: built-twice ( -- s )  {: | inner :}  [: [: s" in" %s ;] build to inner  s" out-" %s  inner % ;] build ;
T{ built-twice dup string s" out-in" s= swap dispose -> true }T
: scratch-fails ( -- )  [: s" kept?" >string 2drop  true abort" inside" ;] with-scratch ;
T{ objects  ' scratch-fails catch 0<>  swap objects = -> true true }T
: build-fails ( -- )  [: s" half" %s  true abort" inside" ;] build drop ;
T{ objects  ' build-fails catch 0<>  swap objects = -> true true }T
T{ [: s" a build after one that failed" %s ;] build dup string nip swap dispose -> 29 }T

testing ... and what can't be is called as ever: an exit in its middle, a frame to make inside an if
: early ( l -- n )  0 swap [: {: x :}  x 0< if  exit  then  x + ;] each ;
T{ { 1 -5 2 } early -> 3 }T
: in-an-if ( l flag -- n )  0 -rot if  [: {: x :}  x + ;] each  else  drop  then ;
T{ { 1 2 3 } true in-an-if  { 1 2 3 } false in-an-if -> 6 0 }T
: holds-one ( l -- n )  {: | t :}  [: {: x :}  [: x t + to t ;] execute ;] each  t ;   \ (a closure made in it)
T{ { 1 2 3 } holds-one -> 6 }T

testing ... see shows it as it was written
: shown ( l -- n )  {: | t :}  [: t + to t ;] each  t ;
T{ s" see shown" [: evaluate ;] with-output dup string s" [: local0 + to local0 ;] each" s-find 0>= swap dispose -> true }T

\ (from here: the stack checker's - a system built without it ends the file here)
checker-tests

testing ... and their code is the quotation's, called where it stands
T{ ' d1 stack-effect -> 2 2 0 0 1 }T
: plain-dip ( a xt -- a )  dip ;   \ (not after a quotation: the word itself, run as it is)
T{ 1 2 [: 1+ ;] plain-dip -> 2 2 }T

testing stack effects through them: a quotation's effect counted inline
: warns ( addr len -- n )  warnings {: before :}  evaluate  warnings before - ;
T{ s" : dip-ok ( a b -- a b ) [: 1+ ;] dip ;" warns -> 0 }T
T{ s" : dip-off ( a b -- a ) [: 1+ ;] dip ;" warns -> 1 }T
T{ s" : when-uneven ( n flag -- n ) [: 1 ;] when ;" warns -> 1 }T

testing ... and through a quotation run where it lies: a walk's turn that leaves an item, a build's that does
T{ s" : each-ok ( l -- n ) {: | t :} [: t + to t ;] each t ;" warns -> 0 }T
T{ s" : each-leaves ( l -- ) {: | t :} [: t + ;] each ;" warns -> 1 }T
T{ s" : all-ok ( s -- flag ) [: 65 = ;] all? ;" warns -> 0 }T
T{ s" : index-ok ( l -- n ) {: | t :} [: * t + to t ;] each-index t ;" warns -> 0 }T
T{ s" : index-short ( l -- ) [: drop ;] each-index ;" warns -> 1 }T
T{ s" : build-ok ( -- s ) [: 65 %c ;] build ;" warns -> 0 }T
T{ s" : build-leaves ( -- s ) [: 65 ;] build ;" warns -> 1 }T

test-summary

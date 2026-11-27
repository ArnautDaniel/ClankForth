\ prelude.fs - the rest of the language, written in Forth on top of the C core (src/): built into the
\ library (forth_prelude), loaded before anything else. Like jonesforth, control structures are ordinary immediate
\ words that lay down branches and patch them: `here` is the next cell of the definition
\ being compiled, and branch targets are absolute addresses.

IN: forth

: true  -1 ;
: false 0 ;
: cell+ cell + ;
: 2*  1 lshift ;
: 2/  1 arshift ;
: not 0= ;
: 2! ( x1 x2 addr -- ) swap over ! cell+ ! ;
: 2@ ( addr -- x1 x2 ) dup cell+ @ swap @ ;

\ ---- conditionals and loops ----
\ if ... else ... then           ( flag -- )
\ begin ... again                forever
\ begin ... until                ( -- flag ) at the end
\ begin ... while ... repeat     ( -- flag ) in the middle

: if     inside-definition postpone 0branch here 0 , ; immediate
: then   inside-definition here swap ! ; immediate
: else   inside-definition postpone branch here 0 , swap postpone then ; immediate
: begin  inside-definition here ; immediate
: again  inside-definition postpone branch , ; immediate
: until  inside-definition postpone 0branch , ; immediate
: while  inside-definition postpone if swap ; immediate
: repeat inside-definition postpone again postpone then ; immediate

\ do ... loop / +loop, ?do (skips when limit = start). (do) keeps the exit address, so
\ `leave` needs no patching: `do` leaves ( exit-slot loop-start ) for `loop` to finish.
: do     inside-definition postpone (do) here 0 , here ; immediate
: ?do    inside-definition postpone (?do) here 0 , here ; immediate
: loop   inside-definition postpone (loop) , here swap ! ; immediate
: +loop  inside-definition postpone (+loop) , here swap ! ; immediate

\ case ... of ... endof ... endcase   ( x -- )
: case    inside-definition 0 ; immediate
: of      inside-definition postpone over postpone = postpone if postpone drop ; immediate
: endof   inside-definition postpone else ; immediate
: endcase inside-definition postpone drop begin ?dup while postpone then repeat ; immediate

\ ---- defining words ----
: variable   create 0 , ;
: constant   create , does> @ ;
: fvariable  create 0e f, ;
: fconstant  create f, does> f@ ;
: value      create , does> @ ;
0 value (a-value)   \ (the checker's: how a value's word is known - its code after does>)
: fvalue     create f, does> f@ ;
\ `to name` stores into a value (from the stack, or the float stack for an fvalue)
: (to)  ( x addr -- ) ! ;
: to    ' >body state @ if postpone literal postpone (to) else (to) then ; immediate
: fto   ' >body state @ if postpone literal postpone f! else f! then ; immediate
\ deferred words: `defer name` (a stack comment right after the name: its callers checked with it), later `' word is name`; `action-of name` what it does now (a mod
\ setting a hook keeps it, to call for the rest)
: noop ;
: is     ' >body state @ if postpone literal postpone ! else (is) then ; immediate
: action-of ( "name" -- xt )  ' >body @ ;
\ a variable, a value, every actor on the node is meant to share (a count, a setting changed as it runs): said so, a
\ handler that writes it and reads it isn't warned of keeping its work there - what is between is its own to see to
: shared-variable ( "name" -- )  variable  here 1 cells - (shared) ;
: shared-value ( x "name" -- )  value  here 1 cells - (shared) ;
\ a run of memory with a name: `16 cells buffer: name`
: buffer: create allot ;
\ a variable each task (each actor's handlers) has its own of: what a word keeps between the words it calls while it
\ works - a parser's place - where another handler may run in between (its slice over) and use the same word
: task-variable ( "name" -- )  create (task-var-new) , does> @ (task-var) ;
: erase ( addr n -- )  0 fill ;

\ strings made a piece at a time: `$clear  s" cdbin/" $+  12 n>str $+  $@` (one buffer each task: use the string
\ before the next $clear; n>str's digits too, till its next)
: $clear ( -- )  0 ($len) ! ;
: $+ ( addr len -- )  255 ($len) @ - min 0 max  >r ($) ($len) @ + r@ move  r> ($len) +! ;
: $@ ( -- addr len )  ($) ($len) @ ;
: abort" ( flag "text" -- )  postpone s" postpone (abort") ; immediate   \ an error with the text when flag
\ records (Forth 2012's structures): a name for each field's offset, and the size -
\   begin-structure /point  ffield: p.x  ffield: p.y  end-structure     ( addr p.y f@ )
: begin-structure ( "name" -- addr 0 )  create here 0 ,  0  does> @ ;
: +field ( n size "name" -- n' )  create over , +  does> @ + ;
: field: ( n "name" -- n' )  1 cells +field ;
: cfield: ( n "name" -- n' )  1 +field ;   \ (a byte: c@ c!)
: lfield: ( n "name" -- n' )  4 +field ;   \ (32 bits: l@ l!)
: ffield: ( n "name" -- n' )  1 floats +field ;
: sffield: ( n "name" -- n' )  4 +field ;   \ (a 32-bit float: sf@ sf!)
: end-structure ( addr n -- )  swap ! ;

\ ---- numbers ----
: within ( x lo hi -- flag ) over - >r - r> u< ;
: hex     16 base ! ;
: decimal 10 base ! ;
: clamp ( x lo hi -- x' ) rot min max ;
: 0<= ( n -- flag ) 0> 0= ;
: 0>= ( n -- flag ) 0< 0= ;

\ ---- floats ----
: fnip ( F: a b -- b )  fswap fdrop ;
3.14159265358979 fconstant pi
: f2*  2e f* ;
: f2/  0.5e f* ;
: fsq  fdup f* ;
: float+ ( addr -- addr' )  1 floats + ;
: f0> ( F: x -- ) ( -- flag )  0e f> ;
: fclamp ( F: x lo hi -- x' ) frot fmin fmax ;
: deg>rad  pi 180e f/ f* ;
: rad>deg  180e pi f/ f* ;
: f~ ( F: a b eps -- ) ( -- flag ) frot frot f- fabs f> ;

\ ---- strings ----
: s= ( a1 n1 a2 n2 -- flag ) compare 0= ;

\ ---- combinators (Factor's): code run on values without shuffling them about ----
\   x [: ... ;] dip        run it under x, x back on top          ( x xt -- x )
\   x [: ... ;] keep       run it on x, x kept after its results  ( x xt -- .. x )
\   x [: p ;] [: q ;] bi   p and q each on x                     ( x p q -- )
\   x [: p ;] [: q ;] [: r ;] tri
\   x y [: q ;] bi@        q on x, then on y                     ( x y q -- )
\   flag [: ... ;] when / unless
\   flag a b ?             a if the flag, else b                 ( flag a b -- a|b )
: dip ( x xt -- x )  swap >r execute r> ;
: 2dip ( x y xt -- x y )  -rot >r >r execute r> r> ;
: keep ( x xt -- .. x )  over >r execute r> ;
: 2keep ( x y xt -- .. x y )  >r 2dup r> -rot >r >r execute r> r> ;
: bi ( x p q -- )  >r keep r> execute ;
: tri ( x p q r -- )  >r >r keep r> keep r> execute ;
: bi@ ( x y q -- )  tuck >r >r execute r> r> execute ;
: when ( flag xt -- )  swap if  execute  else  drop  then ;
: unless ( flag xt -- )  swap if  drop  else  execute  then ;
: ? ( flag a b -- a|b )  rot if  drop  else  nip  then ;

\ ---- sequences (Factor's protocol) and the words that walk them ----
\ A sequence is a list (cells: `{ 1 2 3 }`, `list`), a string (bytes, its length its own: `" text"`) or bytes (that
\ grow: `<bytes>`). The same words take each - length nth nth! push pop clear, append subseq reverse index ... - and
\ what one makes is of the kind it was given (`map` over a string, a string; `like` makes one of another's kind).
\ The words below take a sequence and code - usually a quotation, [: ... ;] - and run the code on each item, on the
\ stack as their caller left it (so `0 l [: + ;] each` adds the items to the 0). Right after a quotation in a
\ definition, each map filter reduce and find are laid down as the walk itself (forth.c: lay_walk) - no call, and
\ their stack effects checked.

: each ( seq xt -- )  \ xt ( x -- )
    {: s xt :}  s length 0 ?do  i s nth  xt execute  loop ;
: map ( seq xt -- seq' )  \ xt ( x -- y ), a new sequence of the same kind
    {: s xt | made :}  s (like-acc) to made  s length 0 ?do  i s nth  xt execute  made push  loop  made s (like-done) ;
: filter ( seq xt -- seq' )  \ xt ( x -- flag ), a new one of the items it keeps
    {: s xt | made :}  s (like-acc) to made
    s length 0 ?do  i s nth  xt execute if  i s nth  made push  then  loop  made s (like-done) ;
: reduce ( seq acc xt -- acc' )  \ xt ( acc x -- acc' )
    {: xt :}  swap {: s :}  s length 0 ?do  i s nth  xt execute  loop ;
: find-word ( addr len -- xt | 0 )  find ;   \ (the dictionary's: `find` below is the sequences')
: find ( seq xt -- x true | false )  \ the first item xt ( x -- flag ) holds for
    {: s xt :}  s length 0 ?do  i s nth  xt execute if  i s nth  true unloop exit  then  loop  false ;
: find-index ( seq xt -- i | -1 )  \ where it first holds
    {: s xt :}  s length 0 ?do  i s nth  xt execute if  i unloop exit  then  loop  -1 ;
: any? ( seq xt -- flag )  find dup if nip then ;
: count ( seq xt -- n )  \ how many it holds for
    {: s xt | n :}  s length 0 ?do  i s nth  xt execute if  n 1+ to n  then  loop  n ;
: all? ( seq xt -- flag )  {: s xt :}  s xt count  s length = ;
: each-index ( seq xt -- )  \ xt ( x i -- )
    {: s xt :}  s length 0 ?do  i s nth  i  xt execute  loop ;
: map-index ( seq xt -- seq' )  \ xt ( x i -- y )
    {: s xt | made :}  s (like-acc) to made  s length 0 ?do  i s nth  i  xt execute  made push  loop  made s (like-done) ;
: range ( n -- l )  \ 0 .. n-1 (none for n under 1)
    {: n | made :}  list to made  n 0 max 0 ?do  i made push  loop  made ;
: contains? ( x seq -- flag )  index 0>= ;
: empty? ( seq -- flag )  length 0= ;
: first ( seq -- x )  0 swap nth ;
: last ( seq -- x )  -1 swap nth ;
: head ( seq n -- seq' )  {: s n :}  s 0 n s length min subseq ;      \ its first n (all, if it has fewer)
: tail ( seq n -- seq' )  {: s n :}  s  n s length min  s length subseq ;   \ all but its first n
: starts? ( seq prefix -- flag )  {: s p :}  p length s length <=  if  s p length head p seq=  else  false  then ;
: ends? ( seq suffix -- flag )
    {: s p :}  p length s length <=  if  s  s length p length -  s length subseq p seq=  else  false  then ;
\ the sequences one after another, a new one of the first's kind (a list for none)
: concat ( seqs -- seq )
    {: ss | made :}  ss empty? if  list exit  then
    0 ss nth (like-acc) to made  ss [: {: x :}  made x append! drop ;] each  made 0 ss nth (like-done) ;
\ the sequences with sep between each, a new one of sep's kind
: join ( seqs sep -- seq )
    {: ss sep | made :}  sep (like-acc) to made
    ss [: {: x i :}  i if  made sep append! drop  then  made x append! drop ;] each-index
    made sep (like-done) ;
\ the pieces between each sep (a sequence: "," or ", "), a list of them, each of the sequence's kind
: split ( seq sep -- l )
    {: s sep | pieces from at :}  list to pieces
    sep empty? if  s 0 s length subseq pieces push  pieces exit  then
    begin  s sep from subseq-index-from to at  at 0>=  while
        s from at subseq pieces push  at sep length + to from
    repeat
    s from s length subseq pieces push  pieces ;
\ each old in it new instead (a new one of its kind)
: replace ( seq old new -- seq' )  {: s old new :}  s old split {: l :}  l new join  s like ;
\ a string's or bytes' spaces, tabs, newlines and returns off both ends (a new one)
: trim ( seq -- seq' )
    {: s | from upto :}  s length to upto
    begin  from upto <  if  from s nth 33 <  else  false  then  while  from 1+ to from  repeat
    begin  upto from >  if  upto 1- s nth 33 <  else  false  then  while  upto 1- to upto  repeat
    s from upto subseq ;
: .list ( l -- )  ." { " [: . ;] each ." }" ;

\ a string built a piece at a time (Factor's make): xt adds to it - % ( seq -- ) a string or bytes, %s ( a n -- ),
\ %n ( n -- ) a number, %c ( c -- ) a byte. Inside a scratch region of its own: what xt makes besides is freed as
\ it ends (as with-scratch); the string is the caller's. Builds nest; each task's its own.
: build ( xt -- s )  {: xt | r :}  (build-open) to r  xt execute  r (build-close) ;

\ objects made in a scratch region freed as it ends (an error, too), but those `rescue`d: `[: ... ;] with-scratch`.
\ (Forth, not C, so the code may wait in it: an actor's handler can `await` there.)
: with-scratch ( xt -- )  (scratch-open) >r  execute  r> (scratch-close) ;

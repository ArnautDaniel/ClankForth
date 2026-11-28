\ long work in one word of C's (docs/actors.md, The schedule): a region of very many objects let go - a handler's, a
\ with-scratch's - and a tree of them rescued are counted against the handler's slice by the clock, and give way in
\ their middle (with machine code under the task: threaded, a word can't stop in its middle); the objects' table is
\ carried to a bigger or a smaller one a few slots at a time. Nothing lost or left on the way.
IN: test-long-work
USING: tester actors ;

: native-here? ( -- flag )  [: ;] native? ;
message work ( xt -- )
message mark ( -- )
variable built   variable worked   variable marked   variable kept
behaviour worker
  on work ( xt -- )  execute  true worked ! ;
  on mark ( -- )  true marked ! ;   \ (handled when the handler before it has ended: what it made let go)
end-behaviour
worker -1 s" worker" spawn constant w
\ the rounds from one flag's setting to another's, as the worker runs xt (the node's other actors' turns meanwhile)
: rounds-in ( xt from to -- n )
    {: xt from till | n :}  false built !  false worked !  false marked !  xt w send work  w send mark
    begin  deliver  from @ if  n 1+ to n  then  till @  n 100000 > or  until  n ;
: long? ( rounds -- flag )  native-here? if  5 >  else  0>  then ;   \ (threaded: it gives way after the word)

: tree ( n -- l )   \ a list of n lists of three strings: four objects a row
    {: n | l row :}  list to l
    n 0 do
        list to row  s" one" >string drop row push  s" two" >string drop row push  s" three" >string drop row push
        row l push
    loop  l ;
30000 constant rows   \ (120 000 objects: ten milliseconds to let go, or to move)

testing a handler's objects, very many, let go as it ends: by its own task, giving way - the node's others have turns
: made-and-left ( -- )  rows tree drop ;
objects constant objects-before
T{ ' made-and-left worked marked rounds-in long? -> true }T
T{ objects -> objects-before }T

testing a with-scratch's, let go at its end: the same
: made-in-scratch ( -- )  [: rows tree drop  true built ! ;] with-scratch ;
T{ ' made-in-scratch built worked rounds-in long? -> true }T
T{ objects -> objects-before }T

testing a tree of them rescued: moved giving way, whole after - every row there, each its three strings
: made-and-kept ( -- )  [: rows tree  true built !  rescue kept ! ;] with-scratch  kept @ rescue drop ;   \ (to the actor)
: whole? ( l -- flag )
    {: l | ok :}  l length rows = to ok
    rows 0 do  i l nth {: row :}  row length 3 =  2 row nth string s" three" s= and  ok and to ok  loop  ok ;
T{ ' made-and-kept built worked rounds-in long? -> true }T
: settled ( -- )  100 0 do  marked @ if  leave  then  deliver  loop ;
T{ settled  kept @ whole? -> true }T
T{ objects objects-before - -> rows 4 * 1+ }T
w kill  deliver
T{ objects -> objects-before }T   \ (the actor gone: what it kept with it)

testing killed in the middle of letting go: what was left is let go with it
: half-let-go ( -- left )
    worker -1 s" doomed" spawn {: d :}  false built !
    ['] made-in-scratch d send work
    begin  deliver  built @  until   \ (its tree made: the scratch's end begun, or about to)
    d kill  deliver  deliver  objects objects-before - ;
T{ half-let-go -> 0 }T

testing the objects' table grown, and shrunk, a few slots at a time: every object found on the way, those let go
testing  no objects, the count right; made again where a burst left it big
: all-three? ( l -- flag )   \ (each row asked for: found in the table, wherever it is on its way)
    {: l | ok :}  true to ok
    l length 0 do  i l nth length 3 = ok and to ok  loop  ok ;
: every-other-gone ( l -- gone-are-none? )   \ every other row let go: no object then, the others as they were
    {: l | ok row :}  true to ok
    l length 0 do
        i 1 and if  i l nth to row  row list-free  row object? 0= ok and to ok
        else  i l nth length 3 = ok and to ok  then
    loop  ok ;
: let-go-rest ( l -- )  {: l :}  l length 0 do  i 1 and 0= if  i l nth list-free  then  loop  l list-free ;
: grown-and-shrunk ( -- found? gone? left )
    {: | l :}  rows tree to l   \ (made in the system's region here, at the prompt: let go by hand)
    l all-three?  l every-other-gone  l let-go-rest
    objects objects-before - ;
T{ grown-and-shrunk -> true true rows 3 * }T   \ (the rows' strings: a list's items aren't let go with it)
: strings-left ( -- n )  [: rows tree all-three? drop ;] with-scratch  objects objects-before - ;
T{ strings-left -> rows 3 * }T   \ (made again, in a scratch: all of that let go - the strings before still there)

test-summary

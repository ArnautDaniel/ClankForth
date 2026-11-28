\ long work in one word of C's (docs/actors.md, The schedule): a region of very many objects let go - a handler's, a
\ with-scratch's - and a tree of them rescued are counted against the handler's slice by the clock, and give way in
\ their middle (with machine code under the task: threaded, a word can't stop in its middle); the objects' table
\ grows and shrinks by a chunk of slots at a time. Nothing lost or left on the way.
IN: test-long-work
USING: tester actors ;

: native-here? ( -- flag )  [: ;] native? ;
message work ( xt -- )
message mark ( -- )
variable built   variable worked   variable marked   variable kept
state: working  cell field its-tree  end-state   \ (what a worker keeps is named by its state: else it is no longer its)
behaviour worker
  on work ( xt -- )  execute  true worked ! ;
  on mark ( -- )  true marked ! ;   \ (handled when the handler before it has ended: what it made let go)
end-behaviour
worker working s" worker" spawn constant w
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
: made-and-kept ( -- )  [: rows tree  true built !  rescue kept ! ;] with-scratch  kept @ rescue its-tree ! ;   \ (to the actor)
: whole? ( l -- flag )
    {: l | ok :}  l length rows = to ok
    rows 0 do  i l nth {: row :}  row length 3 =  2 row nth string s" three" s= and  ok and to ok  loop  ok ;
T{ ' made-and-kept built worked rounds-in long? -> true }T
: settled ( -- )  100 0 do  marked @ if  leave  then  deliver  loop ;
T{ settled  kept @ whole? -> true }T
T{ objects objects-before - -> rows 4 * 1+ }T
w kill  deliver
T{ objects -> objects-before }T   \ (the actor gone: what it kept with it)

testing ... and one that isn't the task's own - an actor's kept tree, sent by its address - rescued in one step: its
testing  owner may let it go at any turn (moved giving way, the move went on in what was freed)
message keep-tree ( -- )   message send-tree ( to -- )   message drop-tree ( -- )   message take ( tree -- )
variable taken
: let-go-tree ( l -- )  dup [: dup [: dispose ;] each  dispose ;] each  dispose ;
state: owning  cell field mine  end-state
behaviour owner
  on keep-tree ( -- )  rows tree rescue mine !  true built ! ;
  on send-tree ( to -- )  mine @ swap send take ;   \ (kept: sent by its address, the owner's still)
  on drop-tree ( -- )  mine @ let-go-tree  0 mine ! ;
end-behaviour
behaviour taker  on take ( tree -- )  rescue drop  true taken ! ;  end-behaviour
: taken-and-dropped ( -- left )
    owner owning s" owner" spawn  taker -1 s" taker" spawn {: o t :}
    false built !  false taken !  o send keep-tree  begin  deliver  built @  until
    t o send send-tree  begin  deliver  t busy?  taken @ or  until   \ (the taker's rescue begun - or done, in a step)
    o send drop-tree  1000 0 do  deliver  loop
    o kill  t kill  deliver  objects objects-before - ;
T{ taken-and-dropped -> 0 }T

testing rescue-all and rescue-actor of the task's own tree: moved giving way too, whole after
: rounds-by ( xt w -- n )
    {: xt w | n :}  false built !  false worked !  false marked !  xt w send work  w send mark
    begin  deliver  built @ if  n 1+ to n  then  worked @  n 100000 > or  until  n ;
: kept-for-good ( -- )  [: rows tree  true built !  rescue-all kept ! ;] with-scratch ;
: kept-by-actor ( -- )  [: rows tree  true built !  rescue-actor dup kept !  its-tree ! ;] with-scratch ;
: moved-long? ( xt -- long? whole? )
    worker working s" keeper" spawn {: xt k :}  xt k rounds-by long?  settled  kept @ whole?  k kill  deliver ;
T{ ' kept-for-good moved-long?  kept @ let-go-tree  objects objects-before - -> true true 0 }T
T{ ' kept-by-actor moved-long?  objects objects-before - -> true true 0 }T   \ (the actor's: gone with it)

testing killed in the middle of letting go: what was left is let go with it
: half-let-go ( -- left )
    worker working s" doomed" spawn {: d :}  false built !
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

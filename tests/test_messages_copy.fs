\ what a message carries is a copy (docs/actors.md, An actor's objects): the receiver's own, nothing of the sender's -
\ no two actors have the same object. The program's own (made as it loads, kept by rescue-all, permanent) are every
\ actor's as they are.
IN: test-messages-copy
USING: tester actors accessors ;

objects constant objects-before
message take ( x -- )
message take-two ( x y -- )
message make-and-send ( to -- )
message keep-and-send ( to -- )
message send-kept ( to -- )
message drop-kept ( -- )
variable got   variable got-2   variable seen   variable sent-cell   variable same-after   variable failed
state: keeping  cell field mine  end-state
: settle ( -- )  20 0 do  deliver  loop ;

behaviour taker
  on take ( x -- )  rescue-actor mine !  mine @ got !  1 seen +! ;
  on take-two ( x y -- )  {: x y :}  x y = got !  x string s" both" s= got-2 !  1 seen +! ;   \ (its handler's: seen here)
end-behaviour
: row ( -- l )  list {: l :}  s" one" >string drop l push  s" two" >string drop l push  l ;
behaviour giver
  on make-and-send ( to -- )   \ made in the handler, sent, changed after: the receiver's is as it was sent
      {: dest :}  row {: l :}  l sent-cell !  l dest send take  s" three" >string drop l push
      l length 3 =  l object? and  same-after ! ;
  on keep-and-send ( to -- )  {: dest :}  row rescue-actor mine !  mine @ sent-cell !  mine @ dest send take ;
  on send-kept ( to -- )  mine @ swap send take ;
  on drop-kept ( -- )  mine @ dup [: dispose ;] each dispose  0 mine ! ;
end-behaviour
taker keeping s" taker" spawn constant t
giver keeping s" giver" spawn constant g

testing an object made in a handler and sent: the receiver has a copy - another cell, the same things in it - and the
testing  sender its own still, to change or let go
T{ 0 seen !  t g send make-and-send  settle  seen @  same-after @ -> 1 true }T
T{ got @ object?  got @ sent-cell @ <>  got @ length  0 got @ nth string s" one" s= -> true true 2 true }T

testing one its actor keeps, sent: a copy too - the keeper lets its own go, the receiver's is there
T{ 0 seen !  t g send keep-and-send  settle  g send drop-kept  settle  seen @ -> 1 }T
T{ sent-cell @ object?  got @ object?  1 got @ nth string s" two" s= -> false true true }T

testing the same object twice in what is sent: one copy, there twice; one that holds itself: a copy that holds itself
: twice ( -- )  [: s" both" >string drop {: s :}  s s t send take-two ;] with-scratch ;
T{ 0 seen !  twice settle  seen @  got @  got-2 @ -> 1 true true }T
: holds-itself ( -- )  [: list {: l :}  l l push  7 l push  l t send take ;] with-scratch ;
T{ 0 seen !  holds-itself settle  seen @  0 got @ nth got @ =  1 got @ nth -> 1 true 7 }T

testing a map with its keys and what its values hold; bytes; a tuple's slots: each copied
TUPLE: parcel label weight ;
: packed ( -- )
    [: <map> {: m :}  s" v" >string drop s" k" m s-at!  <bytes> {: b :}  65 b b-push  b 3 m at!
       s" box" >string drop 7 <parcel> s" p" m s-at!  m t send take ;] with-scratch ;
: unpacked? ( m -- flag )
    {: m :}  s" k" m s-at drop string s" v" s=  3 m at drop b-length 1 = and
    s" p" m s-at drop {: p :}  p label>> string s" box" s=  p weight>> 7 = and and ;
T{ 0 seen !  packed settle  seen @  got @ unpacked? -> 1 true }T

testing the program's own - made at the prompt, permanent - are every actor's as they are: the same cell there, and
testing  an actor that keeps one (rescue-actor) doesn't take it from the others
s" the program's" >string drop constant ours
T{ 0 seen !  ours t send take  settle  seen @  got @ ours = -> 1 true }T
s" for good" >string drop permanent constant fixed
T{ 0 seen !  fixed t send take  settle  got @ fixed = -> true }T
T{ 0 seen !  ours t send take  settle  t kill  deliver  ours object?  ours string s" the program's" s= -> true true }T

testing deeper than objects go: an error where it is sent, nothing of it left in the post
: nested ( n -- l )  list {: n l :}  n 0 do  list {: outer :}  l outer push  outer to l  loop  l ;   \ (n lists deep)
taker keeping s" taker" spawn constant t2
: sends-too-deep ( -- failed? left )
    objects {: before :}  [: [: 70 nested t2 send take ;] catch 0<> ;] with-scratch  objects before - ;
T{ sends-too-deep -> true 0 }T

g kill  t2 kill  deliver  ours dispose
T{ objects -> objects-before 1+ }T   \ (the permanent one: for good)
test-summary

\ the actor kernel: messages, behaviours, state, delivery order, safety
IN: test-actors
USING: tester actors ;

message ping ( n -- )
message pong ( n -- )
message hello ( -- )

state: counter-state
  cell field count
  cell field last
  1 floats field level
end-state

variable log  list log !
: note ( n -- )  log @ push ;

behaviour counting
  on ping ( n -- )  dup last !  1 count +!  note ;
  on hello ( -- )  100 note ;
end-behaviour

behaviour echoing  extends counting
  on ping ( n -- )  dup note  1+ sender send pong ;
end-behaviour

behaviour answering
  on pong ( n -- )  1000 + note ;
end-behaviour

testing spawning and sending
actors-reset
counting counter-state s" a" spawn constant a
T{ a alive? -> -1 }T
T{ 7 a send ping  deliver  log @ length -> 1 }T
T{ a enter  count @  last @  leave-actor -> 1 7 }T

testing behaviours: inherited handlers, become
actors-reset  list log !
echoing counter-state s" e" spawn constant e
answering counter-state s" q" spawn constant q
: ask-e ( -- )  q enter  5 e send ping  leave-actor ;
T{ ask-e deliver  log @ 0 swap nth  log @ 1 swap nth -> 5 1006 }T   \ e answered q's ping
T{ e send hello deliver  log @ 2 swap nth -> 100 }T               \ (from counting)
T{ e enter  counting become  leave-actor  3 e send ping  deliver  e enter count @ leave-actor -> 1 }T

testing delivery rounds and order
actors-reset  list log !
behaviour relay
  on ping ( n -- )  dup note  dup 3 < if  1+ self send ping  else  drop  then ;
end-behaviour
relay counter-state s" r" spawn constant r
T{ 0 r send ping  1 r send ping  deliver  log @ length -> 7 }T
T{ log @ 0 swap nth  log @ 1 swap nth  log @ 2 swap nth -> 0 1 1 }T   \ round by round

testing tick and frame-end subscriptions
actors-reset  list log !
behaviour ticker
  on tick ( -- )  1 note ;
  on frame-end ( -- )  2 note ;
end-behaviour
ticker counter-state s" t" spawn constant t
t subscribe tick  t subscribe frame-end
T{ actors-frame  log @ length  log @ 0 swap nth  log @ 1 swap nth -> 2 1 2 }T
t unsubscribe tick
T{ actors-frame  log @ length -> 3 }T

testing floats travel as cells
T{ 2.5e f>cell cell>f 2.5e f= -> -1 }T

testing safety: a field with no actor, a handler's errors don't spoil the stacks
actors-reset
T{ ' count catch -> -1 }T   \ (an error: no actor being run)
behaviour sloppy
  on ping ( n -- )  drop 1 2 3 ;            \ leaves three cells
  on hello ( -- )  5 12345 send ping ;      \ an error: no actor 12345
end-behaviour
sloppy counter-state s" s" spawn constant s
: depth-after ( -- n )  depth >r  4 s send ping  s send hello  deliver  depth r> - ;
T{ depth-after -> 0 }T
T{ s alive? -> false }T   \ (its `hello` failed with no errors actor to tell: it died of it - below, Failures)

testing last words: kill runs the actor's killed handler first; killing itself, it goes after the handler
actors-reset  list log !
behaviour mortal
  on killed ( -- )  7 note ;
  on ping ( n -- )  drop  self kill  8 note ;   \ (its fields are still there for the rest of the handler)
end-behaviour
mortal counter-state s" m" spawn constant m1
mortal counter-state s" m" spawn constant m2
T{ m1 kill  m1 alive?  log @ length  log @ 0 swap nth -> 0 1 7 }T
T{ 0 m2 send ping  deliver  m2 alive?  log @ 1 swap nth  log @ 2 swap nth -> 0 7 8 }T
testing the slots go round: one just freed isn't the next one's
actors-reset  list log !
mortal counter-state s" m" spawn constant m3
0 m3 send ping   m3 kill
mortal counter-state s" m" spawn constant m4
T{ m4 actor-slot  m3 actor-slot = -> false }T
testing an id is its actor's alone: the next to have its slot, once round, has another id
behaviour quiet  on ping ( n -- )  drop  9 note ;  end-behaviour   \ (no last words; a ping noted)
: round-to ( slot -- id )   \ (quiet ones spawned and killed till one has that slot)
    {: slot :}  actors-max 1+ 0 do  quiet counter-state s" r" spawn  dup actor-slot slot = if  unloop exit  then  kill  loop  nobody ;
m4 kill  deliver  list log !   \ (its and m3's last words, cleared)
mortal counter-state s" m" spawn constant m5
0 m5 send ping   m5 kill
m5 actor-slot round-to constant m6
T{ m6 actor-slot m5 actor-slot =  m6 m5 =  m6 alive?  m5 alive? -> true false true false }T
T{ m6 actors-max <  m5 actors-max < -> false true }T   \ (the first to have a slot has it for its id)
T{ deliver  log @ length -> 1 }T   \ (only m5's last words: its ping was not m6's)
: to-the-dead ( -- )  1 m5 send ping ;
T{ ' to-the-dead catch -> -1 }T   \ (an id whose actor is gone names nobody: an error where it is sent)
T{ 2 m6 send ping  deliver  log @ length  log @ 1 swap nth -> 2 9 }T
T{ nobody actor-slot  m6 actor-slot actors-max < -> -1 true }T

testing a handler sees only its arguments: taking more is an error, the deliverer's stack untouched
actors-reset
behaviour greedy
  on ping ( n -- )  2drop ;   \ (one too many)
end-behaviour
greedy counter-state s" g" spawn constant g
T{ 11 22  3 g send ping  deliver -> 11 22 }T

testing a failure told to the errors-to actor: who, the message, the error, the words running
actors-reset
variable told
behaviour catcher
  on actor-failed ( r -- )  told ! ;
end-behaviour
behaviour faulty
  on ping ( n -- )  drop  0 @ drop ;   \ (an error: a null address)
end-behaviour
catcher counter-state s" catcher" spawn constant ct   ct errors-to
faulty counter-state s" faulty" spawn constant fy
T{ 0 fy send ping  deliver  told @ failure-actor  told @ failure-who s" faulty" s=  told @ failure-what s" ping" s= -> fy true true }T
T{ told @ failure-error s" null address" s=  told @ failure-chain s" faulty:on ping" s= -> true true }T
T{ fy alive? -> true }T   \ (the errors actor told: what becomes of it is that one's to say)
testing the errors actor's own failure isn't told to it (it would fail again and again)
behaviour clumsy-catcher
  on actor-failed ( r -- )  drop  1 told +!  0 @ drop ;
end-behaviour
clumsy-catcher counter-state s" clumsy" spawn constant cc   cc errors-to   0 told !
T{ 0 fy send ping  deliver  deliver  told @ -> 1 }T
testing nobody to tell: the actor dies of its failure - its last words, those monitoring it told why, those linked gone
behaviour mourning
  on spawned ( -- )  ;
  on died ( id reason -- )  note note ;
end-behaviour
behaviour faulty-with-words  extends faulty
  on killed ( -- )  77 note ;
end-behaviour
nobody errors-to   0 told !   list log !
faulty-with-words counter-state s" fw" spawn constant fw
quiet counter-state s" tied" spawn constant tied
mourning counter-state s" mourner" spawn constant mo
: tie ( -- )  mo enter  fw monitor  tied monitor  leave-actor   tied enter  fw link  leave-actor ;
T{ tie  0 fw send ping  deliver  told @  fw alive?  tied alive?  mo alive? -> 0 false false true }T
T{ log @ 0 swap nth  log @ length -> 77 5 }T   \ (its last words, then two `died`s: an id and a reason each)
T{ log @ 1 swap nth  failure-error s" null address" s=  log @ 2 swap nth -> true fw }T
T{ log @ 3 swap nth  log @ 1 swap nth =  log @ 4 swap nth -> true tied }T   \ (the one linked died of the same)
testing killed, not failed: the reason is 0
quiet counter-state s" plain" spawn constant plain   list log !
: watch-plain ( -- )  mo enter  plain monitor  leave-actor ;
T{ watch-plain  plain kill  deliver  log @ 0 swap nth  log @ 1 swap nth -> 0 plain }T

testing a handler that waits: its actor busy, its messages kept for it and handled in order once it is done
actors-reset  list log !
behaviour patient
  on ping ( n -- )  dup note  2 wait  10 + note ;
  on pong ( n -- )  note ;
end-behaviour
patient counter-state s" p" spawn constant pa
T{ 1 pa send ping  2 pa send pong  deliver  log @ length  pa actor-busy?  pa actor-mail -> 1 true 1 }T
T{ 2 tick-tasks actors-frame  log @ length  pa actor-busy? -> 3 false }T   \ (its wait over: 11, then the kept 2)
T{ log @ 1 swap nth  log @ 2 swap nth -> 11 2 }T
testing the frame's own messages are missed while busy, not kept
behaviour busy-ticker
  on ping ( n -- )  drop  3 wait  1 note ;
  on tick ( -- )  5 note ;
end-behaviour
actors-reset  list log !
busy-ticker counter-state s" bt" spawn constant bt   bt subscribe tick
T{ 0 bt send ping deliver  actors-frame  1 tick-tasks actors-frame  bt actor-mail  log @ length -> 0 0 }T
T{ 2 tick-tasks actors-frame  log @ length  log @ 0 swap nth  log @ 1 swap nth -> 2 1 5 }T
testing await: the handler goes on with what it awaits; others kept for after
behaviour asking
  on hello ( -- )  1 note  await pong  note  3 note ;
  on ping ( n -- )  note ;
end-behaviour
actors-reset  list log !
asking counter-state s" ask" spawn constant ak
T{ ak send hello  deliver  ak actor-busy?  7 ak send ping  deliver  ak actor-mail -> true 1 }T
T{ 42 ak send pong  deliver  ak actor-busy?  log @ length -> false 4 }T   \ 1, 42, 3, then the kept 7
T{ log @ 1 swap nth  log @ 3 swap nth -> 42 7 }T
testing await-within: it gives up after so many frames (false), or has it (true)
behaviour impatient
  on hello ( -- )  2 await-within pong  if  note  else  -1 note  then ;
end-behaviour
actors-reset  list log !
impatient counter-state s" imp" spawn constant im
T{ im send hello deliver  1 tick-tasks actors-frame  2 tick-tasks actors-frame  log @ 0 swap nth  im actor-busy? -> -1 false }T
T{ im send hello deliver  9 im send pong deliver  log @ 1 swap nth -> 9 }T
testing kill while a handler waits: its last words run, the waiting one let go
behaviour waits-long
  on ping ( n -- )  drop  100 wait  99 note ;
  on killed ( -- )  7 note ;
end-behaviour
actors-reset  list log !
waits-long counter-state s" wl" spawn constant wl
T{ 0 wl send ping deliver  wl kill  wl alive?  200 tick-tasks actors-frame  log @ length  log @ 0 swap nth -> 0 1 7 }T

testing monitor: told `died` with its id when it is gone; link: killed with it
behaviour watcher
  on spawned ( -- )  ;
  on died ( id reason -- )  drop note ;
end-behaviour
actors-reset  list log !
quiet counter-state s" w1" spawn constant watched
watcher counter-state s" w2" spawn constant watching
: watch ( -- )  watching enter  watched monitor  leave-actor ;
T{ watch  watched kill  deliver  log @ length  log @ 0 swap nth  watched = -> 1 true }T
quiet counter-state s" l1" spawn constant leader
quiet counter-state s" l2" spawn constant follower
: follow ( -- )  follower enter  leader link  leave-actor ;
T{ follow  leader kill  leader alive?  follower alive? -> 0 0 }T
quiet counter-state s" l3" spawn constant left
quiet counter-state s" l4" spawn constant stays
: follow-left ( -- )  stays enter  left link  left unlink  leave-actor ;
T{ follow-left  left kill  stays alive? -> true }T

testing a message to nobody is a dead letter, not its subscribers'
actors-reset  list log !
counting counter-state s" sub" spawn constant sb   sb subscribe ping
T{ 5 nobody send ping  deliver  log @ length -> 0 }T
T{ 5 s" not-there" actor-named send ping  deliver  log @ length -> 0 }T
T{ 6 broadcast ping  deliver  log @ length -> 1 }T

testing a number where an actor's name belongs: an error
: named-number ( -- id )  5 3 actor-named ;
T{ ' named-number catch -> -1 }T
: spawn-number ( -- id )  counting counter-state 5 3 spawn ;
T{ ' spawn-number catch -> -1 }T
T{ s" sub" actor-named  s" su" actor-named -> sb nobody }T

testing a state larger than a field's cell can say: an error where the field is declared
: far-field ( -- )  s" state: huge  70000 field far  end-state" evaluate ;
T{ ' far-field catch -> -1 }T

testing what a handler awaits with no room for it on its stack: the handler's failure, told as any other
message sixteen ( a b c d e f g h i j k l m n o p -- )
behaviour hoarding
  on hello ( -- )  250 0 do  i  loop  await sixteen ;
  on ping ( n -- )  note ;
end-behaviour
actors-reset  list log !   0 told !
catcher counter-state s" catcher" spawn constant ct2   ct2 errors-to
hoarding counter-state s" hoard" spawn constant hd
: sixteen-to ( id -- )  {: id :}  16 0 do  i  loop  id send sixteen ;
T{ hd send hello deliver  hd actor-busy?  hd sixteen-to deliver  hd actor-busy?  hd alive? -> true false true }T
T{ told @ failure-actor  told @ failure-what s" hello" s= -> hd true }T
T{ 8 hd send ping deliver  log @ length -> 1 }T   \ (and it handles the next as ever)

testing a wait far longer than there are frames
behaviour waits-for-ever
  on hello ( -- )  -1 1 rshift await-within pong  if  note  else  -1 note  then ;
end-behaviour
actors-reset  list log !
waits-for-ever counter-state s" wfe" spawn constant wf
T{ wf send hello deliver  3 tick-tasks actors-frame  wf actor-busy?  log @ length -> true 0 }T
T{ 4 wf send pong deliver  log @ 0 swap nth -> 4 }T

testing last words run while a handler waits see their own floats only (fdepth, as depth)
behaviour holds-a-float
  on hello ( -- )  1.5e  100 wait  fdrop ;
  on killed ( -- )  depth note  fdepth note ;
end-behaviour
actors-reset  list log !
holds-a-float counter-state s" hf" spawn constant hf
T{ hf send hello deliver  hf kill  log @ 0 swap nth  log @ 1 swap nth -> 0 0 }T

\ (from here: the stack checker's - a system built without it ends the file here)
checker-tests

testing a handler's effect is its message's: a comment that doesn't say so is a warning
: warns ( addr len -- n )  warnings {: before :}  evaluate  warnings before - ;
T{ s" behaviour b1 on ping ( a b -- ) 2drop ; end-behaviour" warns -> 1 }T
T{ s" behaviour b2 on ping ( n -- ) 1+ ; end-behaviour" warns -> 1 }T
T{ s" behaviour b3 on ping ( n -- ) drop ; end-behaviour" warns -> 0 }T
T{ s" behaviour b4 on ping drop ; end-behaviour" warns -> 0 }T
testing sends checked as their messages have them
T{ s" : sends-ok ( id -- ) 5 swap send ping ;" warns -> 0 }T
T{ s" : sends-off ( id -- ) send ping ;" warns -> 1 }T
T{ s" : broadcasts ( -- ) broadcast hello ;" warns -> 0 }T

test-summary

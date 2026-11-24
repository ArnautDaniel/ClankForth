\ preemption: an actor's handler given slice-steps steps a turn, then another's - a long one doesn't hold its node,
\ isn't stopped as a loop that never ends, and goes on where it gave way; what it works on stays its own
IN: test-preempt
USING: tester actors json ;

slice-steps constant tool-slice
: drain ( -- )  10000 0 do  deliver  loop ;   \ (each deliver: a slice more for each handler that gave way)

testing the tool slices handlers (FORTH_SLICE_STEPS)
T{ tool-slice 0> -> true }T

testing a long handler: past the 20 million steps a turn once had, it ends - its sum right - and another actor's
testing  message is handled while it works (it gave way)
message crunch ( n -- )
message ping ( -- )
variable sum   variable crunched   variable pinged   variable pinged-while
behaviour cruncher
    on crunch ( n -- )  {: n | s :}  n 0 do  i s + to s  loop  s sum !  true crunched ! ;
end-behaviour
behaviour pinger  on ping ( -- )  true pinged !  crunched @ 0= pinged-while ! ;  end-behaviour
cruncher -1 s" cruncher" spawn constant c
pinger -1 s" pinger" spawn constant p
30000000 c send crunch  p send ping  deliver
T{ pinged @  pinged-while @  crunched @ -> true true false }T
: until-crunched ( -- )  begin  crunched @ 0=  while  deliver  repeat ;
until-crunched
T{ sum @ -> 30000000 dup 1- * 2/ }T

testing a handler's place, its locals, its task-variables: kept across thousands of slices - two actors parsing
testing  JSON at once (the parser's place each one's own), each its own answer
message parse ( n -- )
variable a-len   variable b-len
: numbers ( n -- a n )   \ [0,1,2,...,n-1]
    {: n :}  [:  [char] [ %c  n 0 ?do  i if  [char] , %c  then  i n>str %s  loop  [char] ] %c ;] build string ;
behaviour parser-a  on parse ( n -- )  numbers json> {: l :}  l length  l last +  a-len ! ;  end-behaviour
behaviour parser-b  on parse ( n -- )  numbers json> {: l :}  l length  l last +  b-len ! ;  end-behaviour
parser-a -1 s" pa" spawn constant pa
parser-b -1 s" pb" spawn constant pb
50 slice-steps!   \ (a slice of 50 steps: they take turns all through)
0 a-len !  0 b-len !
: parsed ( -- )  1000000 0 do  a-len @ 0<>  b-len @ 0<> and if  leave  then  deliver  loop ;
2000 pa send parse  3000 pb send parse  parsed
T{ a-len @  b-len @ -> 3999 5999 }T
tool-slice slice-steps!

testing slice-steps 0: no slices - a handler runs till it ends; atomically: no slice ends inside it
0 slice-steps!
false crunched !  false pinged !
100000 c send crunch  p send ping  deliver
T{ crunched @  pinged @  pinged-while @ -> true true false }T
tool-slice slice-steps!
variable shared
message add-many ( n -- )
behaviour adder  on add-many ( n -- )  0 do  [: shared @ 1+ shared ! ;] atomically  loop ;  end-behaviour   \ (read, then written)
adder -1 s" a1" spawn constant a1
adder -1 s" a2" spawn constant a2
20 slice-steps!
0 shared !  5000 a1 send add-many  5000 a2 send add-many  drain
T{ shared @ -> 10000 }T
tool-slice slice-steps!

testing a handler's yield: it goes on at the next frame (not spun on between frames)
message yields ( n -- )
variable yielded-all
behaviour yielder  on yields ( n -- )  0 do  yield  loop  true yielded-all ! ;  end-behaviour
yielder -1 s" yielder" spawn constant y
false yielded-all !
: delivers ( n -- )  0 do  deliver  loop ;
20 y send yields  200 delivers
T{ yielded-all @ -> false }T
: frames ( n -- )  0 do  1 tick-tasks  actors-frame  loop ;
25 frames
T{ yielded-all @ -> true }T
y kill

c kill  p kill  pa kill  pb kill  a1 kill  a2 kill
test-summary

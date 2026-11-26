\ rounds: what the system's own loops do (serve's, a node's, a script's `begin round net-poll drop again`) - what can
\ run, runs, and the loop is told how long it may wait for what comes. Nothing in them counts frames: those are a
\ host's whose loop runs them, and their words the `frames` vocabulary's - not in sight unless asked for.
IN: test-rounds
USING: tester actors system ;

: says ( text n -- a n )  ['] evaluate catch drop  2drop  error-message ;   \ what evaluating it says is wrong
: says? ( text n what n -- flag )  {: wa wn :}  says wa wn s-find 0>= ;
: says-in? ( s a n -- flag )  {: s a n :}  s string a n s-find 0>=  s dispose ;   \ (of what with-output kept)

testing a frame's words: not everyone's - a script that hasn't asked for `frames` is told where they are
T{ s" : naps ( -- )  3 wait ;" s" unknown word wait (in vocabulary frames: add it to USING:)" says? -> true }T
T{ s" 1 tick-tasks" s" in vocabulary frames" says? -> true }T
T{ s" actors-frame" s" in vocabulary frames" says? -> true }T
T{ s" behaviour ticked  on tick ( -- ) ;  end-behaviour" s" tick is a message of a host's frames" says? -> true }T
testing ... and a message of one's own may have a frame's name: it is a message like any other
message tick ( -- )
variable ticks
behaviour clock  on tick ( -- )  1 ticks +! ;  end-behaviour
clock -1 s" clock" spawn constant ticking
T{ ticking send tick  deliver  ticks @ -> 1 }T
T{ round drop  round drop  ticks @ -> 1 }T   \ (no round sends it: nothing here counts frames)
testing ... kept for a busy actor as any is (the frame's own are missed by one busy: these aren't those)
message hold-on ( -- )
message go-on ( -- )
behaviour held-clock  on hold-on ( -- )  await go-on ;  on tick ( -- )  1 ticks +! ;  end-behaviour
held-clock -1 s" held-clock" spawn constant holding
T{ 0 ticks !  holding send hold-on  deliver  holding send tick  holding send tick  deliver  holding busy?  ticks @ -> true 0 }T
T{ holding send go-on  deliver  ticks @ -> 2 }T
T{ ' tick >body @ kind-name s" tick" s= -> true }T   \ (a kind of its own, by its own name)
ticking kill  holding kill

testing a round: nothing to run - the loop may wait (a second at most), and each is counted
T{ round 0>  round 1000 <= -> true true }T
T{ rounds  round drop  round drop  rounds swap - -> 2 }T
testing a message waiting: delivered by the round, its handler run
message poke ( n -- )
variable poked
behaviour pokee  on poke ( n -- )  poked +! ;  end-behaviour
pokee -1 s" pokee" spawn constant p
T{ 0 poked !  5 p send poke  round drop  poked @ -> 5 }T

testing a task: its turns are the rounds' - one that yields goes on at the next, and the loop isn't to wait while it can
variable steps
: stepper ( -- )  3 0 do  1 steps +!  yield  loop ;
T{ 0 steps !  ' stepper start-task drop  round  steps @ -> 0 1 }T
T{ round  steps @ -> 0 2 }T
T{ round drop  round drop  round 0>  steps @ -> true 3 }T
testing ... a handler that yields, the same
message step-on ( n -- )
behaviour yielder  on step-on ( n -- )  0 do  1 steps +!  yield  loop ;  end-behaviour
yielder -1 s" yielder" spawn constant y
T{ 0 steps !  3 y send step-on  round  steps @ -> 0 1 }T
T{ round drop  round drop  round drop  round 0>  steps @  y busy? -> true 3 false }T

testing one that sleeps: the loop told to wait no longer than till its time is up, and it goes on then
message nap ( ms -- )
variable napped
behaviour napper  on nap ( ms -- )  sleep-ms  true napped ! ;  end-behaviour
napper -1 s" napper" spawn constant n
: until-napped ( -- )  200 0 do  round 10 min sleep-ms  napped @ if leave then  loop ;
T{ false napped !  60 n send nap  round  dup 0>  swap 70 <  napped @ -> true true false }T
T{ ms-now  until-napped  ms-now swap -  dup 50 >=  swap 500 <  napped @ -> true true true }T

testing a wait counted in frames where nothing counts them: an error said, not a task left waiting for ever
variable waited
: frame-waiter ( -- )  2 frames:wait  true waited ! ;
: failing-round ( -- s )  [: round drop ;] with-output ;
T{ false waited !  ' frame-waiter start-task drop  failing-round  s" wait: nothing counts frames here" says-in?  waited @ -> true false }T
message wait-for ( -- )
message never ( -- )
behaviour frame-awaiter  on wait-for ( -- )  5 frames:await-within never  drop  true waited ! ;  end-behaviour
frame-awaiter -1 s" frame-awaiter" spawn constant fa
: failing-delivery ( -- s )  [: fa send wait-for  round drop ;] with-output ;
T{ false waited !  failing-delivery  s" await-within: nothing counts frames here" says-in?  waited @ -> true false }T
testing ... one that began its wait where frames might have come (a handler run by `deliver`, a task by a frame):
testing  stopped by the first round, and said - a failure of its handler's as any other (no errors actor here: it
testing  dies of it), not an actor left busy for ever with its mail undelivered
message wait-frames ( -- )
behaviour frame-napper  on wait-frames ( -- )  2 frames:wait  true waited ! ;  on poke ( n -- )  poked +! ;  end-behaviour
frame-napper -1 s" frame-napper" spawn constant fn
T{ false waited !  0 poked !  fn send wait-frames  deliver  3 fn send poke  deliver  fn busy?  poked @ -> true 0 }T
T{ failing-round  s" wait: nothing counts frames here" says-in?  fn alive?  waited @ -> true false false }T
T{ false waited !  ' frame-waiter start-task drop  1 frames:tick-tasks  failing-round  s" stopped by the error" says-in?  waited @ -> true false }T
testing ... and where a host runs frames, they are counted as ever (its words asked for by name here)
T{ false waited !  ' frame-waiter start-task drop  1 frames:tick-tasks  waited @  2 frames:tick-tasks  waited @ -> false true }T

p kill  y kill  n kill
test-summary

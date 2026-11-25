\ work that blocks (jobs: docs/actors.md) - in a handler, a file read or written, an fsync, another program, a sleep,
\ a slow hash done on a worker thread while the actor waits and its node's other actors go on; at the prompt, here
IN: test-jobs
USING: tester actors system crypto ;

message work ( xt -- )
message ping ( -- )
variable result   variable worked   variable pinged   variable busy-when-pinged
behaviour worker  on work ( xt -- )  execute result !  true worked ! ;  end-behaviour
behaviour pinger  on ping ( -- )  true pinged !  worked @ 0= busy-when-pinged ! ;  end-behaviour
worker -1 s" worker" spawn constant w
pinger -1 s" pinger" spawn constant p
: settle ( -- )  3000 0 do  deliver  worked @ if  leave  then  1 sleep-ms  loop ;   \ (at the prompt: a real sleep)
: run ( xt -- x )  false worked !  w send work  settle  result @ ;

testing a sleep in a handler: the actor waits, the node's other actors go on meanwhile
: napping ( -- 0 )  200 sleep-ms  0 ;
false pinged !  false worked !
' napping w send work  deliver  p send ping  deliver
T{ pinged @  busy-when-pinged @  worked @ -> true true false }T
settle
T{ worked @ -> true }T

testing a file written and read in a handler (on a worker); one not there: the error, caught in the handler
: path ( -- a n )  s" clankforth-jobs-test.txt" ;
: write-read ( -- n )  s" written on a worker" path write-file  path read-file nip ;
T{ ' write-read run -> 19 }T
: appended ( -- n )  s" , and more" path append-file  path read-bytes b-length ;
T{ ' appended run -> 29 }T
: missing ( -- flag )  [: s" no-such-file-here.txt" read-file 2drop ;] catch  error-message s" no-such-file-here" s-find 0> and ;
T{ ' missing run -> true }T
path delete-file

testing another program's output, from a handler
: echoed ( -- flag )  s" echo hi" shell-output drop  s" hi" s-starts? ;
T{ ' echoed run -> true }T

testing a key made with PBKDF2 in a handler: the same as at the prompt
: key ( -- s n )  s" password" s" salt" 1000 32 pbkdf2-sha256 ;
key >string drop constant prompt-key
: same-key ( -- flag )  key  prompt-key string s= ;
T{ ' same-key run -> true }T

testing a plain task run while an actor is entered (not the actor's own task): its sleep done there, no job left
variable task-slept
: task-nap ( -- )  100 sleep-ms  true task-slept ! ;
false task-slept !
' task-nap start-task drop
variable task-took
p enter  ms-now  1 tick-tasks  ms-now swap -  task-took !  leave-actor
T{ task-took @ 100 >=  task-slept @ -> true true }T

testing sleep-ms through execute (threaded: done where it is, not waited - it couldn't go on there)
: exec-nap ( -- 0 )  20 ['] sleep-ms execute  0 ;
T{ ' exec-nap run -> 0 }T

testing many handlers sleeping don't hold up the workers: a file read in another handler at once
message nap ( -- )
behaviour sleeper  on nap ( -- )  1000 sleep-ms ;  end-behaviour
: sleepers ( -- )  12 0 do  sleeper -1 s" sleeper" spawn  send nap  loop  deliver ;
sleepers
s" read while they sleep" path write-file
: timed-read ( -- ms )  ms-now  path read-file 2drop  ms-now swap - ;
T{ ' timed-read run 500 < -> true }T
path delete-file

testing await-within-ms: the clock's milliseconds, not frames - an answer that never comes given up on in about
testing  that long; one that comes taken, with true
message ask-back ( -- )
message answered-back ( n -- )
message try-wait ( -- )
variable waited   variable wait-took
behaviour mute  on ask-back ( -- ) ;  end-behaviour
behaviour echo-back  on ask-back ( -- )  9 sender send answered-back ;  end-behaviour
mute -1 s" mute" spawn constant mute-a
echo-back -1 s" echo-back" spawn constant echo-a
behaviour waiter
  on try-wait ( -- )  ms-now {: t0 :}  mute-a send ask-back  100 await-within-ms answered-back waited !  ms-now t0 - wait-took !
                      echo-a send ask-back  2000 await-within-ms answered-back if  waited +!  then ;
end-behaviour
waiter -1 s" waiter" spawn constant waiter-a
: wait-settle ( -- )  300 0 do  deliver  actors-frame  2 sleep-ms  loop ;
false waited !  waiter-a send try-wait  wait-settle
T{ waited @  wait-took @ 90 >  wait-took @ 1000 < -> 9 true true }T
mute-a kill  echo-a kill  waiter-a kill

testing an actor killed while its job works: the job let go as it ends (nothing left, nothing freed twice)
' napping w send work  deliver  w kill  deliver
300 sleep-ms   \ (the worker's sleep over: it frees the job no one waits for)
T{ w alive? -> false }T

p kill
test-summary

\ each running piece of Forth's own state: task-variables, base, the last error, n>str's and $+'s buffers - another
\ actor's handler, run between two of a handler's words (here: while it waits), changes none of them; atomically
IN: test-task-state
USING: tester actors ;

task-variable mine
variable a-ok   variable b-ran
message go ( n -- )
: settle ( -- )  20 0 do  1 tick-tasks  actors-frame  loop ;

testing a task-variable, base, error-message, n>str: each handler's own, whatever another does while it waits
behaviour first
    on go ( n -- )
        {: n :}  n mine !  16 base !
        [: s" first's error" >string drop throw ;] catch drop
        42 n>str drop {: digits :}
        3 wait   \ (the other runs now: sets its own of each)
        mine @ n =  base @ 16 =  and  error-message s" first's error" s= and  digits 2 s" 42" s= and  a-ok !
        10 base ! ;
end-behaviour
behaviour second
    on go ( n -- )
        {: n :}  n mine !  2 base !  [: s" second's error" >string drop throw ;] catch drop  7 n>str 2drop
        true b-ran ! ;
end-behaviour
first -1 s" first" spawn constant a
second -1 s" second" spawn constant b
false a-ok !  false b-ran !
1 a send go  deliver  2 b send go  deliver  settle
T{ b-ran @  a-ok @ -> true true }T
T{ mine @  base @ -> 0 10 }T   \ (the interpreter's own: neither handler's)

testing atomically: its code run as it is (its stack effect followed), and no wait inside it
: doubled ( n -- n n )  [: dup 2* ;] atomically ;
T{ 5 doubled -> 5 10 }T
: waits-inside ( -- )  [: 1 wait ;] atomically ;
T{ ' waits-inside catch -> -1 }T
task-variable counted
: bump ( -- n )  [: 1 counted +!  counted @ ;] atomically ;
T{ bump bump -> 1 2 }T

a kill  b kill
test-summary

\ a program's nodes as processes (`as-nodes`, src/supervisor.c): tests/pool/pool.fs run as the tool runs a program,
\ and what its nodes left looked at. ctest gives the tool and that program: clankforth pool_driver.fs -- tool pool.fs
IN: test-pool
USING: tester system ;

: tool ( -- a n )  0 args nth string ;
: program ( -- a n )  1 args nth string ;
: out ( -- a n )  s" pool-test-out" ;
: wipe ( -- )
    out dir? 0= if  out make-dir  exit  then
    out dir-list {: l :}  l length 0 ?do  out s" /" s+  i l nth string s+  delete-file  loop ;
: run ( scenario n -- status )   \ the program run, to its end: the supervisor's status
    {: a n :}  wipe  tool program a n out s\" \"{s}\" \"{s}\" -- {s} {s}" format shell ;
: left ( name n i -- a n )   \ what node i wrote ("" if it wrote nothing)
    {: a n i :}  out a n i s" {s}/{s}-{}" format {: p pn :}
    p pn file-exists? if  p pn read-file  else  s" "  then ;
: left? ( name n i -- flag )  left nip 0> ;

testing a program no supervisor started: the only node there is
T{ node-index node-count node-life -> 1 1 1 }T

testing as-nodes: the program again for each node, a process of its own - each knows which it is of how many
T{ s" hello" run -> 0 }T
T{ s" hello" 1 left s" 1 3 1" s=  s" hello" 2 left s" 2 3 1" s=  s" hello" 3 left s" 3 3 1" s= -> true true true }T
T{ s" hello" 4 left? -> false }T

testing a node that ends badly: started again - its next life - while the others go on; the pool done when all are
T{ s" again" run -> 0 }T
T{ s" again" 1 left s" 1 2 1" s=  s" again" 2 left s" 2 2 2" s= -> true true }T

testing a program that can't run (its node ends as it starts, every time): the pool stops, saying so - not for ever
T{ s" wrong" run 0<> -> true }T
T{ s" wrong" 1 left s" 1 1 5" s= -> true }T   \ (five lives: then no more)

: skip-without-kill ( -- )   \ (the rest sends signals: a system with no kill - Windows - has done)
    s" kill -0 $PPID" shell 0<> if  ." (no kill here: the rest skipped)" cr  test-summary skip-file  then ;
skip-without-kill

testing a node's process killed outright (a crash): started again, the others untouched
T{ s" crash" run -> 0 }T
T{ s" crash" 1 left s" 1 2 2" s=  s" crash" 2 left s" 2 2 1" s= -> true true }T

\ the program left running: its supervisor's pid
: start ( scenario n -- pid )
    {: a n :}  wipe
    tool program a n out s\" \"{s}\" \"{s}\" -- {s} {s} > /dev/null 2>&1 & echo $!" format shell-output drop
    s-trim string>number drop ;
: wait-for ( name n i -- flag )   \ till node i has written it (5 s at most)
    {: a n i :}  100 0 do  a n i left? if  true unloop exit  then  50 sleep-ms  loop  false ;
: alive? ( pid -- flag )  s" kill -0 {} 2> /dev/null" format shell 0= ;
: gone? ( pid -- flag )   \ ... within 5 s
    {: pid :}  100 0 do  pid alive? 0= if  true unloop exit  then  50 sleep-ms  loop  false ;
variable supervisor

testing the pool told to stop (SIGTERM to its supervisor): each node told, its on-shutdown word run, all ended
T{ s" stop" start supervisor !  s" up" 1 wait-for  s" up" 2 wait-for -> true true }T
T{ supervisor @ s" kill -TERM {}" format shell  s" stopped" 1 wait-for  s" stopped" 2 wait-for  supervisor @ gone? -> 0 true true true }T

testing the supervisor killed outright: its nodes end too (their on-shutdown words run) - none left behind
T{ s" stop" start supervisor !  s" up" 1 wait-for  s" up" 2 wait-for -> true true }T
T{ supervisor @ s" kill -KILL {}" format shell  s" stopped" 1 wait-for  s" stopped" 2 wait-for -> 0 true true }T

wipe  out delete-file
test-summary

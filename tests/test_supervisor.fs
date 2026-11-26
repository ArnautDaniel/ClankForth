\ supervisor (basis/supervisor): an errors actor by default - a failing handler's actor lives on (its failure a JSON
\ line), one failing too often killed, an essential one never; actor-bytes: what an actor's objects take
IN: test-supervisor
USING: tester actors supervisor ;

message boom ( -- )
message hold ( n -- )
variable put-right
behaviour fragile
    on boom ( -- )  true abort" broken on purpose" ;
    on start-over ( -- )  1 put-right +! ;
    on hold ( n -- )  {: n :}  [: n 0 do  [char] x %c  loop ;] build rescue drop ;
end-behaviour
: pump ( -- )  20 0 do  deliver  loop ;

testing no supervisor: a failing actor dies of it (the kernel's own rule)
T{ errors-actor -> nobody }T
fragile -1 s" alone" spawn constant alone
T{ alone send boom  pump  alone alive? -> false }T

testing supervise: an errors actor, once; a failing actor lives on, told start-over
supervise  errors-actor constant sup
T{ sup nobody <>  supervise errors-actor sup = -> true true }T
fragile -1 s" lives" spawn constant lives
T{ 0 put-right !  lives send boom  pump  lives alive?  put-right @ -> true 1 }T
testing failing too often (more than restart-max in restart-window-ms): killed
: fail-times ( id n -- )  {: id n :}  n 0 do  id alive? if  id send boom  pump  then  loop ;
T{ lives restart-max 1+ fail-times  lives alive? -> false }T
testing an essential actor: never killed for failing
fragile -1 s" vital" spawn constant vital
vital essential  pump
T{ vital restart-max 3 + fail-times  vital alive? -> true }T

testing actor-bytes: what an actor's objects take (a string it keeps)
fragile -1 s" holder" spawn constant holder
T{ holder actor-bytes -> 0 }T
T{ 100000 holder send hold  pump  holder actor-bytes 100000 >= -> true }T

testing on-actor-failed: another destination told each failure (the actor's name here); one whose own code fails
testing  said and passed over - the supervisor goes on, the actor put right as ever
create heard 64 allot   variable heard-n
: hear ( r -- )  failure-who 63 min dup heard-n !  heard swap move ;
' hear on-actor-failed
: broken-hook ( r -- )  drop  true abort" a hook broken on purpose" ;
' broken-hook on-actor-failed
fragile -1 s" hooked" spawn constant hooked
T{ 0 put-right !  hooked send boom  pump  heard heard-n @ s" hooked" s=  put-right @  hooked alive? -> true 1 true }T
T{ errors-actor alive? -> true }T

testing a handler that held its node, with a supervisor: told it (actor-slow) - a JSON line, not the bare slow: line
message grind ( -- )
behaviour grinding  on grind ( -- )  [: 0  3000000 0 do  1+  loop  drop ;] atomically ;  end-behaviour
grinding -1 s" grinder" spawn constant grinder
1 slow-handler-ms!
: ground ( -- s )  [: grinder send grind  pump ;] with-output ;
: said? ( s a n -- flag )  rot string 2swap s-find 0>= ;
: slow-told ( -- f1 f2 f3 f4 )
    ground {: o :}  o s\" \"event\":\"actor-slow\"" said?  o s\" \"actor\":\"grinder\"" said?  o s\" \"message\":\"grind\"" said?
    o s" slow: grinder" said? ;
T{ slow-told -> true true true false }T
200 slow-handler-ms!
T{ 0 kind-name s" frames:tick" s= -> true }T   \ (the kernel's first: a host's frame's, named apart from a script's own)
grinder kill

test-summary

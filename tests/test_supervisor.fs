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
: pump ( -- )  20 0 do  actors-frame  loop ;

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

test-summary

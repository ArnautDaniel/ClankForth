\ the repl (net's repl-listen): a session's lines run as the prompt's, each answered with what it printed and a last
\ line - ok (ok name#id attached), compiling, error. Here the session's other end is a client actor of this system.
IN: test-repl
USING: tester actors net ;

<bytes> constant heard        \ what the session has said since the last line was sent
variable conn
behaviour repl-client
  on net-connected ( conn -- )  drop ;
  on net-received ( conn -- )  {: c :}  c net-input heard b-append  c net-input nip c net-consume ;
  on net-closed ( conn -- )  drop ;
end-behaviour
repl-client -1 s" client" spawn constant client
deliver

\ the last whole line heard (its newline left off)
: last-line ( -- a n )
    heard b-data {: a n | i :}
    n 2 < if  a 0 exit  then
    n 2 - to i
    begin  i 0>  a i + c@ 10 <>  and  while  i 1- to i  repeat
    i 0> if  i 1+ to i  then
    a i +  n 1- i - ;
: answered? ( -- flag )   \ the session's last line has come: how its line went
    heard b-length 0= if  false exit  then
    heard b-data + 1- c@ 10 <> if  false exit  then
    last-line {: a n :}
    a n s" ok" s-starts?  a n s" error" s-starts? or  a n s" compiling" s-starts? or  a n s" bye" s-starts? or ;
: answer ( -- )  300 0 do  10 net-poll drop  deliver  answered? if leave then  loop ;
: ask ( a n -- )  heard b-clear  conn @ net-write  s\" \n" conn @ net-write  answer ;
: heard? ( a n -- flag )  heard b-data 2swap s-find 0>= ;   \ said, somewhere in the answer
: ended? ( a n -- flag )  last-line s= ;                     \ the last line

testing a session: greeted, then each line answered as the prompt's
0 repl-listen constant listener
client enter  s" 127.0.0.1" listener listener-port connect  leave-actor  conn !
answer
T{ s" repl" heard?  s" ok" ended? -> true true }T
T{ s" 1 2 + ." ask  s" 3 " heard?  s" ok" ended? -> true true }T

testing the session's stack is its own, kept between lines; an error empties it
T{ s" 5 6" ask  s" .s" ask  s" <2> 5 6" heard? -> true }T
T{ s" 0 @" ask  s" error" ended? -> true }T
T{ s" .s" ask  s" <0>" heard? -> true }T

testing a definition over two lines: compiling, then done
T{ s" : seven ( -- n )" ask  s" compiling" ended? -> true }T
T{ s" 7 ;" ask  s" seven ." ask  s" 7 " heard? -> true }T

testing a word defined again in the session: what was compiled for the old one runs it
: greet ( -- n )  1 ;
: use-greet ( -- n )  greet ;
T{ s" IN: test-repl  : greet ( -- n )  2 ;" ask  s" ok" ended?  use-greet -> true 2 }T

testing attach: the session's lines run as that actor (its fields), the last line naming it; detach
message bump ( n -- )
state: counted  cell field total  end-state
behaviour counting  on bump ( n -- )  total +! ;  end-behaviour
counting counted s" counter" spawn constant counter
5 counter send bump  deliver
T{ counter s" {} attach" format ask  s" ok counter#" heard? -> true }T
T{ s" total @ ." ask  s" 5 " heard?  s" ok counter#" heard? -> true true }T
T{ s" detach" ask  s" ok" ended? -> true }T
T{ s" counting bump see-handler" ask  s" total +!" heard? -> true }T
T{ s" counter .actor" ask  s" name: counter" heard?  s" handles: bump" heard? -> true true }T

testing no waiting in a session's line; bye ends the session, not the system
T{ s" 5 wait" ask  s" error" ended? -> true }T
T{ s" bye" ask  s" bye" ended? -> true }T
T{ s" still here" nip -> 10 }T

listener net-close
test-summary

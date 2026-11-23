\ the net vocabulary (the tool has it): sockets and timers, each an actor's, what happens told as messages
IN: test-net
USING: tester actors net ;

variable port   variable got   variable closed-seen   variable fired
create reply 64 allot   variable reply-n

\ an echo server: each connection accepted is its own, what comes sent back
behaviour echo-server
  on net-accepted ( listener conn -- )  nip drop ;
  on net-received ( conn -- )  {: c :}  c net-input  c net-write  c net-input nip c net-consume ;
  on net-closed ( conn -- )  net-close  1 closed-seen +! ;
end-behaviour

\ a client: connects, says hello, keeps what comes back
behaviour echo-client
  on net-connected ( conn -- )  s" hello" rot net-write ;
  on net-received ( conn -- )  {: c :}  c net-input  dup reply-n +!  reply swap move  c net-input nip c net-consume  1 got ! ;
  on net-closed ( conn -- )  drop ;
  on timer-fired ( timer -- )  drop  1 fired +! ;
end-behaviour

echo-server -1 s" server" spawn constant server
echo-client -1 s" client" spawn constant client
deliver
: pump ( var -- )  {: v :}  200 0 do  10 net-poll drop  deliver  v @ if leave then  loop ;

testing a listener on a free port; a client connected, its bytes echoed back
server enter  0 listen  leave-actor  constant listener
T{ listener listener-port dup port !  0> -> true }T
0 got !  client enter  s" 127.0.0.1" port @ connect  leave-actor  constant conn
T{ got pump  got @ -> 1 }T
T{ reply reply-n @ s" hello" compare -> 0 }T
T{ conn conn-peer  over >r  s" 127.0.0.1:" s-starts?  r> dispose -> true }T
testing a listener on IPv6 and IPv4 both: a client by ::1 echoed too
0 got !  0 reply-n !  client enter  s" ::1" port @ connect  leave-actor  constant conn6
T{ got pump  reply reply-n @ s" hello" compare -> 0 }T
0 closed-seen !  conn6 net-close  closed-seen pump   \ (its close seen, before the next test counts)
testing the client closing: the server told net-closed
T{ 0 closed-seen !  conn net-close  closed-seen pump  closed-seen @ -> 1 }T
T{ conn ' net-input catch nip -> -1 }T   \ (a closed connection's handle names nothing)
testing a timer: timer-fired after its time, not before; one cancelled never fires
0 fired !  client enter  30 after drop  10000 after  leave-actor  constant late
T{ 0 net-poll drop deliver  fired @ -> 0 }T
T{ fired pump  fired @ -> 1 }T
T{ late cancel-timer  0 fired !  50 net-poll drop  deliver  fired @ -> 0 }T
testing listen and connect outside an actor: an error (what happens is told to an actor)
T{ 0 ' listen catch nip -> -1 }T
testing a connection whose actor is gone is closed
behaviour holder  on net-connected ( conn -- )  drop ;  on net-closed ( conn -- )  drop ;  end-behaviour
holder -1 s" holder" spawn constant held-by
held-by enter  s" 127.0.0.1" port @ connect  leave-actor  constant held   held-by kill
T{ 20 net-poll drop  20 net-poll drop  held ' net-input catch nip -> -1 }T
listener net-close

testing a burst: 200 clients connecting at once to one node - every one accepted, none refused (Windows' backlog
testing  asked for in full, not its "reasonable" 200)
variable burst-port   variable burst-accepted   variable burst-connected   variable burst-refused
message burst-listen ( -- )
message burst-dial ( n -- )
behaviour burster
  on burst-listen ( -- )  0 listen listener-port burst-port ! ;
  on net-accepted ( listener conn -- )  nip drop  1 burst-accepted +! ;   \ (kept open: a burst holds them all)
  on burst-dial ( n -- )  0 ?do  s" 127.0.0.1" burst-port @ connect drop  loop ;
  on net-connected ( conn -- )  drop  1 burst-connected +! ;
  on net-closed ( conn -- )  drop  1 burst-refused +! ;
end-behaviour
burster -1 s" burster" spawn constant bursting
: burst-pump ( n -- )  0 ?do  actors-frame  10 net-poll drop  loop ;
bursting send burst-listen  5 burst-pump
200 bursting send burst-dial
: burst-settled ( -- )  300 0 do  actors-frame  10 net-poll drop  burst-accepted @ 200 = burst-connected @ 200 = and if leave then  loop ;
burst-settled
T{ burst-connected @  burst-accepted @  burst-refused @ -> 200 200 0 }T

test-summary

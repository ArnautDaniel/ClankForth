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
: burst-pump ( n -- )  0 ?do  deliver  10 net-poll drop  loop ;
bursting send burst-listen  5 burst-pump
200 bursting send burst-dial
: burst-settled ( -- )  300 0 do  deliver  10 net-poll drop  burst-accepted @ 200 = burst-connected @ 200 = and if leave then  loop ;
burst-settled
T{ burst-connected @  burst-accepted @  burst-refused @ -> 200 200 0 }T

testing what waits to go has a limit: a write past it ends the connection (a peer that doesn't read); what has come
testing  not yet taken too: not read on past its limit (till taken)
variable cap-port   variable cap-conn   variable cap-dialled
message cap-listen ( -- )
message cap-dial ( -- )
behaviour capper
  on cap-listen ( -- )  0 listen listener-port cap-port ! ;
  on net-accepted ( listener conn -- )  nip cap-conn ! ;
  on cap-dial ( -- )  s" 127.0.0.1" cap-port @ connect cap-dialled ! ;
  on net-connected ( conn -- )  drop ;
  on net-received ( conn -- )  drop ;
  on net-closed ( conn -- )  drop ;
end-behaviour
capper -1 s" capper" spawn constant capping
: cap-pump ( n -- )  0 ?do  deliver  10 net-poll drop  loop ;
capping send cap-listen  5 cap-pump  capping send cap-dial  20 cap-pump
T{ cap-conn @ 0<> -> true }T
16 cap-conn @ net-input-max!
: big ( -- s len )  [: 4000 0 do  s" 0123456789" %s  loop ;] build string ;
: send-big ( -- )  big {: ba bn :}  ba bn cap-dialled @ net-write  ba dispose ;
send-big  20 cap-pump
T{ cap-conn @ net-input nip -> 16 }T   \ (not all 40000: its limit)
32 cap-conn @ net-input-max!  20 cap-pump
T{ cap-conn @ net-input nip -> 32 }T   \ (more let in: read on)
: has? ( a n b n -- flag )  s-find 0>= ;
10 cap-conn @ net-output-max!
T{ s" 0123456789abcdef" cap-conn @ net-write  cap-conn @ net-ended?  cap-conn @ net-why s" waiting to go" has? -> true true }T
capping kill

testing a handler given way in the middle of what came (a long body, read through): what comes meanwhile waits, and
testing  the input stays where the handler has it - read on, it was moved, and the handler went on in freed memory;
testing  the peer closing meanwhile: the connection, and what came, the handler's till it ends
variable slow-port   variable slow-dialled   variable slow-begun   variable slow-same   variable slow-done
variable slow-total   variable slow-closed
message slow-listen ( -- )
message slow-dial ( -- )
behaviour slow-reader
  on slow-listen ( -- )  0 listen listener-port slow-port ! ;
  on net-accepted ( listener conn -- )  2drop ;
  on slow-dial ( -- )  s" 127.0.0.1" slow-port @ connect slow-dialled ! ;
  on net-connected ( conn -- )  drop ;
  on net-received ( conn -- )
      {: c :}  c net-input {: a n :}  true slow-begun !
      n 5 = if   \ (the first of it: twenty slices over it - the net is looked at between them)
          400000 0 do  loop  c net-input  n =  swap a = and  slow-same !
      then
      n slow-total +!  n c net-consume  true slow-done ! ;
  on net-closed ( conn -- )  drop  true slow-closed ! ;
end-behaviour
: slow-pump ( var -- )  {: v :}  5000 0 do  v @ if  leave  then  deliver  0 net-poll drop  loop ;
: slowly ( close? -- same? total closed? )
    slow-reader -1 s" slow" spawn {: close? r :}
    false slow-begun !  false slow-same !  false slow-done !  0 slow-total !  false slow-closed !  0 slow-dialled !
    r send slow-listen  5 cap-pump  r send slow-dial  20 cap-pump
    s" first" slow-dialled @ net-write  slow-begun slow-pump
    big {: ba bn :}  ba bn slow-dialled @ net-write  ba dispose   \ (the handler in its loop: more comes)
    close? if  slow-dialled @ net-close  then
    slow-done slow-pump  200 cap-pump
    slow-same @  slow-total @  slow-closed @  r kill  deliver ;
T{ false slowly -> true 40005 false }T
T{ true slowly -> true 40005 true }T

test-summary

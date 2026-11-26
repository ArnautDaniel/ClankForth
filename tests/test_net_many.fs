\ net with many connections and timers: what a round does is for those something happened to, not for all there are -
\ the timers a heap, a connection found again by its owner's death, a slot let go taken again
IN: test-net-many
USING: tester actors net system ;

variable fired   variable order   variable echoed   variable echoed-bytes   variable closed-seen   variable port
: pump ( var n -- )  {: v n :}  300 0 do  5 net-poll drop  deliver  v @ n >= if leave then  loop ;

testing how this build waits for its sockets: as it was configured to (the build tells the test what it asked for -
testing  a completion port on Windows, epoll on Linux, else poll; run by hand, whatever it has)
: waits-as-built? ( -- flag )
    s" CLANKFORTH_EXPECT_WAIT" getenv 0= if  true exit  then  {: s n :}
    net-waits {: w wn :}  s n w wn s=  s dispose  w dispose ;
T{ waits-as-built? -> true }T
T{ objects  net-waits over >r  nip 0>  r> dispose  objects rot = -> true true }T

\ ---- timers ----
message set-timers ( -- )
create ids 8 cells allot
: id ( i -- a )  cells ids + ;
behaviour ticker
  on set-timers ( -- )  60 after 0 id !  20 after 1 id !  40 after 2 id !  40 after 3 id ! ;
  on timer-fired ( timer -- )  {: t :}  4 0 do  t i id @ = if  order @ 10 * i 1+ + order !  then  loop  1 fired +! ;
end-behaviour
ticker -1 s" ticker" spawn constant ticking

testing timers: fired the soonest first whatever order they were set in; two due together, the first set first
T{ 0 fired !  0 order !  ticking send set-timers  fired 4 pump  order @ -> 2341 }T
testing each timer's id its own; one cancelled doesn't fire, the rest do; an id fired or cancelled already cancels
testing  nothing (not the timer set since in its place)
T{ 0 fired !  0 order !  ticking send set-timers  deliver  2 id @  3 id @ = -> false }T
T{ 2 id @ cancel-timer  2 id @ cancel-timer  fired 3 pump  order @ -> 241 }T
T{ 0 fired !  0 order !  ticking send set-timers  deliver  0 id @ -> 0 id @ }T
0 id @ constant stale   0 id @ cancel-timer
T{ ticking enter  30 after  leave-actor  stale <> -> true }T   \ (the slot's next: another id)
T{ stale cancel-timer  fired 4 pump  fired @ -> 4 }T            \ (the three left, and that one)

testing many timers set and most cancelled: those left all fire, in the order they are due
message set-many ( n -- )
variable last-fired   variable out-of-order
create many 2000 cells allot
behaviour many-ticker
  on set-many ( n -- )  0 ?do  i 7 * 100 mod 20 + after  i cells many + !  loop ;
  on timer-fired ( timer -- )   \ (ids count up as they are set: among those due together, in that order)
      drop  1 fired +! ;
end-behaviour
many-ticker -1 s" many" spawn constant many-ticking
: cancel-most ( n -- )  0 ?do  i 10 mod if  i cells many + @ cancel-timer  then  loop ;
T{ 0 fired !  2000 many-ticking send set-many  deliver  2000 cancel-most  fired 200 pump  fired @ -> 200 }T
T{ 30 net-poll drop  deliver  fired @ -> 200 }T   \ (the cancelled: never)

testing after-together: timers set a little apart fire together at the clock's next round (three set over 40 ms,
testing  a round every 200: in two rounds at most - the round's edge may fall among them - never one each)
message set-together ( -- )
behaviour together
  on set-together ( -- )  1 200 after-together drop ;
  on timer-fired ( timer -- )  drop  1 fired +! ;
end-behaviour
together -1 s" together" spawn constant rounding
: set-three ( -- )  3 0 do  rounding send set-together  deliver  20 net-poll drop  loop ;
: rounds ( n -- rounds )   \ how many times `fired` was seen to grow till it was n
    {: n | seen r :}  200 0 do  5 net-poll drop  deliver  fired @ seen <> if  fired @ to seen  r 1+ to r  then
    seen n = if leave then  loop  r ;
T{ 0 fired !  set-three  3 rounds 3 <  fired @ -> true 3 }T

\ ---- connections ----
behaviour echo-server
  on net-accepted ( listener conn -- )  nip drop ;
  on net-received ( conn -- )  {: c :}  c net-input  c net-write  c net-input nip c net-consume ;
  on net-closed ( conn -- )  net-close  1 closed-seen +! ;
end-behaviour
message dial ( n -- )
message say-much ( b conn -- )
message say ( conn -- )
message hang-up ( conn -- )
variable conns   \ a list of the client's connections, in the order they connected
behaviour echo-client
  on dial ( n -- )  0 ?do  s" 127.0.0.1" port @ connect drop  loop ;
  on net-connected ( conn -- )  conns @ push ;
  on say ( conn -- )  s" hello" rot net-write ;
  on say-much ( b conn -- )  {: b c :}  b b-data c net-write ;
  on hang-up ( conn -- )  net-close ;
  on net-received ( conn -- )  {: c :}  c net-input nip  dup echoed-bytes +!  c net-consume  1 echoed +! ;
  on net-closed ( conn -- )  drop ;
end-behaviour
echo-server -1 s" server" spawn constant server
echo-client -1 s" client" spawn constant client
list conns !
server enter  0 listen  leave-actor  constant listener
listener listener-port port !
: connected ( n -- )  {: n :}  300 0 do  5 net-poll drop  deliver  conns @ length n >= if leave then  loop ;
: conn ( i -- conn )  conns @ nth ;

testing many connections held: each still its own - one quiet for many rounds, spoken on, answered
T{ 300 client send dial  300 connected  conns @ length -> 300 }T
: quiet-rounds ( -- )  20 0 do  1 net-poll drop  deliver  loop ;
T{ quiet-rounds  0 echoed !  7 conn client send say  echoed 1 pump  echoed @ -> 1 }T
: say-all ( -- )  conns @ length 0 do  i conn client send say  loop ;
T{ 0 echoed !  say-all  echoed 300 pump  echoed @ -> 300 }T
testing a connection answered over and over: each answer goes as it is written - none waits to be told its socket
testing  has room (it had: nothing asked)
: echo-once ( -- )  0 echoed !  7 conn client send say  echoed 1 pump ;
: echoes-ms ( n -- ms )  ms-now swap  0 do  echo-once  loop  ms-now swap - ;
T{ 20 echoes-ms 1000 < -> true }T
testing more written than a socket takes at once (4 MB, and echoed back): what doesn't go waits for room, and goes -
testing  all of it there and back
4194304 constant much
: much-bytes ( -- b )  <bytes> {: b :}  much 0 do  i b b-push  loop  b ;
: echoed-much ( -- )  600 0 do  5 net-poll drop  deliver  echoed-bytes @ much >= if leave then  loop ;
much-bytes constant a-lot
T{ 0 echoed-bytes !  a-lot 9 conn client send say-much  echoed-much  echoed-bytes @ -> much }T
T{ 0 echoed !  9 conn client send say  echoed 1 pump  echoed @ -> 1 }T   \ (and the connection as it was, after)
a-lot dispose

testing connections closed, more made: the slots taken again, each handle its own (a closed one's names nothing)
: hang-up-some ( -- )  300 0 do  i 3 mod 0= if  i conn client send hang-up  then  loop ;
T{ 0 closed-seen !  hang-up-some  closed-seen 100 pump  closed-seen @ -> 100 }T
T{ 100 client send dial  400 connected  conns @ length -> 400 }T
: distinct? ( -- flag )   \ no connection made since has the handle of one closed
    true  400 300 do  300 0 do  j conn i conn = if  drop false  then  loop  loop ;
T{ distinct? -> true }T
T{ 0 conn ' net-input catch nip -> -1 }T
: say-open ( -- )  400 0 do  i 300 >= i 3 mod 0<> or if  i conn client send say  then  loop ;
T{ 0 echoed !  say-open  echoed 300 pump  echoed @ -> 300 }T

testing an actor gone: its connections - all of them, quiet as they are - closed at the next round; one given to
testing  another actor before (net-owner!) stays
behaviour holder
  on dial ( n -- )  0 ?do  s" 127.0.0.1" port @ connect drop  loop ;
  on net-connected ( conn -- )  conns @ push ;
  on net-received ( conn -- )  {: c :}  c net-input nip c net-consume  1 echoed +! ;
  on net-closed ( conn -- )  drop ;
end-behaviour
holder -1 s" holder" spawn constant holding
list conns !
T{ 5 holding send dial  5 connected  conns @ length -> 5 }T
T{ client 2 conn net-owner!  quiet-rounds  0 closed-seen !  holding kill  closed-seen 4 pump  closed-seen @ -> 4 }T
T{ quiet-rounds  closed-seen @ -> 4 }T
T{ 0 conn ' net-input catch nip  4 conn ' net-input catch nip -> -1 -1 }T
T{ 0 echoed !  2 conn client send say  echoed 1 pump  echoed @ -> 1 }T
testing ... and one whose owner had several, one closed by hand before: the rest found all the same
holder -1 s" holder" spawn constant holding-again
list conns !
T{ 4 holding-again send dial  4 connected  conns @ length -> 4 }T
T{ 0 closed-seen !  1 conn net-close  closed-seen 1 pump  holding-again kill  closed-seen 4 pump  closed-seen @ -> 4 }T
listener net-close

test-summary

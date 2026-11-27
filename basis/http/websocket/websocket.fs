\ websocket.fs - WebSockets (RFC 6455), as Erlang would have them: the socket is an actor. A route's handler names
\ the actor that owns it (a new one, a chat room's, on any node); the socket tells its owner what comes, and the
\ owner sends it what to write - messages both ways.
\
\   behaviour echo
\     on ws-text ( s ws -- )  {: s ws :}  s string ws ws-send  s dispose ;
\   end-behaviour
\   : new-echo ( req -- actor )  drop  echo -1 s" echo" spawn ;
\   s" /echo" ' new-echo site on-websocket
\
\ The owner hears ws-opened ( ws -- ), ws-text / ws-binary ( s ws -- ) (s a new string, the owner's to dispose) and
\ ws-closed ( ws -- ); ws is the socket's address, for ws-send, ws-send-binary and ws-close. The socket watches its
\ owner: the owner gone, it closes (1001).
IN: http.websocket
USING: actors net http accessors base64 checksums.sha1 compression ;

1048576 value ws-message-max     \ a message's bytes, at most (its frames together): more closes it (1009)
true value ws-deflate            \ messages compressed (permessage-deflate, RFC 7692) with a client that offers it
128 value ws-deflate-min         \ ... a message this long or more (a shorter one goes as it is)
30000 value ws-ping-ms           \ a socket pinged this often; one that sent nothing since the last ping closed (1001)

\ A browser opens a socket to any site with that site's cookies, whatever page asks it to (WebSockets have no CORS):
\ a handshake whose Origin is another site's is refused (403) - unless allowed here, its whole origin as the
\ browser says it ("https://app.example.org"). One without Origin (no browser's) is let through.
list constant ws-origins
: ws-allow-origin ( a n -- )  s-lower drop rescue-all ws-origins push ;

message ws-opened ( ws -- )
message ws-text ( s ws -- )
message ws-binary ( s ws -- )
message ws-closed ( ws -- )
message ws-out ( s binary? -- )   \ to the socket: send this (ws-send's)
message ws-shut ( code -- )       \ to the socket: close with this code (ws-close's)
message ws-pump ( -- )            \ to the socket itself: read what came before it became one

<PRIVATE
0 constant op-continue   1 constant op-text   2 constant op-binary
8 constant op-close      9 constant op-ping   10 constant op-pong
1000 constant close-normal        1001 constant close-going
1002 constant close-protocol      1007 constant close-bad-data      1009 constant close-too-big
125 constant control-max          \ a control frame's payload, at most
14 constant frame-head-max        \ a frame's head, at most: 2 bytes, a 64-bit length, the mask
126 constant length-16            127 constant length-64      \ a frame's length byte: the length follows in 2, 8
$80 constant fin-bit   $80 constant mask-bit   $70 constant reserved-bits   $0f constant opcode-bits
$40 constant rsv1-bit             \ (permessage-deflate's: a message compressed - on its first frame)
s\" \x00\x00\xff\xff" >string drop constant sync-tail   \ (the end of a compressed message, left off on the wire)
$7f constant length-bits   4 constant mask-bytes
s" 258EAFA5-E914-47DA-95CA-C5AB0DC85B11" >string drop constant handshake-guid

\ the socket's record (the connection actor's hc-extra): its owner, a message in frames so far and its opcode,
\ whether the owner has been told it closed, whether it is closing, whether its messages may be compressed
\ (permessage-deflate agreed) and whether the one in frames so far is
TUPLE: ws-link  owner frag frag-op told closing deflate frag-deflated heard ;
: rec ( -- r )  hc-extra @ ;
: done? ( -- flag )  hc-extra @ 0= ;   \ its last words have run (a `self kill` runs them there and then)

: remote? ( address -- flag )  {: a :}  a 32 rshift 0<>  a node-of node <> and ;
: there? ( address -- flag )   \ an actor there: one on another node taken to be (a message to one gone is dropped)
    {: a :}  a remote? if  true  else  a $ffffffff and alive?  then ;
\ s given to the actor at `to` in a message: a string made in no scratch region, the receiver's (one to another
\ node is copied there: this one let go)
: hand-over ( s receiver -- )  {: s receiver :}  receiver remote? if  s dispose  then ;
: owner-there? ( -- flag )  rec owner>> there? ;

\ a frame written: FIN, the opcode, the length, the payload (a server's frames aren't masked)
: send-frame-with ( a n op flags -- )   \ (flags: RSV1 for a compressed message)
    {: a n op flags | b :}  <bytes> to b
    fin-bit op or flags or b b-push
    n length-16 < if  n b b-push
    else n $10000 < if  length-16 b b-push  n 8 rshift b b-push  n 255 and b b-push
    else  length-64 b b-push  8 0 do  n 56 i 8 * - rshift 255 and b b-push  loop  then then
    a n b b-append
    b b-data hc-conn @ net-write  b dispose ;
: send-frame ( a n op -- )  {: a n op :}  a n op 0 send-frame-with ;

: tell-closed ( -- )   \ the owner told, once
    rec told>> if  exit  then  rec true >>told drop
    owner-there? if  self address rec owner>> send ws-closed  then ;

\ a close frame with the code, the connection closed once it has gone, the owner told, the socket gone
: close-with ( code -- )
    {: code | b :}
    done? if  exit  then  rec closing>> if  exit  then  rec true >>closing drop
    <bytes> to b  code 8 rshift b b-push  code 255 and b b-push
    b b-data op-close send-frame  b dispose
    hc-conn @ net-close  tell-closed  self kill ;

\ the code a client's close is answered with (RFC 6455 7.4): its own, if one a peer may send - none: 1000; one byte,
\ or a code no peer may send (below 1000, 1004-1006, 1015, 1016-2999 unassigned): 1002; a reason not UTF-8: 1007
: close-answer ( b -- code )
    {: b :}  b b-length {: n :}
    n 0= if  close-normal exit  then
    n 1 = if  close-protocol exit  then
    b b-data drop c@ 8 lshift  b b-data drop 1+ c@ or {: code :}
    code 1000 <  code 1004 1007 within or  code 1015 = or  code 1016 3000 within or  code 4999 > or if  close-protocol exit  then
    b b-data drop 2 +  n 2 -  utf8-valid? 0= if  close-bad-data exit  then
    code ;

\ ---- reading frames ----
task-variable fin   task-variable opcode   task-variable payload-at   task-variable payload-n   task-variable mask-at
task-variable rsv1

\ the frame at the start of a n: its length if it is all there (its parts in the variables above), 0 if not yet,
\ or minus the close code for what is wrong
: frame-at ( a n -- used | 0 | -code )
    {: a n | b0 b1 len at :}
    n 2 < if  0 exit  then
    a c@ to b0  a 1+ c@ to b1
    b0 reserved-bits and  rsv1-bit invert and if  close-protocol negate exit  then   \ (RSV1 only: below)
    b0 rsv1-bit and if   \ (a compressed message's first frame: if it was agreed, a data frame's)
        rec deflate>> 0=  b0 opcode-bits and op-text <>  b0 opcode-bits and op-binary <> and  or if
            close-protocol negate exit
        then
    then
    b1 mask-bit and 0= if  close-protocol negate exit  then   \ (a client's frames are masked)
    b1 length-bits and to len  2 to at
    len length-16 = if
        n 4 < if  0 exit  then  a 2 + c@ 8 lshift  a 3 + c@ or to len  4 to at
    else len length-64 = if
        n 10 < if  0 exit  then  0 to len  8 0 do  len 8 lshift  a 2 + i + c@ or to len  loop  10 to at
    then then
    len 0<  len ws-message-max > or if  close-too-big negate exit  then
    at mask-bytes + len + {: total :}
    n total < if  0 exit  then
    b0 fin-bit and 0<> fin !  b0 opcode-bits and opcode !  b0 rsv1-bit and 0<> rsv1 !
    a at + mask-at !  a at + mask-bytes + payload-at !  len payload-n !
    total ;

\ the frame's payload unmasked, added to b
: unmask-into ( b -- )
    {: b | m p :}  mask-at @ to m  payload-at @ to p
    payload-n @ 0 ?do  p i + c@  m i 3 and + c@ xor  b b-push  loop ;

\ a whole message to the owner (compressed: inflated first - not DEFLATE, or longer than ws-message-max, closes it):
\ text that isn't UTF-8 closes the socket (1007)
: hand-message ( a n op deflated? -- )
    {: a0 n0 op deflated? | a n :}
    a0 to a  n0 to n
    deflated? if
        a0 n0 sync-tail string s+ {: z zn :}  z zn ws-message-max inflate
        z dispose
        0= if  close-bad-data close-with  exit  then  to n to a
    then
    op op-text =  a n utf8-valid? 0= and if  close-bad-data close-with  exit  then
    owner-there? 0= if  exit  then
    a n >string drop rescue-all {: s :}
    op op-text = if  s self address rec owner>> send ws-text
    else  s self address rec owner>> send ws-binary  then
    s rec owner>> hand-over ;

: frag-length ( -- n )  rec frag>> ?dup if  b-length  else  0  then ;

\ the frame read (the variables say it) acted on
: on-frame ( -- )
    {: | b :}
    opcode @ 8 >= if   \ a control frame: whole, short
        fin @ 0=  payload-n @ control-max > or if  close-protocol close-with  exit  then
    then
    <bytes> to b  b unmask-into
    opcode @ case
        op-ping of  b b-data op-pong send-frame  0  endof
        op-pong of  0  endof
        op-close of  b close-answer close-with  0  endof
        op-continue of
            rec frag>> 0= if  close-protocol close-with
            else
                b b-data rec frag>> b-append
                frag-length ws-message-max > if  close-too-big close-with
                else fin @ if
                    rec frag>> b-data rec frag-op>> rec frag-deflated>> hand-message
                    done? 0= if  rec frag>> dispose  rec 0 >>frag drop  then   \ (closed by it - bad UTF-8: freed)
                then then
            then  0
        endof
        op-text of  op-text  endof
        op-binary of  op-binary  endof
        close-protocol close-with  0 swap   \ (an opcode not known)
    endcase
    {: data-op :}
    data-op if
        rec frag>> if  close-protocol close-with   \ (a new message in the middle of one)
        else fin @ if  b b-data data-op rsv1 @ hand-message
        else
            <bytes> rescue-all {: f :}  b b-data f b-append
            rec f >>frag drop  rec data-op >>frag-op drop  rec rsv1 @ >>frag-deflated drop
        then then
    then
    b dispose ;

\ every whole frame there read and acted on, its bytes taken
: read-frames ( -- )
    {: | used :}
    begin
        done? if  exit  then  rec closing>> if  exit  then
        hc-conn @ net-input frame-at to used
        used 0< if  used negate close-with  exit  then
        used 0= if  exit  then
        on-frame
        done? if  exit  then
        rec true >>heard drop   \ (something came: alive)
        used hc-conn @ net-consume
    again ;

: let-go ( -- )   \ (last words) the owner told, the record let go
    tell-closed  rec frag>> ?dup if  dispose  then  rec dispose  0 hc-extra ! ;

\ a message out in one frame: compressed (RSV1) if that was agreed and it is long enough
: send-message ( a n op -- )
    {: a n op :}
    rec deflate>>  n ws-deflate-min >= and if
        a n deflate-sync {: z zn :}  z zn op rsv1-bit send-frame-with  z dispose exit
    then
    a n op send-frame ;

\ the next ping's timer. A node holding thousands of sockets would be woken by each one's own timer, and by each
\ pong as it came back: set to the clock's next round instead (ping-rounds of them in ws-ping-ms), a round's
\ sockets are pinged at once.
32 constant ping-rounds
: ping-later ( -- )  ws-ping-ms dup ping-rounds / after-together drop ;
PRIVATE>

behaviour ws-connection
    on ws-pump ( -- )  read-frames ;
    on net-received ( conn -- )  drop  read-frames ;
    on net-closed ( conn -- )  drop  rec true >>closing drop  tell-closed  self kill ;
    on ws-out ( s binary? -- )
        {: s bin :}  rec closing>> 0= if   \ (text not UTF-8 - cut in a character, Latin-1 - sent with U+FFFD in its place:
                                           \  a browser fails the whole socket for one bad byte)
            bin if  s string op-binary  else  s string 2dup utf8-valid? 0= if  utf8-clean  then  op-text  then  send-message
        then
        s dispose ;
    on ws-shut ( code -- )  close-with ;
    on died ( id reason -- )  2drop  close-going close-with ;   \ (its owner gone)
    on timer-fired ( timer -- )   \ (a ping: answered with a pong by any browser - nothing since the last one: gone)
        drop  done? if  exit  then  rec closing>> if  exit  then
        rec heard>> 0= if  close-going close-with exit  then
        rec false >>heard drop  s" " op-ping send-frame  ping-later ;
    on killed ( -- )  let-go ;
end-behaviour

<PRIVATE
: has-token? ( a n token n -- flag )   \ a header's comma-separated value has the token, in any case
    {: a n ta tn | low :}  a n s-lower drop to low  low string ta tn s-find 0>=  low dispose ;
: handshake? ( req -- flag )
    {: r :}
    r req-method s" GET" s= 0= if  false exit  then
    s" upgrade" r req-header 0= if  false exit  then  s" websocket" has-token? 0= if  false exit  then
    s" connection" r req-header 0= if  false exit  then  s" upgrade" has-token? 0= if  false exit  then
    s" sec-websocket-key" r req-header 0= if  false exit  then  2drop
    true ;
: deflate-offered? ( req -- flag )   \ its Sec-WebSocket-Extensions offers permessage-deflate (and we take it)
    {: r :}  ws-deflate 0= if  false exit  then
    s" sec-websocket-extensions" r req-header 0= if  false exit  then  s" permessage-deflate" has-token? ;
: accept-of ( key n -- s len )   \ Sec-WebSocket-Accept: the key and the GUID, SHA-1, base64
    handshake-guid string s+ {: k kn :}  k kn sha1 {: d dn :}  d dn >base64  d dispose  k dispose ;

\ the handshake's Origin is this site's (its host the Host asked), or one allowed, or none
: origin-ok? ( req -- flag )
    {: r :}
    s" origin" r req-header 0= if  true exit  then  {: oa on :}
    oa on s-lower {: la ln :}  false  ws-origins [: string la ln s= or ;] each  {: allowed :}  la dispose
    allowed if  true exit  then
    s" host" r req-header 0= if  false exit  then  {: ha hn :}
    oa on s" //" s-find {: at :}  at 0< if  false exit  then
    oa at 2 + +  on at - 2 -  s-lower {: oh ohn :}  ha hn s-lower {: hh hhn :}
    oh ohn hh hhn s=  oh dispose  hh dispose ;
\ on-websocket's route: the handshake answered 101, the connection the socket's, the owner told
: upgrade ( req -- )
    {: r | owner :}
    r handshake? 0= if  s" a WebSocket handshake expected" 400 r respond  exit  then
    r origin-ok? 0= if  s" Forbidden: another site's page (ws-allow-origin)" 403 r respond  exit  then
    s" sec-websocket-version" r req-header 0= if  s" "  then  s" 13" s= 0= if
        s" 13" s" Sec-WebSocket-Version" r header!  s" WebSocket version 13 expected" 426 r respond  exit
    then
    r r req-data execute to owner
    owner nobody = if  s" Forbidden" 403 r respond  exit  then   \ (the xt refused it)
    s" sec-websocket-key" r req-header drop accept-of {: ac acn :}
    ac acn s" Sec-WebSocket-Accept" r header!  s" websocket" s" Upgrade" r header!  s" Upgrade" s" Connection" r header!
    r deflate-offered? {: deflate? :}
    deflate? if   \ (each message on its own, both ways: no window kept between them)
        s" permessage-deflate; server_no_context_takeover; client_no_context_takeover" s" Sec-WebSocket-Extensions" r header!
    then
    101 r answer-head {: b :}  s\" \r\n" b b-append  b b-data r req-conn net-write  b dispose
    r take-connection
    ws-message-max frame-head-max + control-max +  hc-conn @ net-input-max!   \ (a whole frame fits what is read)
    owner 0 0 0 0 deflate? 0 true <ws-link> rescue-all hc-extra !
    ping-later
    owner monitor
    ws-connection become
    owner-there? if  self address owner send ws-opened  then
    self send ws-pump ;
PRIVATE>

\ WebSocket handshakes to paths of this pattern answered: xt ( req -- actor ) names the socket's owner (it may spawn
\ one; nobody refuses it: 403); a request that is no handshake is a 400
: on-websocket ( pattern n xt router -- )  {: pa pn xt rt :}  s" GET" pa pn ['] upgrade xt in-handler rt route-with ;

\ a text message to the socket (from any actor, on any node): sent as it can be; to a socket gone, nothing
: ws-send ( a n ws -- )
    {: a n ws :}  ws there? 0= if  exit  then
    a n >string drop rescue-all {: s :}  s false ws send ws-out  s ws hand-over ;
: ws-send-binary ( a n ws -- )
    {: a n ws :}  ws there? 0= if  exit  then
    a n >string drop rescue-all {: s :}  s true ws send ws-out  s ws hand-over ;
\ the socket closed (1000, or the code given: ws-close-with)
: ws-close-with ( code ws -- )  {: code ws :}  ws there? 0= if  exit  then  code ws send ws-shut ;
: ws-close ( ws -- )  1000 swap ws-close-with ;

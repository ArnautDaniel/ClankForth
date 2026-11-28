\ http.websocket (basis/http/websocket): the handshake, frames both ways, and the socket an actor with an owner
IN: test-http-websocket
USING: tester actors net http http.websocket vocabs.webclient ;

variable opened   variable closed-told   variable last-ws   variable last-owner
behaviour echo
    on ws-opened ( ws -- )  last-ws !  1 opened +! ;
    on ws-text ( s ws -- )  {: s ws :}  s string ws ws-send  s dispose ;
    on ws-binary ( s ws -- )  {: s ws :}  s string ws ws-send-binary  s dispose ;
    on ws-closed ( ws -- )  drop  1 closed-told +! ;
end-behaviour
: new-echo ( req -- actor )  drop  echo -1 s" echo" spawn  dup last-owner ! ;
<router> constant site
s" /ws" ' new-echo site on-websocket
site 0 http-listen listener-port web-port !

\ ---- what the client sends: the handshake, and frames masked as a client's are ----
<bytes> constant out
: handshake ( -- )
    out b-clear
    s\" GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n" out b-append
    s\" Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" out b-append ;
: frame ( a n op fin? -- )   \ masked with 1 2 3 4
    {: a n op fin :}  a n s-bytes drop to a
    fin if  $80  else  0  then  op or out b-push
    n 126 < if  n $80 or out b-push  else  126 $80 or out b-push  n 8 rshift out b-push  n 255 and out b-push  then
    1 out b-push  2 out b-push  3 out b-push  4 out b-push
    n 0 ?do  a i + c@  i 3 and 1+ xor  out b-push  loop ;
: text ( a n -- )  1 true frame ;
: open-ws ( -- )  out b-data open-with ;
: more ( -- )  out b-data client-conn @ net-write  out b-clear ;   \ (on the connection made)
: wait-for ( bytes -- )  reply b-length + 300 polls-for ;
: heard? ( a n -- flag )  reply b-data 2swap has? ;

s" https://App.Example" ws-allow-origin   \ (kept for good: before what is counted)
objects constant objects-before
actors-alive constant actors-before

testing the handshake: 101, the key's answer (RFC 6455's example); a frame sent with it echoed
T{ handshake  s" hello" text  open-ws  60 wait-for  s" 101 Switching Protocols" heard? -> true }T
T{ s" Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" heard? -> true }T
T{ s\" \x81\x05hello" heard?  opened @ -> true 1 }T
testing a ping answered with a pong of its payload
T{ out b-clear  s" p" 9 true frame  more  3 wait-for  s\" \x8a\x01p" heard? -> true }T
testing a message in frames: one message to the owner
T{ out b-clear  s" ab" 1 false frame  s" cd" 0 true frame  more  6 wait-for  s\" \x81\x04abcd" heard? -> true }T
testing binary, and a longer message (its length in two more bytes)
T{ out b-clear  s\" \x00\x01" 2 true frame  more  4 wait-for  s\" \x82\x02\x00\x01" heard? -> true }T
create long-text 200 allot  long-text 200 char x fill
T{ out b-clear  long-text 200 text  more  204 wait-for  s\" \x81\x7e\x00\xc8xxx" heard? -> true }T
testing the client closing: the close answered, the connection closed, the owner told
T{ 0 closed-told !  out b-clear  s\" \x03\xe8" 8 true frame  more  500 polls
   s\" \x88\x02\x03\xe8" heard?  closed @  closed-told @ -> true 1 1 }T
last-owner @ kill
testing a frame not masked: closed (1002); text that isn't UTF-8: closed (1007)
T{ handshake  out b-data open-with  60 wait-for  s\" \x81\x02hi" client-conn @ net-write  500 polls
   s\" \x88\x02\x03\xea" heard?  closed @ -> true 1 }T
last-owner @ kill
T{ handshake  s\" \xff\xfe" text  open-ws  500 polls  s\" \x88\x02\x03\xef" heard?  closed @ -> true 1 }T
last-owner @ kill
testing a message in frames whose last isn't UTF-8: closed (1007), and the socket not failing after (its record freed)
: frag-bad ( -- )  handshake  s" ok" 1 false frame  s\" \xff\xfe" 0 true frame  open-ws  500 polls ;
: frag-failed? ( -- flag )  ['] frag-bad with-output {: o :}  o string s" error" has?  o dispose ;
T{ frag-failed?  s\" \x88\x02\x03\xef" heard? -> false true }T
last-owner @ kill
testing a client's close answered as RFC 6455 says: 1005 (no peer sends it), one byte, code 0: 1002; a reason not
testing  UTF-8: 1007; 1000 as it came
: close-frame ( a n -- )  8 true frame ;
: closes-with ( a n -- )  {: a n :}  handshake  a n close-frame  open-ws  500 polls ;
T{ s\" \x03\xed" closes-with  s\" \x88\x02\x03\xea" heard? -> true }T   last-owner @ kill
T{ s\" \x03" closes-with  s\" \x88\x02\x03\xea" heard? -> true }T   last-owner @ kill
T{ s\" \x03\xe8\xff" closes-with  s\" \x88\x02\x03\xef" heard? -> true }T   last-owner @ kill
T{ s\" \x03\xe8" closes-with  s\" \x88\x02\x03\xe8" heard? -> true }T   last-owner @ kill
testing the owner closing it (ws-close), and the owner gone (1001)
T{ handshake  open-ws  60 wait-for  last-ws @ ws-close  500 polls  s\" \x88\x02\x03\xe8" heard?  closed @ -> true 1 }T
last-owner @ kill
T{ handshake  open-ws  60 wait-for  last-owner @ kill  500 polls  s\" \x88\x02\x03\xe9" heard?  closed @ -> true 1 }T
testing a request that is no handshake: 400; another version: 426
T{ s" /ws" get status-of -> 400 }T
T{ s\" GET /ws HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: x\r\nSec-WebSocket-Version: 8\r\nConnection: close\r\n\r\n"
   fetch  2dup status-of -rot s" Sec-WebSocket-Version: 13" has? -> 426 true }T
testing another site's page (its Origin): refused, 403 - the site's own, or one allowed (ws-allow-origin), taken
: with-origin ( a n -- code )   \ a handshake from a page of that origin: its status
    {: oa on :}
    oa on s\" GET /ws HTTP/1.1\r\nHost: t\r\nOrigin: {s}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nConnection: close\r\n\r\n"
    format {: q qn :}  q qn fetch status-of  q dispose ;
T{ s" https://evil.example" with-origin -> 403 }T
T{ s" http://T" with-origin -> 101 }T   last-owner @ constant same-site-owner
T{ s" https://app.example" with-origin -> 101 }T   last-owner @ constant allowed-owner
testing permessage-deflate: offered, agreed (no window kept either way); a compressed message read, a long answer
testing  compressed (RSV1), a short one as it is
USING: compression ;
: handshake-deflate ( -- )
    out b-clear
    s\" GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" out b-append
    s\" Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n" out b-append
    s\" Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" out b-append ;
: deflated-text ( a n -- )   \ a text frame, compressed (RSV1)
    deflate-sync {: z zn :}  z zn 1 $40 or true frame  z dispose ;
T{ handshake-deflate  open-ws  60 wait-for
   s" Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover" heard? -> true }T
: answer-inflated ( -- flag )   \ the last frame heard, compressed: inflated, the long text again?
    reply b-data s\" \xc1" s-find {: at :}  at 0< if  false exit  then
    reply b-data drop at + {: f :}  f 1+ c@ {: len :}
    f 2 + len s\" \x00\x00\xff\xff" s+ {: z zn :}  z zn 1000 inflate
    if  {: u un :}  u un long-text 200 s=  u dispose  else  false  then  z dispose ;
T{ reply b-clear  out b-clear  long-text 200 deflated-text  more  6 wait-for  answer-inflated -> true }T
T{ reply b-clear  out b-clear  s" hi" text  more  4 wait-for  s\" \x81\x02hi" heard? -> true }T
T{ reply b-clear  out b-clear  s" hello again" deflated-text  more  13 wait-for  s\" \x81\x0bhello again" heard? -> true }T
testing a long message compressed: inflated and (echoed) deflated on workers, the connection's actor waiting - the same back
: rows ( n -- s len )   \ n bytes of a table's rows
    {: n | b :}  <bytes> to b  begin  b b-length n <  while  s" <tr class=row><td>" b b-append  b b-length n>str b b-append  repeat
    b b>string drop n  b dispose ;
: long-echoed? ( -- flag )   \ (what comes back is what was sent: the same bytes deflated the same way)
    [: 100000 rows deflate-sync {: z zn :}
       reply b-clear  out b-clear  z zn 1 $40 or true frame  more  zn 4 + wait-for
       reply b-data {: r rn :}  rn zn 4 + =  r c@ $c1 = and  r 4 + rn 4 - z zn s= and ;] with-scratch ;
T{ long-echoed? -> true }T
last-owner @ kill  100 polls
testing RSV1 where it wasn't agreed: closed (1002)
T{ handshake  open-ws  60 wait-for  out b-clear  s" hi" deflated-text  more  500 polls  s\" \x88\x02\x03\xea" heard? -> true }T
last-owner @ kill

testing a socket that says nothing: pinged; nothing back by the next ping, closed (1001)
50 to ws-ping-ms
T{ handshake  open-ws  60 wait-for  reply b-clear  300 polls  s\" \x89\x00" heard?  s\" \x88\x02\x03\xe9" heard? -> true true }T
30000 to ws-ping-ms
: kill-if-alive ( id -- )  dup alive? if  kill  else  drop  then ;
last-owner @ kill-if-alive  10 polls

testing nothing kept: the sockets' records and messages freed, their actors gone
same-site-owner kill  allowed-owner kill
T{ 10 polls  objects objects-before - -> 0 }T
T{ actors-alive actors-before - -> 0 }T

test-summary

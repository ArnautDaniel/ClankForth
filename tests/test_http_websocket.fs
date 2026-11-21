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
    {: a n op fin :}
    fin if  $80  else  0  then  op or out b-push
    n 126 < if  n $80 or out b-push  else  126 $80 or out b-push  n 8 rshift out b-push  n 255 and out b-push  then
    1 out b-push  2 out b-push  3 out b-push  4 out b-push
    n 0 ?do  a i + c@  i 3 and 1+ xor  out b-push  loop ;
: text ( a n -- )  1 true frame ;
: open-ws ( -- )  out b-data open-with ;
: more ( -- )  out b-data client-conn @ net-write  out b-clear ;   \ (on the connection made)
: wait-for ( bytes -- )  reply b-length + 300 polls-for ;
: heard? ( a n -- flag )  reply b-data 2swap has? ;

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
testing the owner closing it (ws-close), and the owner gone (1001)
T{ handshake  open-ws  60 wait-for  last-ws @ ws-close  500 polls  s\" \x88\x02\x03\xe8" heard?  closed @ -> true 1 }T
last-owner @ kill
T{ handshake  open-ws  60 wait-for  last-owner @ kill  500 polls  s\" \x88\x02\x03\xe9" heard?  closed @ -> true 1 }T
testing a request that is no handshake: 400; another version: 426
T{ s" /ws" get status-of -> 400 }T
T{ s\" GET /ws HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: x\r\nSec-WebSocket-Version: 8\r\nConnection: close\r\n\r\n"
   fetch  2dup status-of -rot s" Sec-WebSocket-Version: 13" has? -> 426 true }T
testing nothing kept: the sockets' records and messages freed, their actors gone
T{ 10 polls  objects objects-before - -> 0 }T
T{ actors-alive actors-before - -> 0 }T

test-summary

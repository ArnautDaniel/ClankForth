\ (the http tests') a client of the net vocabulary's own: what is to be sent written once it connects, what comes
\ kept in `reply`; and words to read an HTTP answer. `web-port` is the server's port.
IN: vocabs.webclient
USING: actors net ;

variable web-port
<bytes> constant sending   <bytes> constant reply   variable closed   variable client-conn

behaviour web-client
    on net-connected ( conn -- )  sending b-data rot net-write ;
    on net-received ( conn -- )  {: c :}  c net-input reply b-append  c net-input nip c net-consume ;
    on net-closed ( conn -- )  drop  1 closed ! ;
end-behaviour
web-client -1 s" client" spawn constant client

: plain-connect ( port -- conn )  s" 127.0.0.1" rot connect ;
defer connect-to ( port -- conn )   \ how the client connects (a TLS test sets its own)
' plain-connect is connect-to

: polls ( n -- )  0 ?do  10 net-poll drop  deliver  closed @ if  leave  then  loop ;   \ till the server closes
: polls-for ( bytes n -- )   \ till the reply has that many bytes (or the server closes)
    {: bytes n :}  n 0 ?do  10 net-poll drop  deliver  closed @  reply b-length bytes >= or if  leave  then  loop ;
: open-with ( a n -- )   \ a new connection, sending a n once it is made
    sending b-clear  sending b-append  reply b-clear  0 closed !
    client enter  web-port @ connect-to client-conn !  leave-actor ;
: fetch ( a n -- a' n' )  open-with  500 polls  reply b-data ;   \ the reply, the server having closed
: get ( path n -- a' n' )   \ a GET of the path, the connection closed after
    s\" GET {s} HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n" format {: s n :}
    s n fetch  s dispose ;
: get-with ( path n headers n -- a' n' )   \ a GET with more headers (each ending CR LF)
    s\" GET {s} HTTP/1.1\r\nHost: test\r\nConnection: close\r\n{s}\r\n" format {: s n :}
    s n fetch  s dispose ;

: status-of ( a n -- status )  9 3 s-slice string>number drop ;
: body-of ( a n -- a' n' )  {: a n :}  a n s\" \r\n\r\n" s-find 4 + {: at :}  a n at n at - s-slice ;
: has? ( a n a2 n2 -- flag )  s-find 0>= ;
: hits ( a n a2 n2 -- count )   \ how many times the second is in the first
    {: a n pa pn | k :}
    begin  a n pa pn s-find dup 0>=  while  k 1+ to k  pn + {: at :}  a n at n at - s-slice to n to a  repeat
    drop k ;

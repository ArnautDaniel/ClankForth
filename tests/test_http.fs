\ the http vocabulary: a server on a free port, asked by a client of the net vocabulary's own, its replies read
IN: test-http
USING: tester actors net http ;

\ ---- the server's routes ----
<router> constant site
: home ( req -- )  {: r :}  s" home" 200 r respond ;
: show-user ( req -- )  {: r :}  s" id" r req-param drop  s" user {s}" format  200 r respond ;
: echo-body ( req -- )  {: r :}  r req-body 201 r respond ;
: greet ( req -- )  {: r | a n :}
    s" name" r query-param if  to n to a  else  s" nobody" to n to a  then  a n 200 r respond ;
: rest ( req -- )  {: r :}  s" *" r req-param drop  200 r respond ;
: agent ( req -- )  {: r | a n :}
    s" user-agent" r req-header if  to n to a  else  s" none" to n to a  then  a n 200 r respond ;
: moved ( req -- )  {: r :}  s" /" 302 r redirect ;
: page ( req -- )  {: r :}  s" <p>hi</p>" 200 r respond-html ;
: broken ( req -- )  drop  true abort" broken on purpose" ;
: silent ( req -- )  drop ;
: missing ( req -- )  {: r :}  s" not here" 404 r respond ;

s" /" ' home site on-get
s" /users/:id" ' show-user site on-get
s" /echo" ' echo-body site on-post
s" /greet" ' greet site on-get
s" /files/*" ' rest site on-get
s" /agent" ' agent site on-get
s" /moved" ' moved site on-any
s" /page" ' page site on-get
s" /broken" ' broken site on-get
s" /silent" ' silent site on-get

\ a handler that waits for another actor's answer (its connection's actor busy meanwhile, no one else held up)
message what-time ( -- )
message the-time ( n -- )
behaviour clock-keeper  on what-time ( -- )  42 sender send the-time ;  end-behaviour
clock-keeper -1 s" clock" spawn constant clock
: ask-clock ( req -- )  {: r :}  clock send what-time  await the-time  s" time {}" format  200 r respond ;
s" /ask" ' ask-clock site on-get

site 0 http-listen constant listener
listener listener-port constant server-port

\ ---- a client: what is to be sent written once it connects, what comes kept ----
<bytes> constant sending   <bytes> constant reply   variable closed   variable client-conn

behaviour web-client
    on net-connected ( conn -- )  sending b-data rot net-write ;
    on net-received ( conn -- )  {: c :}  c net-input reply b-append  c net-input nip c net-consume ;
    on net-closed ( conn -- )  drop  1 closed ! ;
end-behaviour
web-client -1 s" client" spawn constant client

: polls ( n -- )  0 ?do  10 net-poll drop  deliver  closed @ if  leave  then  loop ;
: open-with ( a n -- )   \ a new connection, sending a n once it is made
    sending b-clear  sending b-append  reply b-clear  0 closed !
    client enter  s" 127.0.0.1" server-port connect client-conn !  leave-actor ;
: fetch ( a n -- a' n' )  open-with  500 polls  reply b-data ;   \ the reply, the server having closed
: get ( path n -- a' n' )   \ a GET of the path, the connection closed after
    s\" GET {s} HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n" format {: s n :}
    s n fetch  s dispose ;

: status-of ( a n -- status )  9 3 s-slice string>number drop ;
: body-of ( a n -- a' n' )  {: a n :}  a n s\" \r\n\r\n" s-find 4 + {: at :}  a n at n at - s-slice ;
: has? ( a n a2 n2 -- flag )  s-find 0>= ;
: hits ( a n a2 n2 -- count )   \ how many times the second is in the first
    {: a n pa pn | k :}
    begin  a n pa pn s-find dup 0>=  while  k 1+ to k  pn + {: at :}  a n at n at - s-slice to n to a  repeat
    drop k ;

<bytes> constant long-request   \ a request whose head is longer than 256 bytes
: make-long ( -- )
    s\" GET / HTTP/1.1\r\nX-Long: " long-request b-append
    300 0 do  [char] x long-request b-push  loop  s\" \r\n\r\n" long-request b-append ;
make-long

objects constant objects-before
actors-alive constant actors-before

testing a route answered: its status, its body, its length
T{ s" /" get  2dup status-of -rot body-of s" home" s= -> 200 true }T
T{ s" /" get  s" Content-Length: 4" has? -> true }T
T{ s" /" get  s" Content-Type: text/plain" has? -> true }T
T{ s" /" get  s" Connection: close" has? -> true }T
testing a :name in the pattern: what it matched, decoded
T{ s" /users/42" get body-of s" user 42" s= -> true }T
T{ s" /users/a%20b" get body-of s" user a b" s= -> true }T
T{ s" /users/42/more" get status-of -> 404 }T
testing a * last in the pattern: the rest of the path, none or more segments
T{ s" /files/css/site.css" get body-of s" css/site.css" s= -> true }T
T{ s" /files" get  2dup status-of -rot body-of nip -> 200 0 }T
testing no route: 404, or what on-missing says; the path's, not the method's: 405 and what it allows
T{ s" /nowhere" get status-of -> 404 }T
' missing site on-missing
T{ s" /nowhere" get body-of s" not here" s= -> true }T
T{ s\" POST / HTTP/1.1\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" fetch  2dup status-of -rot
   s" Allow: GET" has? -> 405 true }T
T{ s\" DELETE /moved HTTP/1.1\r\nConnection: close\r\n\r\n" fetch status-of -> 302 }T
testing a body: kept to its length
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nContent-Length: 3\r\n\r\nabc" fetch
   2dup status-of -rot body-of s" abc" s= -> 201 true }T
testing the query string: a name's value, decoded (+ a space)
T{ s" /greet?x=1&name=ann%21" get body-of s" ann!" s= -> true }T
T{ s" /greet?name=a+b" get body-of s" a b" s= -> true }T
T{ s" /greet?x=1" get body-of s" nobody" s= -> true }T
T{ s" /greet?name" get  2dup status-of -rot body-of nip -> 200 0 }T
T{ s" a%41+b%zz%4" url-decode  over >r  s" aA b%zz%4" s=  r> dispose -> true }T
testing a header by its name in any case
T{ s\" GET /agent HTTP/1.1\r\nUSER-AGENT:   tester  \r\nConnection: close\r\n\r\n" fetch body-of s" tester" s= -> true }T
T{ s" /agent" get body-of s" none" s= -> true }T
testing HEAD: a GET's head, no body
T{ s\" HEAD / HTTP/1.1\r\nConnection: close\r\n\r\n" fetch  2dup s" Content-Length: 4" has? -rot body-of nip -> true 0 }T
testing a handler that awaits another actor's answer
T{ s" /ask" get body-of s" time 42" s= -> true }T
testing redirect, respond-html
T{ s" /moved" get  2dup status-of -rot s" Location: /" has? -> 302 true }T
T{ s" /page" get  s" Content-Type: text/html" has? -> true }T
testing a handler's error, and a handler that gives no answer: 500
T{ s" /broken" get status-of -> 500 }T
T{ s" /silent" get status-of -> 500 }T
testing requests said wrong: 400, and the connection closed
T{ s\" nonsense\r\n\r\n" fetch status-of -> 400 }T
T{ s\" GET / HTTP/2\r\n\r\n" fetch status-of -> 400 }T
T{ s\" GET nowhere HTTP/1.1\r\n\r\n" fetch status-of -> 400 }T
T{ s\" GET / HTTP/1.1\r\nno colon\r\n\r\n" fetch status-of -> 400 }T
T{ s\" POST /echo HTTP/1.1\r\nContent-Length: many\r\n\r\n" fetch status-of -> 400 }T
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" fetch status-of -> 501 }T
testing too much: 431 for the head, 413 for the body
256 to http-headers-max
T{ long-request b-data fetch status-of -> 431 }T
65536 to http-headers-max
10 to http-body-max
T{ s\" POST /echo HTTP/1.1\r\nContent-Length: 20\r\n\r\n01234567890123456789" fetch status-of -> 413 }T
1048576 to http-body-max
testing keep-alive: requests one after another on a connection, each answered
T{ s\" GET / HTTP/1.1\r\n\r\nGET /users/7 HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nConnection: close\r\n\r\n" fetch
   2dup s" HTTP/1.1 200" hits -rot s" user 7" has? -> 3 true }T
T{ s\" GET / HTTP/1.0\r\n\r\n" fetch  2dup status-of -rot s" Connection: close" has? -> 200 true }T
testing a request that comes in pieces: answered once it is whole
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nContent-Length: 5\r\n\r\nab" open-with  20 polls
   reply b-length  s" cde" client-conn @ net-write  500 polls  reply b-data body-of s" abcde" s= -> 0 true }T
testing a connection with no request is closed once http-idle-ms have gone by
50 to http-idle-ms
T{ s" " fetch nip  closed @ -> 0 1 }T
T{ s\" GET / HTTP/1.1\r\n\r\nGET / HT" fetch  s" HTTP/1.1 200" hits  closed @ -> 1 1 }T
30000 to http-idle-ms
testing nothing kept: each request's objects freed, each connection's actor gone
T{ objects objects-before - -> 0 }T
T{ 10 polls  actors-alive actors-before - -> 0 }T
listener net-close

test-summary

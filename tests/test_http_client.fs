\ http.client (basis/http/client): requests from an actor to the http server and to a server of the net vocabulary's
\ own (what a response may be: in chunks, till the close, after a 1xx, said wrong, never), each response awaited
IN: test-http-client
USING: tester actors net http http.client ;

\ ---- an http server to ask ----
<router> constant site
: home ( req -- )  {: r :}  s" home" 200 r respond ;
: echo ( req -- )  {: r :}  r req-body 201 r respond ;
: kind ( req -- )  {: r :}  s" content-type" r req-header 0= if  s" none"  then  200 r respond ;
: query ( req -- )  {: r :}  r req-query 200 r respond ;
: method ( req -- )  {: r :}  r req-method 200 r respond ;
: moved ( req -- )  {: r :}  s" /" 302 r redirect ;
: streamed ( req -- )  {: r :}  200 r respond-start  s" ab" r respond-chunk  s" cd" r respond-chunk ;
create big 100000 allot  big 100000 char x fill
: large ( req -- )  {: r :}  big 100000 200 r respond ;
: broken ( req -- )  drop  true abort" broken on purpose" ;
s" /" ' home site on-get
s" /echo" ' echo site on-post
s" /kind" ' kind site on-post
s" /query" ' query site on-get
s" /method" ' method site on-any
s" /moved" ' moved site on-get
s" /stream" ' streamed site on-get
s" /large" ' large site on-get
s" /broken" ' broken site on-get
site 0 http-listen constant listener
listener listener-port constant port

\ ---- a server of the net vocabulary's own: once a request has come, `canned` answered (the connection closed after,
\ unless `hold` is set) ----
variable canned   variable hold
: can ( a n -- )  canned @ ?dup if  dispose  then  >string drop canned ! ;
behaviour raw-server
    on net-accepted ( listener conn -- )  2drop ;
    on net-received ( conn -- )
        {: c :}  c net-input nip c net-consume
        hold @ 0= if  canned @ string c net-write  c net-close  then ;
    on net-closed ( conn -- )  net-close ;
end-behaviour
raw-server -1 s" raw" spawn constant raw
raw enter  0 listen-local  leave-actor  constant raw-listener
raw-listener listener-port constant raw-port

\ ---- the asker: an actor running a request ( -- res ), what came kept ----
message ask ( xt -- )
create got 200000 allot   variable got-n   variable got-status   variable done   variable failed
create got-header 256 allot   variable got-header-n
create got-error 256 allot   variable got-error-n
: keep ( res -- )
    {: res :}  res res-status got-status !  res res-status 0= failed !
    res res-error {: ea en :}  ea got-error en move  en got-error-n !
    res res-body {: a n :}  a got n move  n got-n !
    s" content-type" res res-header if  {: ha hn :}  ha got-header hn move  hn got-header-n !
    else  0 got-header-n !  then ;
behaviour asker
    on ask ( xt -- )  execute keep  true done ! ;
end-behaviour
asker -1 s" asker" spawn constant the-asker

: run ( xt -- )   \ the request xt makes, run by the asker till it is done
    0 done !  0 failed !  0 got-n !  0 got-status !
    the-asker send ask
    500 0 do  10 net-poll drop  deliver  done @ if  leave  then  loop ;
: settle ( -- )  10 0 do  10 net-poll drop  deliver  loop ;
: body ( -- a n )  got got-n @ ;
: url ( path n -- s len )  {: a n :}  port a n s" http://127.0.0.1:{}{s}" format ;
: raw-url ( -- s len )  raw-port s" http://127.0.0.1:{}/" format ;

objects constant objects-before
actors-alive constant actors-before

testing a GET: its status, its body, its headers by name in any case
T{ [: s" /" url http-get ;] run  got-status @  body s" home" s= -> 200 true }T
T{ got-header got-header-n @ s" text/plain" s-starts? -> true }T
T{ [: s" /query?a=1&b=2" url http-get ;] run  body s" a=1&b=2" s= -> true }T
T{ [: port s" http://127.0.0.1:{}?x=y" format http-get ;] run  body s" home" s= -> true }T
T{ [: port s" HTTP://[::1]:{}/" format http-get ;] run  body s" home" s= -> true }T
testing a POST: its body and content type sent
T{ [: s" abc" s" text/plain" s" /echo" url http-post ;] run  got-status @  body s" abc" s= -> 201 true }T
T{ [: s" a=1" s" application/x-www-form-urlencoded" s" /kind" url http-post ;] run
   body s" application/x-www-form-urlencoded" s= -> true }T
T{ [: s" PUT" s" /method" url s\" X-A: 1\r\n" s" " http-request ;] run  body s" PUT" s= -> true }T
testing a response in chunks, a long one, a HEAD's (no body), a redirect's, a failed handler's
T{ [: s" /stream" url http-get ;] run  body s" abcd" s= -> true }T
T{ [: s" /large" url http-get ;] run  got-n @ -> 100000 }T
T{ [: s" HEAD" s" /" url s" " s" " http-request ;] run  got-status @ got-n @ -> 200 0 }T
T{ [: s" /moved" url http-get ;] run  got-status @ -> 302 }T
T{ [: s" /broken" url http-get ;] run  got-status @ -> 500 }T
testing a response with neither a length nor chunks: its body what came till the close
T{ s\" HTTP/1.1 200 OK\r\nX-A: b\r\n\r\nto the end" can  [: raw-url http-get ;] run
   body s" to the end" s= -> true }T
testing a 1xx before the answer: passed over
T{ s\" HTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok" can
   [: raw-url http-get ;] run  got-status @  body s" ok" s= -> 200 true }T
testing a chunked response with a trailer; a 204's (no body, though more came)
T{ s\" HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\nX-T: 1\r\n\r\n" can
   [: raw-url http-get ;] run  body s" hi" s= -> true }T
T{ s\" HTTP/1.1 204 No Content\r\n\r\n" can  [: raw-url http-get ;] run  got-status @ got-n @ -> 204 0 }T
testing what is no response: status 0, res-error saying why (a request's actor gone after)
T{ s\" garbage\r\n\r\n" can  [: raw-url http-get ;] run  failed @  got-status @ -> true 0 }T
T{ got-error got-error-n @ s" http: not an HTTP response" s= -> true }T
T{ s\" HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort" can  [: raw-url http-get ;] run  failed @ -> true }T
T{ s\" HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n" can  [: raw-url http-get ;] run  failed @ -> true }T
T{ s\" HTTP/1.1 200 OK\r\nContent-Length: 2\r\n" can  [: raw-url http-get ;] run  failed @ -> true }T
testing no answer in http-timeout-ms: none
100 to http-timeout-ms
T{ true hold !  [: raw-url http-get ;] run  failed @ -> true }T
0 hold !  30000 to http-timeout-ms
testing a URL that is no http one, and a port no one listens on: none
T{ [: s" ftp://127.0.0.1/" http-get ;] run  failed @ -> true }T
T{ [: s" http://" http-get ;] run  failed @ -> true }T
T{ [: s" http://user@127.0.0.1/" http-get ;] run  failed @ -> true }T
T{ [: s" http://127.0.0.1:99999/" http-get ;] run  failed @ -> true }T
raw enter  0 listen-local  leave-actor  dup listener-port swap net-close  constant nobody-port
T{ [: nobody-port s" http://127.0.0.1:{}/" format http-get ;] run  failed @ -> true }T
testing nothing kept: each request's actor gone, its objects freed
T{ settle  actors-alive actors-before - -> 0 }T
canned @ dispose
T{ objects objects-before - -> 0 }T
testing outside an actor: an error (its response is a message)
T{ s" http://127.0.0.1/" ' http-get catch nip nip -> -1 }T

listener net-close  raw-listener net-close

test-summary

\ the http vocabulary: a server on a free port, asked by a client of the net vocabulary's own, its replies read
IN: test-http
USING: tester actors net http vocabs.webclient ;

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
: form-name ( req -- )  {: r | a n :}
    s" name" r form-param if  to n to a  else  s" none" to n to a  then  a n 200 r respond ;

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
s" /form" ' form-name site on-post
\ answers long enough to gzip: HTML, and the same as an image (not gzipped)
: make-page ( -- s len )  [: 300 0 do  s" <tr><td>row " %s  i %n  s" </td></tr>" %s  loop ;] build string ;
make-page drop constant the-page   \ (made once, before the objects are counted)
: page-text ( -- a n )  the-page string ;
: big-page ( req -- )  {: r :}  page-text 200 r respond-html ;
: big-png ( req -- )  {: r :}  s" image/png" r content-type!  page-text 200 r respond ;
s" /big" ' big-page site on-get
s" /big.png" ' big-png site on-get

\ a handler that waits for another actor's answer (its connection's actor busy meanwhile, no one else held up)
message what-time ( -- )
message the-time ( n -- )
behaviour clock-keeper  on what-time ( -- )  42 sender send the-time ;  end-behaviour
clock-keeper -1 s" clock" spawn constant clock
: ask-clock ( req -- )  {: r :}  clock send what-time  await the-time  s" time {}" format  200 r respond ;
s" /ask" ' ask-clock site on-get

\ answers streamed: in chunks, with a wait between; and a route's data
: streamed ( req -- )  {: r :}  200 r respond-start  s" ab" r respond-chunk  r req-gone? if  exit  then
    clock send what-time  await the-time drop  s" cd" r respond-chunk ;
: show-data ( req -- )  {: r :}  r req-data s" data {}" format  200 r respond ;
s" /stream" ' streamed site on-get
s" GET" s" /data" ' show-data 77 site route-with

site 0 http-listen constant listener
listener listener-port web-port !

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
testing a header by its name in any case
T{ s\" GET /agent HTTP/1.1\r\nUSER-AGENT:   tester  \r\nConnection: close\r\n\r\n" fetch body-of s" tester" s= -> true }T
T{ s" /agent" get body-of s" none" s= -> true }T
testing HEAD: a GET's head, no body
T{ s\" HEAD / HTTP/1.1\r\nConnection: close\r\n\r\n" fetch  2dup s" Content-Length: 4" has? -rot body-of nip -> true 0 }T
testing every answer dated (RFC 9110's form)
T{ s" /" get  s" Date: " has? -> true }T
T{ s" /" get  s" GMT" has? -> true }T
testing an answer streamed: chunks to HTTP/1.1 (the handler waiting between), the body to the close to 1.0
T{ s" /stream" get  2dup s" Transfer-Encoding: chunked" has?  -rot body-of s\" 2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n" s= -> true true }T
T{ s\" GET /stream HTTP/1.0\r\n\r\n" fetch  2dup s" Transfer-Encoding" has?  -rot body-of s" abcd" s= -> false true }T
T{ s\" HEAD /stream HTTP/1.1\r\nConnection: close\r\n\r\n" fetch body-of nip -> 0 }T
T{ s\" GET /stream HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nConnection: close\r\n\r\n" fetch s" HTTP/1.1 200" hits -> 2 }T
testing a route's data, given its handler
T{ s" /data" get body-of s" data 77" s= -> true }T
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
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n" fetch status-of -> 501 }T
testing a form's body: a name's value, decoded; none without the form's content type
T{ s\" POST /form HTTP/1.1\r\nConnection: close\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 15\r\n\r\nname=a+b%21&x=1" fetch
   body-of s" a b!" s= -> true }T
T{ s\" POST /form HTTP/1.1\r\nConnection: close\r\nContent-Type: Application/X-WWW-Form-Urlencoded; charset=UTF-8\r\nContent-Length: 6\r\n\r\nname=z" fetch
   body-of s" z" s= -> true }T
T{ s\" POST /form HTTP/1.1\r\nConnection: close\r\nContent-Length: 6\r\n\r\nname=z" fetch body-of s" none" s= -> true }T
testing a body in chunks: put together (extensions and trailer fields read past), the connection kept after
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2;x=y\r\nde\r\n0\r\nX-T: 1\r\n\r\n" fetch
   2dup status-of -rot body-of s" abcde" s= -> 201 true }T
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: Chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\nGET /users/8 HTTP/1.1\r\nConnection: close\r\n\r\n" fetch
   2dup s" 0123456789" has? -rot s" user 8" has? -> true true }T
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n" fetch
   2dup status-of -rot body-of nip -> 201 0 }T
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab" open-with  20 polls
   reply b-length  s\" c\r\n0\r\n\r\n" client-conn @ net-write  500 polls  reply b-data body-of s" abc" s= -> 0 true }T
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nd" open-with  20 polls
   s\" e\r\n1\r\n" client-conn @ net-write  20 polls  s\" f\r\n0\r\n\r\n" client-conn @ net-write  500 polls
   reply b-data body-of s" abcdef" s= -> true }T   \ (read on from the chunks already read, not again)
testing a body in chunks said wrong: 400 (a size not hex, data not ended by CR LF, a Content-Length too)
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nab\r\n0\r\n\r\n" fetch status-of -> 400 }T
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabc\r\n0\r\n\r\n" fetch status-of -> 400 }T
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n2\r\nab\r\n0\r\n\r\n" fetch status-of -> 400 }T
testing Expect: 100-continue: told to go on once its head is in, then answered
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n" open-with
   25 30 polls-for  reply b-data s\" HTTP/1.1 100 Continue\r\n\r\n" s= -> true }T
T{ s" abc" client-conn @ net-write  500 polls  reply b-data  2dup s" 100 Continue" hits -rot s" HTTP/1.1 201" has? -> 1 true }T
T{ s\" POST /echo HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n" open-with  20 polls  reply b-length -> 0 }T
client-conn @ net-close
testing too much: 431 for the head, 413 for the body
256 to http-headers-max
T{ long-request b-data fetch status-of -> 431 }T
65536 to http-headers-max
10 to http-body-max
T{ s\" POST /echo HTTP/1.1\r\nContent-Length: 20\r\n\r\n01234567890123456789" fetch status-of -> 413 }T
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n8\r\n01234567\r\n8\r\n01234567\r\n0\r\n\r\n" fetch status-of -> 413 }T
T{ s\" POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffffffffffff\r\n" fetch status-of -> 413 }T
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
testing gzip: a long HTML answer to a client that takes it, gzipped (Vary said); not to one that doesn't; not an
testing  image's; not a short answer
USING: compression ;
: gz ( -- a n )  s\" Accept-Encoding: deflate, gzip, br\r\n" ;
create got 20000 allot   variable got-n
: keep-got ( a n -- )  dup got-n !  got swap move ;
T{ s" /big" gz get-with keep-got  got got-n @ s" Content-Encoding: gzip" s-find 0>= -> true }T
T{ got got-n @ s" Vary: Accept-Encoding" s-find 0>= -> true }T
: unzipped-right? ( -- flag )  got got-n @ body-of 100000 gunzip 0= if  false exit  then  {: u un :}  u un page-text s=  u dispose ;
T{ unzipped-right? -> true }T
T{ got got-n @ body-of nip  page-text nip 4 / < -> true }T
T{ s" /big" get keep-got  got got-n @ s" Content-Encoding" s-find 0<  got got-n @ s" Vary: Accept-Encoding" s-find 0>= -> true true }T
T{ s" /big.png" gz get-with  s" Content-Encoding" s-find 0< -> true }T
T{ s" /" gz get-with  s" Content-Encoding" s-find 0< -> true }T
T{ s" /big" s\" Accept-Encoding: gzip;q=0\r\n" get-with  s" Content-Encoding" s-find 0< -> true }T

testing nothing kept: each request's objects freed, each connection's actor gone
T{ objects objects-before - -> 0 }T
T{ 10 polls  actors-alive actors-before - -> 0 }T
listener net-close

test-summary

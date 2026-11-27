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
message let-go ( -- )   \ (http-drain's test: a request that waits to be let go)
variable held-by
: held ( req -- )  {: r :}  self held-by !  await let-go  s" let go" 200 r respond ;
s" /held" ' held site on-get
s" /files/*" ' rest site on-get
s" /agent" ' agent site on-get
s" /moved" ' moved site on-any
s" /page" ' page site on-get
s" /broken" ' broken site on-get
s" /silent" ' silent site on-get
\ the objects alive as each request on a connection is handled: the first's count, the latest's
shared-variable counted-first  variable counted-last   \ (the test's own record, across requests)
: count-objects ( req -- )  {: r :}  objects  counted-first @ 0= if  dup counted-first !  then  counted-last !  s" ok" 200 r respond ;
s" /count" ' count-objects site on-get
s" /form" ' form-name site on-post
: not-modified ( req -- )  {: r :}  s" " 304 r respond ;   \ (a cache's answer: no body, no length, no type)
s" /cached" ' not-modified site on-get
\ answers long enough to gzip: HTML, and the same as an image (not gzipped)
: make-page ( -- s len )  [: 300 0 do  s" <tr><td>row " %s  i %n  s" </td></tr>" %s  loop ;] build string ;
make-page drop constant the-page   \ (made once, before the objects are counted)
: page-text ( -- a n )  the-page string ;
: big-page ( req -- )  {: r :}  page-text 200 r respond-html ;
: big-png ( req -- )  {: r :}  s" image/png" r content-type!  page-text 200 r respond ;
s" /big" ' big-page site on-get
: secret-page ( req -- )  {: r :}  s" secret" 200 r respond ;
s" /admin/x" ' secret-page site on-get
s" /admin" [: {: r x :}  s" no" 403 r respond  false ;] 0 site before-under-with
: echo-header ( req -- )  {: r :}  s" v" r query-param drop  s" X-Echo" r header!  s" ok" 200 r respond ;
s" /echo-header" ' echo-header site on-get
s" /big.png" ' big-png site on-get
: late-fail ( req -- )  {: r :}  s" done" 200 r respond  true abort" failed after its answer" ;
s" /late" ' late-fail site on-get
: own-encoding ( req -- )  {: r :}  s" br" s" content-encoding" r header!  page-text 200 r respond ;
s" /own-encoding" ' own-encoding site on-get
: own-type ( req -- )  {: r :}  s" application/octet-stream" s" content-type" r header!  page-text 200 r respond ;
s" /own-type" ' own-type site on-get
: nothing ( req -- )  {: r :}  s" " 204 r respond ;
s" /nothing" ' nothing site on-get

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
testing a 304 answered by respond: no Content-Length, no Content-Type (it would say the 200's, RFC 9110)
T{ s" /cached" get  2dup status-of -rot  2dup s" Content-Length" has? -rot s" Content-Type" has? -> 304 false false }T
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
testing a connection kept: a request's objects freed as it is answered, not as the connection ends
T{ 0 counted-first !  s\" GET /count HTTP/1.1\r\nConnection: keep-alive\r\n\r\nGET /count HTTP/1.1\r\nConnection: keep-alive\r\n\r\nGET /count HTTP/1.1\r\nConnection: close\r\n\r\n" fetch
   s" HTTP/1.1 200" hits  counted-last @ counted-first @ - -> 3 0 }T
testing a request that comes in pieces: answered once it is whole
T{ s\" POST /echo HTTP/1.1\r\nConnection: close\r\nContent-Length: 5\r\n\r\nab" open-with  20 polls
   reply b-length  s" cde" client-conn @ net-write  500 polls  reply b-data body-of s" abcde" s= -> 0 true }T
testing a connection with no request is closed once http-idle-ms have gone by
50 to http-idle-ms
T{ s" " fetch nip  closed @ -> 0 1 }T
T{ s\" GET / HTTP/1.1\r\n\r\nGET / HT" fetch  s" HTTP/1.1 200" hits  closed @ -> 1 1 }T
30000 to http-idle-ms
testing a guard on a path can't be gone round: //admin and /./ are the routes' /admin (the path the routes see)
T{ s" /admin/x" get status-of -> 403 }T
T{ s" //admin/x" get status-of -> 403 }T
T{ s" /admin//x" get status-of -> 403 }T
testing what a proxy may read otherwise refused: a space before the colon, a length not in digits, a bare LF, NUL,
testing  too many header lines
: raw ( a n -- status )  fetch status-of ;
T{ s\" GET / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding : chunked\r\nConnection: close\r\n\r\n" raw -> 400 }T
T{ s\" POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: $5\r\nConnection: close\r\n\r\nhello" raw -> 400 }T
T{ s\" GET / HTTP/1.1\r\nHost: t\r\nX-A: 1\nX-B: 2\r\nConnection: close\r\n\r\n" raw -> 400 }T
T{ s\" GET / HTTP/1.1\r\nHost: t\r\nX-A: 1\x00\r\nConnection: close\r\n\r\n" raw -> 400 }T
\ (a header's name: RFC 9110's token - letters, digits, these marks; nothing else, a byte past ASCII neither)
T{ s\" GET / HTTP/1.1\r\nHost: t\r\naZ09!#$%&'*+-.^_`|~: 1\r\nConnection: close\r\n\r\n" raw -> 200 }T
T{ s\" GET / HTTP/1.1\r\nHost: t\r\nX(A): 1\r\nConnection: close\r\n\r\n" raw -> 400 }T
T{ s\" GET / HTTP/1.1\r\nHost: t\r\nX\xc3\xa9: 1\r\nConnection: close\r\n\r\n" raw -> 400 }T
T{ s\" GET / HTTP/1.1\r\nHost: t\r\nX-A: a\tb\r\nConnection: close\r\n\r\n" raw -> 200 }T   \ (a tab in a value: fine)
T{ s\" GET / HTTP/1.1\r\nHost: t\r\nX-A: 1\rX\r\nConnection: close\r\n\r\n" raw -> 400 }T   \ (a CR not before an LF)
: many-headers ( -- a n )   \ (101 header lines)
    [: s\" GET / HTTP/1.1\r\n" %s  101 0 do  s\" X-H: 1\r\n" %s  loop  s\" Connection: close\r\n\r\n" %s ;] build string ;
: many-raw ( -- status )  many-headers {: a n :}  a n raw  a dispose ;
T{ many-raw -> 400 }T
testing what waits to go has a limit: past it, the requests after wait till it has gone - each still answered
0 to http-pending-max
T{ s\" GET /big HTTP/1.1\r\n\r\nGET /big HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nConnection: close\r\n\r\n" fetch s" HTTP/1.1 200" hits -> 3 }T
1048576 to http-pending-max
testing a node's connections have a limit: one past it answered 503 and closed (the others go on)
0 to http-connections-max
T{ s" /" get status-of -> 503 }T
10000 to http-connections-max
T{ s" /" get status-of 503 <> -> true }T
testing a guard can't be gone round by %-escapes: /%61dmin and /admin%2Fx are /admin/x to the routes, and to it
T{ s" /%61dmin/x" get status-of  s" /admin%2Fx" get status-of -> 403 403 }T
testing an answer given, then the handler failed: the answer still goes out (not dropped with the connection)
T{ s" /late" get  2dup status-of -rot body-of s" done" s= -> 200 true }T
testing a header the handler set in its own case: Content-Encoding not gzipped again, Content-Type not given twice
: gz-get ( path n -- a n )  s\" Accept-Encoding: gzip\r\n" get-with ;
T{ s" /own-encoding" gz-get s" Content-Encoding: gzip" has? -> false }T
T{ s" /own-type" gz-get  2dup s" Content-Type:" hits  -rot s" content-type:" hits + -> 1 }T
testing gzip;q=0.8 takes gzip, q=0 not; a 204 without a length or a type; HTTP/1.0 kept alive says so
T{ s" /big" s\" Accept-Encoding: gzip;q=0.8\r\n" get-with  s" Content-Encoding: gzip" has? -> true }T
T{ s" /big" s\" Accept-Encoding: gzip;q=0, deflate\r\n" get-with  s" Content-Encoding: gzip" has? -> false }T
T{ s" /nothing" get  2dup status-of  -rot 2dup s" Content-Length" has?  -rot s" Content-Type" has? -> 204 false false }T
T{ s\" GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET / HTTP/1.0\r\n\r\n" fetch  s" Connection: keep-alive" has? -> true }T

testing a header's value with a CR LF in it (a request's text put in a header): refused, not a header of its own
T{ s" /echo-header?v=a%0d%0aSet-Cookie:%20x=1" get  s" Set-Cookie: x=1" has? -> false }T

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

testing http-drain: a request in flight that waits on a task (not an actor) - the drain runs the tasks' turns too,
testing  and it is answered before the drain's time is up
: releaser ( -- )  begin  held-by @ 0=  while  yield  repeat  yield yield  held-by @ send let-go ;
T{ 0 held-by !  s\" GET /held HTTP/1.1\r\nConnection: close\r\n\r\n" open-with  5 polls  http-in-flight -> 1 }T
T{ ' releaser start-task drop  500 http-drain  http-in-flight  20 polls  reply b-data status-of -> 0 200 }T
false to http-draining
: settle-down ( -- )  10 0 do  10 net-poll drop  deliver  loop ;   \ (its connection's actor gone, what it held freed)
settle-down

testing nothing kept: each request's objects freed, each connection's actor gone
T{ objects objects-before - -> 0 }T
T{ 10 polls  actors-alive actors-before - -> 0 }T
listener net-close

test-summary

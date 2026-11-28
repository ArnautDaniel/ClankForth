\ http.fs - an HTTP/1.1 server on the net vocabulary (docs/http.md). Each connection is an actor of its own, so a
\ handler may wait (`await` another actor's answer) without holding up the rest; each request is read, routed and
\ answered inside a `with-scratch`, so what it and its handler make is freed once it is answered.
\
\   <router> constant site
\   : home ( req -- )  {: r :}  s" hello" 200 r respond ;
\   s" /" ' home site on-get
\   site 8080 http-listen drop  serve
\
\ Its parts, each a vocabulary of its own: http.cookies, http.files (static files), http.sse (server-sent events),
\ http.websocket, http.tls (https-listen), http.client (requests to other servers).
IN: http
USING: actors net system accessors calendar urls.encoding compression supervisor ;

65536 value http-headers-max     \ a request's line and headers, in bytes, at most: more is 431
1048576 value http-body-max      \ its body, at most: more is 413
30000 value http-idle-ms         \ a connection with no whole request in this long is closed
variable http-log                \ set: each request answered printed (its method, path and status)
false value http-draining        \ set (http-drain): each connection closed after the answer it is giving
1048576 value http-pending-max   \ a connection's answers waiting to go, at most, before it answers more of its requests
10000 value http-connections-max \ connections a node keeps open at once: more are answered 503 and closed
20 constant pump-ms              \ (a connection whose answers wait: looked at again so often)
1024 value http-gzip-min         \ an answer's body this long or more - text, JSON, JavaScript, XML, SVG - gzipped to a
                                 \ client that takes it (Accept-Encoding: gzip); 0: never

4 constant blank-line-bytes      \ the CR LF CR LF that ends a request's head
s\" \r\n" >string drop permanent constant crlf
s\" \r\n\r\n" >string drop permanent constant blank-line
1 constant stream-chunked        \ a streamed answer's body in chunks (HTTP/1.1) ...
2 constant stream-to-close       \ ... or up to the connection's close (1.0)
3 constant stream-sized          \ ... or as they are, its Content-Length said (respond-start-sized)
262144 value http-stream-ahead   \ a streamed answer's bytes waiting to go, at most, before its writer waits for the
                                 \ client to take them (respond-drained)

\ ---- a request ----
\ method path query version body: strings (`string` gives one's bytes); headers: a map, names lower-cased; params:
\ a map of what the route's :names matched; out: the answer's headers so far; status: 0 until it is answered;
\ streaming: how its body goes, once respond-start began it; taken: the connection is another protocol's now (a
\ WebSocket's); data: its route's (route-with); attrs: what the handler and the words around it keep for the request
\ (req-attr: its session, its user ...); typed: the Content-Type said (a string), or 0
TUPLE: request  method path query version headers body params conn out status keep typed streaming taken data attrs ;

: empty-string ( -- s )  s" " >string drop ;
s" " >string drop permanent constant no-text   \ (what a request's text is till it is read: one string, every request's)
\ A request with nothing in it, then what a new one holds - apart, so a connection's actor can keep the request
\ itself (its last words see it) while what it holds is the scratch region's: rescued whole, a kept connection's
\ actor held every request's strings and maps till the connection ended (`dispose` frees the tuple, not its slots').
: blank-request ( -- req )  0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 <request> ;
: fill-request ( conn req -- req )
    swap >>conn  no-text >>method  no-text >>path  no-text >>query  no-text >>version
    <map> >>headers  no-text >>body  <map> >>params  <bytes> >>out  <map> >>attrs ;
: new-request ( conn -- req )  blank-request fill-request ;

: req-method ( req -- a n )  method>> string ;
: req-path ( req -- a n )  path>> string ;      \ as sent: %-escapes and all
: req-query ( req -- a n )  query>> string ;    \ what came after the ?, as sent ("" for none)
: req-version ( req -- a n )  version>> string ;
: req-body ( req -- a n )  body>> string ;
: req-headers ( req -- m )  headers>> ;
: req-conn ( req -- conn )  conn>> ;
: req-data ( req -- x )  data>> ;               \ what its route was given (route-with), 0 for none
: head? ( req -- flag )  req-method s" HEAD" s= ;
\ the client has gone - nothing more can be sent it (a handler streaming asks). One that only ended its own sending
\ (a request, then a half-close) still gets its answer.
: req-gone? ( req -- flag )  conn>> net-gone? ;
\ what the request keeps by a name till it is answered - its session, its user (any value; an object made in the
\ handler lives as long as the request does)
: req-attr ( name n req -- x true | false )  attrs>> s-at ;
: req-attr! ( x name n req -- )  attrs>> s-at! ;

\ a header in a map of them (names lower-cased), by its name in any case (http.client's answers' too)
: header-of ( name n m -- a n true | false )
    {: a n m | k :}  a n s-lower drop to k
    k string m s-at  k dispose
    dup if  drop string true  then ;
: req-header ( name n req -- a n true | false )  headers>> header-of ;

\ what the route's :name matched (decoded); "*" what its * did
: req-param ( name n req -- a n true | false )
    params>> s-at  dup if  drop string true  then ;

<PRIVATE
\ the value of a name in name=value pairs joined by & (a query string, a form's body), decoded: a new string
: pair-value ( name n text n -- s len true | false )
    {: a n ta tn | pairs :}
    ta tn s" &" s-split to pairs
    pairs length 0 ?do
        i pairs nth string {: pa pn :}  pa pn s-bytes drop to pa
        pa pn s" =" s-find {: eq :}
        eq 0< if  pn to eq  then
        pa eq url-decode {: k kn :}
        k kn a n s= if
            eq pn < if  pa pn eq 1+ pn eq - 1- s-slice url-decode  else  empty-string 0  then
            true  k dispose  pairs [: dispose ;] each  pairs list-free  unloop exit
        then
        k dispose
    loop
    pairs [: dispose ;] each  pairs list-free  false ;

: form? ( req -- flag )   \ its body a form's (application/x-www-form-urlencoded)
    s" content-type" rot req-header 0= if  false exit  then
    s-lower s" application/x-www-form-urlencoded" s-starts? ;
PRIVATE>

\ the value of a name in the query string (decoded: a new string)
: query-param ( name n req -- s len true | false )  {: a n r :}  a n r req-query pair-value ;
\ the value of a name in a form's body (a POST of an HTML form: application/x-www-form-urlencoded), decoded
: form-param ( name n req -- s len true | false )
    {: a n r :}  r form? 0= if  false exit  then  a n r req-body pair-value ;

\ ---- the answer ----
: reason ( status -- a n )
    case
        101 of s" Switching Protocols" endof
        200 of s" OK" endof                  201 of s" Created" endof
        202 of s" Accepted" endof            204 of s" No Content" endof
        206 of s" Partial Content" endof
        301 of s" Moved Permanently" endof   302 of s" Found" endof
        303 of s" See Other" endof           304 of s" Not Modified" endof
        307 of s" Temporary Redirect" endof  308 of s" Permanent Redirect" endof
        400 of s" Bad Request" endof         401 of s" Unauthorized" endof
        403 of s" Forbidden" endof           404 of s" Not Found" endof
        405 of s" Method Not Allowed" endof  408 of s" Request Timeout" endof
        409 of s" Conflict" endof            410 of s" Gone" endof
        411 of s" Length Required" endof     413 of s" Content Too Large" endof
        414 of s" URI Too Long" endof        415 of s" Unsupported Media Type" endof
        416 of s" Range Not Satisfiable" endof
        422 of s" Unprocessable Content" endof  426 of s" Upgrade Required" endof
        429 of s" Too Many Requests" endof   431 of s" Request Header Fields Too Large" endof
        500 of s" Internal Server Error" endof  501 of s" Not Implemented" endof
        503 of s" Service Unavailable" endof    505 of s" HTTP Version Not Supported" endof
        s" " rot
    endcase ;

: answered? ( req -- flag )  status>> 0<> ;
\ no CR, LF nor NUL: text that can go in a header (a value from a request put in one can't add one of its own)
: line-char? ( c -- flag )  {: c :}  c 13 =  c 10 = or  c 0= or ;   \ a CR, LF or NUL
: crlf-free? ( a n -- flag )   \ (s-under: only the bytes under 14 are looked at here - rare, a tab mostly)
    {: a n | i j :}  a n s-bytes drop to a
    begin  a i +  n i -  14 s-under to j  j 0>=  while
        i j + to i  a i + c@ line-char? if  false exit  then  i 1+ to i
    repeat
    true ;
: req-status ( req -- status )  status>> ;   \ the answer's status (0 till it is answered: an after-each's to log)

\ a header of the answer (before it)
: header! ( value n name n req -- )
    {: v vn k kn r | out :}
    r answered? abort" header!: the request has been answered"
    v vn crlf-free? 0=  k kn crlf-free? 0= or  kn 0= or abort" header!: a CR, LF or NUL in a header (or no name)"
    r out>> to out
    k kn out b-append  s" : " out b-append  v vn out b-append  s\" \r\n" out b-append
    k kn s-lower {: l ln :}  l ln s" content-type" s= if  r v vn >string drop >>typed drop  then  l dispose ;
: content-type! ( a n req -- )  s" Content-Type" rot header! ;   \ (header! sees its name, any case: typed)

\ a formatted piece of the answer's head, added to b
: b-format ( .. a n b -- )  {: b :}  format {: s n :}  s n b b-append  s dispose ;

<PRIVATE
variable date-at   create date-text 32 allot   variable date-n
: date-now ( -- a n )   \ this second's Date (made once a second: the node's connections share it, so made with the
    [:  time-now {: t :}   \ others kept out - one half way through isn't another's to read)
        t date-at @ <> if  t >http-date {: s n :}  s n s-bytes drop date-text n move  n date-n !  s dispose  t date-at !  then
        date-text date-n @ ;] atomically ;
: log ( status req -- )
    {: status r :}  http-log @ if  r req-method type space r req-path type space status . cr  then ;
\ (to a client that has gone - its connection let go while the handler waited - nothing: not an error)
: send-out ( a n req -- )  {: a n r :}  r req-gone? if  exit  then  a n r conn>> net-write ;
PRIVATE>

\ the status line and the headers so far (Date, and Connection: close if the connection goes after), the request
\ answered: a new bytes, the caller's to send and dispose. The ground every answer stands on.
: answer-head ( status req -- b )
    {: status r | b :}
    r answered? abort" respond: the request has been answered already"
    http-draining if  r false >>keep drop  then
    r keep>> 0= if  s" close" s" Connection" r header!
    else r req-version s" HTTP/1.0" s= if  s" keep-alive" s" Connection" r header!  then then   \ (1.0 kept: said so)
    <bytes> to b
    status  status reason  s\" HTTP/1.1 {} {s}\r\n" b b-format
    s" Date: " b b-append  date-now b b-append  s\" \r\n" b b-append
    r out>> b-data b b-append
    r status >>status drop
    b ;

: default-type ( req -- )  {: r :}  r typed>> 0= if  s" text/plain; charset=utf-8" r content-type!  then ;

<PRIVATE
: compressible? ( req -- flag )   \ its type text, JSON, JavaScript, XML or SVG (not an image's, an archive's)
    typed>> ?dup 0= if  true exit  then  string s-lower {: t tn :}
    t tn s" text/" s-starts?  t tn s" json" s-find 0>= or  t tn s" javascript" s-find 0>= or  t tn s" xml" s-find 0>= or
    t dispose ;
: zero-q? ( a n -- flag )   \ an encoding's parameters say q=0 (0, 0.0, 0.000): not wanted
    {: a n | at :}  a n s-bytes drop to a  a n s" q=" s-find to at
    at 0< if  false exit  then
    true  n at 2 + ?do  a i + c@ {: c :}  c [char] , = if  leave  then
        c [char] 0 =  c [char] . = or  c 32 = or 0= if  drop false leave  then  loop ;
: takes-gzip? ( req -- flag )   \ its Accept-Encoding names gzip (not with q=0: gzip;q=0.8 takes it)
    {: r :}  s" accept-encoding" r req-header 0= if  false exit  then  s-lower {: e en :}
    e en s" gzip" s-find {: at :}
    at 0>=  if  e en s-bytes drop at + 4 +  en at - 4 -  2dup s" ," s-find dup 0< if  drop  else  nip  then  zero-q? 0=  else  false  then
    e dispose ;
: encoded? ( req -- flag )   \ (one the handler said, in any case: as it is)
    out>> b-data s-lower {: o on :}  o on s" content-encoding:" s-find 0>=  o dispose ;
\ the body gzipped, if it should be: long enough, compressible, a client that takes it, a status with a body
: gzipped ( a n status req -- a' n' )
    {: a n status r :}
    http-gzip-min 0=  n http-gzip-min < or  status 200 < or  status 204 = or  status 304 = or
    r encoded? or  r compressible? 0= or if  a n exit  then
    s" Accept-Encoding" s" Vary" r header!
    r takes-gzip? 0= if  a n exit  then
    s" gzip" s" Content-Encoding" r header!
    a n gzip ;
PRIVATE>

\ the answer: its status line, headers and body, sent (a HEAD request's without its body); once only. A body of
\ http-gzip-min bytes or more, text or the like, goes gzipped to a client that takes it.
: respond ( a n status req -- )
    {: a0 n0 status r | b a n :}
    r answered? abort" respond: the request has been answered already"
    status 200 <  status 204 = or  status 304 = or {: bodiless :}   \ (1xx, 204, 304: no body, no length, no type -
                                                                  \  RFC 9110: a 304's would say the 200's length)
    bodiless 0= if  r default-type  then
    a0 n0 status r gzipped to n to a
    status r answer-head to b
    bodiless if  s\" \r\n" b b-append  0 to n  else  n s\" Content-Length: {}\r\n\r\n" b b-format  then
    b b-data r send-out  b dispose
    r head? 0= if  a n r send-out  then
    status r log ;

: respond-html ( a n status req -- )  {: r :}  s" text/html; charset=utf-8" r content-type!  r respond ;
: respond-json ( a n status req -- )  {: r :}  s" application/json" r content-type!  r respond ;
: redirect ( url n status req -- )  {: status r :}  s" Location" r header!  s" " status r respond ;
: not-found ( req -- )  {: r :}  404 reason 404 r respond ;

\ ---- an answer streamed: its head now, its body a piece at a time (the handler may wait between) ----
\ in chunks to HTTP/1.1 (the connection kept after), to 1.0 up to the close
: respond-start ( status req -- )
    {: status r | b :}
    r answered? abort" respond-start: the request has been answered already"
    r default-type
    r req-version s" HTTP/1.1" s= if
        s" chunked" s" Transfer-Encoding" r header!  r stream-chunked >>streaming drop
    else  r false >>keep drop  r stream-to-close >>streaming drop  then
    status r answer-head to b
    s\" \r\n" b b-append  b b-data r send-out  b dispose
    status r log ;
\ ... its length known: the head says it, and the pieces go as they are - to 1.0 and 1.1 alike, the connection
\ kept after. The pieces must come to that many bytes: fewer, and the client waits for the rest.
: respond-start-sized ( length status req -- )
    {: n status r | b :}
    r answered? abort" respond-start-sized: the request has been answered already"
    r default-type
    r stream-sized >>streaming drop
    status r answer-head to b
    n s\" Content-Length: {}\r\n\r\n" b b-format  b b-data r send-out  b dispose
    status r log ;
\ the handler waits here while more than http-stream-ahead bytes of its answer are still to go (a client slower
\ than what is read for it: what waits is bounded, not the file's size) - not for one that has gone
: respond-drained ( req -- )
    {: r :}  begin  r conn>> net-pending http-stream-ahead >  r req-gone? 0= and  while  pump-ms sleep-ms  repeat ;
: respond-chunk ( a n req -- )   \ a piece of the body (none for a HEAD request)
    {: a n r :}
    r streaming>> 0= abort" respond-chunk: no respond-start"
    n 0=  r head? or if  exit  then
    r streaming>> stream-chunked = if
        n s\" {x}\r\n" format {: s sn :}  s sn r send-out  s dispose
        a n r send-out  s\" \r\n" r send-out
    else  a n r send-out  then ;
: respond-end ( req -- )   \ the body's end (a handler that returns streaming ends it so)
    {: r :}
    r streaming>> stream-chunked =  r head? 0= and if  s\" 0\r\n\r\n" r send-out  then
    r 0 >>streaming drop ;

\ the connection is another protocol's from here (a WebSocket's: answered 101 by it): no more requests read on it
: take-connection ( req -- )  true >>taken drop ;

\ ---- routes ----
\ a route: its method ("*" any), its pattern's segments, its handler ( req -- ), what it gives the handler (req-data)
TUPLE: http-route  verb segments action data ;
\ befores and afters: the hooks run round every handler (a list of http-hook each)
TUPLE: http-router  routes fallback befores afters ;
: <router> ( -- router )  list 0 list list <http-router> ;
\ a hook: the paths it is for (a prefix: "/admin" for /admin and /admin/..., 0 every path), its word ( req x --
\ continue? ) - an after's ( req x -- ) - and its x
TUPLE: http-hook  prefix xt data ;

<PRIVATE
\ a path's segments (new strings, as sent), the empty ones left out: "/a//b/" is a, b
: segments ( a n -- l )
    {: a n | all made :}  a n s" /" s-split to all  list to made
    all [: dup string nip if  made push  else  dispose  then ;] each
    all list-free  made ;
PRIVATE>

\ requests with this method to paths of this pattern handled by xt ( req -- ), x its req-data. A segment of the
\ pattern ":name" matches any one segment (`req-param` gives it); a last "*" matches what is left, none or more
: route-with ( method n pattern n xt x router -- )
    {: ma mn pa pn xt x rt :}  ma mn >string drop  pa pn segments  xt in-handler x <http-route>  rt routes>> push ;
: route ( method n pattern n xt router -- )  {: ma mn pa pn xt rt :}  ma mn pa pn xt 0 rt route-with ;
: on-get ( pattern n xt router -- )  {: pa pn xt rt :}  s" GET" pa pn xt rt route ;
: on-post ( pattern n xt router -- )  {: pa pn xt rt :}  s" POST" pa pn xt rt route ;
: on-put ( pattern n xt router -- )  {: pa pn xt rt :}  s" PUT" pa pn xt rt route ;
: on-delete ( pattern n xt router -- )  {: pa pn xt rt :}  s" DELETE" pa pn xt rt route ;
: on-any ( pattern n xt router -- )  {: pa pn xt rt :}  s" *" pa pn xt rt route ;
: on-missing ( xt router -- )  swap in-handler >>fallback drop ;   \ what answers a path no route has (else a 404)

\ ---- hooks: words run before every handler (a check, a header for every answer) and after (a log) ----
<PRIVATE
: plain-before ( req xt -- continue? )  execute ;
: plain-after ( req xt -- )  execute ;
: prefix-of ( a n -- s | 0 )  {: a n :}  n 0=  a n s" /" s= or if  0  else  a n >string drop  then ;
PRIVATE>
\ xt ( req x -- continue? ) run before the handler of each request to a path under the prefix ("" or "/": every path),
\ in the order they were added: one that gives false has answered (a 401, a 429, a redirect) - no handler runs
: before-under-with ( prefix n xt x router -- )
    {: pa pn xt x rt :}  pa pn prefix-of xt in-handler x <http-hook>  rt befores>> push ;
: before-each-with ( xt x router -- )  {: xt x rt :}  s" " xt x rt before-under-with ;
\ xt ( req -- continue? ) before every handler, or those under the prefix
: before-each ( xt router -- )  {: xt rt :}  ['] plain-before xt in-handler rt before-each-with ;
: before-under ( prefix n xt router -- )  {: pa pn xt rt :}  pa pn ['] plain-before xt in-handler rt before-under-with ;
\ xt ( req x -- ) / ( req -- ) after each handler - the request answered by then (a log's, a count's)
: after-each-with ( xt x router -- )  {: xt x rt :}  0 xt in-handler x <http-hook>  rt afters>> push ;
: after-each ( xt router -- )  {: xt rt :}  ['] plain-after xt in-handler rt after-each-with ;

false value http-trust-proxy   \ set: the client's address the last of X-Forwarded-For (behind a proxy of your own)
\ the client's address (no port): the connection's other end - or, http-trust-proxy set, the last address its
\ X-Forwarded-For names: the one the proxy in front added (those before it are what the client said - anything). A
\ new string.
: client-address ( req -- a n )
    {: r :}
    http-trust-proxy if  s" x-forwarded-for" r req-header if
        {: fa fn :}  fa fn s-bytes drop to fa  -1 {: comma :}
        fn 0 ?do  fa i + c@ [char] , = if  i to comma  then  loop
        fa comma 1+ +  fn comma - 1-  s-trim >string exit
    then  then
    r req-conn conn-peer {: pa pn :}
    pa pn s-bytes drop {: a :}  pn {: at :}  pn 0 ?do  a i + c@ [char] : = if  i to at  then  loop
    pa at >string  pa dispose ;

<PRIVATE
\ the segments from `from` on, joined by / and decoded
: rest-of ( segs from -- s len )
    {: segs from | b :}  <bytes> to b
    segs length from ?do  b b-length if  [char] / b b-push  then  i segs nth string b b-append  loop
    b b-data path-decode  b dispose ;

\ the route's pattern fits the path's segments: what its :names matched kept in the request's params
: fits? ( segs route req -- flag )
    {: segs rt r | pat :}  rt segments>> to pat  r params>> map-clear
    pat length 0 ?do
        i pat nth string {: pa pn :}  pa pn s-bytes drop to pa
        pa pn s" *" s=  i pat length 1- =  and if
            segs i rest-of drop  s" *" r params>> s-at!  true unloop exit
        then
        i segs length >= if  false unloop exit  then
        i segs nth string {: sa sn :}
        pa c@ [char] : = if
            sa sn path-decode drop  pa 1+ pn 1- r params>> s-at!
        else
            pa pn sa sn s= 0= if  false unloop exit  then
        then
    loop
    pat length segs length = ;

: verb-fits? ( a n req -- flag )
    {: va vn r :}  va vn s" *" s=  va vn r req-method s= or  va vn s" GET" s= r head? and or ;
PRIVATE>

\ the request to the first route that fits it; one whose path fits, with no route for its method, is a 405
: dispatch ( req router -- )
    {: r rt | segs allow :}
    r req-path segments to segs  <bytes> to allow
    rt routes>> length 0 ?do
        i rt routes>> nth {: ro :}
        segs ro r fits? if
            ro verb>> string {: va vn :}
            va vn r verb-fits? if  r ro data>> >>data drop  r ro action>> execute  unloop exit  then
            allow b-length if  s" , " allow b-append  then  va vn allow b-append
        then
    loop
    allow b-length if  allow b-data s" Allow" r header!  405 reason 405 r respond  exit  then
    rt fallback>> ?dup if  r swap execute  exit  then
    r not-found ;

<PRIVATE
\ the handler run; one that gives no answer is answered 500 (the connection closed after), one streaming has its
\ body ended. One that fails ends its connection's actor - the error said, as any handler's - whose last words answer
\ 500 (http-connection, below).
\ the path under the prefix: it, or it and a / and more
: under? ( path n prefix n -- flag )
    {: a n pa pn :}  a n s-bytes drop to a  pa pn s-bytes drop to pa  a n pa pn s-starts? 0= if  false exit  then
    n pn =  if  true exit  then
    a pn + c@ [char] / =  pa pn + 1- c@ [char] / =  or ;
: canonical ( req -- s len )   \ its path as the routes see it: its segments joined and decoded - //admin/x, /%61dmin/x
    req-path segments {: segs :}                \ and /admin%2Fx are all /admin/x (the empty ones out, as `fits?` does)
    segs empty? if  s" /" >string exit  then
    [: segs [: [char] / %c  string path-decode {: d dn :}  d dn %s  d dispose ;] each ;] build string ;
: befores-pass? ( req router -- flag )   \ every before for its path let it through (the path the routes see)
    {: r rt | pa pn :}  r canonical to pn to pa
    rt befores>> length 0 ?do
        i rt befores>> nth {: h :}
        h prefix>> 0=  if  true  else  pa pn h prefix>> string under?  then  if
            r h data>> h xt>> execute 0= if  false unloop exit  then
        then
    loop
    true ;
: run-handler ( req router -- )
    {: r rt :}
    r rt befores-pass? if  r rt dispatch  then
    r streaming>> if  r respond-end  then
    r answered? 0= if
        ." http: " r req-method type space r req-path type ." : no answer given" cr
        r false >>keep drop  500 reason 500 r respond
    then
    rt afters>> [: {: h :}  r h data>> h xt>> execute ;] each ;
PRIVATE>

\ ---- reading a head's headers, and a body in chunks (a request's, http.client's answer's) ----
\ A head's header lines (those after its first, the request's or status line) put in the map, their names
\ lower-cased (said twice: both, joined by ", "); false if a line has no name, or one a header may not have.
100 value http-header-lines-max  \ a head's header lines at most: more is no request (400)
\ What comes on a connection is read as a sequence (net-view: a slice of what has come - no address to keep past
\ the bytes): where a head ends, a line ends, is looked for from a place in it; a piece of it is a slice, no copy,
\ and what a request keeps past the read - its target, its headers, its body - a string of its own.
\ a head - the first h bytes of what came - is its first line, to the first CR LF (at h, if it has that line alone),
\ and the lines after: those a slice, for the words that read them
: line-end-in ( seq h -- e )  {: s h :}  s crlf subseq-index {: e :}  e 0<  e h > or if  h  else  e  then ;
: lines-after ( seq e h -- slice )  {: s e h :}  e 2 + h min  h  s <slice> ;
: parse-headers ( seq m -- flag )  {: s m :}  s string http-header-lines-max m fields>map ;   \ (fields>map: C's)

: digits>length ( a n -- n true | false )   \ a length as HTTP says one: decimal digits only (not $5, -0, 'A')
    {: a n | b :}  n 0=  n 15 > or if  false exit  then
    a n scan-digits n <> if  false exit  then
    base @ to b  decimal  a n string>number  b base ! ;
1024 constant chunk-line-max     \ a chunk's size line (or a trailer's field), at most
16384 constant trailer-max       \ a body in chunks' trailer fields together, at most

<PRIVATE
create hex-class 256 allot   create blank-class 256 allot
: of-class ( a n table -- )  {: a n t :}  t 256 erase  n 0 ?do  1  a i + c@ t +  c!  loop ;
s" 0123456789abcdefABCDEF" hex-class of-class   s\"  \t" blank-class of-class
\ a chunk's size: the hex digits at `from` of in, before any `;` extension, in the line that ends before eol - capped
\ at limit+1; false if there is no hex number there, or something after it that is no extension
: chunk-size ( in from eol limit -- size true | false )
    {: in from eol limit | d e :}
    in hex-class from span eol min to d
    d from = if  false exit  then
    in blank-class d span eol min to e
    e eol < if  e in nth [char] ; <> if  false exit  then  then
    in from d 16 digits>number 0= if  limit 1+ true exit  then   \ (past a cell: more than any limit)
    limit 1+ min true ;

\ the length of the line at pos of in with its CR LF, 0 if it isn't all there; -400 if too long to be one
: line-end ( in pos -- le | 0 | -400 )
    {: in pos :}  in crlf pos subseq-index-from {: eol :}
    eol 0< if  in length pos - chunk-line-max > if  -400  else  0  then  exit  then
    eol pos - 2 + ;
PRIVATE>

\ a body in chunks (RFC 9112's Transfer-Encoding: chunked) at the start of in (a string, a slice), its data added to
\ b: the bytes it takes if it is all there (its trailer's fields read past, not kept), 0 if not yet, or minus the
\ status of what is wrong - -400 said wrong, -413 more than limit bytes. It goes on from where it stopped: `at`
\ holds how far into the body the chunks put in b so far go (0 to begin) - so a body that comes a piece at a time is
\ read once, not again each time more comes (the same b and at kept between, the bytes before at as they were)
: dechunk ( in limit b at -- used | 0 | -status )
    {: in limit b at | pos n le size :}  in length to n  at @ to pos
    begin
        pos limit 2* chunk-line-max + > if  -413 exit  then   \ (its size lines and extensions too: bounded, not just its data)
        in pos line-end to le
        le 1 < if  le exit  then
        in pos  pos le + 2 -  limit chunk-size 0= if  -400 exit  then  to size
        pos le + to pos
        size 0= if   \ the last: trailer fields, up to a blank line (as many bytes as a head may have, at most)
            pos {: trailer-at :}
            begin
                pos trailer-at - trailer-max > if  -431 exit  then
                in pos line-end to le
                le 1 < if  le exit  then
                pos le + to pos
                le 2 = if  pos exit  then
            again
        then
        size b b-length + limit > if  -413 exit  then
        n pos - size 2 + < if  0 exit  then
        pos size + in nth 13 <>  pos size + 1+ in nth 10 <>  or if  -400 exit  then
        in  pos  pos size +  b b-append-seq
        pos size + 2 + to pos  pos at !   \ (this chunk in b: from here next time)
    again ;

<PRIVATE
\ ---- reading a request ----
\ the request line and headers (a n: up to the blank line) kept in r; false if they are no request's
\ the CR, LF or NUL at i of a n is where one may be: a CR before an LF, an LF after a CR, no NUL
: clean-head? ( a n -- flag )  crlf-lines? ;   \ no NUL, no CR but before LF, no LF but after CR: lines a proxy
                                              \ reads the same
: parse-head ( in h req -- flag )   \ (the head: in's first h bytes - text, as `in h`)
    {: in h r | e parts t q :}
    in h clean-head? 0= if  false exit  then
    in h line-end-in to e
    in e s"  " s-split to parts
    parts length 3 <> if  false exit  then
    r 0 parts nth >>method drop
    2 parts nth  r over >>version drop  string s" HTTP/1." s-starts? 0= if  false exit  then
    1 parts nth to t
    t length 0= if  false exit  then
    0 t nth [char] / <> if  false exit  then
    [char] ? t index to q
    q 0< if  r t >>path drop
    else  r t q head >>path drop  r t q 1+ tail >>query drop  then
    in e h lines-after r headers>> parse-headers ;

\ what a body in chunks has given so far - kept at keep (two cells: the bytes, 0 none; how far into the body they
\ go) between the reads of its request as more of it comes (the connection actor's)
: chunks-done ( keep -- )  {: k :}  k @ ?dup if  dispose  then  0 k !  0 k cell+ ! ;
\ a request's body in chunks, after its head (at h of in): the request's whole length, 0 not yet, or -status - read
\ on from what keep has
: chunked-at ( in h req keep -- total | 0 | -status )
    {: in h r k | used :}
    s" content-length" r req-header if  2drop -400 exit  then   \ (both said: no telling which is meant)
    k @ 0= if  <bytes> rescue-actor k !  0 k cell+ !  then
    in h tail-slice http-body-max k @ k cell+ dechunk to used
    used 0< if  k chunks-done  used exit  then
    used 0= if  0 exit  then
    r k @ b>string drop >>body drop  k chunks-done
    h used + ;

\ the request at the start of in (what has come: a slice): its length if it is all there, 0 if not yet, or minus the
\ status of what is wrong (keep: where a body in chunks read so far is kept - chunked-at's)
: request-at ( in req keep -- used | 0 | -status )
    {: in r k | h cl n :}  in length to n
    in blank-line subseq-index to h
    h 0< if  n http-headers-max > if  -431  else  0  then  exit  then
    h http-headers-max > if  -431 exit  then
    in h r parse-head 0= if  -400 exit  then
    s" transfer-encoding" r req-header if   \ (chunked alone: another coding isn't read)
        s-lower s-trim s" chunked" s= 0= if  -501 exit  then
        in h blank-line-bytes + r k chunked-at exit
    then
    s" content-length" r req-header if
        digits>length 0= if  -400 exit  then  to cl
        cl 0< if  -400 exit  then
        cl http-body-max > if  -413 exit  then
    then
    h blank-line-bytes + cl + {: total :}
    n total < if  0 exit  then
    r  in  h blank-line-bytes +  total  subseq  >>body drop   \ (its own: what came is taken as the handler begins)
    total ;

\ HTTP/1.1 keeps the connection unless it says close; 1.0 closes it unless it says keep-alive
: keep-alive? ( req -- flag )
    {: r | said :}
    s" connection" r req-header if  s-lower drop to said  else  empty-string to said  then
    r req-version s" HTTP/1.1" s= if  said string s" close" s-find 0<
    else  said string s" keep-alive" s-find 0>=  then ;
PRIVATE>

\ ---- the actors ----
message http-take ( router conn -- )     \ a connection for an http-connection actor, and the routes it answers by
message http-routes ( router -- )        \ the routes for the listener's actor

state: http-conn-state
    cell field hc-router
    cell field hc-conn
    cell field hc-timer     \ the idle timer running, 0 none
    cell field hc-request   \ the request being answered, 0 none (kept by the actor: its last words see it)
    cell field hc-extra     \ a protocol's that took the connection over (http.websocket's record)
    cell field hc-continued \ the request being read has been told 100 Continue
    cell field hc-running   \ its request's handler is running (counted in http-in-flight)
    cell field hc-chunks    \ the body in chunks of the request coming, read so far (bytes), 0 none ...
    cell field hc-chunk-at  \ ... and how far into it that goes (chunked-at's keep: these two, in this order)
    cell field hc-pump      \ the timer to answer on once what waits to go has gone (0 none)
end-state

variable running            \ requests whose handlers are running, on this node
: http-in-flight ( -- n )  running @ ;   \ requests being answered on this node now

: stop-idle ( -- )  hc-timer @ ?dup if  cancel-timer  then  0 hc-timer ! ;

<PRIVATE
1 constant code-kept       \ next-request's: answered, the connection kept for the next
2 constant code-close      \ answered, the connection to be closed
3 constant code-taken      \ answered, the connection another protocol's now

\ a request said wrong: answered with its status, the connection closed after
: refuse ( status req -- )  {: status r :}  r false >>keep drop  status reason status r respond ;

s\" HTTP/1.1 100 Continue\r\n\r\n" >string drop constant continue-answer
\ a head read whose body hasn't all come, that asks to be told to send it (Expect: 100-continue): told, once
: go-on ( req -- )
    {: r :}
    hc-continued @  r req-method nip 0=  or if  exit  then   \ (told already; or its head isn't all there)
    r req-version s" HTTP/1.1" s= 0= if  exit  then
    s" expect" r req-header 0= if  exit  then
    s-lower s" 100-continue" s= if  continue-answer string hc-conn @ net-write  true hc-continued !  then ;

\ the connection's next request, if it is all there, answered: 0 not all there yet, or one of the codes above
: next-request ( conn -- code )
    {: c :}
    c net-input nip 0= if  0 exit  then   \ (nothing there: no request made to find that out - after every answer)
    [:  c blank-request rescue-actor fill-request {: r :}  r hc-request !
        c net-view r hc-chunks request-at {: used :}
        used 0< if  used negate r refuse  code-close
        else used 0= if  r go-on  0
        else
            stop-idle  0 hc-continued !  used c net-consume
            r  r keep-alive? http-draining 0= and  >>keep drop
            1 running +!  true hc-running !
            r hc-router @ run-handler
            -1 running +!  false hc-running !
            r taken>> if  code-taken  else  r keep>> if  code-kept  else  code-close  then  then
        then then
    ;] with-scratch
    hc-request @ dispose  0 hc-request ! ;

s\" HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" >string drop constant failed-answer

\ the actor's last words: a request whose handler failed (or that was killed in the middle of one) answered 500
: last-words ( -- )
    stop-idle
    hc-running @ if  -1 running +!  false hc-running !  then   \ (a handler that failed: no longer running)
    hc-request @ ?dup if
        answered? 0= if  [: failed-answer string hc-conn @ net-write ;] catch drop  then
        [: hc-conn @ net-close ;] catch drop   \ (answered, then failed: what it sent goes out first - not dropped)
        hc-request @ dispose  0 hc-request !
    then ;

\ every request there answered in turn - while what waits to go isn't more than http-pending-max (a client that
\ asks and doesn't read: answered as it reads); then the connection closed, or the idle timer started
: answer-all ( -- )
    begin
        hc-conn @ net-gone? if  self kill  exit  then   \ (gone as its last request was answered: no more to ask)
        hc-conn @ net-pending http-pending-max > if   \ (looked at again soon: its answers going meanwhile)
            hc-pump @ 0= if  pump-ms after hc-pump !  then  exit
        then
        hc-conn @ next-request  dup code-kept =
    while  drop  repeat
    dup code-taken = if  drop exit  then
    code-close = if  hc-conn @ net-close  self kill  exit  then
    hc-timer @ 0= if  http-idle-ms after hc-timer !  then ;
PRIVATE>

behaviour http-connection
    on http-take ( router conn -- )
        hc-conn !  hc-router !
        http-headers-max http-body-max 2* +  chunk-line-max +  hc-conn @ net-input-max!   \ (one request's, at most)
        answer-all ;
    on net-received ( conn -- )  drop  answer-all ;
    on net-closed ( conn -- )  drop  self kill ;
    on timer-fired ( timer -- )
        dup hc-pump @ = if  drop  0 hc-pump !  answer-all exit  then
        hc-timer @ = if  0 hc-timer !  hc-conn @ net-close  self kill  then ;
    on killed ( -- )  last-words ;
    on start-over ( -- )  self kill ;   \ (its handler failed, the supervisor says so: a 500, and the connection gone)
end-behaviour

state: http-door-state
    cell field door-router
    cell field door-open     \ connections open (their actors watched: one gone, one less)
end-state
s\" HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" >string drop constant busy-answer

behaviour http-door
    on http-routes ( router -- )  door-router ! ;
    on net-accepted ( listener conn -- )
        nip {: c :}
        door-open @ http-connections-max >= if  busy-answer string c net-write  c net-close  exit  then
        http-connection http-conn-state s" http" spawn {: a :}
        a monitor  1 door-open +!
        door-router @ c a send http-take
        a c net-owner! ;
    on died ( id reason -- )  2drop  -1 door-open +! ;
end-behaviour

\ requests to the listener xt ( -- listener ) makes answered by the router's routes, each connection by an actor of
\ its own; the listener. (xt is run as the listener's actor: from the interpreter or inside an actor alike.)
: http-listen-with ( router xt -- listener )
    {: rt xt | door l :}
    supervise   \ (a handler's bug logged, its actor on: the server stays up)
    http-door http-door-state s" http-door" spawn to door
    door essential
    rt door send http-routes
    self 0< if  door enter  xt execute to l  leave-actor
    else  xt execute to l  then
    door l net-owner!
    l ;

\ requests to this port (0: a free one) answered by the router's routes; the listener (`listener-port` says its
\ port, `net-close` stops it). Then `serve`, or a loop that polls.
: http-listen ( router port -- listener )  {: rt port :}  rt [: port listen ;] http-listen-with ;

\ the server drained: each connection closed after the answer it is giving, and this node's loop run till no request
\ is being answered, or ms have gone by - a node's on-shutdown words (system's), after its listeners are closed:
\   [: listener net-close  10000 http-drain ;] on-shutdown
: http-drain ( ms -- )
    {: ms | t0 :}  true to http-draining  ms-now to t0
    begin  http-in-flight 0>  ms-now t0 - ms <  and  while
        round drop  10 net-poll drop   \ (a round: the tasks' turns too - what is in flight may wait on one)
    repeat
    round drop ;   \ (what the last wait told of: heard before the program goes on - a connection ended then is
                   \  let go at the next wait)

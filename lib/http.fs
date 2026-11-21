\ http.fs - an HTTP/1.1 server on the net vocabulary (docs/http.md). Each connection is an actor of its own, so a
\ handler may wait (`await` another actor's answer) without holding up the rest; each request is read, routed and
\ answered inside a `with-scratch`, so what it and its handler make is freed once it is answered.
\
\   <router> constant site
\   : home ( req -- )  {: r :}  s" hello" 200 r respond ;
\   s" /" ' home site on-get
\   site 8080 http-listen drop  serve
IN: http
USING: actors net accessors ;

65536 value http-headers-max     \ a request's line and headers, in bytes, at most: more is 431
1048576 value http-body-max      \ its body, at most: more is 413
30000 value http-idle-ms         \ a connection with no whole request in this long is closed
variable http-log                \ set: each request answered printed (its method, path and status)

32 constant space-char
4 constant blank-line-bytes      \ the CR LF CR LF that ends a request's head

\ ---- a request ----
\ method path query version body: strings (`string` gives one's bytes); headers: a map, names lower-cased; params:
\ a map of what the route's :names matched; out: the response's headers so far; status: 0 until it is answered
TUPLE: request  method path query version headers body params conn out status keep typed ;

: empty-string ( -- s )  s" " >string drop ;
: new-request ( conn -- req )
    {: c :}  empty-string empty-string empty-string empty-string <map> empty-string <map> c <bytes> 0 0 0 <request> ;

: req-method ( req -- a n )  method>> string ;
: req-path ( req -- a n )  path>> string ;      \ as sent: %-escapes and all
: req-query ( req -- a n )  query>> string ;    \ what came after the ?, as sent ("" for none)
: req-version ( req -- a n )  version>> string ;
: req-body ( req -- a n )  body>> string ;
: req-headers ( req -- m )  headers>> ;
: req-conn ( req -- conn )  conn>> ;
: head? ( req -- flag )  req-method s" HEAD" s= ;

\ a header the request has, by its name in any case
: req-header ( name n req -- a n true | false )
    {: a n r | k :}  a n s-lower drop to k
    k string r headers>> s-at  k dispose
    dup if  drop string true  then ;

\ what the route's :name matched (decoded); "*" what its * did
: req-param ( name n req -- a n true | false )
    params>> s-at  dup if  drop string true  then ;

\ ---- %-escapes ----
: hex-digit ( c -- n )   \ -1 for none
    {: c :}
    c [char] 0 [char] 9 1+ within if  c [char] 0 -  exit  then
    c 32 or {: l :}
    l [char] a [char] f 1+ within if  l [char] a - 10 +  exit  then
    -1 ;
\ a new string: each %HH the byte, and (plus? set) each + a space; a % with no two hex digits kept as it is
: (decode) ( a n plus? -- s len )
    {: a n plus? | b i c :}  <bytes> to b
    begin  i n <  while
        a i + c@ to c
        c [char] % =  i 2 + n <  and if
            a i + 1+ c@ hex-digit  a i + 2 + c@ hex-digit {: hi lo :}
            hi 0>= lo 0>= and if  hi 16 * lo +  b b-push  i 3 + to i
            else  c b b-push  i 1+ to i  then
        else
            c [char] + = plus? and if  space-char  else  c  then  b b-push  i 1+ to i
        then
    repeat
    b b>string  b dispose ;
: url-decode ( a n -- s len )  true (decode) ;   \ a query's: + is a space

\ the value of a name in the query string (decoded: a new string)
: query-param ( name n req -- s len true | false )
    {: a n r | pairs :}
    r req-query s" &" s-split to pairs
    pairs length 0 ?do
        i pairs nth string {: pa pn :}
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

\ ---- the response ----
: reason ( status -- a n )
    case
        200 of s" OK" endof                  201 of s" Created" endof
        202 of s" Accepted" endof            204 of s" No Content" endof
        301 of s" Moved Permanently" endof   302 of s" Found" endof
        303 of s" See Other" endof           304 of s" Not Modified" endof
        307 of s" Temporary Redirect" endof  308 of s" Permanent Redirect" endof
        400 of s" Bad Request" endof         401 of s" Unauthorized" endof
        403 of s" Forbidden" endof           404 of s" Not Found" endof
        405 of s" Method Not Allowed" endof  408 of s" Request Timeout" endof
        409 of s" Conflict" endof            410 of s" Gone" endof
        411 of s" Length Required" endof     413 of s" Content Too Large" endof
        414 of s" URI Too Long" endof        415 of s" Unsupported Media Type" endof
        422 of s" Unprocessable Content" endof  429 of s" Too Many Requests" endof
        431 of s" Request Header Fields Too Large" endof
        500 of s" Internal Server Error" endof  501 of s" Not Implemented" endof
        503 of s" Service Unavailable" endof    505 of s" HTTP Version Not Supported" endof
        s" " rot
    endcase ;

: answered? ( req -- flag )  status>> 0<> ;

\ a header of the response (before `respond`)
: header! ( value n name n req -- )
    {: v vn k kn r | out :}
    r answered? abort" header!: the request has been answered"
    r out>> to out
    k kn out b-append  s" : " out b-append  v vn out b-append  s\" \r\n" out b-append ;
: content-type! ( a n req -- )  {: r :}  s" Content-Type" r header!  r true >>typed drop ;

\ a formatted piece of the response's head, added to b
: b-format ( .. a n b -- )  {: b :}  format {: s n :}  s n b b-append  s dispose ;

\ the response: its status line, headers and body, sent (a HEAD request's without its body); once only
: respond ( a n status req -- )
    {: a n status r | b :}
    r answered? abort" respond: the request has been answered already"
    r typed>> 0= if  s" text/plain; charset=utf-8" r content-type!  then
    r keep>> 0= if  s" close" s" Connection" r header!  then
    <bytes> to b
    status  status reason  s\" HTTP/1.1 {} {s}\r\n" b b-format
    r out>> b-data b b-append
    n s\" Content-Length: {}\r\n\r\n" b b-format
    b b-data r conn>> net-write  b dispose
    r head? 0= if  a n r conn>> net-write  then
    r status >>status drop
    http-log @ if  r req-method type space r req-path type space status . cr  then ;

: respond-html ( a n status req -- )  {: r :}  s" text/html; charset=utf-8" r content-type!  r respond ;
: respond-json ( a n status req -- )  {: r :}  s" application/json" r content-type!  r respond ;
: redirect ( url n status req -- )  {: status r :}  s" Location" r header!  s" " status r respond ;
: not-found ( req -- )  {: r :}  404 reason 404 r respond ;

\ ---- routes ----
\ a route: its method ("*" any), its pattern's segments, its handler ( req -- )
TUPLE: http-route  verb segments action ;
TUPLE: http-router  routes fallback ;
: <router> ( -- router )  list 0 <http-router> ;

\ a path's segments (new strings, as sent), the empty ones left out: "/a//b/" is a, b
: segments ( a n -- l )
    {: a n | all made :}  a n s" /" s-split to all  list to made
    all [: dup string nip if  made push  else  dispose  then ;] each
    all list-free  made ;

\ requests with this method to paths of this pattern handled by xt ( req -- ). A segment of the pattern ":name"
\ matches any one segment (`req-param` gives it); a last "*" matches what is left, none or more
: route ( method n pattern n xt router -- )
    {: ma mn pa pn xt rt :}  ma mn >string drop  pa pn segments  xt <http-route>  rt routes>> push ;
: on-get ( pattern n xt router -- )  {: pa pn xt rt :}  s" GET" pa pn xt rt route ;
: on-post ( pattern n xt router -- )  {: pa pn xt rt :}  s" POST" pa pn xt rt route ;
: on-put ( pattern n xt router -- )  {: pa pn xt rt :}  s" PUT" pa pn xt rt route ;
: on-delete ( pattern n xt router -- )  {: pa pn xt rt :}  s" DELETE" pa pn xt rt route ;
: on-any ( pattern n xt router -- )  {: pa pn xt rt :}  s" *" pa pn xt rt route ;
: on-missing ( xt router -- )  swap >>fallback drop ;   \ what answers a path no route has (else a 404)

\ the segments from `from` on, joined by / and decoded
: rest-of ( segs from -- s len )
    {: segs from | b :}  <bytes> to b
    segs length from ?do  b b-length if  [char] / b b-push  then  i segs nth string b b-append  loop
    b b-data false (decode)  b dispose ;

\ the route's pattern fits the path's segments: what its :names matched kept in the request's params
: fits? ( segs route req -- flag )
    {: segs rt r | pat :}  rt segments>> to pat  r params>> map-clear
    pat length 0 ?do
        i pat nth string {: pa pn :}
        pa pn s" *" s=  i pat length 1- =  and if
            segs i rest-of drop  s" *" r params>> s-at!  true unloop exit
        then
        i segs length >= if  false unloop exit  then
        i segs nth string {: sa sn :}
        pa c@ [char] : = if
            sa sn false (decode) drop  pa 1+ pn 1- r params>> s-at!
        else
            pa pn sa sn s= 0= if  false unloop exit  then
        then
    loop
    pat length segs length = ;

: verb-fits? ( a n req -- flag )
    {: va vn r :}  va vn s" *" s=  va vn r req-method s= or  va vn s" GET" s= r head? and or ;

\ the request to the first route that fits it; one whose path fits, with no route for its method, is a 405
: dispatch ( req router -- )
    {: r rt | segs allow :}
    r req-path segments to segs  <bytes> to allow
    rt routes>> length 0 ?do
        i rt routes>> nth {: ro :}
        segs ro r fits? if
            ro verb>> string {: va vn :}
            va vn r verb-fits? if  r ro action>> execute  unloop exit  then
            allow b-length if  s" , " allow b-append  then  va vn allow b-append
        then
    loop
    allow b-length if  allow b-data s" Allow" r header!  405 reason 405 r respond  exit  then
    rt fallback>> ?dup if  r swap execute  exit  then
    r not-found ;

\ the handler run; one that gives no answer is answered 500 (the connection closed after). One that fails ends its
\ connection's actor - the error said, as any handler's - whose last words answer 500 (http-connection, below).
: run-handler ( req router -- )
    {: r rt :}
    r rt dispatch
    r answered? 0= if
        ." http: " r req-method type space r req-path type ." : no answer given" cr
        r false >>keep drop  500 reason 500 r respond
    then ;

\ ---- reading a request ----
\ the request line and headers (a n: up to the blank line) kept in r; false if they are no request's
: parse-head ( a n req -- flag )
    {: a n r | lines parts :}
    a n s\" \r\n" s-split to lines
    0 lines nth string s"  " s-split to parts
    parts length 3 <> if  false exit  then
    r 0 parts nth >>method drop
    2 parts nth  r over >>version drop  string s" HTTP/1." s-starts? 0= if  false exit  then
    1 parts nth string {: ta tn :}
    tn 0= if  false exit  then
    ta c@ [char] / <> if  false exit  then
    ta tn s" ?" s-find {: q :}
    q 0< if  r ta tn >string drop >>path drop
    else  r ta q >string drop >>path drop  r ta tn q 1+ tn q - 1- s-slice >string drop >>query drop  then
    lines length 1 ?do
        i lines nth string {: la ln :}
        la ln s" :" s-find {: colon :}
        colon 1 < if  false unloop exit  then
        la ln 0 colon s-slice s-lower drop {: k :}
        la ln colon 1+ ln colon - 1- s-slice s-trim >string drop {: v :}
        k string r headers>> s-at if  string s" , " s+ v string s+ drop to v  then   \ (said twice: both, joined)
        v k string r headers>> s-at!
    loop
    true ;

\ the request at the start of a n: its length if it is all there, 0 if not yet, or minus the status of what is wrong
: request-at ( a n req -- used | 0 | -status )
    {: a n r | h cl :}
    a n s\" \r\n\r\n" s-find to h
    h 0< if  n http-headers-max > if  -431  else  0  then  exit  then
    h http-headers-max > if  -431 exit  then
    a h r parse-head 0= if  -400 exit  then
    s" transfer-encoding" r req-header if  2drop -501 exit  then   \ (a body in chunks: not read)
    s" content-length" r req-header if
        string>number 0= if  drop -400 exit  then  to cl
        cl 0< if  -400 exit  then
        cl http-body-max > if  -413 exit  then
    then
    h blank-line-bytes + cl + {: total :}
    n total < if  0 exit  then
    r a h blank-line-bytes + + cl >string drop >>body drop
    total ;

\ HTTP/1.1 keeps the connection unless it says close; 1.0 closes it unless it says keep-alive
: keep-alive? ( req -- flag )
    {: r | said :}
    s" connection" r req-header if  s-lower drop to said  else  empty-string to said  then
    r req-version s" HTTP/1.1" s= if  said string s" close" s-find 0<
    else  said string s" keep-alive" s-find 0>=  then ;

\ ---- the actors ----
message http-take ( router conn -- )     \ a connection for an http-connection actor, and the routes it answers by
message http-routes ( router -- )        \ the routes for the listener's actor

state: http-conn-state
    cell field hc-router
    cell field hc-conn
    cell field hc-timer     \ the idle timer running, 0 none
    cell field hc-request   \ the request being answered, 0 none (kept out of its scratch region: its last words see it)
end-state

: stop-idle ( -- )  hc-timer @ ?dup if  cancel-timer  then  0 hc-timer ! ;

\ a request said wrong: answered with its status, the connection closed after
: refuse ( status req -- )  {: status r :}  r false >>keep drop  status reason status r respond ;

\ the connection's next request, if it is all there, answered: 0 not all there yet; 1 answered, the connection kept;
\ 2 answered, the connection to be closed
: next-request ( conn -- code )
    {: c :}
    [:  c new-request rescue {: r :}  r hc-request !
        c net-input r request-at {: used :}
        used 0< if  used negate r refuse  2
        else used 0= if  0
        else
            stop-idle  used c net-consume
            r r keep-alive? >>keep drop
            r hc-router @ run-handler
            r keep>> if  1  else  2  then
        then then
    ;] with-scratch
    hc-request @ dispose  0 hc-request ! ;

s\" HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" >string drop constant failed-answer

\ the actor's last words: a request whose handler failed (or that was killed in the middle of one) answered 500
: last-words ( -- )
    stop-idle
    hc-request @ ?dup if
        answered? 0= if  [: failed-answer string hc-conn @ net-write  hc-conn @ net-close ;] catch drop  then
        hc-request @ dispose  0 hc-request !
    then ;

\ every request there answered in turn; then the connection closed, or the idle timer started
: answer-all ( -- )
    begin  hc-conn @ next-request  dup 1 =  while  drop  repeat
    2 = if  hc-conn @ net-close  self kill  exit  then
    hc-timer @ 0= if  http-idle-ms after hc-timer !  then ;

behaviour http-connection
    on http-take ( router conn -- )  hc-conn !  hc-router !  answer-all ;
    on net-received ( conn -- )  drop  answer-all ;
    on net-closed ( conn -- )  drop  self kill ;
    on timer-fired ( timer -- )  hc-timer @ = if  0 hc-timer !  hc-conn @ net-close  self kill  then ;
    on killed ( -- )  last-words ;
end-behaviour

state: http-door-state
    cell field door-router
end-state

behaviour http-door
    on http-routes ( router -- )  door-router ! ;
    on net-accepted ( listener conn -- )
        nip {: c :}
        http-connection http-conn-state s" http" spawn {: a :}
        door-router @ c a send http-take
        a c net-owner! ;
end-behaviour

\ requests to this port (0: a free one) answered by the router's routes, each connection by an actor of its own;
\ the listener (`listener-port` says its port, `net-close` stops it). Then `serve`, or a loop that polls.
: http-listen ( router port -- listener )
    {: rt port | door l :}
    http-door http-door-state s" http-door" spawn to door
    rt door send http-routes
    self 0< if  door enter  port listen to l  leave-actor
    else  port listen to l  then
    door l net-owner!
    l ;

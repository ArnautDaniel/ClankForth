\ client.fs - an HTTP/1.1 client, as Erlang's httpc: each request a connection on an actor of its own, the asker
\ awaiting its response (its other messages kept meanwhile). Asked from inside an actor (a handler, or `enter`ed):
\
\   s" http://localhost:8080/users/7" http-get {: res :}
\   res res-status .  res res-body type
\
\ A request that gets no response (no connection, a response said wrong, http-timeout-ms gone by) has status 0,
\ `res-error` saying why - not an error thrown, as Erlang gives {error, Reason}.
\ https:// in a build with TLS (net's tls-connect: the server's certificate checked). A redirect is a response
\ like another (its Location: res-header); the connection is closed after each.
IN: http.client
USING: actors net http accessors ;

30000 value http-timeout-ms          \ a request with no whole response in this long fails
16777216 value http-response-max     \ a response's head and body, in bytes, at most: more fails it

\ ---- a response ----
\ status: its number (0: no response); headers: a map, names lower-cased; body: a string; error: why there was no
\ response (a string), or 0
TUPLE: response  status headers body error ;

: res-status ( res -- n )  status>> ;
: res-headers ( res -- m )  headers>> ;
: res-body ( res -- a n )  body>> string ;
: res-header ( name n res -- a n true | false )  headers>> header-of ;   \ by its name in any case
: res-error ( res -- a n )  error>> ?dup if  string  else  s" "  then ;    \ why there was no response ("" if there was)

message http-start ( request head? conn asker -- )   \ to a request's actor: these bytes, on this connection
message http-answer ( res -- )                       \ to the asker: its response (or what went wrong)

<PRIVATE
80 constant http-port   443 constant https-port   65535 constant port-max
100 constant informational    \ 1xx: an answer before the answer (103 Early Hints ...), passed over
200 constant successful   101 constant switching   \ (2xx on; 101 is a protocol switched: an answer)
204 constant no-content   304 constant not-modified

\ ---- a URL's parts ----
task-variable url-tls   task-variable url-port   \ (each handler its own: url-parts sets them, connect-url reads them)
task-variable url-host   \ (a string, in the handler's region)
\ how many bytes of a scheme's: 8 for https:// (url-tls set), 7 for http://, 0 for another
: scheme ( a n -- skip )
    8 min s-lower {: la ln :}
    la ln s" https://" s-starts? if  true url-tls !  https-port url-port !  8 exit  then
    la ln s" http://" s-starts? if  false url-tls !  http-port url-port !  7 exit  then
    0 ;
\ where the authority ends: at the first / or ?, or the end
: authority-end ( a n -- i )
    {: a n :}  n 0 ?do  a i + c@ {: c :}  c [char] / =  c [char] ? = or if  i unloop exit  then  loop  n ;
\ where the host ends and the port begins (its : at), -1 for no port; false if it is said wrong. An IPv6
\ address's in brackets: its brackets off.
: host-at ( a n -- host n colon true | false )
    {: a n | close :}
    a c@ [char] [ <> if  a n s" :" s-find {: colon :}  a  colon 0< if  n  else  colon  then  colon true exit  then
    a n s" ]" s-find to close
    close 0< if  false exit  then
    close 1+ n = if  a 1+ close 1- -1 true exit  then
    a close 1+ + c@ [char] : <> if  false exit  then
    a 1+ close 1-  close 1+  true ;

\ no byte a request line or a header can't hold as it is - a space, a control byte (CR, LF), DEL: a URL with them
\ (one a user's input made) would end the line early and write headers, or a second request, of its own
: plain-url? ( a n -- flag )  {: a n :}  n 0 ?do  a i + c@ {: c :}  c 32 <=  c 127 = or if  false unloop exit  then  loop  true ;
: token-method? ( a n -- flag )   \ GET, POST ...: letters only
    {: a n :}  n 0= if  false exit  then
    n 0 ?do  a i + c@ {: c :}  c [char] A [char] Z 1+ within 0= if  false unloop exit  then  loop  true ;
: header-lines? ( a n -- flag )   \ the caller's headers: CR only before LF, LF only after CR, no 0 byte
    {: a n :}
    n 0 ?do  a i + c@ {: c :}
        c 0= if  false unloop exit  then
        c 13 = if  i 1+ n <  a i + 1+ c@ 10 = and 0= if  false unloop exit  then  then
        c 10 = if  i 0>  a i + 1- c@ 13 = and 0= if  false unloop exit  then  then
    loop  true ;

\ the authority (host and port: what Host says) and the target (path and query) of a URL, as slices of it; its host
\ and port in the variables above, url-tls set for https. False if it is no http or https URL.
: url-parts ( a n -- authority n target n true | false )
    {: a n | rest rn an :}
    a n plain-url? 0= if  false exit  then
    a n s" #" s-find {: hash :}  hash 0>= if  hash to n  then   \ (its fragment the browser's: not sent)
    a n scheme {: skip :}
    skip 0= if  false exit  then
    a skip +  n skip -  to rn to rest
    rest rn authority-end to an
    an 0= if  false exit  then
    rest an s" @" s-find 0>= if  false exit  then   \ (a user and password in a URL: not taken)
    rest an host-at 0= if  false exit  then  {: ha hn colon :}
    hn 0=  hn 255 >  or if  false exit  then
    ha hn >string drop url-host !
    colon 0>= if
        rest an colon 1+ an colon - 1- s-slice string>number 0= if  drop false exit  then
        dup 1 port-max 1+ within 0= if  drop false exit  then
        url-port !
    then
    rest an  rest an +  rn an -  true ;

\ the request's bytes: its line, Host, the caller's headers (each ending CR LF), the body's length, the body
: request-bytes ( method n authority n target n headers n body n -- b )
    {: ma mn aa an ta tn ha hn ba bn | b :}  <bytes> to b
    ma mn b b-append  s"  " b b-append
    tn 0= if  s" /"  else  ta c@ [char] ? = if  s" /" b b-append  then  ta tn  then  b b-append
    s\"  HTTP/1.1\r\nHost: " b b-append  aa an b b-append
    clankforth-version s\" \r\nUser-Agent: clankforth/{s}\r\nConnection: close\r\n" format {: s sn :}
    s sn b b-append  s dispose
    ha hn b b-append
    bn 0>  ma mn s" POST" s= or  ma mn s" PUT" s= or  ma mn s" PATCH" s= or if
        bn s\" Content-Length: {}\r\n" format {: c cn :}  c cn b b-append  c dispose
    then
    s\" \r\n" b b-append  ba bn b b-append
    b ;

\ ---- the request's actor: reads the response, tells the asker ----
state: http-client-state
    cell field cl-conn
    cell field cl-asker
    cell field cl-request   \ the bytes to send once connected
    cell field cl-head?     \ a HEAD request: its response has no body
    cell field cl-timer
    cell field cl-told      \ the asker has been told
    cell field cl-chunks    \ a body in chunks read so far (bytes, 0 none) ...
    cell field cl-chunk-at  \ ... and how far into it that goes (read on from as more comes: dechunk's)
end-state

: answer ( res -- )   \ the asker given the response, once (none if it never asked: killed before)
    cl-told @  cl-request @ 0=  or if  drop exit  then  true cl-told !  cl-asker @ send http-answer ;
: failed ( a n -- res )  >string drop {: e :}  0 <map> empty-string e <response> ;
: tell ( res -- )   \ the asker given the response; the connection closed, this actor gone
    answer
    cl-timer @ ?dup if  cancel-timer  0 cl-timer !  then
    cl-conn @ ?dup if  net-close  0 cl-conn !  then
    self kill ;
: fail ( a n -- )  failed tell ;   \ the asker told what went wrong

: body-ends-at-close? ( res -- flag )   \ neither a length nor chunks: the body is what comes till the close
    {: res :}  s" content-length" res res-header if  2drop false exit  then
    s" transfer-encoding" res res-header if  2drop false exit  then
    true ;
: no-body? ( res -- flag )
    res-status {: st :}  cl-head? @  st no-content = or  st not-modified = or ;

\ the status line and headers (a n: up to the blank line) as a new response; false if they are no response's
: parse-status ( a n -- res true | false )
    {: a n | lines :}
    a n s\" \r\n" s-split to lines
    0 lines nth string {: la ln :}
    ln 12 <  la ln s" HTTP/1." s-starts? 0= or if  false exit  then
    la ln 9 3 s-slice string>number 0= if  drop false exit  then {: st :}
    st 100 1000 within 0= if  false exit  then
    st <map> empty-string 0 <response> {: res :}
    lines res headers>> parse-headers 0= if  false exit  then
    res true ;

\ the response at the start of a n, if it is all there (closed?: nothing more comes): the response, its length;
\ 0 not yet; or -1 and what is wrong told
16 constant informational-max   \ answers before the answer (100 Continue, 103 Early Hints ...) passed over, at most
: answer-at ( a n closed? skipped -- res used | 0 | -1 )
    {: a n closed? skipped | h res b used :}
    skipped informational-max > if  s" http: too many answers before the answer (1xx)" fail -1 exit  then
    a n s\" \r\n\r\n" s-find to h
    h 0< if  n http-response-max > if  s" http: a response's head too long" fail -1  else  0  then  exit  then
    a h parse-status 0= if  s" http: not an HTTP response" fail -1 exit  then  to res
    h blank-line-bytes + to h
    res res-status dup informational successful within  swap switching <> and if   \ (an answer before the answer: passed over)
        a h + n h - closed? skipped 1+ recurse  dup 0> if  h +  then  exit
    then
    res no-body? if  res h exit  then
    s" transfer-encoding" res res-header if
        s-lower s" chunked" s-ends? 0= if  s" http: a body in a coding not read" fail -1 exit  then
        cl-chunks @ 0= if  <bytes> rescue-actor cl-chunks !  0 cl-chunk-at !  then
        cl-chunks @ to b
        a h + n h - http-response-max b cl-chunk-at dechunk to used
        used 0< if  s" http: a body in chunks said wrong, or too long" fail -1 exit  then
        used 0= if  0 exit  then
        res b b>string drop >>body drop
        res h used + exit
    then
    s" content-length" res res-header if
        digits>length 0= if  s" http: a Content-Length not a number" fail -1 exit  then {: cl :}   \ (decimal digits only)
        cl 0<  cl http-response-max > or if  s" http: a body too long" fail -1 exit  then
        n h - cl < if  0 exit  then
        res a h + cl >string drop >>body drop
        res h cl + exit
    then
    closed? 0= if  n h - http-response-max > if  s" http: a body too long" fail -1  else  0  then  exit  then
    res a h + n h - >string drop >>body drop
    res n ;

: response-at ( a n closed? -- res used | 0 | -1 )  0 answer-at ;
: try-response ( closed? -- )   \ the response, if it is all there, given the asker
    {: closed? :}
    cl-conn @ net-input closed? response-at
    dup 0< if  drop exit  then
    dup 0= if  drop  closed? if  s" http: the connection closed before a whole response"  fail  then  exit  then
    drop  tell ;
PRIVATE>

behaviour http-client
    on http-start ( request head? conn asker -- )
        cl-asker !  cl-conn !  cl-head? !  rescue cl-request !
        http-timeout-ms after cl-timer ! ;
    on net-connected ( conn -- )  cl-request @ string rot net-write ;
    on net-received ( conn -- )  drop  false try-response ;
    on net-closed ( conn -- )  drop  true try-response ;
    on timer-fired ( timer -- )  drop  0 cl-timer !  s" http: no response in http-timeout-ms" fail ;
    on killed ( -- )  s" http: the request's actor failed" failed answer ;
end-behaviour

<PRIVATE
\ a connection to the URL's host (TLS for https: a build without it can't)
: connect-url ( -- conn )
    url-host @ string  url-port @
    url-tls @ if
        s" net:tls-connect" find-word ?dup 0= if  true s" http: https needs a build with TLS" (abort")  then
        execute exit
    then
    connect ;
PRIVATE>

\ a request: its method, URL, more headers (each line ending CR LF: s\" Accept: text/html\r\n"; "" none) and body
\ ("" none); its response, once it has all come (this actor awaiting it, its other messages kept) - status 0 and
\ `res-error` if none came. Inside a handler (an actor's: one that can wait).
: http-request ( method n url n headers n body n -- res )
    {: ma mn ua un ha hn ba bn | b a :}
    self 0< abort" http-request: inside an actor (its response is a message)"
    ma mn token-method? 0= if  s" http: a method of letters only" failed exit  then
    ha hn header-lines? 0= if  s" http: headers with a CR or LF not ending a line" failed exit  then
    ua un url-parts 0= if  ua un s" http: not an http:// or https:// URL (no spaces or control bytes): {s}" format failed exit  then
    {: aa an ta tn :}
    ma mn aa an ta tn ha hn ba bn request-bytes to b
    http-client http-client-state s" http-client" spawn to a
    ['] connect-url catch if  a kill  b dispose  error-message failed exit  then {: c :}
    b b>string drop  ma mn s" HEAD" s=  c  self  a send http-start
    b dispose
    a c net-owner!
    await http-answer ;

: http-get ( url n -- res )  {: ua un :}  s" GET" ua un s" " s" " http-request ;
\ a POST of the body, of that content type
: http-post ( body n type n url n -- res )
    {: ba bn ta tn ua un :}
    ta tn s\" Content-Type: {s}\r\n" format {: h hn :}
    s" POST" ua un h hn ba bn http-request ;

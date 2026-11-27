\ ops.fs - what operating a server wants of it: health and readiness for a load balancer or an orchestrator, numbers
\ to watch, and a JSON line for each request with an id that follows it (X-Request-Id).
\
\   site ops-routes                \ GET /health, /ready, /metrics
\   site request-log               \ request ids, and a JSON line to stdout for each request
\   [: db-reachable? ;] ready-check   \ /ready answers 503 while one says false (and while draining)
\
\ The numbers are this node's (each node has its own: a request is answered by the node that took it).
IN: http.ops
USING: actors http json logging system crypto.tokens ;

<PRIVATE
variable requests      \ answered since the start
variable failures      \ ... with a 5xx
variable request-us    \ the time they took, together (microseconds)
variable slowest-us
time-now constant started
list constant ready-checks   \ xts ( -- ok? )
list constant extra-metrics  \ name and xt ( -- n ) pairs
s" request-started" >string drop constant started-key
s" request-id" >string drop constant id-key

: all-ready? ( -- flag )  http-draining 0=  ready-checks [: execute ;] all? and ;
: health ( req -- )  {: r :}  s\" {\"status\":\"ok\"}" 200 r respond-json ;
: ready ( req -- )
    {: r :}  all-ready? if  s\" {\"status\":\"ready\"}" 200  else  s\" {\"status\":\"not ready\"}" 503  then  r respond-json ;
: metrics ( req -- )
    {: r | m :}  <map> to m
    node s" node" m s-at!  time-now started - s" uptime_s" m s-at!
    actors-alive s" actors" m s-at!  objects s" objects" m s-at!
    requests @ s" requests" m s-at!  failures @ s" errors_5xx" m s-at!
    requests @ if  request-us @ requests @ / 1000 /  else  0  then  s" latency_ms_avg" m s-at!
    slowest-us @ 1000 / s" latency_ms_max" m s-at!
    extra-metrics length 0 ?do  i 1+ extra-metrics nth execute  i extra-metrics nth string m s-at!  2 +loop
    m >json 200 r respond-json ;

: id-ok? ( a n -- flag )   \ a client's request id taken as it is: 1 to 64 of letters, digits, - _ . (else one made)
    {: a n :}  n 1 65 within 0= if  false exit  then
    true  n 0 ?do  a i + c@ {: c :}
        c [char] a [char] z 1+ within  c [char] A [char] Z 1+ within or  c [char] 0 [char] 9 1+ within or
        c [char] - = or  c [char] _ = or  c [char] . = or  and
    loop ;
: start-request ( req x -- true )   \ (a before: its time, its id - taken from the client's X-Request-Id, else made)
    drop {: r :}
    us-now started-key string r req-attr!
    s" x-request-id" r req-header if  2dup id-ok? if  >string drop  else  2drop  8 random-hex drop  then
    else  8 random-hex drop  then {: id :}
    id id-key string r req-attr!
    id string s" X-Request-Id" r header!
    true ;
: end-request ( req x -- )   \ (an after: counted, a JSON line)
    drop {: r | us m :}
    started-key string r req-attr if  us-now swap -  else  0  then to us
    1 requests +!  us request-us +!  [: us slowest-us @ max slowest-us ! ;] atomically   \ (read and written: atomically)
    r req-status 500 >= if  1 failures +!  then
    <map> to m
    id-key string r req-attr if  s" request_id" m s-at!  then
    r req-method >string drop s" method" m s-at!  r req-path >string drop s" path" m s-at!
    r req-status s" status" m s-at!  us 1000 / s" ms" m s-at!
    r client-address >string drop s" client" m s-at!
    s" info" s" request" m log-event ;
PRIVATE>

\ GET /health (alive: 200), /ready (200 - or 503 while draining or a ready-check says false), /metrics (this node's
\ numbers as JSON: requests, 5xx, latency, actors, objects, uptime, and those metric added)
: ops-routes ( router -- )
    {: rt :}  s" /health" ['] health rt on-get  s" /ready" ['] ready rt on-get  s" /metrics" ['] metrics rt on-get ;
\ each request given an id (its X-Request-Id, else a new one - said back in X-Request-Id) and timed; a JSON line to
\ stdout once it is answered: {"time", "level": "info", "event": "request", "request_id", "method", "path",
\ "status", "ms", "client"}. The first before added: it times the rest.
: request-log ( router -- )  {: rt :}  ['] start-request 0 rt before-each-with  ['] end-request 0 rt after-each-with ;
\ /ready answers 503 while xt ( -- ok? ) says false (the database not there ...); added from the program's top
: ready-check ( xt -- )  in-handler ready-checks push ;   \ (run in a request's handler: checked as one)
\ a number /metrics says: xt ( -- n ), under that name (a pool's connections ...)
: metric ( xt name n -- )  {: xt a n :}  a n >string drop extra-metrics push  xt in-handler extra-metrics push ;
\ the id of the request being answered (request-log's), or ""
: request-id ( req -- a n )  {: r :}  id-key string r req-attr if  string  else  s" "  then ;

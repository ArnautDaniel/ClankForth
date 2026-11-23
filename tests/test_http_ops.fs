\ http.ops (basis/http/ops): /health, /ready (a check, draining), /metrics; request ids and their JSON lines
IN: test-http-ops
USING: tester actors net http http.ops json vocabs.webclient ;

<router> constant site
: hello ( req -- )  {: r :}  s" hello" 200 r respond ;
s" /hello" ' hello site on-get
site request-log
site ops-routes
variable ready?  true ready? !
[: ready? @ ;] ready-check
[: 42 ;] s" answer" metric
site 0 http-listen listener-port web-port !

testing /health, /ready (503 while a check says false)
T{ s" /health" get  2dup status-of -rot body-of s\" {\"status\":\"ok\"}" s= -> 200 true }T
T{ s" /ready" get status-of -> 200 }T
T{ false ready? !  s" /ready" get status-of  true ready? ! -> 503 }T
testing a request's id: its own, given back; else one made (16 hex digits)
T{ s" /hello" s\" X-Request-Id: abc-123\r\n" get-with  s" X-Request-Id: abc-123" has? -> true }T
: made-id ( -- n )   \ the id's length in the answer's X-Request-Id
    s" /hello" get {: a n :}  a n s" X-Request-Id: " s-find 14 + {: at :}  a n at n at - s-slice s\" \r\n" s-find ;
T{ made-id -> 16 }T
testing /metrics: the requests counted, a metric of the program's
: metrics-json ( -- v )  s" /metrics" get body-of json> ;
T{ s" requests" metrics-json json-int@ drop 4 >= -> true }T
T{ s" answer" metrics-json json-int@ -> 42 true }T
T{ s" errors_5xx" metrics-json json-int@ -> 1 true }T   \ (the 503 of /ready)

test-summary

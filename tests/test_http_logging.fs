\ http.logging (basis/http/logging): a line for each request a router answers - who, what, the status, how long
IN: test-http-logging
USING: tester actors net system http logging http.logging vocabs.webclient ;

: wipe ( a n -- )  {: a n :}  a n file-exists? if  a n delete-file  then ;
: logs ( -- a n )  s" clankforth-test-access.log" ;
logs wipe
logs log-open constant access
<router> constant site
: home ( req -- )  {: r :}  s" home" 200 r respond ;
access site access-log
s" /" ' home site on-get
site 0 http-listen listener-port web-port !

testing each request answered: the client's address, the method, the path, the status, its time
s" /" get 2drop  s" /missing" get 2drop
: logged? ( a n -- flag )  {: a n :}  logs read-file {: s sn :}  s sn a n s-find 0>=  s dispose ;
: written ( -- )   \ (the log's writes are done on a worker: waited for, not a fixed few rounds)
    2000 0 do  10 polls  s" 127.0.0.1 GET /missing 404 " logged? if  leave  then  1 sleep-ms  loop ;
written
T{ s" 127.0.0.1 GET / 200 " logged?  s" 127.0.0.1 GET /missing 404 " logged?  s" ms" logged? -> true true true }T
access kill  logs wipe

test-summary

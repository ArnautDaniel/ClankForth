\ The web benchmark's routes (bench/http/run.sh): plain text, a route's parameter, a small JSON object - the same
\ three bench/http/server.js answers with Node's http and Bun's serve. Loaded by every node, each listening on the
\ one port.
IN: site.routes
USING: actors net system http json ;

: port ( -- n )   \ BENCH_PORT, or 8101
    {: | s n :}  s" BENCH_PORT" getenv 0= if  8101 exit  then  to n to s  s n string>number drop  s dispose ;
: plain ( req -- )  {: r :}  s" Hello, World!" 200 r respond ;
: hello ( req -- )  {: r :}  s" name" r req-param drop  s" Hello, {s}!" format  200 r respond ;
: as-json ( req -- )
    {: r | m :}  <map> to m  s" Hello, World!" >string drop s" message" m s-at!  42 s" id" m s-at!  json-true s" ok" m s-at!
    m >json 200 r respond-json ;

<router> constant site
s" /plain" ' plain site on-get
s" /hello/:name" ' hello site on-get
s" /json" ' as-json site on-get
site port http-listen drop

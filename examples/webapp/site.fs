\ The web app's routes (examples/web-app.fs runs it): loaded by every node, each listening on the one port - the
\ system spreads the connections over them. A handler is its connection's actor's: it may ask another actor, on this
\ node or another, and `await` the answer, holding up nobody else.
IN: webapp.site
USING: actors net system http ;

message visit ( -- )             \ to the "visits" actor (web-app.fs's, on the first node): one more
message visits-now ( n -- )      \ its answer: how many so far

: arg ( i default -- n )   \ the program's argument i as a number, or the default
    {: i default | l n :}  args to l  default to n
    i l length < if  i l nth string string>number if  to n  else  drop  then  then
    l [: dispose ;] each  l list-free  n ;
: site-port ( -- n )  0 8080 arg ;

: home ( req -- )   \ asks the visits actor (a message to the first node, its answer awaited)
    {: r :}  s" visits" whereis send visit  await visits-now {: n :}
    n node s" <h1>Hello from ClankForth</h1><p>visit {}, answered by node {}</p>" format  200 r respond-html ;
: hello ( req -- )  {: r :}  s" name" r req-param drop  s" Hello, {s}!" format  200 r respond ;
: echo ( req -- )  {: r :}  r req-body 200 r respond ;
: stats ( req -- )  {: r :}  node actors-alive s\" {{\"node\": {}, \"actors\": {}}}" format  200 r respond-json ;

<router> constant site
s" /" ' home site on-get
s" /hello/:name" ' hello site on-get
s" /echo" ' echo site on-post
s" /stats" ' stats site on-get

site site-port http-listen drop

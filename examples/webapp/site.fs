\ The web app's routes (examples/web-app.fs runs it): loaded by every node, each listening on the one port - the
\ system spreads the connections over them. A handler is its connection's actor's: it may ask another actor, on this
\ node or another, and `await` the answer, holding up nobody else.
IN: webapp.site
USING: actors net system http http.files http.sse http.websocket json ;

message visit ( -- )             \ to the "visits" actor (web-app.fs's, on the first node): one more
message visits-now ( n -- )      \ its answer: how many so far

: arg ( i default -- n )   \ the program's argument i as a number, or the default
    {: i default | l n :}  args to l  default to n
    i l length < if  i l nth string string>number if  to n  else  drop  then  then
    l [: dispose ;] each  l list-free  n ;
: site-port ( -- n )  0 8080 arg ;

\ the visits actor asked (a message to the first node, its answer awaited); the answer as JSON
: visits ( req -- )
    {: r | m :}  s" visits" whereis send visit  await visits-now {: n :}
    <map> to m  n s" visits" m s-at!  node s" node" m s-at!
    m >json 200 r respond-json ;
: hello ( req -- )  {: r :}  s" name" r req-param drop  s" Hello, {s}!" format  200 r respond ;
: echo ( req -- )  {: r :}  r req-body 200 r respond ;

\ the clock, as events: one a second till the browser goes (the handler sleeps between, holding up nobody)
: clock ( req -- )
    {: r :}  r respond-events
    begin  time-now s" {}" format  s" tick" r send-event  1000 sleep-ms  r req-gone? until ;

\ the chat: every socket's owner the room (web-app.fs's actor on the first node), whichever node took it
: to-room ( req -- actor )  drop  s" room" whereis ;

<router> constant site
s" /visits" ' visits site on-get
s" /hello/:name" ' hello site on-get
s" /echo" ' echo site on-post
s" /clock" ' clock site on-get
s" /chat" ' to-room site on-websocket
source-dir s" /public" s+ s" " site serve-files   \ (the page, its script: beside this file)

site site-port http-listen drop

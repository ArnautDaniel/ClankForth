\ http.sse (basis/http/sse): an answer that stays open, events sent down it as an actor hears them
IN: test-http-sse
USING: tester actors net http http.sse vocabs.webclient ;

message headline ( n -- )
message subscribe-news ( -- )
variable reader   \ the handler's actor, once it subscribes
behaviour news-desk
    on subscribe-news ( -- )  sender reader ! ;
end-behaviour
news-desk -1 s" desk" spawn constant desk

\ the handler: subscribed, then each headline an event, till the third (or the client goes)
: news ( req -- )
    {: r :}  r respond-events  desk send subscribe-news
    3 0 do  await headline {: n :}  n s\" story {}\nmore" format  s" headline" r send-event  r req-gone? if  leave  then  loop
    r send-ping ;
<router> constant site
s" /news" ' news site on-get
site 0 http-listen listener-port web-port !

: tell ( n -- )  reader @ send headline ;
testing the answer begun: text/event-stream, held open while the handler waits
s\" GET /news HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n" open-with  20 polls
T{ reply b-data s" Content-Type: text/event-stream" has?  closed @ -> true 0 }T
testing each headline an event: its name, each line of its data a data: line
T{ 1 tell  20 polls  reply b-data s\" event: headline\ndata: story 1\ndata: more\n\n" has? -> true }T
T{ 2 tell  3 tell  500 polls  reply b-data s" story 3" has?  closed @ -> true 1 }T
T{ reply b-data s\" : ping\n\n" has? -> true }T

test-summary

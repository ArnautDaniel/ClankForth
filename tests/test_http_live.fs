\ http.live (basis/http/live): a live view - its page rendered by a GET, its socket joined with its secret, events
\ answered with the regions that changed, pushes (live-tell, a database's change), what isn't its own refused
IN: test-http-live
USING: tester actors net system json jsondb http html http.live vocabs.webclient ;

: wipe ( dir n -- )
    {: a n :}  a n dir? 0= if  exit  then
    a n dir-list {: l :}  l length 0 ?do  a n s" /" s+  i l nth string s+  delete-file  loop  a n delete-file ;
s" jsondb-live-data" wipe
s" jsondb-live-data" db-open constant db

\ ---- a view: a count, a name, how many changes a collection it watches has had ----
: show-count ( -- )  <p  s" count" state json-int@ drop number  p> ;
: show-name ( -- )  <p  s" name" state json-string@ drop text  p> ;
: show-items ( -- )  <p  s" items" state json-int@ drop number  p> ;
: c-mount ( params -- )
    {: p :}  0 s" count" state!  0 s" items" state!
    s" name" p json-string@ 0= if  s" nobody"  then  s" name" state-string!
    s" items" db db-watch drop ;
: c-render ( -- )
    <h1 s" Counter" text h1>
    s" count" ['] show-count region  s" name" ['] show-name region  s" items" ['] show-items region ;
: inc ( v -- )  drop  s" count" state json-int@ drop 1+ s" count" state! ;
: away ( v -- )  drop  s" /elsewhere" live-redirect ;
: rename ( v -- )  {: v :}  s" to" v json-string@ if  s" name" state-string!  then ;
: c-info ( x -- )  s" count" state! ;
: tick ( v -- )  drop  20 after drop ;                     \ (a timer of the view's own)
: c-timer ( timer -- )  drop  s" count" state json-int@ drop 1+ s" count" state! ;
: c-changed ( c id rev json -- )  2drop 2drop  s" items" state json-int@ drop 1+ s" items" state! ;
live-view counter
    mount: c-mount
    render: c-render
    event: inc inc
    event: away away
    event: rename rename
    event: tick tick
    timer: c-timer
    info: c-info
    changed: c-changed
end-live-view

<router> constant site
s" /c" counter site live-route
site 0 http-listen listener-port web-port !

\ ---- the page: what its root says ----
create page-html 8192 allot   variable page-n
: get-page ( -- )  s" /c?name=ann" get body-of {: a n :}  a page-html n move  n page-n ! ;
: page-text ( -- a n )  page-html page-n @ ;
: attr-value ( name n -- a n )   \ name="..." in the page: its value
    {: na nn :}  page-text  na nn s\" {s}=\"" format s-find  nn + 2 + {: at :}
    page-html at +  page-n @ at -  {: va vn :}
    va  va vn s\" \"" s-find ;
get-page
: page-addr ( -- a n )  s" data-live-page" attr-value ;
: secret ( -- a n )  s" data-live-secret" attr-value ;

testing the GET: the whole page - its root (page, secret), its script, its regions as its mount left them
T{ page-text s\" id=\"live-root\"" s-find 0> -> true }T
T{ page-text s\" <script src=\"/live.js\" defer></script>" s-find 0> -> true }T
T{ page-text s\" <div id=\"count\"><p>0</p></div>" s-find 0> -> true }T
T{ page-text s\" <div id=\"name\"><p>ann</p></div>" s-find 0> -> true }T
T{ secret nip -> 43 }T
T{ s" /live.js" get  2dup status-of -rot s" application/javascript" has? -> 200 true }T

\ ---- a socket: its handshake, frames sent masked as a browser's, the server's read back ----
<bytes> constant out
: handshake ( origin n -- )   \ (from that Origin, to the page)
    {: oa on :}  out b-clear
    page-addr  web-port @  oa on
    s\" GET /live?p={s} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nOrigin: {s}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
    format {: h hn :}  h hn out b-append  h dispose ;
: our-origin ( -- a n )  web-port @ s" http://127.0.0.1:{}" format ;
: text-frame ( a n -- )   \ masked with 1 2 3 4
    {: a n :}
    $81 out b-push
    n 126 < if  n $80 or out b-push  else  126 $80 or out b-push  n 8 rshift out b-push  n 255 and out b-push  then
    1 out b-push  2 out b-push  3 out b-push  4 out b-push
    n 0 ?do  a i + c@  i 3 and 1+ xor  out b-push  loop ;
: say ( a n -- )  out b-clear  text-frame  out b-data client-conn @ net-write ;
: join-text ( -- a n )  secret s\" {{\"join\":\"{s}\"}}" format ;
\ what the server has sent since the 101: its text frames joined (the last ones looked at)
: heard ( -- a n )  reply b-data ;
: wait-reply ( -- )  100 0 do  10 net-poll drop  deliver  loop ;
: says? ( a n -- flag )  heard 2swap has? ;

testing a socket from this site's page: joined with its secret; an event - only the region that changed sent
T{ our-origin handshake  join-text text-frame  out b-data open-with  wait-reply  s" 101 Switching Protocols" says? -> true }T
T{ s\" {\"e\":\"inc\",\"v\":null}" say  wait-reply  s\" [\"count\",\"<p>1</p>\"]" says? -> true }T
T{ s\" \"name\"" says? -> false }T   \ (the name's region the same: not sent)
T{ s\" {\"e\":\"rename\",\"v\":{\"to\":\"<b>bo</b>\"}}" say  wait-reply  s\" [\"name\",\"<p>&lt;b&gt;bo&lt;/b&gt;</p>\"]" says? -> true }T
testing a redirect; an event of no name the view has (said, nothing sent)
T{ s\" {\"e\":\"away\",\"v\":null}" say  wait-reply  s\" \"redirect\":\"/elsewhere\"" says? -> true }T
T{ reply b-clear  s\" {\"e\":\"nothing\",\"v\":null}" say  wait-reply  s\" {\"r\":[]}" says? -> true }T   \ (an answer, nothing in it)
testing pushes: a message to the page (live-tell), a change to a collection it watches - each sends what changed
: the-page ( -- addr )  page-addr string>number drop ;
: page-alive? ( -- flag )  the-page $ffffffff and alive? ;   \ (alive? takes this node's id: the address's low bits)
T{ reply b-clear  41 the-page live-tell  wait-reply  s\" [\"count\",\"<p>41</p>\"]" says? -> true }T
T{ reply b-clear  s" {}" s" items" s" i1" db db-put 2drop  wait-reply  s\" [\"items\",\"<p>1</p>\"]" says? -> true }T
testing a timer the view started: its timer: word (not the page's wait for its join), then what changed sent
T{ reply b-clear  s\" {\"e\":\"tick\",\"v\":null}" say  wait-reply  s\" [\"count\",\"<p>42</p>\"]" says?  page-alive? -> true true }T
testing the socket closed: its page gone
T{ client-conn @ net-close  wait-reply  page-alive? -> false }T

testing another page's socket with the wrong secret: closed (the page goes on); a socket from another site refused
get-page
: wrong-join ( -- a n )  s\" {\"join\":\"not-the-secret-not-the-secret-not-the-se\"}" ;
our-origin handshake  wrong-join text-frame  out b-data open-with  wait-reply
T{ s\" \x88" says?  closed @ -> true 1 }T
T{ page-alive? -> true }T
T{ s" http://evil.example" handshake  out b-data open-with  wait-reply  s" 403 Forbidden" says? -> true }T
testing a page whose script never joins: gone after live-join-ms
100 to live-join-ms
get-page
: waits ( -- )  30 0 do  10 sleep-ms  10 net-poll drop  deliver  loop ;
T{ waits  page-alive? -> false }T
60000 to live-join-ms

test-summary

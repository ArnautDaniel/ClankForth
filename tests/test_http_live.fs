\ http.live (basis/http/live): a live view - its page rendered by a GET, its socket joined with its secret, events
\ answered with the regions that changed, pushes (live-tell, a database's change), what isn't its own refused
IN: test-http-live
USING: tester actors net system json jsondb http html http.live vocabs.webclient vocabs.livemsgs ;

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
: away-bad ( v -- )  drop  s" /\evil.example/x" live-redirect ;   \ (a browser reads /\ as //: another site)
: rename ( v -- )  {: v :}  s" to" v json-string@ if  s" name" state-string!  then ;
: c-info ( x -- )  s" count" state! ;
: tick ( v -- )  drop  20 after drop ;                     \ (a timer of the view's own)
: c-timer ( timer -- )  drop  s" count" state json-int@ drop 1+ s" count" state! ;
: c-changed ( c id rev json -- )  2drop 2drop  s" items" state json-int@ drop 1+ s" items" state! ;
state-field: clicks   \ (declared page state: clicks@ clicks!)
state-int: taps   state-flag: open   state-text: note   \ (typed: 0, false, "" till set)
: typed ( v -- )   \ taps 1 more, open turned over, note "x" once: count = taps + 10 if open + 100 x note's length
    drop  taps@ 1+ taps!  open@ 0= open!  note@ nip 0= if  s" x" note!  then
    taps@  open@ if  10 +  then  note@ nip 100 * +  s" count" state! ;
: click ( v -- )  drop  clicks@ json-null = if  0  else  clicks@  then  1+ clicks!  clicks@ 100 + s" count" state! ;
: c-guard ( name n -- ok? )  s" forbidden" s= 0= ;   \ (one event this page's user may not send)
: forbidden ( v -- )  drop  999 s" count" state! ;
live-view counter
    guard: c-guard
    event: forbidden forbidden
    event: click click
    event: typed typed
    mount: c-mount
    render: c-render
    event: inc inc
    event: away away
    event: away-bad away-bad
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
T{ page-text s\" <script src=\"/live.js?v=" s-find 0> -> true }T   \ (its URL with the script's hash)
T{ page-text s\" <div id=\"count\"><p>0</p></div>" s-find 0> -> true }T
T{ page-text s\" <div id=\"name\"><p>ann</p></div>" s-find 0> -> true }T
T{ secret nip -> 43 }T
T{ s" /live.js" get  2dup status-of -rot s" application/javascript" has? -> 200 true }T
T{ s" /live.js" get  s" findAgain" has? -> true }T   \ (a focused field without an id found again: by name, by place)
testing the script by its hashed URL kept a year (immutable); without it asked again, its ETag a 304
: script-path ( -- a n )   \ (the page's src: /live.js?v=...)
    page-text s\" src=\"/live.js?v=" s-find 5 + {: at :}  page-html at +  page-n @ at -  {: a n :}  a  a n s\" \"" s-find ;
T{ script-path get  s" Cache-Control: public, max-age=31536000, immutable" has? -> true }T
T{ s" /live.js" get  2dup s" Cache-Control: no-cache" has?  -rot s\" ETag: \"" has? -> true true }T
: etag-of ( -- a n )   \ (the ETag the script is served with)
    s" /live.js" get {: a n :}  a n s\" ETag: \"" s-find 6 + {: at :}  a at +  n at -  {: ea en :}  ea  ea en s\" \r" s-find ;
create etag 64 allot   variable etag-n
: keep-etag ( -- )  etag-of {: a n :}  a etag n move  n etag-n ! ;
keep-etag
: revalidated ( -- status )  etag etag-n @ s\" If-None-Match: {s}\r\n" format {: h hn :}  s" /live.js" h hn get-with status-of ;
T{ revalidated -> 304 }T

\ ---- a socket: its handshake, frames sent masked as a browser's, the server's read back ----
<bytes> constant out
: handshake-with ( origin n secret n -- )   \ (from that Origin, to the page, saying that secret)
    {: oa on sa sn :}  out b-clear
    page-addr  sa sn  web-port @  oa on
    s\" GET /live?p={s}&s={s} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nOrigin: {s}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
    format {: h hn :}  h hn out b-append  h dispose ;
: handshake ( origin n -- )  secret handshake-with ;
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
T{ reply b-clear  s\" {\"e\":\"away-bad\",\"v\":null}" say  wait-reply  s" evil.example" says? -> false }T   \ (refused: an error, no redirect)
testing a guard: an event it refuses isn't run (answered, nothing changed)
T{ reply b-clear  s\" {\"e\":\"forbidden\",\"v\":null}" say  wait-reply  s\" {\"r\":[]}" says?  s\" <p>999</p>" says? -> true false }T
testing declared state (state-field:): a field's words, json-null till set
T{ reply b-clear  s\" {\"e\":\"click\",\"v\":null}" say  wait-reply  s\" [\"count\",\"<p>101</p>\"]" says? -> true }T
T{ reply b-clear  s\" {\"e\":\"click\",\"v\":null}" say  wait-reply  s\" [\"count\",\"<p>102</p>\"]" says? -> true }T
T{ reply b-clear  s\" {\"e\":\"typed\",\"v\":null}" say  wait-reply  s\" [\"count\",\"<p>111</p>\"]" says? -> true }T
T{ reply b-clear  s\" {\"e\":\"typed\",\"v\":null}" say  wait-reply  s\" [\"count\",\"<p>102</p>\"]" says? -> true }T
testing pushes: a message to the page (live-tell), a change to a collection it watches - each sends what changed
: the-page ( -- addr )  page-addr string>number drop ;
: page-alive? ( -- flag )  the-page $ffffffff and alive? ;   \ (alive? takes this node's id: the address's low bits)
T{ reply b-clear  41 the-page live-tell  wait-reply  s\" [\"count\",\"<p>41</p>\"]" says? -> true }T
T{ reply b-clear  s" {}" s" items" s" i1" db db-put 2drop  wait-reply  s\" [\"items\",\"<p>1</p>\"]" says? -> true }T
testing a timer the view started: its timer: word (not the page's wait for its join), then what changed sent
T{ reply b-clear  s\" {\"e\":\"tick\",\"v\":null}" say  wait-reply  s\" [\"count\",\"<p>42</p>\"]" says?  page-alive? -> true true }T
testing the socket closed: its page gone
T{ client-conn @ net-close  wait-reply  page-alive? -> false }T
T{ json-true object?  json-false object?  json-null object? -> true true true }T   \ (its state-flag: held json's own: not freed with it)
: still-true ( -- flag ok )  s\" {\"v\":true}" json> {: v :}  s" v" v json-bool@ ;
T{ still-true -> true true }T

testing a push before the page's socket joins: sent as it joins (not held till the next event)
get-page
77 the-page live-tell  wait-reply
T{ our-origin handshake  join-text text-frame  out b-data open-with  wait-reply  s\" [\"count\",\"<p>77</p>\"]" says? -> true }T
client-conn @ net-close  wait-reply

testing another page's socket with the wrong secret: refused at the upgrade (403; the page goes on); a socket
testing  naming an actor that isn't a page: refused; a socket from another site: refused
get-page
T{ our-origin s" not-the-secret-not-the-secret-not-the-secret" handshake-with  out b-data open-with  wait-reply
   s" 403 Forbidden" says?  page-alive? -> true true }T
: not-a-page ( -- )   \ (the test's own asker actor named as the page: it doesn't answer live-check)
    out b-clear  web-port @ our-origin
    s\" GET /live?p=0&s=x HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nOrigin: {s}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
    format {: h hn :}  h hn out b-append  h dispose ;
: wait-frames-too ( -- )  400 0 do  10 net-poll drop  1 tick-tasks  actors-frame  s" 403" says? if  leave  then  loop ;   \ (await-within counts frames)
T{ not-a-page  out b-data open-with  wait-frames-too  s" 403 Forbidden" says? -> true }T
T{ s" http://evil.example" handshake  out b-data open-with  wait-reply  s" 403 Forbidden" says? -> true }T
testing a page on this node, its socket upgraded by another node's /live: asked across the nodes, joined (101)
s" vocabs.livenode" start-node drop
variable other-port
behaviour port-asker
  on live-port-is ( port -- )  other-port ! ;
end-behaviour
port-asker -1 s" port-asker" spawn constant asking
: other-up ( -- )   \ (the node's actor registered, then its port asked)
    200 0 do  s" livenode" whereis nobody <> if  leave  then  10 net-poll drop  deliver  loop
    asking enter  s" livenode" whereis send live-port?  leave-actor
    300 0 do  other-port @ if  leave  then  10 net-poll drop  deliver  loop ;
other-up
get-page
variable here-port   web-port @ here-port !
: via-other ( -- )   \ (the handshake to the other node's port: its Host and Origin that port's)
    other-port @ web-port !  our-origin handshake  out b-data open-with  wait-reply  here-port @ web-port ! ;
T{ other-port @ 0<> -> true }T
T{ via-other  s" 101 Switching Protocols" says? -> true }T

testing a page whose script never joins: gone after live-join-ms
100 to live-join-ms
get-page
: waits ( -- )  30 0 do  10 sleep-ms  10 net-poll drop  deliver  loop ;
T{ waits  page-alive? -> false }T
60000 to live-join-ms

test-summary

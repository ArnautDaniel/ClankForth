\ http.live.test (basis/http/live/test): a live view tested without a browser - mounted, its events sent, its
\ regions, body, state and redirect read back; a push seen with live-look
IN: test-http-live-test
USING: tester actors json html http.live http.live.test ;

state-int: count
state-text: who
state-map: picked      \ (typed: the map or 0; stored as a copy)
state-list: items      \ (the list or an empty one; stored as a copy)
: show-picked ( -- )  <p  picked@ ?dup if  s" name" rot json-text  else  s" none"  then  text  p> ;
: show-items ( -- )  <p  items@ length number  s"  items, first " text  items@ length if  s" t" 0 items@ nth json-text  else  s" -"  then  text  p> ;
: show-count ( -- )  <p  count@ number  p> ;
: show-who ( -- )  <p  who@ text  p> ;
: t-mount ( params -- )  {: p :}  s" name" p json-string@ 0= if  s" nobody"  then  who!  0 count! ;
: t-render ( -- )  <h1  s" Tested" text  h1>  s" count" ['] show-count region  s" who" ['] show-who region
    s" picked" ['] show-picked region  s" items" ['] show-items region ;
: pick ( v -- )  picked! ;   \ (the event's value - freed after the handler: kept only if the store copied it)
: fill ( v -- )  items! ;
: inc ( v -- )  drop  count@ 1+ count! ;
: rename ( v -- )  {: v :}  s" to" v json-string@ if  who!  then ;
: away ( v -- )  drop  s" /elsewhere" live-redirect ;
: t-info ( x -- )  count! ;
live-view tested
    mount: t-mount
    render: t-render
    event: inc inc
    event: rename rename
    event: away away
    event: pick pick
    event: fill fill
    info: t-info
end-live-view

: params ( -- m )  s\" {\"name\":\"ann\"}" json> ;
testing a page mounted with its params: its regions as its mount left them, its body round them
params tested live-open
T{ s" count" live-region s" <p>0</p>" s=  s" who" live-region s" <p>ann</p>" s= -> true true }T
T{ live-body s" <h1>Tested</h1>" s-find 0>= -> true }T
testing an event: the region it changed, the state; a form's map as its value
json-null s" inc" live-do
json-null s" inc" live-do
T{ s" count" live-region s" <p>2</p>" s=  s" count" tested-state json-int@ -> true 2 true }T
s\" {\"to\":\"<b>bo</b>\"}" json> s" rename" live-do
T{ s" who" live-region s" <p>&lt;b&gt;bo&lt;/b&gt;</p>" s= -> true }T   \ (escaped, as the browser gets it)
testing a redirect said; a push (live-tell) seen with live-look
json-null s" away" live-do
T{ live-redirected s" /elsewhere" s= -> true }T
41 tested-page live-tell  live-look
T{ s" count" live-region s" <p>41</p>" s=  live-redirected nip -> true 0 }T
testing state-map: and state-list: - 0 and an empty list till set; a value stored kept as a copy (the event's own
testing  value freed after its handler: the region still right at the next look)
T{ s" picked" live-region s" <p>none</p>" s=  s" items" live-region s" <p>0 items, first -</p>" s= -> true true }T
s\" {\"name\":\"Bond\"}" json> s" pick" live-do
s\" [{\"t\":\"first\"},{\"t\":\"second\"}]" json> s" fill" live-do
live-look
T{ s" picked" live-region s" <p>Bond</p>" s=  s" items" live-region s" <p>2 items, first first</p>" s= -> true true }T
testing closed: the page gone
tested-page $ffffffff and constant the-tested
live-close
T{ the-tested alive? -> false }T

test-summary

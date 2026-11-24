\ live.fs - live views (Nitrogen's, LiveView's): each open page an actor. Its state lives on the server; the browser
\ sends it events over a WebSocket (live.js, a page of script of ours); after each, the page renders again and sends
\ the regions of it that changed. Anything the page actor hears - a database's change, a message - is handled so too.
\
\   live-view queue-view
\       mount: queue-mount          \ ( params -- ) what it shows, loaded into state; what it watches, watched
\       render: queue-render        \ ( -- ) its HTML, from state (html's words, templates)
\       event: claim do-claim       \ <button live-click="claim" live-value="t7"> -> do-claim ( value -- )
\       changed: queue-changed      \ ( c id rev json -- ) a collection it watches changed (jsondb's db-watch)
\   end-live-view
\   s" /queue" queue-view site live-route
\
\ docs/live.md.
IN: http.live
USING: actors net http http.websocket html json jsondb crypto crypto.tokens accessors system urls.encoding checksums.crc32 ;

60000 value live-join-ms   \ a page whose script hasn't joined it in this long goes (a bot's, a page with no script)
10000 value live-mount-ms  \ a page that hasn't rendered in this long: its request answered 500

\ ---- views ----
\ a view: its name, its words (mount ( params -- ), render ( -- ), changed ( c id rev json -- ), info ( x -- ),
\ layout ( content-xt -- ); 0 for those not given), and its events (a list of live-event)
TUPLE: view-record  name mount render changed info layout events request timer guard ;
TUPLE: live-event  name xt ;

<PRIVATE
variable defining   \ the view being declared
: ?defining ( -- view )  defining @ ?dup 0= abort" live: only between live-view and end-live-view" ;
: word-named ( "name" -- xt )
    parse-name {: a n :}  a n find-word ?dup 0= if  true a n s" live: no word {s} (define it before the view)" format (abort")  then ;
PRIVATE>

\ a view declared: its words named on the lines up to end-live-view; the constant `name` it is
: live-view ( "name" -- )
    parse-name {: a n :}
    a n >string drop 0 0 0 0 0 list 0 0 0 <view-record> rescue-all {: v :}
    v a n s" constant {s}" format evaluate
    v defining ! ;
: mount: ( "word" -- )  ?defining word-named >>mount drop ;        \ ( params -- ): the page opened
: render: ( "word" -- )  ?defining word-named >>render drop ;      \ ( -- ): its HTML, from state
: changed: ( "word" -- )  ?defining word-named >>changed drop ;    \ ( c id rev json -- ): a watched collection changed
: info: ( "word" -- )  ?defining word-named >>info drop ;          \ ( x -- ): live-info from another actor
: layout: ( "word" -- )  ?defining word-named >>layout drop ;      \ ( content-xt -- ): the document round it
\ ( req params -- continue? ): the GET's, before the page is made - what it needs of its request put in its params
\ (the user its session has), or the request answered itself (a redirect to sign in): false, and no page
: request: ( "word" -- )  ?defining word-named >>request drop ;
: timer: ( "word" -- )  ?defining word-named >>timer drop ;
\ ( name n -- ok? ): every event asked of first - its name - a false one refused, not run (an event is an endpoint: a
\ role checked here as a route's is - the user's roles in the page's state, put there by its mount)
: guard: ( "word" -- )  ?defining word-named >>guard drop ;        \ ( timer -- ): a timer the page started (net's after)
: event: ( "name" "word" -- )   \ an event of that name (live-click="name" ...) to the word ( value -- )
    ?defining {: v :}  parse-name >string drop  word-named  <live-event> rescue-all  v events>> push ;
: end-live-view ( -- )
    ?defining {: v :}
    v render>> 0= abort" live: a view needs render:"
    0 defining ! ;

\ ---- the page actor ----
message live-start ( params view -- )   \ to a new page: mount, render; live-html to the sender
message live-html ( s -- )              \ its first render: the whole page
message live-check ( secret asker -- )  \ to an actor a socket names as its page: is this your secret?
message live-checked ( ok -- )          \ ... its answer (only a live page gives one)
quiet-message live-check   quiet-message live-checked
message live-info ( x -- )              \ to a page, from anyone: its view's info: word, then it renders again
message live-test-event ( name value asker -- )   \ (http.live.test's) an event run as the browser's would be ...
message live-test-look ( asker -- )               \ ... or none: the page as it is now
message live-test-seen ( json -- )                \ ... its answer: {"html", "regions": {id: html}, "state", "redirect"}

state: live-state
    cell field lp-view
    cell field lp-state      \ what it shows (a map: state, state!)
    cell field lp-sent       \ each region's HTML as last sent, as its SHA-256 (id -> 32 bytes: a page holds no copy of it)
    cell field lp-ws         \ its socket, joined; 0 till then
    cell field lp-secret     \ what its script joins with (a string)
    cell field lp-timer      \ the wait for its script to join
    cell field lp-changed    \ (a render's) the regions that changed, as JSON (bytes); 0: none to send
    cell field lp-regions    \ (a render's) how many regions it had
    cell field lp-redirect   \ where the browser is to go (a string), 0 none
end-state

\ the running actor is a live page (a word a view's code and the code under it share - a log line's, a helper's - may
\ ask: any other actor, or none, false - not an error; its state type looked at before a page's field is read)
: in-live-page? ( -- flag )
    self 0< if  false exit  then  self actor-type live-state <> if  false exit  then  lp-view @ 0<> ;
<PRIVATE
: page? ( -- flag )  in-live-page? ;
: ?page ( -- )  page? 0= abort" live: only in a live view's words (its page actor's)" ;
PRIVATE>

\ ---- what a view's words use ----
\ what the page shows: a map (json-string@ ... read it)
: state ( -- m )  ?page lp-state @ ;
\ a value kept in the page's state (a number, or an object json reads and writes - kept by the page from now; the
\ one it replaces let go, all of it, so keep nothing of that)
: state! ( v key n -- )
    {: v ka kn :}  ?page
    ka kn lp-state @ s-at if  {: old :}  old v <>  old object? and if  old json-free  then  then
    v object? if  v rescue-actor drop  then
    v ka kn lp-state @ s-at! ;
: state-string! ( a n key n -- )  {: a n ka kn :}  a n >string drop ka kn state! ;
\ a field of the page's state, declared: `state-field: tasks` defines `tasks@ ( -- v )` (json-null till it is set)
\ and `tasks! ( v -- )` - a misspelt one is an unknown word as it compiles, not a key that is quietly another
: state-field: ( "name" -- )
    parse-name {: a n :}
    a n a n s\" : {s}@ ( -- v )  s\" {s}\" state s-at 0= if  json-null  then ;" format evaluate
    a n a n s\" : {s}! ( v -- )  s\" {s}\" state! ;" format evaluate ;
\ ... of a kind, so one not yet set reads as that kind's nothing (json-null, a state-field:'s, is true to `if`):
\ `state-int: page` - `page@ ( -- n )` 0 till set, `page! ( n -- )`; `state-text: query` - `query@ ( -- a n )` "" till
\ set, `query! ( a n -- )`; `state-flag: open` - `open@ ( -- flag )` false till set, `open! ( flag -- )`
: state-int: ( "name" -- )
    parse-name {: a n :}
    a n a n s\" : {s}@ ( -- n )  s\" {s}\" state json-int@ 0= if  0  then ;" format evaluate
    a n a n s\" : {s}! ( n -- )  s\" {s}\" state! ;" format evaluate ;
: state-text: ( "name" -- )
    parse-name {: a n :}
    a n a n s\" : {s}@ ( -- a n )  s\" {s}\" state json-string@ 0= if  s\" \"  then ;" format evaluate
    a n a n s\" : {s}! ( a n -- )  s\" {s}\" state-string! ;" format evaluate ;
\ ... a row or a list: `state-map: row` - `row@ ( -- m | 0 )` the map, 0 till set (or set to what isn't one);
\ `state-list: items` - `items@ ( -- l )` the list, an empty one till set. Each `name! ( v -- )` keeps a copy of what
\ it is given (json-copy): a message's, a query's, the handler's own - no copy needed before, nothing of it kept after
: state-map: ( "name" -- )
    parse-name {: a n :}
    a n a n s\" : {s}@ ( -- m )  s\" {s}\" state s-at if  dup map? 0= if  drop 0  then  else  0  then ;" format evaluate
    a n a n s\" : {s}! ( v -- )  dup if  json-copy  else  drop json-null  then  s\" {s}\" state! ;" format evaluate ;
: state-list: ( "name" -- )
    parse-name {: a n :}
    a n a n s\" : {s}@ ( -- l )  s\" {s}\" state s-at if  dup list? 0= if  drop list  then  else  list  then ;" format evaluate
    a n a n s\" : {s}! ( v -- )  dup if  json-copy  else  drop json-null  then  s\" {s}\" state! ;" format evaluate ;
: state-flag: ( "name" -- )
    parse-name {: a n :}
    a n a n s\" : {s}@ ( -- flag )  s\" {s}\" state json-bool@ 0= if  false  then ;" format evaluate
    a n a n s\" : {s}! ( flag -- )  json-bool s\" {s}\" state! ;" format evaluate ;
\ the browser sent elsewhere after this event (a new page)
: plain-url? ( a n -- flag )   \ no space, tab, CR, LF or other control byte (a browser drops some: /<tab>/host)
    {: a n :}  true  n 0 ?do  a i + c@ 33 < if  drop false leave  then  loop ;
: live-redirect ( url n -- )   \ (a path of the site, or an http(s) URL - not javascript:, not /\host ...)
    {: a n :}  ?page
    a n s" /" s-starts?  n 1 > if  a 1+ c@ {: c :}  c [char] / =  c [char] \ = or 0=  else  true  then  and
    a n s" https://" s-starts? or  a n s" http://" s-starts? or  a n plain-url? and
    0= abort" live-redirect: a path of the site (/...) or an http(s) URL"
    a n >string drop rescue-actor lp-redirect ! ;
\ a message to a page (from any actor): its view's info: word given x, then it renders again
: live-tell ( x page -- )  send live-info ;

\ a part of the page sent alone when it changes: an element <div id="id"> holding what xt writes. A page with
\ none sends its whole body. Regions don't nest.
task-variable capture   \ (a test's render: each region's HTML as JSON, "id": "html", added to these bytes - 0 none)
: keep-region ( inner id n b -- )   \ (the bytes copied in: a region's HTML is its render's, gone after it)
    {: s ia in b :}  b b-length if  s" ,"  else  s" {"  then  b b-append
    ia in >string drop >json b b-append  s" :" b b-append  s >json b b-append ;
: region ( id n xt -- )
    {: ia in xt | inner old digest :}  ?page
    1 lp-regions +!
    xt html to inner
    capture @ ?dup if  inner swap ia in rot keep-region  then
    inner string sha256 drop to digest
    ia in lp-sent @ s-at if  to old  then
    old if  old string digest string s=  else  false  then  0= if
        lp-changed @ ?dup if  {: b :}
            b b-length 1 > if  s" ," b b-append  then
            s" [" b b-append  ia in >string drop >json b b-append  s" ," b b-append  inner >json b b-append  s" ]" b b-append
        then
        old ?dup if  dispose  then
        digest rescue-actor  ia in lp-sent @ s-at!
    then
    s\" <div id=\"" raw  ia in %escaped  s\" \">" raw  inner string raw  s" </div>" raw ;

\ live elements' attributes: an event on a click, a form's submit, a form's change (as one types), a value with it
: live-click= ( name n -- )  s" live-click" attr= ;
: live-submit= ( name n -- )  s" live-submit" attr= ;
: live-change= ( name n -- )  s" live-change" attr= ;
: live-value= ( a n -- )  s" live-value" attr= ;
variable script-url   \ /live.js?v= and its hash: a new script (a new build's) a new URL - no browser runs an old one
: live-script ( -- )  <script  script-url @ string src=  true s" defer" flag-attr=  script> ;

<PRIVATE
: root-attrs ( -- )   \ the root's: which page, and its secret
    s" live-root" id=
    self address s" {}" format {: s n :}  s n s" data-live-page" attr=  s dispose
    lp-secret @ string s" data-live-secret" attr= ;
: default-layout ( content-xt -- )
    {: xt :}
    doctype  <html  <head
        <meta  s" utf-8" charset=
        <meta  s" viewport" name=  s" width=device-width, initial-scale=1" content=
        s" title" lp-state @ json-string@ if  <title text title>  then
        live-script
    head>  <body  xt execute  body>  html> ;
: render-body ( -- )  lp-view @ render>> execute ;
: whole ( body -- s )   \ the whole page: the layout round the root round the body (rendered already)
    {: body :}  lp-view @ layout>> ?dup 0= if  ['] default-layout  then {: lay :}
    [: [: <div root-attrs  body string raw  div> ;] lay execute ;] html ;
: sent-root ( s -- )   \ the root's HTML kept as sent (a page with no regions) - its SHA-256
    {: s :}  s" live-root" lp-sent @ s-at if  dispose  then
    s string sha256 drop rescue-actor  s" live-root" lp-sent @ s-at! ;

\ rendered again: the regions that changed (or the whole body, a page with none) sent - and where to go, if it was told
\ what changed sent (an event's answer always: {"r":[]} when nothing did - a push's only if something did)
task-variable answering
: refresh ( -- )
    {: | body b :}
    lp-ws @ 0= if  exit  then
    <bytes> to b  b lp-changed !  0 lp-regions !
    [: render-body ;] html to body
    0 lp-changed !
    lp-regions @ 0= if
        s" live-root" lp-sent @ s-at if  string  body string sha256 {: d dn :}  d dn s=  d dispose  else  false  then  0= if
            s" [" b b-append  s" live-root" >string drop >json b b-append  s" ," b b-append  body >json b b-append
            s" ]" b b-append
            body sent-root
        then
    then
    b b-length 0=  lp-redirect @ 0= and  answering @ 0= and if  exit  then
    false answering !
    [:  s\" {\"r\":[" %s  b b-data %s  s" ]" %s
        lp-redirect @ ?dup if  s\" ,\"redirect\":" %s  >json %s  then
        s" }" %s ;] build {: m :}
    lp-redirect @ ?dup if  dispose  0 lp-redirect !  then
    m string lp-ws @ ws-send ;

: start ( params view -- )   \ mounted, rendered: the whole page to whoever asked
    {: p v | asker :}
    sender to asker   \ (now: after mount awaits anything, sender is whoever answered it)
    v lp-view !  <map> rescue-actor lp-state !  <map> rescue-actor lp-sent !
    random-token >string drop rescue-actor lp-secret !
    v mount>> ?dup if  p swap execute  then
    0 lp-changed !  0 lp-regions !
    [: render-body ;] html {: body :}   \ (once: its regions kept as sent)
    lp-regions @ 0= if  body sent-root  then   \ (no regions: the root's HTML as sent)
    body whole {: s :}
    s string >string drop asker send live-html
    live-join-ms after lp-timer ! ;
: run-event ( name n value -- )   \ an event to its word (its view's guard asked first)
    {: na nn v | found :}
    lp-view @ guard>> ?dup if  na nn rot execute 0= if   \ (refused: said, nothing run)
        na nn lp-view @ name>> string s" live: event {s} refused by {s}'s guard" format type cr  exit
    then then
    lp-view @ events>> [: {: e :}  found 0=  e name>> string na nn s= and if  e to found  then ;] each
    found if  v found xt>> execute  else  ." live: no event " na nn type ."  in " lp-view @ name>> string type cr  then ;
\ (http.live.test's) the page drawn now, told to the asker as JSON: its body's HTML, each region's, its state, where
\ it was told to go - a page tested needs no socket: its wait for one stopped
: test-seen ( asker -- )
    {: asker | body regs :}
    lp-timer @ ?dup if  cancel-timer  0 lp-timer !  then
    <bytes> to regs  regs capture !  0 lp-regions !
    [: [: render-body ;] html to body ;] catch  0 capture !  if  error-message type cr  then
    regs b-length if  s" }"  else  s" {}"  then  regs b-append
    [:  s\" {\"html\":" %s  body ?dup if  >json  else  s\" \"\""  then  %s
        s\" ,\"regions\":" %s  regs b-data %s  s\" ,\"state\":" %s  lp-state @ >json %s
        s\" ,\"redirect\":" %s  lp-redirect @ ?dup if  >json  else  s" null"  then  %s  s" }" %s ;] build {: j :}
    lp-redirect @ ?dup if  dispose  0 lp-redirect !  then
    j asker send live-test-seen ;
: joined ( s ws -- )   \ its socket's first message: the secret (else that socket closed)
    {: s ws | v :}
    [: s string json> to v ;] catch if  ws ws-close exit  then
    s" join" v json-string@ if  lp-secret @ string same-bytes?  else  false  then  0= if  ws ws-close exit  then
    ws lp-ws !
    lp-timer @ ?dup if  cancel-timer  0 lp-timer !  then
    refresh ;   \ (what changed since its GET - a push, a timer, a database's change - sent now, not at the next event)
: browser-said ( s ws -- )   \ a message from the browser: an event, or (first) its join
    {: s ws | v :}
    lp-ws @ 0= if  s ws joined exit  then
    ws lp-ws @ <> if  ws ws-close exit  then   \ (a second socket for one page)
    [: s string json> to v ;] catch if  exit  then
    s" e" v json-string@ 0= if  exit  then  {: ea en :}
    s" v" v map? if  v s-at 0= if  json-null  then  else  2drop json-null  then  {: val :}
    ea en val run-event
    true answering !  refresh  false answering ! ;
PRIVATE>

behaviour live-page
    on live-start ( params view -- )  start ;
    on live-check ( secret asker -- )   \ (a socket's upgrade asking: is this page's secret the one it was given?)
        {: s asker :}  s string lp-secret @ string same-bytes? asker send live-checked ;
    on ws-opened ( ws -- )  drop ;   \ (it joins with its first message)
    on ws-text ( s ws -- )  browser-said ;
    on ws-closed ( ws -- )  lp-ws @ = if  self kill  then ;
    on db-changed ( c id rev json -- )
        lp-view @ changed>> ?dup if  execute  refresh  else  2drop 2drop  then ;
    on live-info ( x -- )  lp-view @ info>> ?dup if  execute  refresh  else  drop  then ;
    on live-test-event ( name value asker -- )  {: s v asker :}  s string v run-event  asker test-seen ;
    on live-test-look ( asker -- )  test-seen ;
    on timer-fired ( timer -- )
        dup lp-timer @ = if   \ (its join's wait over)
            drop  0 lp-timer !  lp-ws @ 0= if  self kill  then
        else  lp-view @ timer>> ?dup if  execute  refresh  else  drop  then  then ;
end-behaviour

\ ---- routes ----
60 constant wait-frames   \ the first render's wait, a piece at a time (its deadline looked at between)
<PRIVATE
: params-of ( req -- m )   \ the route's :names and the query's names, decoded (a map of strings)
    {: r | m :}  <map> to m
    r req-query nip if   \ (the query's first: a route's :name of the same name wins, set after)
        r req-query >string drop " &" split [: string {: pa pn :}
            pa pn s" =" s-find {: eq :}
            eq 0> if
                pa eq 1+ +  pn eq - 1-  url-decode {: va vn :}
                pa eq url-decode {: ka kn :}
                va vn >string drop  ka kn m s-at!
            then ;] each
    then
    r params>> [: {: k v :}  v string >string drop  k string m s-at! ;] map-each
    m ;
\ a view's GET: its page actor spawned, mounted and rendered - the whole page the answer
: open-page ( req view -- )
    {: r v | page t0 p :}
    r params-of to p
    v request>> ?dup if  r p rot execute 0= if  exit  then  then
    live-page live-state s" live" spawn to page
    p v page send live-start
    ms-now to t0
    begin
        wait-frames await-within live-html if
            {: s :}  s" no-store" s" Cache-Control" r header!  s string 200 r respond-html  exit
        then
        page alive? 0=  ms-now t0 - live-mount-ms >  or if
            page alive? if  page kill  then
            s" live: the page didn't render" 500 r respond  exit
        then
    again ;
: here? ( addr -- flag )  {: a :}  a 32 rshift 0=  a node-of node =  or ;
\ the socket's owner: the page of its ?p= - from this site's pages only (Origin the site's)
wait-frames 4 * constant check-frames   \ (how long a page has to answer: a node's frames - a few seconds)
\ the actor the socket names a live page - and the secret its page was given (asked of it: on any node)? An actor
\ that isn't one doesn't answer: refused, not bound to the socket
: live-page? ( page secret n -- flag )
    {: page sa sn :}
    sa sn >string drop self address page send live-check   \ (its address: the page may be on another node)
    check-frames await-within live-checked 0= if  false  then ;
: page-of ( req -- actor )   \ (nobody: refused - 403)
    {: r :}
    s" origin" r req-header 0= if  nobody exit  then  {: oa on :}
    s" host" r req-header 0= if  nobody exit  then  {: ha hn :}
    oa on s" //" s-find {: at :}
    at 0< if  nobody exit  then
    oa at 2 + +  on at - 2 -  ha hn s= 0= if  nobody exit  then   \ (a page of another site's)
    s" p" r query-param 0= if  nobody exit  then  {: pa pn :}
    pa pn string>number 0= if  drop nobody exit  then  {: page :}
    page here? if  page $ffffffff and alive? 0= if  nobody exit  then  then
    s" s" r query-param 0= if  nobody exit  then  {: sa sn :}
    page sa sn live-page? if  page  else  nobody  then ;
PRIVATE>

\ ---- live.js: the browser's side (no framework; no " nor \ in it: it lives in Forth strings) ----
[:
" (function () {
  var root = document.getElementById('live-root');
  if (!root || !window.WebSocket) return;
  var page = root.getAttribute('data-live-page'), secret = root.getAttribute('data-live-secret');
  var ws, timers = {};
  function send(e, v) { if (ws && ws.readyState === 1) ws.send(JSON.stringify({e: e, v: v})); }
  function formValue(form) {
    var o = Object.create(null);
    new FormData(form).forEach(function (val, key) {
      if (key in o) { if (!Array.isArray(o[key])) o[key] = [o[key]]; o[key].push(val); } else o[key] = val;
    });
    form.querySelectorAll('input[type=checkbox][name]').forEach(function (c) {
      if (!c.disabled && !(c.name in o)) o[c.name] = 'off';
    });
    return o;
  }
  function valueOf(el) {
    var v = el.getAttribute('live-value');
    if (v !== null) return v;
    var o = null;
    for (var i = 0; i < el.attributes.length; i++) {
      var a = el.attributes[i];
      if (a.name.indexOf('live-value-') === 0) { o = o || {}; o[a.name.slice(11)] = a.value; }
    }
    return o;
  }
" %
" document.addEventListener('click', function (ev) {
    var el = ev.target.closest('[live-click]');
    if (!el || !root.contains(el)) return;
    ev.preventDefault();
    send(el.getAttribute('live-click'), valueOf(el));
  });
  document.addEventListener('submit', function (ev) {
    var f = ev.target.closest('form[live-submit]');
    if (!f || !root.contains(f)) return;
    ev.preventDefault();
    send(f.getAttribute('live-submit'), formValue(f));
  });
  function changed(ev) {
    var f = ev.target.closest('form[live-change]');
    if (!f || !root.contains(f)) return;
    var name = f.getAttribute('live-change');
    clearTimeout(timers[name]);
    timers[name] = setTimeout(function () { send(name, formValue(f)); }, 200);
  }
  document.addEventListener('input', changed);
  document.addEventListener('change', changed);
" %
" function named(el, name) {
    return Array.prototype.filter.call(el.querySelectorAll('[name]'), function (x) { return x.name === name; });
  }
  function keyOf(f, el) {   /* the focused field found again after the region is drawn: by id, by name, by place */
    if (f.id) return {id: f.id};
    if (f.name) return {name: f.name, i: named(el, f.name).indexOf(f)};
    var path = [];
    for (var n = f; n && n !== el; n = n.parentNode) path.unshift(Array.prototype.indexOf.call(n.parentNode.children, n));
    return {path: path};
  }
  function findAgain(el, k) {
    if (k.id) return document.getElementById(k.id);
    if (k.name) { var same = named(el, k.name); return same[k.i] || same[0] || null; }
    var n = el;
    for (var i = 0; i < k.path.length && n; i++) n = n.children[k.path[i]];
    return n && n !== el ? n : null;
  }
  function swap(el, html) {
    var focus = document.activeElement, inside = focus && focus !== el && el.contains(focus);
    var key = inside ? keyOf(focus, el) : null, value = inside && 'value' in focus ? focus.value : null;
    var start = null, end = null;
    if (inside) { try { start = focus.selectionStart; end = focus.selectionEnd; } catch (e) {} }
    el.innerHTML = html;
    if (key) {
      var again = findAgain(el, key);
      if (again) {
        if (value !== null && 'value' in again && again.type !== 'checkbox' && again.type !== 'radio') again.value = value;
        again.focus();
        try { if (start !== null) again.setSelectionRange(start, end); } catch (e) {}
      }
    }
  }
  function apply(m) {
    (m.r || []).forEach(function (r) { var el = document.getElementById(r[0]); if (el) swap(el, r[1]); });
    if (m.redirect) location.href = m.redirect;
  }
  var opened = false, tries = +(sessionStorage.getItem('live-tries') || 0);
  function reload() {
    setTimeout(function () {
      fetch('/live.js', {method: 'HEAD', cache: 'no-store'}).then(function () { location.reload(); }, reload);
    }, 1000);
  }
  function lost() {
    document.documentElement.classList.add('live-lost');
    if (opened) { sessionStorage.setItem('live-tries', '0'); reload(); return; }
    if (tries < 3) { sessionStorage.setItem('live-tries', String(tries + 1)); reload(); }
  }
  ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/live?p=' + page + '&s=' + encodeURIComponent(secret));
  ws.onopen = function () { opened = true; sessionStorage.setItem('live-tries', '0'); ws.send(JSON.stringify({join: secret})); };
  ws.onmessage = function (m) { apply(JSON.parse(m.data)); };
  ws.onclose = lost;   /* (a socket refused every time - a proxy's Host - reloads three times, then stays lost) */
})();
" %
;] build constant live-js
live-js string crc32-hex drop constant script-tag   \ (its CRC-32: what changes as the script does)
script-tag string s" /live.js?v={s}" format drop script-url !

<PRIVATE
: serve-script ( req -- )   \ its URL with this script's hash: kept a year; else (no v, an old one) asked again
    {: r :}
    script-tag string s\" \"{s}\"" format {: ea en :}
    ea en s" ETag" r header!
    s" v" r query-param if  script-tag string s=  else  false  then
    if  s" public, max-age=31536000, immutable"  else  s" no-cache"  then  s" Cache-Control" r header!
    s" if-none-match" r req-header if  ea en s=  else  false  then if  s" " 304 r respond exit  then
    s" application/javascript; charset=utf-8" r content-type!
    live-js string 200 r respond ;
: open-view ( req -- )  {: r :}  r r req-data open-page ;
list constant live-routers   \ the routers given /live.js and /live already
: ?live-routes ( router -- )
    {: rt :}
    rt live-routers contains? if  exit  then
    s" /live.js" ['] serve-script rt on-get
    s" /live" ['] page-of rt on-websocket
    rt live-routers push ;
PRIVATE>

\ the view's pages at the path (a route's pattern: :names given its mount in params): each GET a page actor of its
\ own, mounted, rendered - the router given /live (its sockets) and /live.js (its script) too
: live-route ( path n view router -- )
    {: pa pn v rt :}  rt ?live-routes  s" GET" pa pn ['] open-view v rt route-with ;

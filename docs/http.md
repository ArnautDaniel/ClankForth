# The http vocabulary: a web server of actors

An HTTP/1.1 server written in Forth on the `net` vocabulary (docs/net.md), built in (`basis/http/`) for hosts that
give `net` and `system` - the `clankforth` tool does. `USING: actors net http ;`

```forth
<router> constant site
: home ( req -- )  {: r :}  s" hello" 200 r respond ;
: user ( req -- )  {: r :}  s" id" r req-param drop  s" user {s}" format  200 r respond ;
s" /" ' home site on-get
s" /users/:id" ' user site on-get
site 8080 http-listen drop
serve
```

It is Erlang's shape: a listener's actor hands each connection to an **actor of its own**, which reads the requests,
runs their handlers and answers. So a handler is an actor's handler: it may ask other actors - a session keeper, a
cache, a database's actor, on this node or another - and `await` the answer, and the other connections go on
meanwhile. A handler that fails takes its connection's actor down with it (the error said), as Erlang lets a process
crash: its last words answer 500, and nothing else is touched.

Its parts are vocabularies of their own (`basis/http/<part>/`):

| vocabulary | what |
|---|---|
| `http` | requests, answers, routes, streamed answers, the listener |
| `http.cookies` | the cookies a request sends, the ones an answer sets |
| `http.files` | static files: a folder served, types, `Last-Modified`, 304 |
| `http.sse` | server-sent events: an answer held open, events pushed down it |
| `http.websocket` | WebSockets, each socket an actor with an owner |
| `http.tls` | `https-listen` (a build with OpenSSL) |

`examples/web-app.fs` uses them all, on every core: its routes (`examples/webapp/site.fs`) on a node for each, all
listening on one port; a count of visits and a chat room as actors on the first node, which every node's handlers
and sockets talk to; the clock as events; its page served from a folder.

## Handlers and requests

A handler is a word `( req -- )` that answers its request once (`respond` and the words after it). It runs inside a
scratch region (docs/forth.md, Tuples): the request, and every object the handler makes, is freed once it is
answered - `format`'s strings, lists, a parse's pieces need no `dispose`. What must outlive the request is
`rescue-all`ed (as for a message).

- `req-method ( req -- a n )`, `req-path` (as sent: %-escapes and all), `req-query` (after the `?`, as sent; "" for
  none), `req-version`, `req-body` (its `Content-Length` bytes), `req-conn ( req -- conn )`.
- `req-header ( name n req -- a n true | false )`: by its name in any case. Said twice, its values joined with ", ".
  `req-headers ( req -- m )`: the map, its names lower-cased.
- `req-param ( name n req -- a n true | false )`: what the route's `:name` matched, decoded; `"*"` what its `*` did.
- `query-param ( name n req -- s len true | false )`: a name's value in the query string, decoded (`+` a space): a
  new string. (`urls.encoding`'s `url-decode` does the decoding: docs/libraries.md.)
- `req-data ( req -- x )`: what its route was given (`route-with`).
- `req-gone? ( req -- flag )`: the client has gone - what a handler that streams, or waits long, asks (its
  connection's `net-closed` is kept till the handler is done).

## Answers

- `respond ( a n status req -- )`: the status line, the headers, `Date`, `Content-Length`, the body (a HEAD
  request's without it). Once: twice is an error. With no `Content-Type` given, `text/plain; charset=utf-8`.
- `respond-html`, `respond-json ( a n status req -- )`: with that content type (`json`'s `>json` makes the text).
- `header! ( value n name n req -- )`: a header of the answer (before it), `content-type! ( a n req -- )`.
- `redirect ( url n status req -- )` (301, 302, 303, 307, 308), `not-found ( req -- )`.

A handler that returns without answering is answered 500 (and said: `http: GET /path: no answer given`).

### Streamed

`respond-start ( status req -- )` sends the head; then `respond-chunk ( a n req -- )` a piece of the body at a time -
the handler may wait between (`await`, `wait`) - and `respond-end ( req -- )` (a handler that returns streaming ends
it so). To HTTP/1.1 the body goes in chunks (`Transfer-Encoding: chunked`, the connection kept after); to 1.0, up to
the connection's close. A writer faster than its client can watch `req-conn net-pending` (bytes not yet sent).

### Server-sent events (`http.sse`)

```forth
: news ( req -- )  {: r :}  r respond-events
    begin  60 await-within headline if  {: s :}  s string s" headline" r send-event  s dispose  then
    r req-gone? until ;
```

`respond-events ( req -- )` begins the answer (`text/event-stream`, not cached); `send-event ( data n name n req -- )`
sends one (name "" for a plain message; each line of the data a `data:` line); `send-ping ( req -- )` a comment, to
keep what is between from closing an idle connection. In the browser, `new EventSource(url)`.

## Routes

`<router> ( -- router )`, then a route for each method and pattern - `route ( method n pattern n xt router -- )`,
or `on-get`, `on-post`, `on-put`, `on-delete`, `on-any ( pattern n xt router -- )`; `route-with ( method n pattern n
xt x router -- )` gives the handler x (`req-data`). A pattern's segments match the path's (empty ones left out:
`/a//b/` is `/a/b`): `:name` any one segment, a last `*` what is left (none or more). The first route that fits
answers; HEAD takes a GET route. A path that fits routes of other methods only is a 405 with `Allow`; one that fits
none is `on-missing ( xt router -- )`'s handler, or a 404.

## Cookies (`http.cookies`)

- `req-cookie ( name n req -- s len true | false )`: the value of the cookie of that name the request sent (its
  quotes off): a new string.
- `set-cookie! ( value n name n attrs n req -- )`: a `Set-Cookie` of the answer, its attributes after
  (`s" Path=/; HttpOnly; SameSite=Lax; Max-Age=3600"`, or ""). A name that isn't a token, or a value with a space, a
  quote, a comma, a semicolon or a backslash, is an error.
- `expire-cookie! ( name n path n req -- )`: the browser told to forget it.

## Static files (`http.files`)

- `serve-files ( dir n prefix n router -- )`: GET (and HEAD) of `prefix/...` answered by the file of that path in the
  folder - a folder's `index.html`; prefix "" for the root. A path with a `.` or `..` segment, a backslash or a NUL
  (as sent or %-escaped) is a 404: nothing outside the folder.
- `respond-file ( path n req -- )`: a file as the answer (in a handler of your own).
- Each with its type by its name (`mime-type ( path n -- a n )`: html, css, js, json, svg, png, jpeg, gif, webp, ico,
  wasm, fonts, pdf, xml, mp4, webm, mp3 ...; `application/octet-stream` else), `Last-Modified`, and 304 (no body) to a
  request whose `If-Modified-Since` is as late. A file is read whole into memory.

## WebSockets (`http.websocket`)

The socket is an actor. A route's handler names the actor that **owns** it - a new one, or one there already (a chat
room's, on any node); the socket tells its owner what comes, and the owner sends it what to write. Messages, both
ways:

```forth
behaviour echo
  on ws-text ( s ws -- )  {: s ws :}  s string ws ws-send  s dispose ;
end-behaviour
: new-echo ( req -- actor )  drop  echo -1 s" echo" spawn ;
s" /echo" ' new-echo site on-websocket
```

- `on-websocket ( pattern n xt router -- )`: handshakes to those paths answered (101), xt `( req -- actor )` naming
  the owner. A request that is no handshake is a 400; a version but 13, 426.
- The owner hears `ws-opened ( ws -- )`, `ws-text ( s ws -- )` and `ws-binary ( s ws -- )` - s a new string, the
  owner's to `dispose` - and `ws-closed ( ws -- )`. `ws` is the socket's address.
- `ws-send ( a n ws -- )`, `ws-send-binary`, `ws-close ( ws -- )`, `ws-close-with ( code ws -- )`: from any actor, on
  any node (a socket gone: nothing).
- The socket answers pings, puts a message's frames together, and closes for what is wrong (a frame not masked:
  1002; text that isn't UTF-8: 1007; a message over `ws-message-max`, 1 MiB: 1009). It watches its owner: the owner
  gone, it closes (1001).

## Serving

`http-listen ( router port -- listener )`: a listener on the port (0: a free one; `listener-port` says which,
`net-close` stops it) and its actor; then `serve` (or a loop of the host's that polls). From the interpreter or from
inside an actor alike. `http-listen-with ( router xt -- listener )` takes the listener xt `( -- listener )` makes.

HTTPS (`http.tls`, a build with OpenSSL): `https-listen ( router port cert n key n -- listener )`, the certificate
(its chain, PEM) and its key from those files.

On a node per core (`start-node`), each listening on the same port, the system spreads the connections over them
(Linux, the BSDs). A connection stays with the node that took it; its handlers reach the others by messages. The
program's `args` are every node's.

Limits, each a `value` (`to` sets it): `http-headers-max` (64 KiB: a request's line and headers; more is a 431),
`http-body-max` (1 MiB; more is a 413), `http-idle-ms` (30000: a connection with no whole request in this long is
closed - kept-alive or slow, alike). `-1 http-log !` prints each request answered.

## What it speaks

HTTP/1.1 with keep-alive (1.1 keeps the connection unless it says `close`; 1.0 closes it unless it says
`keep-alive`) and pipelining (requests one after another, answered in order). A request the server can't take is
answered and the connection closed: 400 (no request line, a target not starting `/`, a header line without a name, a
`Content-Length` not a number), 431, 413, 501 for a request body in chunks (`Transfer-Encoding`: not read). Not
there: HTTP/2, ranges (206), compression.

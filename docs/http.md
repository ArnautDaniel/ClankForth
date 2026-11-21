# The http vocabulary: a web server of actors

An HTTP/1.1 server written in Forth on the `net` vocabulary (docs/net.md), built in (`lib/http.fs`) for hosts that
give `net` - the `clankforth` tool does. `USING: actors net http ;`

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

`examples/web-app.fs` runs on every core: its routes (`examples/webapp/site.fs`) on a node for each, all listening on
one port, and every request asking one actor on the first node for a count - a message across nodes, awaited.

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
  new string. `url-decode ( a n -- s len )` does the decoding.

## Answers

- `respond ( a n status req -- )`: the status line, the headers, `Content-Length`, the body (a HEAD request's
  without it). Once: twice is an error. With no `Content-Type` given, `text/plain; charset=utf-8`.
- `respond-html`, `respond-json ( a n status req -- )`: with that content type.
- `header! ( value n name n req -- )`: a header of the answer (before it), `content-type! ( a n req -- )`.
- `redirect ( url n status req -- )` (301, 302, 303, 307, 308), `not-found ( req -- )`.

A handler that returns without answering is answered 500 (and said: `http: GET /path: no answer given`).

## Routes

`<router> ( -- router )`, then a route for each method and pattern - `route ( method n pattern n xt router -- )`,
or `on-get`, `on-post`, `on-put`, `on-delete`, `on-any ( pattern n xt router -- )`. A pattern's segments match the
path's (empty ones left out: `/a//b/` is `/a/b`): `:name` any one segment, a last `*` what is left (none or more).
The first route that fits answers; HEAD takes a GET route. A path that fits routes of other methods only is a 405
with `Allow`; one that fits none is `on-missing ( xt router -- )`'s handler, or a 404.

## Serving

`http-listen ( router port -- listener )`: a listener on the port (0: a free one; `listener-port` says which,
`net-close` stops it) and its actor; then `serve` (or a loop of the host's that polls). From the interpreter or from
inside an actor alike.

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
`Content-Length` not a number), 431, 413, 501 for a body in chunks (`Transfer-Encoding`: not read). Answers have a
`Content-Length`: no chunked answers, no streaming. Not there yet: TLS, HTTP/2, WebSockets, `Date`, cookies' parsing,
static files.

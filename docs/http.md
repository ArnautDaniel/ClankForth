# The http vocabulary: a web server of actors, and a client

An HTTP/1.1 server (and a client: `http.client`, below) written in Forth on the `net` vocabulary (docs/net.md), built in (`basis/http/`) for hosts that
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
| `http.sessions` | sessions kept in a jsondb, signing in and out, CSRF tokens; `require-user`, `require-csrf` |
| `http.security` | security headers on every answer; CORS (`allow-origin`) |
| `http.limits` | rate limits: so many requests a client in so long |
| `http.uploads` | a form's files and fields (multipart/form-data) |
| `http.logging` | a line in a log (`logging`'s) for each request answered |
| `http.json` | a JSON API's: a body read as JSON, a value or an error answered as JSON |
| `http.live` | live views: each open page an actor, its events over a WebSocket, what changed sent (docs/live.md) |
| `http.files` | static files: a folder served, types, `Last-Modified`, 304 |
| `http.sse` | server-sent events: an answer held open, events pushed down it |
| `http.websocket` | WebSockets, each socket an actor with an owner |
| `http.tls` | `https-listen` (a build with OpenSSL) |
| `http.client` | requests to other servers, each on an actor of its own, the response awaited |
| `http.oidc` | signing in by OpenID Connect (Entra ID): PKCE, the ID token checked; `require-signed-in`, `require-role` |
| `http.ops` | `/health`, `/ready`, `/metrics`; request ids and a JSON line each request |

`examples/web-app.fs` uses them all, on every core: its routes (`examples/webapp/site.fs`) on a node for each, all
listening on one port; a count of visits and a chat room as actors on the first node, which every node's handlers
and sockets talk to; the clock as events; its page served from a folder.

## Handlers and requests

A handler is a word `( req -- )` that answers its request once (`respond` and the words after it). It runs inside a
scratch region (docs/forth.md, Tuples): the request, and every object the handler makes, is freed once it is
answered - `format`'s strings, lists, a parse's pieces need no `dispose`. What must outlive the request is
rescued: `rescue-actor` keeps it for the connection's actor (till the connection ends), `rescue-all` till `dispose`.
One sent in a message is the receiver's (docs/actors.md, An actor's objects).

- `req-method ( req -- a n )`, `req-path` (as sent: %-escapes and all), `req-query` (after the `?`, as sent; "" for
  none), `req-version`, `req-body` (its `Content-Length` bytes), `req-conn ( req -- conn )`.
- `req-header ( name n req -- a n true | false )`: by its name in any case. Said twice, its values joined with ", ".
  `req-headers ( req -- m )`: the map, its names lower-cased.
- `req-param ( name n req -- a n true | false )`: what the route's `:name` matched, decoded; `"*"` what its `*` did.
- `query-param ( name n req -- s len true | false )`: a name's value in the query string, decoded (`+` a space): a
  new string. (`urls.encoding`'s `url-decode` does the decoding: docs/libraries.md.)
- `form-param ( name n req -- s len true | false )`: the same of a form's body - an HTML form's POST, its
  `Content-Type` `application/x-www-form-urlencoded` (false for a body of another type). A JSON body is `req-body`
  given to `json`'s `json>`.
- `req-data ( req -- x )`: what its route was given (`route-with`).
- `req-attr ( name n req -- x true | false )`, `req-attr! ( x name n req -- )`: what the request keeps by a name
  till it is answered - its session (`http.sessions`), the user a check before the handler found.
- `req-gone? ( req -- flag )`: the client has gone - what a handler that streams, or waits long, asks (its
  connection's `net-closed` is kept till the handler is done).

## Answers

- `respond ( a n status req -- )`: the status line, the headers, `Date`, `Content-Length`, the body (a HEAD
  request's without it). Once: twice is an error. With no `Content-Type` given, `text/plain; charset=utf-8`.
- `respond-html`, `respond-json ( a n status req -- )`: with that content type (`json`'s `>json` makes the text).
- **gzip**: an answer's body of `http-gzip-min` bytes or more (1024; 0: never) - text, JSON, JavaScript, XML, SVG,
  not an image's or an archive's - goes gzipped to a client whose `Accept-Encoding` takes it (`Content-Encoding:
  gzip`, `Vary: Accept-Encoding`): a page of HTML is an eighth of itself. Static files too (`http.files`). Not a
  streamed answer, nor one whose handler said a `Content-Encoding` of its own.
- `header! ( value n name n req -- )`: a header of the answer (before it), `content-type! ( a n req -- )`.
- `redirect ( url n status req -- )` (301, 302, 303, 307, 308), `not-found ( req -- )`.

A handler that returns without answering is answered 500 (and said: `http: GET /path: no answer given`).

### Streamed

`respond-start ( status req -- )` sends the head; then `respond-chunk ( a n req -- )` a piece of the body at a time -
the handler may wait between (`await`, `sleep-ms`) - and `respond-end ( req -- )` (a handler that returns streaming ends
it so). To HTTP/1.1 the body goes in chunks (`Transfer-Encoding: chunked`, the connection kept after); to 1.0, up to
the connection's close. A writer faster than its client can watch `req-conn net-pending` (bytes not yet sent) - or
say `respond-drained ( req -- )` after each piece: the handler waits there while more than `http-stream-ahead` bytes
(256 KiB) of its answer are still to go, so what waits for a slow client is bounded, whatever the answer's size.

A body whose length is known before it is made - an export counted first, a file - starts with
`respond-start-sized ( length status req -- )`: the head says `Content-Length`, the pieces (`respond-chunk`) go as
they are, to 1.0 and 1.1 alike, and the connection is kept after. The pieces must come to that many bytes.

```forth
: export ( req -- )   \ rows sent as they are read (rows-from, row>csv: the program's own)
    {: r :}  s" text/csv" r content-type!  200 r respond-start
    0 begin  dup 500 rows-from dup length  while  [: row>csv r respond-chunk ;] each  r respond-drained  500 +  repeat
    2drop ;
```

### Server-sent events (`http.sse`)

```forth
: news ( req -- )  {: r :}  r respond-events
    begin  1000 await-within-ms headline if  {: s :}  s string s" headline" r send-event  s dispose  then
    r req-gone? until ;
```

`respond-events ( req -- )` begins the answer (`text/event-stream`, not cached); `send-event ( data n name n req -- )`
sends one (name "" for a plain message; each line of the data a `data:` line); `send-ping ( req -- )` a comment, to
keep what is between from closing an idle connection. In the browser, `new EventSource(url)`.

## Hooks: before every handler, and after

What every request, or every request under a path, goes through - a check, a header, a log - is a hook on the
router, not a line at the top of each handler:

- `before-each ( xt router -- )`, `before-under ( prefix n xt router -- )`: xt `( req -- continue? )` runs before the
  handler of each request (under the prefix: `/admin` is `/admin` and `/admin/...`, not `/administrator`), in the
  order they were added. One that gives false has answered (a 401, a 429, a redirect): no handler runs, nor the
  hooks after it. The prefix is matched against the path as the routes see it - its segments, the empty ones left
  out, each %-decoded - so `//admin/x`, `/admin//x`, `/%61dmin/x` and `/admin%2Fx`, which reach `/admin/x`'s
  handler, go through `/admin`'s hooks too. One may add a header (`header!`) every answer gets, or keep something for the handler (`req-attr!`).
- `after-each ( xt router -- )`: xt `( req -- )` after each handler - the request answered by then: a log's, a count's
  (`req-status ( req -- status )` the answer's status).
- `before-each-with ( xt x router -- )`, `before-under-with ( prefix n xt x router -- )`, `after-each-with`: the same,
  xt given x too (`( req x -- continue? )`): a hook with data of its own - a limiter, a store.
- `client-address ( req -- a n )`: the client's address (a new string, no port). Behind a proxy of your own (nginx,
  Caddy) set `true to http-trust-proxy`: then the last address of `X-Forwarded-For` - the one the proxy added (those
  before it are what the client sent: anything). Only behind one proxy, as anyone can send that header.

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

## Sessions (`http.sessions`)

A session is a random id in a cookie (`sid`: HttpOnly, SameSite=Lax, Secure, Path=/), what it holds kept in a jsondb
collection (docs/jsondb.md) - so it outlives a restart, and every web node sees it (each reading its replica). The
database keeps the id's SHA-256, not the id: a copy of it gives no cookie to use.

```forth
db <session-store> constant sessions          \ (db: the database, or the node's replica of it)
: sign-in ( req -- )
    {: r | s :}  r sessions session to s
    ... check the password (crypto.passwords) ...
    user-id s log-in  s" /" 303 r redirect ;
: home ( req -- )
    {: r :}  r sessions session session-user if  ( id n ) ...  else  s" /sign-in" 303 r redirect  then ;
```

- `<session-store> ( db -- store )`: kept in the collection "sessions", the cookie "sid", two weeks unused (each use
  with under a week left makes it two weeks again), Secure; `>>coll`, `>>cookie`, `>>max-age` (seconds), `>>secure`
  change them. A browser takes a Secure cookie from `localhost` over plain HTTP too.
- `session ( req store -- sess )`: the request's session - read once from its cookie and kept for the request
  (`req-attr`); one with nothing in it if it has none, or its time is up (deleted then). Its words wait for the
  database: they are a handler's.
- `session@ ( key n sess -- v true | false )`, `session! ( v key n sess -- )` (a number, or what `json` writes: a
  string object, a map ...), `session-string! ( a n key n sess -- )`, `session-forget ( key n sess -- )`. A change is
  written at once; the first one gives a session with nothing in it its id, and the answer its cookie - so before
  the answer.
- `log-in ( user n sess -- )`: the session given a new id - the old one deleted (one someone else fixed in the browser
  isn't the one signed in) - and the user kept in it. `session-user ( sess -- user n true | false )`. `log-out ( sess
  -- )`: the session deleted, its cookie forgotten.
- `csrf-token ( sess -- a n )`: the session's CSRF token (made the first time) - put it in each form, a hidden field
  `csrf` (an upload form's part `csrf`), or send it from a script as the header `X-CSRF-Token`. It is masked by a pad
  new each time (other bytes in every page, the same token): a page's size compressed tells nothing of it (BREACH).
  `csrf-ok? ( req sess -- flag )`: the request sent it (compared all the way: `same-bytes?`) - check it before any
  POST, PUT or DELETE changes anything. `log-in` forgets the session's token: one seen before the sign-in isn't the
  user's.
- `sweep-sessions ( store -- n )`: the sessions whose time is up deleted (a timer's, an hour or a day apart).

Guards (hooks of `http.sessions`'s):

- `require-user ( prefix n store router -- )`: requests under the prefix only for a user signed in - else answered
  401, `{"error":"sign in first"}` (an API's: the front end goes to its sign-in).
- `require-csrf ( store router -- )`: every POST, PUT, PATCH and DELETE only with the session's CSRF token - else
  answered 403.

## Security headers and CORS (`http.security`)

- `security-headers ( router -- )`: every answer with `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
  `Referrer-Policy: strict-origin-when-cross-origin`, `Cross-Origin-Opener-Policy: same-origin` and
  `Content-Security-Policy: default-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'` (the page
  loads only from its own site). `security-headers-with ( csp n hsts router -- )`: a policy of your own ("" none) and
  HSTS's seconds - a site on HTTPS gives `31536000` (a year: the browser uses HTTPS only); 0 none.
- `allow-origin ( origin n router -- )`: a front end served from elsewhere - `http://localhost:5173`, its dev server;
  `https://app.example.org` - may call the site from its scripts and read the answers, its cookies sent
  (`Access-Control-Allow-Origin` that origin, `-Credentials: true`, `Vary: Origin`); a preflight (`OPTIONS` asking
  which methods) answered 204 here. Once for each origin (one hook holds them all: an origin named wins over `"*"`,
  each header said once; `Vary: Origin` on every answer to a request with an Origin). `"*"` any origin - but without
  cookies.

## Rate limits (`http.limits`)

- `rate-limit-under ( prefix n count seconds router -- )`: requests under the prefix so many in so many seconds from
  each client address (`client-address`) - more answered 429, with `Retry-After`. `s" /login" 10 60 site
  rate-limit-under`: ten sign-ins a minute an address.
- `<rate-limit> ( count seconds -- limiter )`, `rate-ok? ( key n limiter -- flag )` (one more for the key: within the
  limit?), `retry-after ( key n limiter -- seconds )`: by a key of your own - an account, an email: password resets an
  hour.
- A fixed window each key. Each node counts its own: a client whose connections the system spreads over N nodes gets
  up to N times the count.

## A JSON API (`http.json`)

```forth
: make-post ( req -- )
    {: r :}  r req-json 0= if  s" not JSON" 400 r respond-error exit  then  {: v :}
    s" title" v json-string@ 0= if  s" a title, please" 422 r respond-error exit  then  ( title n ) ... ;
```

- `req-json ( req -- v true | false )`: the body read as JSON (false: none, or not JSON). Read its fields with
  `json-string@`, `json-int@` ... (docs/libraries.md, json): each false for a field missing or of another kind.
- `respond-value ( v status req -- )`: a value as the answer's JSON body. `respond-error ( a n status req -- )`:
  `{"error": the text}`, with that status - 400 for what can't be read, 401/403 for who may not, 404, 409 for a clash
  (a unique index's), 422 for what doesn't make sense.

## Uploads (`http.uploads`)

A form with `enctype="multipart/form-data"` (a file input) sends its fields and files as parts:

- `req-parts ( req -- l true | false )`: the parts (a list, read once and kept for the request); false if the body
  isn't multipart/form-data or isn't whole. `req-part ( name n req -- part true | false )` the first of that name;
  `part-text ( name n req -- a n true | false )` a plain field's value.
- `part-name`, `part-type` (`text/plain` if it said none), `part-data ( part -- a n )` (its bytes, binary as they
  came), `part-filename ( part -- a n true | false )` - the name the browser gave it: show it, never write to it (name
  the file yourself: `uuid4`).
- The body is read whole before the handler runs: `http-body-max` (1 MiB) is the most - a site that takes files
  raises it (`10485760 to http-body-max`) and checks each part's size and type.

## Logging (`http.logging`)

`access-log ( logger router -- )`: a line in the log (`logging`'s `log-open`: docs/libraries.md) for each request
the router answers - `2026-11-22T09:05:00Z 203.0.113.9 GET /posts 200 3ms` (the client's address, the method, the
path, the status, the time its hooks and handler took). Add it first, so it times the other hooks too. One logger
for the site: open it on one node, register its name, and give each node's router `s" access" whereis`.

## Stopping (draining)

Told to stop - SIGTERM from a service manager, Ctrl-C - a server finishes what it is answering:

```forth
site 8080 http-listen constant listener
[: listener net-close  10000 http-drain ;] on-shutdown      \ (system's: each node its own)
serve
```

- `on-shutdown ( xt -- )` (system's: docs/forth.md): xt runs in this Forth's loop when the process is told to stop;
  then the loop ends - `serve` returns. A node with words of its own runs them and ends; the main loop waits for
  those (30 s at most). A node with none - a database's - runs on till the process ends, for those still asking it.
- `http-drain ( ms -- )`: each connection closed after the answer it is giving, and the loop run till no request is
  being answered here (`http-in-flight`), or ms have gone by. `http-draining` is set from then.

## Static files (`http.files`)

- `serve-files ( dir n prefix n router -- )`: GET (and HEAD) of `prefix/...` answered by the file of that path in the
  folder - a folder's `index.html`; prefix "" for the root. A path with a `.` or `..` segment, a backslash or a NUL
  (as sent or %-escaped) is a 404: nothing outside the folder. So is a hidden one (`.git/`, `.env` - a deploy's
  leavings), but `.well-known/` (ACME's challenges, `security.txt`). A symbolic link in the folder is followed, as
  nginx does: whoever can make one there chose what it shows.
- `respond-file ( path n req -- )`: a file as the answer (in a handler of your own).
- **A long file is never read whole**: one over `http-file-whole-max` (1 MiB) goes a piece at a time (64 KiB), as
  fast as its client takes it - a download of gigabytes costs the server a piece and what the client hasn't taken
  yet. A shorter one is read whole, and gzipped as any answer is.
- **A part of a file** (`Range: bytes=...`: a download gone on with, a video sought in): 206 with `Content-Range`,
  the bytes from a place to another, from a place on, or its last so many; 416 for a part the file has none of.
  `If-Range` with the file's date gives the part only of the file the client has a part of - else the whole file.
  Every file's answer says `Accept-Ranges: bytes`. Several ranges at once, or one not of bytes, are passed over:
  the whole file (as a server may).
- Each with its type by its name (`mime-type ( path n -- a n )`: html, css, js, json, svg, png, jpeg, gif, webp, ico,
  wasm, fonts, pdf, xml, mp4, webm, mp3 ...; `application/octet-stream` else), `Last-Modified`, and 304 (no body) to a
  request whose `If-Modified-Since` is as late.

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
  the owner (`nobody`: refused, answered 403). A request that is no handshake is a 400; a version but 13, 426.
- Only this site's pages: a browser opens a socket to any site with that site's cookies, whatever page asks
  (WebSockets have no CORS), so a handshake whose `Origin` is another site's is refused (403) before xt runs.
  `ws-allow-origin ( a n -- )` allows one, whole as the browser says it (`s" https://app.example.org"
  ws-allow-origin`, each node). A handshake with no `Origin` - no browser's - is let through.
- The owner hears `ws-opened ( ws -- )`, `ws-text ( s ws -- )` and `ws-binary ( s ws -- )` - s a new string, the
  owner's to `dispose` - and `ws-closed ( ws -- )`. `ws` is the socket's address.
- `ws-send ( a n ws -- )`, `ws-send-binary`, `ws-close ( ws -- )`, `ws-close-with ( code ws -- )`: from any actor, on
  any node (a socket gone: nothing).
- The socket is pinged every `ws-ping-ms` (30000); one that has sent nothing - not even the pong - by the next ping is
  closed (1001): a socket whose other end went without a word doesn't stay. A node's sockets are pinged together, a
  thirty-second of them at a time (net's `after-together`): thousands held wake the node some once a second for
  their pings, not once for each.
- The socket answers pings, puts a message's frames together, and closes for what is wrong (a frame not masked:
  1002; text that isn't UTF-8: 1007; a message over `ws-message-max`, 1 MiB: 1009). It watches its owner: the owner
  gone, it closes (1001).
- **Compressed messages** (permessage-deflate, RFC 7692) with a client that offers them - every browser does: each
  message on its own (no window kept between them, either way); a message of `ws-deflate-min` bytes or more (128)
  sent compressed, one the client compressed inflated as it comes. `false to ws-deflate`: none.

## Serving

`http-listen ( router port -- listener )`: a listener on the port (0: a free one; `listener-port` says which,
`net-close` stops it) and its actor; then `serve` (or a loop of the host's that polls). From the interpreter or from
inside an actor alike. `http-listen-with ( router xt -- listener )` takes the listener xt `( -- listener )` makes.

HTTPS (`http.tls`, a build with OpenSSL): `https-listen ( router port cert n key n -- listener )`, the certificate
(its chain, PEM) and its key from those files.

On a node per core (`cores ( -- n )`, `start-node`), each listening on the same port, the connections are spread
over them: by the system where it can (Linux, the BSDs), else (Windows) by the node that bound the port, which
hands them round in turn. A connection stays with the node that took it; its handlers reach the others by messages.
The program's `args` are every node's.

Limits, each a `value` (`to` sets it): `http-headers-max` (64 KiB: a request's line and headers; more is a 431),
`http-body-max` (1 MiB; more is a 413), `http-idle-ms` (30000: a connection with no whole request in this long is
closed - kept-alive or slow, alike), `http-header-lines-max` (100 header lines; more is a 400), `http-pending-max`
(1 MiB: a connection's answers waiting to go - past it the requests after wait till the client has read them: one
that pipelines and doesn't read holds no more), `http-connections-max` (10000 on a node at once: one more is answered
503 and closed). What a connection has come and not been read is bounded too: a request's head and body, at most.
`-1 http-log !` prints each request answered.

## What it speaks

HTTP/1.1 with keep-alive (1.1 keeps the connection unless it says `close`; 1.0 - answered `Connection: keep-alive`
when kept - closes it unless it says
`keep-alive`) and pipelining (requests one after another, answered in order). A client that shuts its sending side
after its requests (`nc -N`, some proxies) is still answered. A 204 or a 1xx goes without a length or a type. A request the server can't take is
answered and the connection closed: 400 (no request line, a target not starting `/`, a header line whose name isn't a
token - a space before its colon -, a `Content-Length` not decimal digits, a CR or LF not as CR LF, a NUL, more than
`http-header-lines-max` header lines), 431, 413 - what a proxy in front might read otherwise isn't read at all
(request smuggling). `header!` refuses a CR, LF or NUL in a header's name or value: a request's text put in a header
can't add one of its own. A body in chunks (`Transfer-Encoding: chunked`) is read and put together
as one with a `Content-Length` (its extensions and trailer fields passed over; `http-body-max` its data); another
transfer coding is a 501, and both a `Transfer-Encoding` and a `Content-Length` a 400. A request that says `Expect:
100-continue` is told `100 Continue` once its head is in, before its body comes. Answers are gzipped for a client
that takes it (Answers, above); a request's body isn't (a `Content-Encoding` on one is the handler's to read). Not
there: HTTP/2; ranges of anything but a file (`http.files`: 206, 416).

## The client (`http.client`)

Erlang's `httpc`: a request is a connection on an actor of its own, which reads the response and sends it to the
actor that asked - who `await`s it, its other messages kept meanwhile. So it is asked from a handler (an actor's: one
that can wait), and other actors go on while it waits.

```forth
USING: http.client ;
behaviour weather
  on look ( -- )
      s" http://api.example/today" http-get {: res :}
      res res-status 200 = if  res res-body ...  else  res res-error type  then ;
end-behaviour
```

- `http-get ( url n -- res )`, `http-post ( body n type n url n -- res )` (the body of that `Content-Type`), and
  `http-request ( method n url n headers n body n -- res )`: more headers each a line ending CR LF (`s\" Accept:
  application/json\r\n"`, or ""), a body or "".
- `res-status ( res -- n )`, `res-body ( res -- a n )`, `res-header ( name n res -- a n true | false )` by its name
  in any case, `res-headers ( res -- m )` (the map, names lower-cased). The response is the handler's, as a message's
  objects are: freed as it ends unless rescued (docs/actors.md, An actor's objects).
- **No response is status 0**, and `res-error ( res -- a n )` says why (no connection, the name not found, a response
  said wrong or too long, none in `http-timeout-ms`): not an error thrown, as Erlang gives `{error, Reason}`. A URL
  not `http://` or `https://` is one of these too.
- `https://` in a build with TLS: the server's certificate checked against the trusted ones (net's `tls-ca!`) and the
  host. A redirect is a response like another (its `Location` by `res-header`).
- What it sends: HTTP/1.1, `Host`, `User-Agent: clankforth/<version>`, `Connection: close` (a connection for each
  request), a body's `Content-Length`. What it reads: a body by `Content-Length`, in chunks, or till the close; a 1xx
  before the answer passed over; none for HEAD, 204, 304.
- Limits, each a `value`: `http-timeout-ms` (30000: the request fails with no whole response in this long),
  `http-response-max` (16 MiB: a response's head and body).

## Operating it (http.ops)

`USING: http.ops ;`

- `ops-routes ( router -- )`: `GET /health` (200 while the server runs), `GET /ready` (200, or 503 while draining or
  while a `ready-check ( xt -- )` says false - xt `( -- ok? )`: the database there ...), `GET /metrics` (this node's
  numbers as JSON: requests, 5xx answers, average and slowest latency, actors, objects, uptime, and each `metric (
  xt name n -- )` added - xt `( -- n )`).
- `request-log ( router -- )`: each request given an id - its `X-Request-Id`, else 16 hex digits made - said back in
  `X-Request-Id` (taken if it is 1 to 64 letters, digits, `-`, `_`, `.`; else one made), kept for the handler (`request-id ( req -- a n )`), timed, and a JSON line on stdout once it is
  answered (`"event": "request"`, its id, method, path, status, ms, client). Added first, it times the rest.
- The numbers are the node's: each node counts what it answers. `/metrics` and `/ready` say how the server is: on a
  site that faces the internet, put them under a hook (`before-under`) that lets only your monitoring in.
- A container image: `Dockerfile` at the repository's top (Debian, the tool and OpenSSL; an app's scripts at /app).
  It logs to stdout and drains on SIGTERM (`on-shutdown`, `http-drain`).

## Signing in (http.oidc)

`USING: http.oidc ;` - OpenID Connect's authorization code flow with PKCE: Microsoft Entra ID, or any provider that
speaks it. The ID token is checked (`crypto.jwt`: RS256 by the provider's keys - fetched, cached a day, fetched again
for a key they haven't - its issuer, audience, nonce and time); the user and their app roles go in the session
(`http.sessions`).

```forth
s" <tenant id>" entra  s" <client id>"  s" https://app.example/auth/callback"  sessions  oidc-config
    s" CLIENT_SECRET" getenv drop rot oidc-secret  constant sign-in
sign-in site oidc-routes                          \ GET /auth/login (?next=/path), /auth/callback, /auth/logout
s" /" sign-in site require-signed-in              \ signed in, or a GET sent to sign in and back (401 else)
s" /admin" s" Admin" sign-in site require-role    \ the role too: 403 without it
```

- `entra ( tenant n -- url n )`: Entra's discovery URL for the tenant - its id or domain, or `common` /
  `organizations` for a sign-in open to every organization's users: then the provider's issuer has `{tenantid}` in
  it, and a token's own `tid` goes there before its issuer is checked; `tenant` in the user says which organization
  (an app open to some only checks it). `oidc-config ( discovery n client n redirect n
  store -- cfg )`; `oidc-secret ( a n cfg -- cfg )` (a confidential client's; PKCE always), `oidc-scopes` (`openid
  profile email` by default).
- A token the callback can't take (another issuer, audience, an expired one) is answered 400, saying why.
- `signed-in-user ( req cfg -- user | 0 )`: a map - `id` (Entra's oid, else sub), `name`, `email`, `tenant` (tid),
  `roles` (the app
  roles assigned to them: the token's `roles`). `has-role? ( role n req cfg -- flag )`, `user-role? ( role n user --
  flag )`.
- **Live events are endpoints too**: a view's `guard:` word sees each event's name first and refuses what the user
  may not do - the page's mount puts the user's roles in its state (its `request:` word reads them from the session).
- The sign-in under way (its state, PKCE verifier, nonce, where to go after) is a signed cookie, `oidc_flow` (HttpOnly,
  Secure, SameSite=Lax, under `/auth/`, ten minutes): nothing is written to the session store till someone has
  signed in, so `/auth/login` asked a million times fills nothing. It is signed by `oidc-key ( a n cfg -- cfg )` - 32
  random bytes, the same on every node of the site (a login on one, its callback maybe on another) - else by the
  client secret's hash, else by a key of the node's own (one node only). A callback whose state isn't the cookie's
  is refused (400), as is a cookie changed or too old, and a token whose nonce isn't the sign-in's. Where to go after
  (`next`) is a path of the site only: letters, digits and `-._~/?=&%+,;`, not `//` - else `/`.
- The ID token must have an `exp` (`crypto.jwt` refuses one without: a token has to end).


# Live views (`http.live`)

Each open page is an actor (Erlang's Nitrogen, Elixir's Phoenix LiveView). Its state lives on the server; the browser
sends it events over a WebSocket; after each, it renders again and sends **only the parts of the page that
changed**. Anything the page actor hears - a database's change, another actor's message - is handled the same way.
No JavaScript framework, no build step: the browser runs `live.js`, a page of script the server gives.

```forth
USING: actors http html json jsondb http.live ;

: task-item ( record -- )   \ (a query's record: {"id", "rev", "value"})
    {: r :}  s" value" r json-map@ drop {: v :}
    <li  s" title" v json-string@ drop text
        <button  s" claim" live-click=  s" id" r json-string@ drop live-value=  s" Claim" text  button>  li> ;
: show-tasks ( -- )  <ul  s" tasks" state json-list@ drop [: task-item ;] each  ul> ;
: load-tasks ( -- )
    s\" {\"where\":{\"assignee\":null}}" s" tasks" db db-query json> {: q :}
    s" records" q json-list@ drop  s" tasks" state! ;
: queue-mount ( params -- )  drop  load-tasks  s" tasks" db db-watch drop ;
: queue-render ( -- )  <h1 s" Free tasks" text h1>  s" list" ['] show-tasks region ;
: claim ( id -- )   \ (a real one checks the revision: examples/queue/site.fs)
    {: id :}  s\" {\"assignee\":\"me\"}" s" tasks" id string db db-put 2drop  load-tasks ;
: queue-changed ( c id rev json -- )  2drop 2drop  load-tasks ;

live-view queue-view
    mount: queue-mount
    render: queue-render
    event: claim claim
    changed: queue-changed
end-live-view
s" /queue" queue-view site live-route
```

`examples/work-queue.fs` (its site: `examples/queue/site.fs`) is a whole one: signing in, a queue filtered as one
types and claimed with a click, a task's form checked and saved with its revision - every open screen updating as
tasks change, the pages spread over the nodes, each reading its node's replica.

## A view

`live-view name ... end-live-view` declares one - the constant `name` - naming the words it is made of:

- `mount: word` - `( params -- )`: the page opened. `params` is a map of strings: the route's `:names` and the query's
  names. Load what the page shows into its state; `db-watch` what it shows, so its changes come.
- `render: word` - `( -- )` (required): its HTML, from its state - `html`'s words, templates (docs/html.md).
- `event: name word` - `( value -- )`: an event of that name from the browser (below); `value` is what came with it (a
  string, a map of a form's fields, or null). After it, the page renders again, and the browser is answered - with
  `{"r":[]}` if nothing changed.
- `changed: word` - `( c id rev json -- )`: a collection the page watches changed (jsondb's `db-changed`; json 0 for
  deleted). After it, the page renders again.
- `info: word` - `( x -- )`: `live-tell ( x page -- )` from any actor (a chat room, a job done). After it, the page
  renders again.
- `timer: word` - `( timer -- )`: a timer the page started (net's `after`, in its mount or an event) has fired.
  After it, the page renders again. (The page's own wait for its script to join is told apart.)
- `guard: word` - `( name n -- ok? )`: every event asked of first, by its name - false refuses it (not run, the
  browser answered with nothing changed). A view without one takes every event it has a word for, from whoever
  holds its page. An event is an endpoint: check the user's role here, as a route's is (their
  roles in the page's state, from its mount).
- `state-field: name` (outside a view, before it): declared page state - `name@ ( -- v )` (json-null till set) and
  `name! ( v -- )`, so a misspelt field is an unknown word as it compiles, not a key quietly another. json-null is
  true to `if`: a field of one kind reads as that kind's nothing till it is set - `state-int: name` (`name@ ( -- n
  )`, 0), `state-text: name` (`( -- a n )`, ""), `state-flag: name` (`( -- flag )`, false), each `name!` taking the
  same.
- `request: word` - `( req params -- continue? )`: run in the page's GET, before the page is made - what the page
  needs of its request put in `params` (the user its session has: `r sessions session session-user if ... s" user"
  params s-at! then`); or the request answered here (a redirect to sign in) and false: no page.
- `layout: word` - `( content-xt -- )`: the document round the page (its head, styles, scripts) - it calls
  `live-script` in its head and `content-xt execute` where the page goes. Without one: a plain document, its title
  the state's `title`.

`s" /path" view router live-route`: the view's pages at that path (a route's pattern: `/tasks/:id`). The router is
given `/live` (the pages' sockets) and `/live.js` (their script) too. The router's hooks (`require-user`, ...) run
for the page's GET and its socket's.

## Its state, its words

In the view's words (its page actor's handlers):

- `state ( -- m )`: what the page shows, a map - read it with `json-string@`, `json-int@`, `json-list@` ...
- `state! ( v key n -- )`, `state-string! ( a n key n -- )`: a value kept in it - a number, or an object json reads
  and writes (a query's records, a map). The page keeps it from then; **the one it replaces is let go, all of it**:
  keep nothing of that.
- `region ( id n xt -- )`: a part of the page sent alone when it changes - `<div id="id">` holding what xt writes.
  After each event, every region is rendered and compared with what was last sent; those that changed are sent. A
  page with no regions sends its whole body when it changes. Regions don't nest.
- `live-redirect ( url n -- )`: the browser sent elsewhere after this event (a new page: after a save, say).

Everything rendered is escaped unless `raw` (docs/html.md).

## Events

Attributes on the page's elements say what the browser sends (in a template, written as they are; in Forth,
`live-click=` ...):

- `live-click="name"` - a click on the element (or inside it) sends the event `name`; its value the element's
  `live-value="..."` - or, with `live-value-x="..."` attributes, a map of them (`{"x": ...}`); else null.
- `live-submit="name"` on a form - its submit sends `name` with the form's fields (a map; a name given twice, a list
  of its values; a checkbox not ticked, `"off"` - so `model-form` can turn one off). The page doesn't reload.
- `live-change="name"` on a form - a change to any of its fields (as one types: 200 ms after the last key) sends `name`
  with its fields: filters, a search box.

A field the user is typing in keeps its value and caret while the region round it is swapped.

## How it runs

1. The GET spawns the page's actor; it mounts and renders, and the whole page is the answer - shown at once, without
   the script. The page carries its actor's address and a random secret (`data-live-page`, `data-live-secret`).
2. `live.js` opens a WebSocket to `/live?p=<page>&s=<secret>` - its `Origin` must be the site's (a socket from another
   site's page is refused, 403). Before the upgrade the page is asked (`live-check`) whether the secret is its own:
   an actor that isn't a live page, or a secret that isn't the page's, is refused (403) - a socket can't be bound to
   any actor by its address. The script sends the secret first too: the socket is that page's from then. The page's
   GET answer is `Cache-Control: no-store`. A page whose script
   doesn't join in `live-join-ms` (60 s: a bot's) goes; one whose mount hasn't rendered in `live-mount-ms` (10 s) is
   killed, its GET answered 500. The socket may land on another node than the page: its
   owner is the page, wherever it is.
3. Events go in, the regions that changed come back: `{"r": [["id", "<html>"], ...], "redirect": "..."}`.
4. The socket closed (the tab gone), the page actor ends - and with it what it watched. The browser that loses its
   socket (the server restarted) reloads the page once the server answers again: a new page, a new actor, its state
   from the database.

A page actor costs its state and its socket: of the HTML its regions last sent it keeps only a SHA-256 each (32
bytes), enough to tell what changed. What goes over the socket is compressed (permessage-deflate) - a region of a
large table an eighth of itself. A render is
a word adding to a string: microseconds for a business screen. Keep the words short: a page actor shares its node
with the others (docs/actors.md).

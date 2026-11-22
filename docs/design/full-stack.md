# Full stack: sequences, pages, live views (a design)

**Status: being built.** Part 1 (sequences) is built; the rest is the plan. When a part is built, its reference moves to the usual
docs (forth.md, http.md, ...) and this file records why.

## What we are building toward

A business app, written in ClankForth end to end:

- **tables** of jsondb records: filtered, sorted, paged;
- **detail and edit forms**: validation messages beside the fields, saves that can't silently overwrite someone
  else's (revisions);
- **work queues**: records picked by their JSON fields (status, assignee, priority...), claimed, moved through states -
  every screen showing one updating as it changes, without a reload.

Shaped by what ClankForth already is: lean, actor-first, easy for a person or a model to read and write. **No
JavaScript framework, no build step, no npm.** The browser gets server-rendered HTML and one small script of ours.

The model is Erlang's Nitrogen and Elixir's Phoenix LiveView: **each open page is an actor**. Its state lives on the
server; the browser sends it events over a WebSocket; it answers with the parts of the page that changed. That is
the one idea everything else serves - and an actor per page is what ClankForth does best.

Four parts, in the order they would be built:

1. **Sequences**: one protocol over lists, strings and byte buffers, as Factor has it - and with it, strings that grow,
   and a way to build one.
2. **Pages**: HTML written as Forth words, or as templates compiled into Forth words.
3. **Live views**: the page actor, its events, its updates; the script in the browser.
4. **jsondb, for queues**: queries by several fields and comparisons, sorting; and changes told to whoever watches.

Then an example app proves it: a work queue.

---

## 1. Sequences

### Today

Three kinds that hold items in order, each with words of its own:

| kind | holds | words |
|---|---|---|
| list | cells | `length nth nth! push pop`, `each map filter reduce find any? all? count contains?`, `sort` |
| string | bytes (UTF-8 text) | as `addr len`: `s+ s-split s-join s-find s-slice s-trim s-starts? s-upper` ...; not growable |
| bytes | bytes | `b-push b-append b@ b! b-length b-resize b-clear b>string` |

So `map` works on a list but not a string; appending to a string makes a new one each time (`s+`); the code that
builds HTML today (http, json) builds a `<bytes>` by hand and converts it at the end.

### Proposed: one protocol, Factor's

A **sequence** is a list, a string, or a byte buffer (later perhaps more: a range, a slice). Every sequence word takes
any of them, and what it makes is **the same kind as what it was given** - `map` over a string gives a string,
`append` of two lists a list (Factor's `like`).

**The protocol** (in C, by the object's kind - the rest is built on these):

| word | effect | |
|---|---|---|
| `length` | `( seq -- n )` | |
| `nth` | `( i seq -- x )` | a list's cell; a string's or a byte buffer's byte. Negative counts from the end. |
| `nth!` | `( x i seq -- )` | |
| `push` | `( x seq -- )` | one item onto the end (a byte to a string) |
| `pop` | `( seq -- x )` | |
| `new-like` | `( seq -- seq' )` | an empty one of the same kind |
| `clear` | `( seq -- )` | |

**On it** (in the prelude, any kind):

- walking: `each map filter reduce find find-index any? all? count` (as today's, generalized), `each-index`,
  `map-index`;
- looking: `index ( x seq -- i | -1 )`, `contains?`, `first last`, `subseq-index ( seq sub -- i | -1 )` (a string's
  `s-find`), `starts? ends?`, `= ` of sequences by items (`seq= ( a b -- flag )`);
- making: `append ( a b -- c )` (new), `append! ( b a -- )` (b onto the end of a, in place), `concat ( seqs -- seq )`,
  `join ( seqs sep -- seq )`, `split ( seq sep -- list )`, `head tail subseq ( seq from to -- seq' )`, `reverse`,
  `replace ( seq old new -- seq' )`, `trim`;
- ordering: `sort sort-by` (in place, as today; strings by their bytes).

**Strings are sequences of bytes.** A string's items are its bytes (0-255): `nth`, `map`, `filter` work on bytes, which
is right for ASCII work (case, digits, separators) and safe for UTF-8 as long as a word doesn't split a character.
Characters are a separate, explicit walk: `each-char`, `chars ( s -- l )` (the code points), `utf8-length`. (Factor's
strings are code points; ours stay bytes, so `length` and `nth` are O(1) and a string is its UTF-8, as the web
wants.)

**Strings keep their length** (as built: Factor's strings are fixed-length too, its string buffers grow). A string
object is its address - it can't move, so it can't grow: `push`, `append!` are a list's and bytes'; a string is built
(`build`) or made of others (`append`, `like` ...), and `nth!` changes one in place.

**Building a string** - Factor's `make`, which is how HTML gets written:

```forth
[: s" <li>" %  name %  s" </li>" % ;] build   ( -- s )
```

`build ( xt -- s )` runs xt with bytes being built; `% ( x -- )` appends to them - a string or bytes, or `addr len`
with `%s`; `%n ( n -- )` a number; `%c ( c -- )` a byte - and the string made of them is given back. (As built: the
build is a scratch region, its bytes found by the task's regions - so each task's is its own, a wait in the middle is
safe, an error leaves nothing behind, and what the code makes besides is freed as it ends.) Builds nest. The page
words below are written on `%`.

**`addr len` stays.** Forth's own strings (`s"`, `type`, `move`) keep their `addr len` form and the `s-` words keep
working; the sequence words are for objects. The way across: `>string ( a n -- s len )` as today, `string ( s -- a n )`.

**Compatibility.** Today's list words keep their names and meanings - they just take more kinds. `b-` and `s-` words
stay as they are. The compiler's inlined walks (`[: ... ;] each` and the like, today lists only) stay fast for lists,
and dispatch by kind otherwise.

### Open questions

- **A literal for a string object.** Today `s" text"` gives `addr len` and an object needs `>string drop`. A word for
  an object literal - `" text"` (Factor writes `"text"`) - would make sequence code read much better. Proposal: `"`
  as a parsing word, `" text"` an object.
- **Slices.** Factor's slices (`subseq` without copying) are worth having for parsing; they can come later, as a
  fourth kind, without changing the protocol.

---

## 2. Pages: HTML as Forth, or as templates

Pages are text. A page, or a part of one, is **a word that appends its HTML to the string being built** (`%`) -
no tree of nodes, no virtual DOM. A part is re-rendered by running its word again.

### In Forth

A word for each HTML element: `<div` opens the element, attribute words follow, and its content and `div>` close it.

```forth
: task-row ( task -- )
    {: t :}
    <tr  t record-id s" task-{s}" id=
        <td  s" title" t field text  td>
        <td  s" status" t field text  td>
        <td  <button  s" claim" t record-id on-click  s" Claim" text  button>  td>
    tr> ;

: queue-table ( tasks -- )
    {: ts :}
    <table  s" queue" class=
        <thead  <tr  <th s" Task" text th>  <th s" Status" text th>  <th th>  tr>  thead>
        <tbody  ts ['] task-row each  tbody>
    table> ;
```

- `<tag` (`<div <table <tr <td <form <input <button <a ...`, every element HTML has) opens; attributes come before
  its content: `id= class= href= value= name= type=` and `attr= ( value n name n -- )` for any other; `tag>` closes.
  Void elements close themselves (`<input ... />`? no: `<input` needs no closing word).
- `text ( a n -- )` appends text, **escaped** (`& < > " '`); `raw ( a n -- )` appends it as it is - the only way to
  put markup in unescaped, so a hole for someone's script has to be written on purpose. Attribute values are escaped
  too.
- Components are words. Layouts are words that take the content as a quotation:
  `[: queue-table ;] s" Work queue" page-layout`.
- Events on elements (`on-click on-submit on-change on-input`) belong to live views (part 3); on a page without one
  they are inert.

### As templates

An HTML file compiled into a Forth word when it loads (Phoenix compiles its templates the same way):

```html
<!-- task-row.html: the word task-row ( task -- ) -->
<tr id="task-{id}">
  <td>{title}</td>
  <td>{status}</td>
  <td><button live-click="claim" live-value="{id}">Claim</button></td>
</tr>
```

```html
<!-- queue-table.html: the word queue-table ( tasks -- ) -->
<table class="queue">
  <thead><tr><th>Task</th><th>Status</th><th></th></tr></thead>
  <tbody>
    {#each}<task-row/>{/each}
  </tbody>
</table>
```

- `{name}`: the field of the value on the stack (a JSON object; or a word of that name, `( v -- a n )`), escaped.
- `{#each}...{/each}` the items of a sequence, each the value inside; `{#each name}` a field's.
- `{#if name}...{#else}...{/if}`: a field there and not false, null, 0 or "".
- `<task-row/>`: another template or word, given the value (a component).
- `{{` `}}` braces; nothing else - **no Forth inside templates**. Logic lives in words; a template is HTML with holes,
  which a person and a model read at a glance.

`template" task-row.html"` (or a folder of them: `templates" views/"`) compiles each file into a word in the current
vocabulary. Errors name the file and line. Reloading (`reload`) recompiles them.

Both forms make the same text; a template can call a word and a word can call a template.

---

## 3. Live views

### The shape

```forth
live: queue-view
    : mount ( params -- )        \ the page opened: what it shows, loaded; what it watches, subscribed
        drop  s" tasks" db watch  load-tasks ;
    : render ( -- )              \ the whole page's body, from the state
        s" Work queue" h1  filter-form  tasks@ queue-table ;
    on-event claim ( id n -- )   \ a click on a live-click="claim"
        user claim-task  load-tasks ;
    on-event filter ( form -- )  \ the filter form changed (live-change)
        s" status" form-field status!  load-tasks ;
    on db-changed ( c id rev json -- )   \ someone else changed a task
        2drop 2drop  load-tasks ;
end-live

s" /queue" ' queue-view site live-route
```

- **Each open page is an actor** of that behaviour, its state (`state@`/`state!`, a map: Phoenix's assigns) its own.
  It lives while the page is open, ends when it closes.
- **The first request is plain HTTP**: `GET /queue` runs `mount` and `render` and answers the whole page - it works
  without the script, and shows at once. The page carries the script and a **signed token** (crypto.tokens' `sign`:
  the view, its parameters, the session).
- **The script connects a WebSocket** (`/live`, with the token). A page actor is spawned for it, `mount`s again (state
  never crosses the gap: what matters is in the database), renders, and from then on is the page.
- **Events**: an element with `live-click="name"` (`on-click`), a form with `live-submit` / `live-change` (its fields,
  `live-change` debounced as one types) sends `{"event": name, "value": ..., "form": {...}}`. The page actor's
  `on-event name` handler runs.
- **After every handler** the actor renders again, and sends **only the regions that changed**: each element with
  an id that `live-region` marks (`s" queue" ' queue-table live-region`) is rendered, compared with what was last
  sent, and sent if different - `{"regions": [["queue", "<table>..."]]}`. The script swaps each one in by id. No
  fine-grained diff: whole regions, by id - simple, and smooth enough for business screens.
- **Pushes**: the page actor is an actor - anything it hears (`db-changed` from jsondb, a message from another
  actor: a chat room, a job finishing) is handled the same way, and the regions that changed sent.
- **The browser keeps what the user is doing**: a region holding the focused input is swapped keeping that input's
  value and caret; scroll positions kept.
- **Navigation is links** - a new page, a new actor. `live-patch` (the URL's parameters changed in place - a filter
  in the address bar) can come later.
- **Flash and redirect**: `flash ( a n -- )` (a message shown once), `redirect ( url n -- )` (after a save).

### The script

One file, `live.js`, served by http.files - on the order of 200 lines, no dependencies:

- connect the WebSocket (the token from the page); reconnect with backoff, showing a quiet "reconnecting" state;
- send events: delegated listeners for `live-click`, `live-submit`, `live-change` (debounced), `live-value-*`
  attributes as the value;
- apply updates: swap regions by id (keeping the focused field), flash, redirect, title.

### Safety

- The WebSocket upgrade passes the router's hooks (sessions, `require-user`); its `Origin` must be the site's (no
  cross-site WebSocket); the token must be the session's (signed: `unsign`), so a page can't be opened as someone
  else, or a view not given.
- Events carry no authority: each handler checks what the user may do, as an HTTP handler would.
- Everything rendered is escaped unless `raw`.

### What it costs

A page actor per open page: its state (small), its last-sent regions (the size of the page), its socket. A thousand
open pages is a few MB. A render is a word appending to a string: tens of microseconds for a business screen.

---

## 4. jsondb, for queues

### Queries

`db-query ( query n c n db -- json n )` - in the database actor, so a filtered queue never ships every record:

```json
{ "where": { "status": "open", "priority": { ">=": 2 }, "assignee": null },
  "sort": ["-priority", "created"],
  "offset": 0, "limit": 50 }
```

- conditions on top-level fields, all of them true: a value (equal), or `{"=" | "!=" | "<" | "<=" | ">" | ">=":
  v}`, `{"in": [...]}`, `{"contains": "text"}` (a string field), `{"exists": true|false}`;
- `sort` by fields, `-` for descending; then `offset`, `limit`;
- an equality condition on an indexed field uses the index;
- the answer: `{"total": n, "records": [{"id": ..., "rev": ..., "value": {...}}, ...]}` - with each record's
  revision, for an edit's `db-put-if`, and the total, for paging.

### Watching

`db-watch ( c n db -- )`: the asking actor is told of every change to the collection from then - `db-changed ( c id
rev json -- )` (json 0: deleted) - once it lasts (after the fsync). `db-unwatch`; a watcher that dies is forgotten
(monitored). A replica tells its own watchers as the leader's lines come: a page actor watches its node's replica,
so a change reaches every node's pages from one message per node.

---

## 5. The example: a work queue

A small app that uses all of it, kept in `examples/queue/`: tasks (title, status, priority, assignee, notes);

- a queue page: filter by status and assignee (live), sorted by priority, claim with a click - every open queue
  updating as tasks are claimed or changed;
- a task page: its fields in a form, validation beside them, saved with its revision (a conflict said, not lost);
- sign-in (http.sessions, crypto.passwords); the database on a node of its own, pages on the others (replicas).

---

## Order of work

1. Sequences: the protocol in C, the words on it, growable strings, `build`/`%`, the string literal - with tests, and
   the libraries that build strings by hand (http, json, jsondb) moved onto it where it reads better.
2. Pages: element words, escaping, templates.
3. jsondb: `db-query`, `db-watch`.
4. Live views: the page actor, its route and socket, regions, events; `live.js`.
5. The example work queue.

Each a pull request with its tests and docs, as before.

## Out of scope (for now)

Fine-grained DOM diffing; client-side routing; offline use; a component library or CSS framework (the example gets a
small stylesheet of its own); file uploads from a live view (a plain form does them); internationalization.

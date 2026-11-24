# Looking into a running system

ClankForth is meant to be debugged while it runs - by a person, or by a model given a socket. Everything here is
plain text, one fact a line, so it reads the same to both.

## Getting in

The `clankforth` tool's prompt, a host's console (`forth_eval`), or a **repl** on a socket:

```forth
USING: net ;
7000 repl-listen drop     \ on 127.0.0.1 only: whoever connects runs any code (a web page can't - below)
serve                     \ (a host's own loop: forth_net_poll, as for any socket)
```

```
$ nc 127.0.0.1 7000
clankforth 0.3.0 repl: each line runs as at the prompt; `id attach` runs them as that actor, `detach`; ...
ok
2 3 + .
5
ok
```

The repl has no password: anyone who can connect to 127.0.0.1 runs code as the program - so not on a machine others
share, and nothing that forwards a port to it. A web page the machine's browser has open could reach the port too
(a `text/plain` POST needs no preflight, and its body would run line by line): a session sending a line of HTTP -
`... HTTP/1.1`, or a `Host:` header - is ended before that line runs, and the program says so.

Each line runs as one typed at the prompt does, between the frames - never inside a handler, so `deliver`,
`actors-frame`, `reload` all work - and is answered with what it printed and **a last line saying how it went**:

| last line | meaning |
|---|---|
| `ok` | done |
| `ok name#id` | done, the session attached to that actor |
| `compiling` | a definition not finished: the next line goes on with it (the line is run again with it then) |
| `error` | an error (said above it, with the words that were running); the session's stack emptied |
| `bye` | `bye` ended the session (not the system) |

Each session has a stack of its own, kept between its lines. A line can't wait (`wait`, `await`: an error): use
`actors-frame` or `deliver` to move the world on.

## Looking

```forth
.actors                  \ every actor: id, name, behaviour, state, handled, queued, objects; busy ones' waits
42 .actor                \ one, in full (below)
42 .state                \ its state's fields, by name: numbers, floats, objects as what they are ("text", list(3))
42 .mail                 \ messages kept for it while busy, then those queued
20 .recent               \ the last 20 messages delivered: frame  from -> to  kind args
.failures                \ the latest failures: who, in which handler, the error, the words running
.unhandled               \ messages a behaviour doesn't handle, counted
room .behaviour          \ what a behaviour handles, and from which parent
room said see-handler    \ a handler's code, decompiled
see word                 \ a word's
42 .actor-objects        \ the objects it holds, by kind (a count that only grows: a leak)
42 actor-bytes .         \ the bytes its objects take (.actors shows each one's in KB)
trace-on  ...  trace-off \ every message printed as it is delivered
```

`.actor`, for example:

```
actor: 42
name: room
behaviour: room < place
handles: spawned ws-opened ws-text ws-closed
state: room-state
handled: 1337
busy: no
mail: 0 kept, 0 queued
objects: 3
watchers: 7 (lobby)
links:
subscribes: tick
last failure: 12, frame 9001, in `on ws-text`: not a list: 94236
  in ws-text <- room:on ws-text
```

## Being an actor

`42 attach` makes the session's lines run **as** actor 42 - its fields (`members @`), `self`, a `send` from it -
until `detach`. (The tool's prompt says `ok room#42` while attached.) `enter` / `leave-actor` do the same for one
line. Messages can be sent to it and the world moved on to see what it does:

```forth
42 attach
members @ length .       \ its state
s" hello" >string drop  self send said   deliver     \ a message, delivered now
detach
```

## Changing it

- **A word defined again** at the prompt or in a session - `: greet ( -- ) ." hi!" ;` - is the one every caller
  gets from its next call: other vocabularies' words, actors' handlers, machine code (docs/forth.md, Reloading).
- **`reload name`** reads a vocabulary's file again: the fix in the file, taken by the running system. Behaviours
  defined again start afresh (the next message runs the new handler); a state type defined again moves its actors'
  states over by field name.
- An actor that failed and died is gone; one that lives on (an errors actor's) can be asked to `start-over`.

## A way to debug

1. `.failures` and `.actors`: who failed, who is busy, whose mail or objects keep growing.
2. `id .actor`, `id .state`, `id .mail`: what that one holds and is waiting for.
3. `20 .recent`, or `trace-on` and a frame: what was said, in what order.
4. `beh kind see-handler`: what its handler does.
5. `id attach`, try a message and `deliver`, look again.
6. Fix the word or the file (`reload`); look again.

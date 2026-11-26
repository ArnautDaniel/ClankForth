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

Each line runs as one typed at the prompt does, between the rounds - never inside a handler, so `deliver`,
`round`, `reload` all work - and is answered with what it printed and **a last line saying how it went**:

| last line | meaning |
|---|---|
| `ok` | done |
| `ok name#id` | done, the session attached to that actor |
| `compiling` | a definition not finished: the next line goes on with it (the line is run again with it then) |
| `error` | an error (said above it, with the words that were running); the session's stack emptied |
| `bye` | `bye` ended the session (not the system) |

Each session has a stack of its own, kept between its lines. A line can't wait (`await`, `sleep-ms` in a handler's sense: an error, or
done where it is): use `deliver` to move the world on.

## Looking

```forth
.actors                  \ every actor: id, name, behaviour, state, handled, queued, objects; busy ones' waits
42 .actor                \ one, in full (below)
42 .state                \ its state's fields, by name: numbers, floats, objects as what they are ("text", list(3))
42 .mail                 \ messages kept for it while busy, then those queued
20 .recent               \ the last 20 messages delivered: ms  from -> to  kind args
.failures                \ the latest failures: who, in which handler, the error, the words running
.unhandled               \ messages a behaviour doesn't handle, counted
room .behaviour          \ what a behaviour handles, and from which parent
room said see-handler    \ a handler's code, decompiled
see word                 \ a word's
42 .actor-objects        \ the objects it holds, by kind (a count that only grows: a leak)
42 actor-bytes .         \ the bytes its objects take (.actors shows each one's in KB)
trace-on  ...  trace-off \ every message printed as it is delivered
.handler-times           \ each handler that has run: behaviour, message, runs, time on the node (total, avg, max)
handler-times-reset      \ ... counted afresh from now (a load test's own)
' word timed             \ a word run: how long it took, and how many more objects are alive after it (a leak)
.objects                 \ every kind of object: how many alive, how many made
process-bytes .          \ (system) the memory the process holds, as Task Manager and top say it (0: no telling)
trim-memory              \ (system) the allocator's free memory handed back to the system, now
memory-trims .           \ (system) the times this node has done so
```

**A server larger than what it holds.** What a handler frees goes to C's allocator, which keeps it for the next
request: after a burst, `process-bytes` stays near the burst's peak though `.objects` and `actor-bytes` are back where
they were. `serve` and the nodes' loops hand that memory back when they have nothing to run (after some thousands of
objects freed, at most every two seconds); `trim-memory` does it at once. If `process-bytes` stays high after
`trim-memory`, the memory is held, not free: `.objects`, `.actors` (each actor's KB) and `.actor-objects` say by whom.

`.handler-times` is where to start when a page feels slow: the handler with the most time is first, and its max
says whether one run held the node (a waiting handler's waits aren't counted - only its turns).

**Which word, inside a handler.** A profiler sees the native backend's machine code as addresses with no names.
`CLANKFORTH_CODE_MAP=file` in the environment lists each definition's code there as it is made - where (hex), how
long (hex), its name (a quotation: `[: ;]`) - emptied as the program starts, a line written at once, its nodes' too.
With valgrind's callgrind (slow, but it counts every instruction, and needs no privileges), `tools/word-costs.py`
puts the two together:

```sh
CLANKFORTH_CODE_MAP=code.map valgrind --tool=callgrind --callgrind-out-file=cg.out ./clankforth app.fs
# ... load it (a tenth of its speed or less: a few thousand requests), then `kill` it: cg.out is written as it ends
python3 tools/word-costs.py code.map cg.out        # the words whose own code took the most
callgrind_annotate cg.out | head -40               # the C side, by function
```

A word's count is its own code's, not what it calls: a loop in Forth over every byte of something shows as its word
at the top, and the answer is a primitive that does the walk (`s-find`, `s-under`) or a table. The lines are the form
Linux's `perf` reads for code made at run time, from `/tmp/perf-<pid>.map`: name the file so (`sh -c
'CLANKFORTH_CODE_MAP=/tmp/perf-$$.map exec ./clankforth app.fs'`) and `perf top -p <pid>` shows the words.

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
last failure: 12, at 150240 ms, in `on ws-text`: not a list: 94236
  in ws-text <- room:on ws-text
```

## Being an actor

`42 busy? .` says whether a handler of its is under way - waiting for an answer or a file, or its slice spent: its
state is then half way through a change, to be read, not run through (jsondb's words from the prompt wait it out).

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
3. `20 .recent`, or `trace-on`: what was said, in what order, and when.
   `rounds` twice, a moment apart: is its loop asleep, or going round (a handler that yields over and over)?
4. `beh kind see-handler`: what its handler does.
5. `id attach`, try a message and `deliver`, look again.
6. Fix the word or the file (`reload`); look again.

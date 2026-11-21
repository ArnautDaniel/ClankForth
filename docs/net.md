# The net vocabulary: sockets and timers on actors

TCP sockets and clock timers for scripts whose host gives them (`forth_net`: the `clankforth` tool does). Each
socket is an **actor's**, and what happens on it comes to that actor as a message, like everything else an actor
hears. Nothing blocks: an event loop waits for the sockets, the timers and other nodes' messages, and the frames run
in between. `USING: actors net ;`

```forth
behaviour echo
  on spawned ( -- )  7000 listen drop ;                     \ connections to port 7000 come to this actor
  on net-accepted ( listener conn -- )  2drop ;              \ each one this actor's (net-owner! hands one on)
  on net-received ( conn -- )  {: c :}  c net-input c net-write  c net-input nip c net-consume ;
  on net-closed ( conn -- )  net-close ;
end-behaviour
echo -1 s" echo" spawn drop
serve                                                        \ the loop, until `bye`
```

`examples/hello-server.fs` is a web server in a page: each connection handed to an actor of its own, which answers
and goes.

## The messages

| message | when |
|---|---|
| `net-accepted ( listener conn -- )` | a connection came to a listener: the new connection, its listener's actor's |
| `net-received ( conn -- )` | bytes came: `net-input` has them (all not yet taken) |
| `net-closed ( conn -- )` | the other end closed, or an error ended it (after a last `net-received`); its handle names nothing after this handler's frame |
| `net-connected ( conn -- )` | `connect`'s connection is made (one that can't be: `net-closed`) |
| `timer-fired ( timer -- )` | `after`'s time has come |

An actor that doesn't handle one of them doesn't hear it (they are the kernel's kind: no "doesn't handle" said).

## The words

- `listen ( port -- listener )`: TCP on every address of this machine; port 0 takes a free one, `listener-port (
  listener -- port )` says which.
- `connect ( host n port -- conn )`: to a name or an address. The name is looked up now (that waits); the connection
  is made as the loop runs.
- `net-write ( a n conn -- )`: the bytes are kept and sent as the socket takes them. To a connection ended or closing:
  dropped.
- `net-input ( conn -- a n )`: what has come and not been taken, its address good until the next poll or
  `net-consume`. A request split over several reads is all there: take it when it is whole.
- `net-consume ( n conn -- )`: the first n bytes taken.
- `net-close ( conn -- )`: closed once what is to go has gone (a listener at once). No `net-closed` comes for it.
- `net-owner! ( actor conn -- )`: what happens on it is told to that actor from now (the listener's actor hands each
  connection to one of its own).
- `conn-peer ( conn -- s len )`: "address:port" of the other end, a new string.
- `after ( ms -- timer )`: `timer-fired` to this actor once ms have gone by (the clock's, not frames);
  `cancel-timer ( timer -- )`.
- `serve ( -- )`: the loop - the tasks' turns, the actors' frame, then a wait (16 ms at most) - until `bye`. From the
  interpreter: a program's last line.
- `net-poll ( ms -- flag )`: one wait, its events queued as messages (no delivery: `deliver` runs them). For tests, and
  for a loop of the script's own.

`listen`, `connect` and `after` are an actor's - inside a handler, or `enter`ed at the console - since what happens is
told to it. A connection whose actor is gone is closed (after sending what it was closing with), as an Erlang port
dies with its process.

## Nodes and hosts

Each node has its own sockets: a node started by `start-node` from the tool has the vocabulary too (the host's setup
gives it), and its loop waits on its sockets and its inbox at once - a message from another node wakes it as a socket
does. A server uses every core with a listener on each node, all on one port: where the system allows it (Linux, the
BSDs: `SO_REUSEPORT`) it spreads the connections over them. A connection stays on the node that accepted it.

A host with a loop of its own calls `forth_net_poll(f, ms)` where it would sleep (it waits for the sockets, the timers
and other nodes' messages; 1 if anything came), then its frame.

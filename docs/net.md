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
and goes. The `http` vocabulary (docs/http.md) is the whole of one: requests read, routed and answered.

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

- `listen ( port -- listener )`: TCP on every address of this machine, IPv6 and IPv4 (IPv4 alone where there is no
  IPv6); port 0 takes a free one, `listener-port (
  listener -- port )` says which.
- `listen-local ( port -- listener )`: as `listen`, on this machine only (127.0.0.1).
- `repl-listen ( port -- listener )`: a repl on this machine's port (127.0.0.1 only: whoever connects runs any code) -
  each connection a session whose lines run as the prompt's, answered line by line (docs/debugging.md). No actor's: the
  loop (`serve`, a host's forth_net_poll) runs its lines between frames.
- `connect ( host n port -- conn )`: to a name or an address. The name is looked up now (that waits); the connection
  is made as the loop runs - to its addresses in turn, one refused (an IPv6 one with no one there) the next tried.
- `net-write ( a n conn -- )`: the bytes are kept and sent as the socket takes them. To a connection ended or closing:
  dropped.
- `net-input ( conn -- a n )`: what has come and not been taken, its address good until the next poll or
  `net-consume`. A request split over several reads is all there: take it when it is whole.
- `net-consume ( n conn -- )`: the first n bytes taken.
- `net-close ( conn -- )`: closed once what is to go has gone (a listener at once). No `net-closed` comes for it.
- `net-owner! ( actor conn -- )`: what happens on it is told to that actor from now (the listener's actor hands each
  connection to one of its own).
- `net-ended? ( conn -- flag )`: the other end gone, an error ended it, it is closing, or the handle names nothing
  now - what a handler that is busy (waiting, streaming) asks, the `net-closed` kept till it is done.
  `net-pending ( conn -- n )`: bytes written and not yet sent (0 for one gone): a writer that keeps up waits for it.
- `conn-peer ( conn -- s len )`: "address:port" of the other end, a new string.
- `net-why ( conn -- s len )`: why an error ended it - "Connection refused", a TLS handshake that failed and why
  ("TLS: the certificate: ...", OpenSSL's reason) - or "" (the other end closed it, or it hasn't ended). What a
  `net-closed` handler asks, a new string.
- `after ( ms -- timer )`: `timer-fired` to this actor once ms have gone by (the clock's, not frames);
  `cancel-timer ( timer -- )`.
- `serve ( -- )`: the loop - the tasks' turns, the actors' frame, then a wait (16 ms at most) - until `bye`. From the
  interpreter: a program's last line.
- `net-poll ( ms -- flag )`: one wait, its events queued as messages (no delivery: `deliver` runs them). For tests, and
  for a loop of the script's own.

`listen`, `connect` and `after` are an actor's - inside a handler, or `enter`ed at the console - since what happens is
told to it. A connection whose actor is gone is closed (after sending what it was closing with), as an Erlang port
dies with its process.

## TLS

In a build with OpenSSL (`CLANKFORTH_TLS`, on where OpenSSL is found: the build says which), a connection may be
TLS's - the words and the messages are the same as for plain TCP; the handshake goes on in the loop as the bytes
come.

- `tls-listen ( port cert n key n -- listener )`: as `listen`, each connection it accepts TLS - the certificate (its
  chain, PEM) and its private key from these files.
- `tls-connect ( host n port -- conn )`: as `connect`, the connection TLS. The server's certificate is checked against
  the trusted ones and the host - its name (sent too, for the server to choose by), or its address; `net-connected`
  comes once the handshake is done, `net-closed` if it fails.
- `tls-connect-as ( host n port name n alpn n -- conn )`: as `tls-connect`, the certificate checked against `name`
  (sent too) instead of the host's - a server reached at another address than the name it answers to ("" the
  host's) - and ALPN offering `alpn`'s protocols, split by commas (`tds/8.0`, `h2,http/1.1`; "" none).
- `tls-record-max! ( n conn -- )`: what is written on the connection goes in TLS records of n bytes at most (0:
  OpenSSL's own, 16 KB) - for a protocol whose packets must each be a record of their own (SQL Server's TDS 8.0).
- `tls-ca! ( path n -- )`: the certificates `tls-connect` trusts from then, from this file (PEM); "" the system's (the
  default).

A build without OpenSSL has none of these words (`s" tls-listen" find-word` says). TLS 1.2 at least. With TLS
compiled in, `SIGPIPE` is ignored once a TLS context is made (OpenSSL writes to the socket itself).

## Nodes and hosts

Each node has its own sockets: a node started by `start-node` from the tool has the vocabulary too (the host's setup
gives it), and its loop waits on its sockets and its inbox at once - a message from another node wakes it as a socket
does. A server uses every core with a listener on each node, all on one port (`cores` says how many there are):
where the system allows it (Linux, the BSDs: `SO_REUSEPORT`) it spreads the connections over them; where it doesn't
(Windows) the node that bound the port accepts each connection and hands it to the nodes listening on it in turn
(itself among them) - each takes it as its own listener's. A connection stays on the node that accepted it. A port
is this process's alone (Windows: `SO_EXCLUSIVEADDRUSE` - another process listening on it is refused, not let in
beside it), and its listener holds a burst of connections the system's most (`SOMAXCONN`) till they are accepted.

A host with a loop of its own calls `forth_net_poll(f, ms)` where it would sleep (it waits for the sockets, the timers
and other nodes' messages; 1 if anything came), then its frame.

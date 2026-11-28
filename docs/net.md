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
| `net-closed ( conn -- )` | the other end closed, or an error ended it (after a last `net-received`); its handle names nothing after this handler's frame - but for a clean end of input (the other end shut its sending): what is written to it before then, or a `net-close` then, still goes out first |
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
- `net-view ( conn -- slice )`: what has come and not been taken, as a sequence - a slice (docs/forth.md): read by
  the sequences' words (`length`, `nth`, `subseq-index`, `<slice>`, `be>` ...) and by every word that takes text
  (`v string`). It is those bytes until they are taken (`net-consume`), more come (after a wait) or the connection
  goes: then it names nothing, and reading it is an error - ask again. **What reads a protocol reads this**: no
  address to hold past the bytes (the library's HTTP, WebSocket and SQL Server readers do).
- `net-input ( conn -- a n )`: what has come and not been taken, its address good until the next poll or
  `net-consume`. A request split over several reads is all there: take it when it is whole.
- `net-consume ( n conn -- )`: the first n bytes taken.
- `net-close ( conn -- )`: closed once what is to go has gone (a listener at once). No `net-closed` comes for it; its
  handle names nothing from then. A connection closed so has its sending shut and what still comes read and let go
  till the other end closes (2 s at most): closed at once with bytes unread, the system would reset it, and the other
  end could lose the answer before it (a 413 sent before the body).
- `net-input-max! ( n conn -- )`: what has come and not been taken, at most (64 MiB at first) - past it the socket isn't
  read till some is taken: a sender can't fill the memory faster than the program reads.
  `net-output-max! ( n conn -- )`: what is still to go, at most (64 MiB at first) - a `net-write` past it ends the
  connection (`net-why`: "more than n bytes waiting to go"), a peer that asks and doesn't read.
- `net-owner! ( actor conn -- )`: what happens on it is told to that actor from now (the listener's actor hands each
  connection to one of its own).
- `net-shut ( conn -- )`: this end has no more to send - what was written goes, then its sending is shut: the other
  end sees a clean end of what it reads, and what it sends still comes (a request whole, its answer awaited). Not a
  TLS connection's (that one's end is its close).
- `net-ended? ( conn -- flag )`: the other end gone, an error ended it, it is closing, or the handle names nothing
  now - what a handler that is busy (waiting, streaming) asks, the `net-closed` kept till it is done.
  `net-gone? ( conn -- flag )`: nothing more can be sent it - as `net-ended?`, but not so for one whose other end only
  shut its sending (a clean end: its answer still goes, and the connection is kept while its actor's handler is at
  work on it). What a handler sending a long answer asks.
  `net-pending ( conn -- n )`: bytes written and not yet sent (0 for one gone): a writer that keeps up waits for it.
- `conn-peer ( conn -- s len )`: "address:port" of the other end, a new string.
- `net-why ( conn -- s len )`: why an error ended it - "Connection refused", a TLS handshake that failed and why
  ("TLS: the certificate: ...", OpenSSL's reason) - or "" (the other end closed it, or it hasn't ended). What a
  `net-closed` handler asks, a new string.
- `after ( ms -- timer )`: `timer-fired` to this actor once ms have gone by;
  `cancel-timer ( timer -- )`. Timers fire the soonest first (two due together: the first set first); setting and
  cancelling one costs the same whether there are ten or a hundred thousand.
- `after-together ( ms step -- timer )`: as `after`, at the clock's next multiple of `step` ms once ms have gone by.
  Timers so set fire together: a thousand actors that each do a little every so often (a websocket's ping) wake the
  node once for all of them, not a thousand times.
- `net-waits ( -- s len )`: how this build waits for its sockets - "epoll", "iocp" or "poll" (Many connections,
  below). A new string.
- `serve ( -- )`: the loop - a round (what can run, runs: the tasks' turns, the actors' messages and handlers), then
  a wait for what comes - until `bye`. From the interpreter: a program's last line. The wait ends with a socket, a
  timer, a message from another node, a job done, a handler's sleep over; a quiet server sleeps (it looks up once
  a second). Nothing in it counts frames. A loop of the script's own is `begin round net-poll drop again`.
- `net-poll ( ms -- flag )`: one wait, its events queued as messages (no delivery: `deliver` runs them). For tests, and
  for a loop of the script's own.

`listen`, `connect` and `after` are an actor's - inside a handler, or `enter`ed at the console - since what happens is
told to it. A connection whose actor is gone is closed (after sending what it was closing with), as an Erlang port
dies with its process.

**Many connections.** What a round of the loop does is for the connections something happened to - a word named
them, bytes came, their actor died - not for every one held: ten thousand quiet sockets cost a round nothing. On
Linux the wait itself is `epoll`'s, so a wake costs what happened, whatever the number held (a build with
`-DCLANKFORTH_EPOLL=OFF` waits with `poll`). On Windows it is a completion port's: each connection's read is asked
of the port (a read of no bytes, ended by the system when there is something to read), and the port tells those
that ended; the few sockets it can't tell of that way - listeners, those connecting, those whose bytes didn't all
go - are polled, an event of theirs waking the port (`-DCLANKFORTH_IOCP=OFF`: `WSAPoll` for all). With `poll`
(macOS, the BSDs) the system looks at every socket at each wake (measured on Linux: 0.3 to 1.3 ms a wake for 5000
sockets) - little for a quiet server, a share of a busy one's time.

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
beside it), and its listener holds a burst of connections the system's most till they are accepted (`SOMAXCONN`; on
Windows, where that asks only for a "reasonable" 200 or so, a hint for 65535).

A host with a loop of its own calls `forth_net_poll(f, ms)` where it would sleep (it waits for the sockets, the timers
and other nodes' messages; 1 if anything came), then its frame.

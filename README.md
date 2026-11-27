# ClankForth

A small Forth written in C, built around actors. It is embedded in a host program that adds its own words: the games
[OpenHuey](https://github.com/ArnautDaniel/OpenHuey) and [OpenMizzurna](https://github.com/ArnautDaniel/OpenMizzurna),
and ClankMUD, a MUD server. It also runs on its own as a server (`clankforth`): a web stack of actors, a JSON
database, SQL Server. An improvement here (error reports, safety checks, speed) reaches every host that takes it.

## The language

- Indirect threaded, jonesforth-style, with the rest of the language in Forth (`core/prelude.fs`), built in. On
  x86-64 (Linux, the BSDs, Windows with MinGW) each colon definition is translated to machine code as well
  (subroutine threading, `src/native.c`).
- Vocabularies as in Factor: `IN:`, `USING:`, `<PRIVATE ... PRIVATE>`; a file sees only what it asks for.
- Stack effects as in Factor: `;` checks a definition's code against its stack comment `( a b -- c )` and warns
  where they differ - through words kept in slots too (`variable step  holds: ( -- )`).
- Locals (`{: a b F: x | c -- :}`), quotations that see them, lists and Factor's combinators (compiled inline);
  floats on their own stack; tuples and generic words (`TUPLE:`, `GENERIC:`, `M:`); maps, byte buffers, `sort`,
  `format`, UTF-8, `throw`.
- Objects known by their address, freed by a scratch region (`with-scratch`), with their actor, or by `dispose`.
- Tasks: each running piece of Forth has its own stacks, and its own `base`, last error and `task-variable`s;
  `yield`, `wait`; `[: ... ;] atomically` for a short look at what they share.
- Defined behaviour: sums wrap round, as Forth's do; every address a script reaches is checked.

[docs/forth.md](docs/forth.md) is the language.

## Actors and nodes

- Actors with private state and behaviours (message handlers), messages checked as they are compiled; a handler may
  `await` a reply. Monitors, links, and failures told to an errors actor - the `supervisor` library by default,
  which logs a failure and keeps the actor on (or kills one that fails too often). [docs/actors.md](docs/actors.md)
- Fair, as Erlang's: a handler runs a slice of steps, then the node's other actors have their turns and it goes on
  where it stopped - a long one (a report, a big parse) doesn't hold up the rest, nor is it stopped as a loop that
  never ends. What blocks - a file read or written, an fsync, another program, a password's hash - is done on a pool
  of worker threads while the handler waits and its node goes on; `sleep-ms` is a timed wait.
- Nodes: many Forths in one process, one per thread, their actors messaging, watching and linking to each other's
  (Erlang's way), so a program uses every core. `spawn-on` makes an actor on another node, `spawn-anywhere` on the
  least busy one.
- Or nodes as processes: `cores as-nodes` at a program's top, and it is run again for each node under a supervisor
  that starts one again when it ends - a crash takes one node, for a moment, not the program. Their actors reach
  each other the same way. `examples/nodes-server.fs`.
- Reloading: `reload name` reads a vocabulary's file again, and every caller of its words - other vocabularies',
  actors' handlers, machine code - gets the new ones from its next call. A state type defined again moves its
  actors' states over by field name.
- Looking into it while it runs ([docs/debugging.md](docs/debugging.md)): a repl on a socket (`repl-listen`), `attach`
  an actor and run as it; `.actors`, `.actor`, `.mail`, `.failures`, `see-handler` - one fact a line, for a person or
  a model. Errors print the words that were running; crashes and sanitizer reports too.

## Libraries

Built in, laid out as Factor's `basis/` ([docs/libraries.md](docs/libraries.md) has the list). Every host has the
language's own; those that need the host's `system` (files, arguments, the clock) or `net` (sockets and timers on
actors, [docs/net.md](docs/net.md)) load where the host gives them - the `clankforth` tool gives both.

- **Web** ([docs/http.md](docs/http.md)): `http`, an HTTP/1.1 server of actors - a connection each, its handlers free
  to `await` other actors, a node per core on one port. Routes and hooks, streamed answers, server-sent events,
  WebSockets (each socket an actor), static files, gzip, cookies and sessions, CSRF, rate limits, uploads, HTTPS
  (OpenSSL), a client. Signing in by OpenID Connect - Entra ID - with roles per route (`http.oidc`); health,
  readiness, metrics and JSON request logs (`http.ops`). Bounded against what the internet sends: limits on heads,
  bodies, connections and what waits to go, and a fuzzer over its parsers.
- **Pages** ([docs/html.md](docs/html.md), [docs/live.md](docs/live.md)): HTML as Forth words or templates, and live
  views - each open page an actor, its events over a WebSocket, only the regions that changed sent back.
  `examples/work-queue.fs` is a whole app.
- **Data**: `jsondb`, a JSON database that is an actor - every write fsynced, queries, indexes, watching
  ([docs/jsondb.md](docs/jsondb.md)). `sql`: SQL Server and Azure SQL from actors - TLS first, a pool, models of
  existing tables with forms and validation, watching, logins by password or Entra ID (a managed identity), locked to
  one database ([docs/sql.md](docs/sql.md)).
- **C libraries**: `LIBRARY: zlib libz.so.1` and `zlib FUNCTION: crc32 ( crc bytes n -- uint )` - a library loaded
  as the program runs, its functions words ([docs/c.md](docs/c.md)).
- **The rest**: `crypto` (SHA-256, HMAC, PBKDF2, secure random bytes, RSA signatures, JWTs), `compression` (DEFLATE,
  gzip), `json`, `base64`, `checksums`, `calendar`, `logging`, `urls.encoding`, `tester`.

## Building

```sh
cmake -S . -B build -G Ninja && ninja -C build
cd build && ctest                 # the language's tests
./clankforth                      # a prompt; ./clankforth file.fs runs one (-i: then a prompt)
(cd .. && build/clankforth examples/web-app.fs -- 8080 4)   # a web app: port, nodes
```

On a Raspberry Pi (64-bit Raspberry Pi OS) or another small board: [docs/small-devices.md](docs/small-devices.md) -
CI runs the suite on ARM64; there every word is threaded (the native backend is x86-64's).

Options: `-DCLANKFORTH_CHECKER=OFF` (no stack checker - a smaller system that reads a stack comment as a comment;
`effects-checked?` says which), `-DCLANKFORTH_FUZZ=ON` with clang (the fuzzers, docs/forth.md's Tests),
`-DCLANKFORTH_WERROR=ON` (as CI builds). A `Dockerfile` at the top builds a container for a web app (docs/http.md).

## In a host

As a submodule, built with the host:

```sh
git submodule add https://github.com/ArnautDaniel/ClankForth.git external/clankforth
```
```cmake
add_subdirectory(external/clankforth)            # the library `clankforth` (forth.h), the tool
target_link_libraries(game PRIVATE clankforth)
```

```c
Forth *f = forth_new(16 << 20);
forth_prim(f, "my-word", p_my_word);   /* the host's words */
forth_actors(f, sizeof(MyBody));       /* the actor kernel (actors.h): each actor with a body of the host's */
forth_report_crashes(f);               /* a crash names the Forth words running */
forth_prelude(f);                      /* the rest of the language */
forth_add_root(f, "scripts");          /* where USING: finds vocabularies' files */
forth_set_eval_budget(f, 50000000);    /* (if wanted) a script that loops for ever as it loads is an error */
forth_include(f, "scripts/main.fs");
/* each frame: */ forth_run_tasks(f);  forth_actors_frame(f);
```

`forth_system(f)` and `forth_net(f)` give a host's scripts the `system` and `net` vocabularies (and with them the
libraries on them). A `Forth` is a handle: what a host needs of the system has a function in `forth.h`
(`forth_compiling`, `forth_bye`, `forth_task`, `forth_current`, `forth_errors`, `forth_last_error`,
`forth_set_budget`, `forth_set_reader` ...). The system's own state is `src/forth_internal.h`, for the system's own
files. [docs/hosts.md](docs/hosts.md) lists the functions, and what a host changes to take each newer version.

A host's own tool (its tests, with its words) is `forth_tool_main(argc, argv, bind)` - see `src/tool.c`.

## Changing it

Pull requests here; then each host moves its submodule forward and runs its own tests (`tools/check-hosts.sh` runs
the hosts checked out beside this repo against this checkout). `forth.h` is the API the hosts build against: a
change to it says so in the pull request, so the hosts can adapt. CLAUDE.md has the rules. Versions are in
[CHANGELOG.md](CHANGELOG.md): before 1.0 the minor number goes up when a host or an app must change something.

## License

GPL-3.0 (LICENSE), as its hosts.

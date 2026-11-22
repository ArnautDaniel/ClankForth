# ClankForth

A small Forth for game scripting, written in C, shared by [OpenHuey](https://github.com/ArnautDaniel/OpenHuey)
and [OpenMizzurna](https://github.com/ArnautDaniel/OpenMizzurna). It is embedded in a host - a game engine - that
adds its own words; an improvement here (error reports, safety checks, speed) reaches every host that takes it.

- Indirect threaded, jonesforth-style, with the rest of the language in Forth (`core/prelude.fs`), built in; on
  x86-64 (Linux, the BSDs, Windows with MinGW) each colon definition is translated to machine code as well
  (subroutine threading, `src/native.c`).
- Vocabularies as in Factor: `IN:`, `USING:`, `<PRIVATE ... PRIVATE>`; a file sees only what it asks for.
- Stack effects as in Factor: `;` checks a definition's code against its stack comment `( a b -- c )` and warns
  where they differ - through words kept in slots too (`variable step  holds: ( -- )`: `step @ execute` is followed,
  what is stored there checked).
- Locals (`{: a b F: x | c -- :}`), quotations that see them, lists and Factor's combinators (compiled inline, the
  lists' `each map filter reduce find` too);
  floats on their own stack.
- Tuples and generic words as in Factor (`TUPLE:`, `GENERIC:`, `M:`); objects known by their address, freed by a
  scratch region (`with-scratch`) or `dispose`; strings that last, and words to build, split and read them.
- Tasks: each running piece of Forth has its own stacks, a little each frame; `yield`, `wait`.
- An actor kernel ([docs/actors.md](docs/actors.md)): actors with private state and behaviours (message handlers),
  messages checked as they are compiled; a handler may wait (`await` a reply); monitors, links, failures told to an
  errors actor. Nodes: many Forths in one process, one per thread, their actors messaging, watching and linking to
  each other's (Erlang's way), so a program uses every core.
- For programs, not only games, the vocabularies a host may give: `system` (files, arguments, other programs, the
  clock) and `net` (TCP sockets and timers on actors: [docs/net.md](docs/net.md)). On `net`, `http`: a web server
  of actors - a connection each, its handlers free to `await` other actors, a node per core on one port
  ([docs/http.md](docs/http.md), `examples/web-app.fs`): routes, streamed answers, server-sent events, WebSockets
  (each socket an actor), static files, cookies, HTTPS (TLS with OpenSSL); and a client (`http.client`). On
  `system`, `jsondb`: a JSON database that is an actor, every write fsynced ([docs/jsondb.md](docs/jsondb.md)). Maps,
  byte buffers, `sort`, `format`, UTF-8 and `throw` are the language's own; libraries in `basis/` as Factor lays
  them out - `json`, `base64`, `checksums.sha1`, `calendar`, `urls.encoding` ([docs/libraries.md](docs/libraries.md)).
- Reloading: `reload name` reads a vocabulary's file again, and every caller of its words - other vocabularies',
  actors' handlers, machine code - gets the new ones from its next call; a word defined again at the prompt does the
  same. A state type defined again moves its actors' states over by field name.
- Looking into it while it runs (docs/debugging.md): a repl on a socket (`repl-listen`) whose lines run as the
  prompt's, answered line by line with a last line saying how it went; `attach` an actor and run as it; `.actor`,
  `.mail`, `.failures`, `see-handler` - one fact a line, for a person or a model.
- Errors print the words that were running; crashes and sanitizer reports too. In a debug build, reads past
  the end of memory the host lends (`forth_guard`) are errors naming it. Sums wrap round, as Forth's do.

The language: [docs/forth.md](docs/forth.md); its actors: [docs/actors.md](docs/actors.md).

## Building

```sh
cmake -S . -B build -G Ninja && ninja -C build
cd build && ctest                 # the language's tests
./clankforth                      # a prompt; ./clankforth file.fs runs one (-i: then a prompt)
```

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

A `Forth` is a handle: what a host needs of the system has a function in `forth.h` (`forth_compiling`,
`forth_bye`, `forth_task`, `forth_current`, `forth_errors`, `forth_last_error`, `forth_set_budget`,
`forth_set_reader` ...). The system's own state is `src/forth_internal.h`, for the system's own files; a
host built against it is built against what changes without notice. [docs/hosts.md](docs/hosts.md) lists the
functions, and what each replaced for a host written when the struct was open.

Without the stack checker - a smaller system that reads a stack comment as a comment and checks nothing, for a
release or a target short of room - set `CLANKFORTH_CHECKER` off before `add_subdirectory` (or `-DCLANKFORTH_CHECKER=OFF`).
Scripts load the same; `effects-checked?` says which they are in.

A host's own tool (its tests, with its words) is `forth_tool_main(argc, argv, bind)` - see `src/tool.c`.

## Changing it

Pull requests here; then each host moves its submodule forward and runs its own tests
(`tools/check-hosts.sh` runs the hosts checked out beside this repo against this checkout, before that). `forth.h` is the API both
hosts build against: a change to it says so in the pull request, so the hosts can adapt. CLAUDE.md has the rules.
Versions are in [CHANGELOG.md](CHANGELOG.md): before 1.0 the minor number goes up when a host must change something.
What a host changes to take a newer ClankForth is in [docs/hosts.md](docs/hosts.md), step by step.

## License

GPL-3.0 (LICENSE), as its hosts.

# ClankForth

A small Forth for game scripting, written in C, shared by [OpenHuey](https://github.com/ArnautDaniel/OpenHuey)
and [OpenMizzurna](https://github.com/ArnautDaniel/OpenMizzurna). It is embedded in a host - a game engine - that
adds its own words; an improvement here (error reports, safety checks, speed) reaches every host that takes it.

- Indirect threaded, jonesforth-style, with the rest of the language in Forth (`lib/prelude.fs`), built in.
- Vocabularies as in Factor: `IN:`, `USING:`, `<PRIVATE ... PRIVATE>`; a file sees only what it asks for.
- Stack effects as in Factor: `;` checks a definition's code against its stack comment `( a b -- c )` and warns
  where they differ.
- Locals (`{: a b F: x | c -- :}`), quotations that see them, lists and Factor's combinators (compiled inline);
  floats on their own stack.
- Tuples and generic words as in Factor (`TUPLE:`, `GENERIC:`, `M:`); objects known by their address, freed by a
  scratch region (`with-scratch`) or `dispose`.
- Tasks: each running piece of Forth has its own stacks, a little each frame; `yield`, `wait`.
- An actor kernel ([docs/actors.md](docs/actors.md)): actors with private state and behaviours (message handlers),
  messages checked as they are compiled; a handler may wait (`await` a reply); monitors, links, failures told to an
  errors actor.
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
forth_include(f, "scripts/main.fs");
/* each frame: */ forth_run_tasks(f);  forth_actors_frame(f);
```

A host's own tool (its tests, with its words) is `forth_tool_main(argc, argv, bind)` - see `src/tool.c`.

## Changing it

Pull requests here; then each host moves its submodule forward and runs its own tests. `forth.h` is the API both
hosts build against: a change to it says so in the pull request, so the hosts can adapt. CLAUDE.md has the rules.

## License

GPL-3.0 (LICENSE), as its hosts.

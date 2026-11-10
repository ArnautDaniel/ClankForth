# ClankForth: notes for an AI assistant

The Forth system shared by OpenHuey and OpenMizzurna (each has it as a submodule, `external/clankforth`). Read
`README.md`, then `docs/forth.md` and `docs/actors.md`.

- **Two hosts depend on it.** `forth.h` is their API: keep it compatible, or say in the pull request what
  changed and how a host adapts (docs/hosts.md lists it). A change is done when this repo's tests pass and both
  hosts' suites do with it (`tools/check-hosts.sh`: the hosts checked out beside this repo, built against this
  checkout).
- **A `Forth` is a handle to a host**: the system's state is `src/forth_internal.h` (forth.c's and actors.c's).
  What a host needs of it gets a function in `forth.h`, not a field made public.
- **Nothing of a host here**: no game or engine words - those are the host's (`forth_prim`, its own
  vocabularies). The actor kernel is the language's (`src/actors.c`, docs/actors.md); an actor's body - where it is
  in a host's world - is the host's. Hooks a host fills (`forth_set_reader`, `forth_set_task_failed`,
  `forth_guard`, a body's size) are how it reaches in.
- **Readable**: name every magic number; C that reads like the code around it. Forth in `lib/` follows
  docs/forth.md's style: locals over stack juggling, no `>r`/`r>` shuffles or deep `pick`s.
- **Defined behaviour**: arithmetic wraps through unsigned casts (no signed overflow in C); every address the
  scripts reach goes through `addr_n`.
- **Tests with every change** (`tests/test_*.fs`, `T{ ... -> ... }T`); docs move with the code.
- `lib/*.fs` are built in (cmake/embed.cmake): a new file there is a vocabulary every host has.

```sh
cmake -S . -B build -G Ninja && ninja -C build && (cd build && ctest)
cmake -S . -B build-asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined" && ninja -C build-asan
tools/check-hosts.sh                      # the hosts beside this repo, with this checkout
```

CI builds with gcc and clang, warnings as errors (`-DCLANKFORTH_WERROR=ON`). `tests/fuzz_eval.c` is a fuzzer
(docs/forth.md, Tests).

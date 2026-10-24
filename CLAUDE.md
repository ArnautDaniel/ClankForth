# ClankForth: notes for an AI assistant

The Forth system shared by OpenHuey and OpenMizzurna (each has it as a submodule, `external/clankforth`). Read
`README.md`, then `docs/forth.md`.

- **Two hosts depend on it.** `forth.h` is their API: keep it compatible, or say in the pull request what
  changed and how a host adapts. A change is done when this repo's tests pass and both hosts' suites do with it.
- **Nothing of a host here**: no game, engine, or actor words - those are the host's (`forth_prim`, its own
  vocabularies). Hooks a host fills (`reader`, `task_failed`, `forth_guard`) are how it reaches in.
- **Readable**: name every magic number; C that reads like the code around it. Forth in `lib/` follows
  docs/forth.md's style: locals over stack juggling, no `>r`/`r>` shuffles or deep `pick`s.
- **Defined behaviour**: arithmetic wraps through unsigned casts (no signed overflow in C); every address the
  scripts reach goes through `addr_n`.
- **Tests with every change** (`tests/test_*.fs`, `T{ ... -> ... }T`); docs move with the code.
- `lib/*.fs` are built in (cmake/embed.cmake): a new file there is a vocabulary every host has.

```sh
cmake -S . -B build -G Ninja && ninja -C build && (cd build && ctest)
cmake -S . -B build-asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined" && ninja -C build-asan
```

# C libraries (the c vocabulary)

A C library's functions as words: the library loaded as the program runs, each function called with the cells on the
stack.

```forth
USING: c ;
LIBRARY: zlib libz.so.1
zlib FUNCTION: zlibVersion ( -- cell )
zlib FUNCTION: crc32 ( crc bytes n -- uint )

zlibVersion z>string type                       \ 1.3
: crc ( a n -- crc )  {: a n :}  0 a n crc32 ;
s" hello" crc hex .                             \ 3610a686
```

**This vocabulary is not checked the way the rest is.** Everywhere else a number where an address belongs is an
error; here a stack comment that doesn't say what the C function takes, or an address it writes past, is a crash or
worse - C's rules, not Forth's. The `clankforth` tool has it (a program's scripts are the program); a host gives it
with `forth_c(f)` only to scripts it trusts with its whole process (docs/hosts.md).

## The words

- `LIBRARY: name path` opens the library (as the system's loader does: its search path, or a path with a `/` in
  it) and makes `name` its number. An error if it can't be opened, said as the loader says it.
- `lib FUNCTION: symbol ( args -- result )` makes a word `symbol` calling the library's function of that name -
  looked up now: an error if the library has none. The comment is read:
  - each item before `--` is one argument, a cell: a whole number or an address. Eight at most. Their names are
    yours.
  - the item after `--` is what the function returns: `int` (32 bits, with their sign), `uint` (32 bits, without),
    `cell` (64 bits: a `long`, a `size_t`, a pointer) - or nothing, for a `void` function or a result not wanted.
    A function returning `int` declared `cell` gives 32 bits of sense and 32 of whatever was in the register.
- `lib SLOW-FUNCTION: symbol ( args -- result )` the same, for a call that takes a while (a query, a file, a
  compression): in an actor's handler it is made on a worker thread while the handler waits and its node's other
  actors go on, as `read-file` is (docs/actors.md, "Work that blocks"); at the prompt, where it stands. What its
  arguments point at must stay where it is till it returns - a handler's own objects do; `net-input`'s bytes don't.
  The library must take being called from another thread. (Where a waiting word does its work itself - docs/actors.md,
  "Work that blocks": without machine code, one reached through `execute` or a deferred word - so does this.)
- `c-library ( path n -- lib )` a library opened by a path on the stack (one with a space in it; one worked out). An
  empty path is the program itself and what it has loaded - C's own library, on Linux.
- `c-symbol ( name n lib -- addr )` where a function, or a variable, of the library is.
- `addr c-function name ( args -- result )` a word calling the function at an address: a name of your own for a
  symbol (`s" sqlite3_prepare_v2" sqlite c-symbol c-function prepare ( db sql n stmt* tail* -- int )`), one longer
  than a word's 31 characters, a function a C structure points to.
- `z>string ( z -- s len )` C's text - bytes up to a 0 - copied into a new string.

## Passing things

- **Numbers**: as they are. An `int` argument takes the cell's low 32 bits.
- **Text in**: a string object is C's text already - its address, a 0 after its bytes. `s" name" >string drop` makes
  one (`dispose` it, or let the handler's end). Text as `addr len` is not: `s" x"` inside a definition has no 0
  after it.
- **Text out**: `z>string`. Whose memory C's text was is the library's to say (`free` is a function of the C library
  like any other: `FUNCTION: free ( addr -- )`).
- **Bytes**: a byte buffer's `b-data drop`, or memory of the dictionary (`create buf 64 allot`).
- **A result through a pointer** (`int f(long *out)`): a `variable`'s address, then `@`. A 32-bit one: `l@`, `sl@`.
- **Structures**: by address only - laid out with `begin-structure` (docs/forth.md), read and written with `@ ! l@
  l! w@ c@`.

## What it doesn't do (a first version)

- floats and doubles, as arguments or results;
- a structure passed or returned whole; a function with a variable count of arguments (`printf`);
- C calling back into Forth (a function pointer for a callback);
- closing a library: one opened stays for the program's life.

A build with `-DCLANKFORTH_FOREIGN=OFF` (a board with everything linked in) has the words; `LIBRARY:` says it can't.
`tests/test_c.fs` runs against a small library of its own, `tests/foreign/clanktest.c`.

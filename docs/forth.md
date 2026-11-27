# ClankForth

A small Forth in the spirit of jonesforth, written in C (`src/forth.c`, the core; `src/objects.c`, lists, tuples and
strings; `src/effects.c`, the stack checker; `src/actors.c`, the actor kernel) with the rest of the language in Forth
(`core/prelude.fs`, built into the library: `forth_prelude`). It is not ANS Forth; it is a scripting language for
games, embedded in a host (a game engine) that adds its own words. Names are case-insensitive.

Below, "the host" is the program that embeds it: its C words, its scripts' folder, its actors. OpenHuey and
OpenMizzurna are two.

## The model

- **Cells** are pointer-sized integers, and addresses are real addresses: `@` and `!` reach C
  structs directly. Memory words come in sizes: `c@ w@ sw@ l@ sl@ @` (8/16/32 bits, signed
  variants) and their stores. Sums, differences and products wrap round (`+ - * 1+ 1- negate abs +! +loop cells floats`):
  the largest cell `1+` is the smallest, as Forth has it; so does the smallest `-1 /`. `*/` multiplies to twice a
  cell's bits before it divides. A shift by a cell's width or more (or a negative count) shifts every bit out:
  `lshift` and `rshift` give 0, `arshift` 0 or -1 by the sign. A literal may be any cell's bits (`$ffffffffffffffff`
  is -1); one larger is an error. Cells are 64 bits: ClankForth builds only for 64-bit machines.
- **Mistakes are errors, not crashes**: a number used as an address (below 64 KiB, or negative) - fetched, stored,
  typed, compared, or executed - is an error, and so are a negative length (`type`, `compare`, `evaluate`,
  `included`, `find-word`) and a reach past the dictionary's end. An execution token (`execute`, `catch`, `jump`,
  `start-task`, what a deferred word is set to) must be a word's: an address in the dictionary where no word begins
  (`here execute`, a word's body) is an error, as is a word whose space was given back. The compiler's own words
  (`(lit)`, `branch`, `0branch`, `(do)` ... - they read the cells after them in compiled code) are an error typed
  at the prompt or executed. `base` holding what isn't a base (2 to 36) counts as ten, so `10 base !` puts it
  right. `allot` gives space back with a negative count (the words in it forgotten),
  but not past the system's own words (the primitives' and the prelude's: the C holds them); `;` and `;]` refuse a
  definition with an `if`, a loop or a `[:` not ended; deferred words set to each other in a loop are an error; `unused ( -- n )` is what is still free. A definition an error leaves
  unfinished is dropped, its space given back. This doesn't make a script safe to run untrusted: real
  addresses can still reach anything (docs/hosts.md, What a script can do).
- **Floats** have their own stack (doubles). A number with a `.` or an exponent is a float:
  `1.5`, `2e`, `-0.25e3`. `f>s` (and `3f>s`) truncates toward zero; a float too large for a cell gives the largest
  cell (the smallest, if negative), not-a-number 0. In memory floats are 32-bit, as game data has them: `sf@ sf!` (and `f@ f!` for
  a Forth `fvariable`, which holds a double).
- **Unsigned and double-width**: `u/ umod u/mod`; `um* ( u1 u2 -- lo hi )`, `m* ( n1 n2 -- lo hi )` (the product
  twice a cell wide, its high cell on top), `um/mod ( lo hi u -- r q )` (a quotient past a cell is an error).
- **Random numbers**: `random ( n -- u )` 0 .. n-1, each as likely; `random-cell ( -- x )`; `frandom ( F: -- r )` 0
  up to 1. Each Forth has its own (xoshiro256**), seeded from the clock as it is made: `x seed-random` gives the same
  numbers again for the same x (a replay, a test).
- **Words** are indirect threaded: each has a code field (a C function) and a body. `see word`
  decompiles a colon definition.
- **Machine code** (the native backend, `src/native.c`; x86-64: Linux, the BSDs, Windows with MinGW): a colon definition, once
  finished, is translated to machine code too - subroutine threading: each word a call, the branches and loops
  jumps. Its threaded code stays what it means: `see`, the stack checker and the call chain read that, and errors,
  the return stack, `catch` are as they were. One with `does>` or `jump` stays threaded; the two call each other.
  So does one with an `r>` or `rdrop` where none of its own `>r`'s items is on the return stack (it takes its
  caller's return address - `: skip r> drop ;`, `r> cell+ >r` - to go on where its caller wouldn't have): machine
  code returns by its own stack, so a word that does that, called from machine
  code, is an error ("went back past its caller") - such words run among threaded ones only.
  The hot words (`dup` `+` `<` `0=` `within` `i` `j` `>r`, `@` `!` `c@` `c!`, a literal, a local and a word's frame
  of them, `0branch`, `do` `?do` `loop`, `exit` ...) are written in line, and joined where they meet (`5 +` one instruction,
  `< if` a compare and a jump); when one's checks fail (an underflow, no loop open, what is no address) its C
  primitive runs instead, so its errors and edges are the primitive's. Those that follow one another with nothing
  between - no call, no word a branch lands on - are a run: the stack's top two items are in registers through it
  (`x 3 * 7 +` arithmetic on a register, `swap` and `drop` no instruction at all; a third item pushed, the deepest
  goes to its cell), as is the locals frame, and what the whole run needs of the stack (so many items, so much
  room) is checked once as it begins; that check failing, each word's primitive runs in turn, so the one that
  hasn't what it needs says so as it always did. Where the run ends - a call, a branch - everything is written
  back, so a word called, an error and the debugger find the stack where it always was. A `do` loop's index and
  limit are in registers too: `i` is a register, `loop` counts one and compares two (the return stack's cells are
  kept right, so `j`, `leave` and a word called find what they did; after a word called - it may have a loop of
  its own - or `>r`, they are read again). A call of another definition pushes where it goes on in line - and a small definition, straight through
  to its end (no branch, no loop, nothing of the return stack's) with every word of it written in line, is written
  into its callers too: no call, its words part of the caller's run (what a call leaves on the return stack is left
  all the same, so an error in it names its callers as ever; defined again at the prompt or by a reload, the
  callers it was written into are translated again). `' word native-inlined ( -- n )` counts the calls so written in
  a word's code. What is still a call: a definition with a branch in it, or one that calls C, a
  word made by `create`, `variable`, `constant` or `value` - in a loop over many bytes, its address in a local first.
  `' word native? ( -- flag )` says which a word is, `.native` counts them. Each task then runs on a native stack of
  its own, so machine code can wait (`yield`, `wait`, `await`) in the middle of a word. A step is counted at each
  word's start and each jump back, not each word (so a loop that never ends is still stopped, a little later). Off:
  `CLANKFORTH_NATIVE=OFF` building, or `CLANKFORTH_THREADED` set in the environment (to compare, or to look for a
  fault of its own); `CLANKFORTH_CALLS_ONLY` keeps the machine code but writes nothing in line, `CLANKFORTH_STUBS_ONLY` writes the runs
  but sends each word round by its primitive (the registers written back and read again at every word: for the
  tests);
  `CLANKFORTH_CODE_MAP=file` lists each definition's code and name there, for a profiler (docs/debugging.md). The code's memory
  is never writable and runnable at once (W^X): run and read only, made writable for the moment a word is written
  in; a system that refuses even that leaves the word threaded.
- **Tasks**: every running piece of Forth has its own stacks. `' word start-task` starts one (it gives
  an id); whatever loop runs the system gives each ready task its turn. Inside a task, `yield` gives way: the
  others have their turn, and it goes on at the loop's next round; `ms sleep-ms` (system's) waits by the clock.
  `id stop-task` stops one, `.tasks` lists them
  (a task stopping itself does it where it could wait: not inside `with-scratch` or `catch`, an error there).
- **Rounds**: the system's own loops - `serve`, a node's - are rounds: what can run, runs (the tasks' turns, the
  actors' messages and handlers), then the loop waits for what comes. `round ( -- ms )` is one, for a loop of the
  program's own: it gives how long the loop may wait (0 while something can still run; a second at most) -
  `begin round net-poll drop again` is `serve`. `rounds ( -- n )` counts them: a loop with nothing to do runs none.
- **Frames are a host's**: a host whose loop is a frame at a time (a game's) calls `forth_run_tasks` each one, and
  the words that count them are the `frames` vocabulary (`USING: frames ;`): `n wait` (n frames on; a host may add
  `seconds`: OpenMizzurna's `0.5e seconds wait`), `frame ( -- n )`, `tick-tasks ( n -- )` (so many frames of the
  tasks, from the interpreter: tests), `tasks-paused! ( flag -- )` (no task runs, their waits don't count: the host
  paused) - and the actors' (docs/actors.md, "Frames"). `yield` there goes on at the next frame. Where nothing
  counts frames, `wait` is an error.
  `start-held-task` starts one the scheduler leaves alone: `id resume` gives it its turn (whoever drives
  it decides when; it answers whether the task is still alive). `xt restart` makes the running
  task start over in another word at once, its stacks emptied (a script's "go to").
- **Each task's own, not the node's**: `base`, `error-message` (the last error raised in this task - another's since
  isn't it), `n>str`'s and `$clear $+ $@`'s buffers. `task-variable name` is a variable each task has its own cell
  of: what a word keeps between the words it calls - a parser's place - where another task may run in between
  (an actor's handler is lent a task for as long as it runs: these are the handler's, new each time - docs/actors.md).
  A plain `variable` (and a `value`) is the node's - every actor's: a handler that writes one and reads it back is
  warned of; `shared-variable name` / `x shared-value name` declare one that is meant so (docs/actors.md, What a
  handler may keep where).
  `[: ... ;] atomically` runs its code with no other task between its words (from C, as `map-each`'s code is: nothing
  in it may wait) - a short look at, or change to, what the node's actors share: a cache's entry copied, a counter
  read and written. A plain `variable` is the node's: every actor on it sees the same one.
- **A task that never yields** is stopped after about 20 million steps in one turn ("ran too
  long without yielding"), so one script can't freeze the host - but an actor's handler, sliced
  (`forth_set_slice`, docs/actors.md), gives way instead and goes on later. Text the host interprets (a file as it loads, a
  line at a prompt) has no limit unless the host sets one (`forth_set_eval_budget`): then it is stopped the same
  way ("ran too long").
- **Errors** print a message with the file and line and unwind: at the prompt the stacks are
  emptied; a task that errs is stopped (and the host told: `task_failed`). `' word catch`
  runs a word and gives 0, or for an error -1 instead of unwinding (`error-message` has the text). A task - an actor's
  handler - may wait inside it (`wait`, `yield`, `await`): it goes on there in a later turn, and an error after the
  wait is still the `catch`'s. (No wait inside a word called from C, though: `map-each`'s, a `sort`'s comparison, a
  `change-` accessor's. Threaded, 16 catches within each other may each be waited in; one deeper is as those.)
  `x throw` raises one of the script's own: `catch` gives x (0 throws nothing). A string object thrown is the
  message; anything else - a number, a tuple saying what went wrong - is said as a number (`thrown: 42`). (A string made in a `with-scratch`
  and thrown out of it is gone by the `catch`: its text is in `error-message`.)
- **The call chain**: an error prints the words that were running under its message, innermost first
  (`in meta-c@ <- shown? <- show-objects <- areas:on show-scene`). A host may name its unnamed
  definitions for it, leaving their length 0 so no word is found by that name (OpenMizzurna's actor kernel
  names each handler for its behaviour and message: `areas:on show-scene`). So does a crash, a bad
  address (`crashed (a bad address), in the words ...`) or a sanitizer's report. `backtrace ( -- addr len )`
  gives the chain now: the word calling it first. It is read from the return stacks and from where each
  call from C (`forth_execute`, a task's turn) began. `forth_report_crashes` turns the crash report on.
- **Lent memory** (`forth_guard`: memory the host gives the scripts with a known end - OpenMizzurna's library
  files): in a debug build (`memory-checked?` true) a fetch or store that reaches past its end is an error
  naming it (`past the end of cdbin/027_4/0.bin (its 4 bytes): 1 at byte 8`), not a quiet read of whatever lies
  beyond; padded by the host (its `span`), an overrun lands where it is seen. A release (`NDEBUG`) doesn't check.
- **Looking**: `see name` decompiles a word, `see-xt ( xt -- )` any token (a quotation, a handler).
  `with-output ( xt -- s )` runs xt with what it prints gathered into a new string (an error in it: what it printed
  goes out, and the error goes on). `>body` of a word with no data - a primitive, a colon definition - is an error
  (`to` and `is` take a value's, a deferred word's). docs/debugging.md: looking into a running system.
- **The version**: `clankforth-version ( -- addr len )` says it (`0.8.0`), as `clankforth --version` does; what
  each version changed is in CHANGELOG.md.

## Stack effects (as in Factor)

A definition's stack comment is its contract: `;` works out what the code does to the stacks and warns where it
differs from the comment (`warning: way-scene ( movie cutscene -- ): its code is ( 2 -- 1 )`). The word is still
made; the warning names the file and line.

```forth
: sq ( n -- n*n )  dup * ;                 \ fine
: leaky ( a -- )  dup ;                     \ warning: its code is ( 1 -- 2 )
: uneven ( a -- b )  if 1 else 1 2 then ;   \ warning: its ways meet at other depths
```

- **The comment**: the first `( ... )` comments at the start of the definition, before its code: inputs before `--`,
  outputs after; `F:` the float stack (`( addr -- ) ( F: -- x )`). `"name"` is parsed from the input, not on a
  stack; `R:` and `C:` parts aren't counted. `( F: x -- flag )` says the inputs are floats but not where the result
  goes: the code decides (`( F: x -- F: y )` says a float). `( -- x true | false )`: the two exits, the flag on top;
  a caller's `if` right after it takes them apart (so does `?dup if`). `..`, or alternatives of other sizes on no
  flag, say it varies: not checked.
- **Without a comment**, a word's effect is what its code does, and its callers are checked with that. `effect.
  name` shows a word's effect as callers see it; `' name stack-effect ( -- in out fin fout kind )` as numbers.
- **What can't be told is not checked** (never a warning): a token run from the stack (`execute`, `catch`, `each`
  and the combinators - but `[: ... ;] execute`, `['] w catch` are, and the combinators and the lists' `each map
  filter reduce find` right after their quotations), a loop whose turns take or leave items (n stores in an n-turn
  loop - but a list's walk is checked: `[: dup ;] map` is a warning), a deferred word with no stack comment right
  after its name (`defer on-hit ( damage -- )` gives it one: its callers are checked with it), `evaluate`, a host's C word with no effect given (`forth_set_effect`), and an
  immediate word (its comment is the code it lays down's).
- **Words kept in slots.** A state machine keeps "what it does now" as a word in a variable or a field, and runs
  it: `step @ execute`. A token from the stack can't be followed - unless the slot says what it holds:

  ```forth
  variable step  holds: ( -- )                \ `holds: ( effect )` after any word that gives a slot's address:
  cell field her-act  holds: ( -- )           \ a variable, an actor's field, a record's (`field: walk.xt  holds: ( -- )`)
  : frame ( -- )  step @ execute ;            \ followed: its effect the slot's ( `step @ ?dup if execute then` too )
  : wait! ( -- )  ['] leaky step ! ;          \ warning: leaky ( 0 -- 1 ) is stored in step, which holds ( 0 -- 0 ) words
  : step! ( xt -- )  step !  0 step-t ! ;     \ it stores its input in a slot: found as it is compiled, so
  : chase ( -- )  ['] leaky step! ;           \ warning: leaky ( 0 -- 1 ) is given to step!, which stores it where ...
  ```

  A deferred word with its effect said (`defer on-hit ( damage -- )`) is such a slot: `['] w is on-hit` is checked,
  in a definition or outside one. What is stored is checked when it is a word the compiler sees - `['] w`, a
  quotation - not a token from the stack. A misfit is said whatever the definition's own comment.
- **A token fetched and run** from memory the checker knows nothing of (`cells acts + @ execute`) isn't followed, but
  the word isn't one that runs its *caller's* token: its comment is what its callers are checked with. Only a word
  that runs a token it was given (`: with-him ( xt -- ) him enter execute leave-actor ;`) has "the token's effect
  too" - and given a known word where it is called (`['] bark with-him`, `[: ... ;] with-him`), its code is
  followed for that call with the word in the token's place, so the caller is checked all the same.
- **The return stack** is followed too, and what it finds is said whatever the comment (with none, too), for the
  run would crash: a way out with a `>r` not taken back (`: lost >r ;` - "it returns with 1 still on the return
  stack"), and an `exit` inside a `do` loop without `unloop`. Both return into the cells left over the return
  address. A word with locals is spared: its frame is dropped as it leaves, and what was put over it. A quotation
  is checked at its `;]`. (`r>` with nothing of the word's own there - its caller's return address - is not
  warned of.)
- **A system built without the checker** (`CLANKFORTH_CHECKER=OFF`: `src/effects_none.c` in `src/effects.c`'s
  place) checks none of this: a stack comment is a comment, `holds: ( ... )` is read and let go, and `effect.`,
  `stack-effect` and `.checked` aren't there. `effects-checked? ( -- flag )` says which a script is in. The other
  warnings (a word hiding another, one defined again) are the compiler's, and stay.
- `warnings ( -- n )` counts the warnings given (a test can say a file loads without any).
- `.checked` says how much of what is loaded was followed through - by vocabulary where much isn't known (a host's C
  words with no effect given, mostly) - and how much runs a token or varies.

## Vocabularies (as in Factor)

Words live in vocabularies. A file starts by saying where its definitions go and what it uses:

```forth
\ scripts/player.fs
IN: player
USING: engine keys vectors ;
```

- A name is looked for in the file's own vocabulary, then those it uses, then the core. A name no vocabulary but
  the core has (most of what a script says - `dup`, `+` - and every number) goes straight to the core, however many
  vocabularies the file uses.
- `USING: a b ;` loads each vocabulary that isn't loaded yet from its file - `a` is
  `a.fs` in a root (`forth_add_root`: the host's scripts' folder), `a.b` is `a/b.fs`, or `a/b/b.fs` (a folder of its
  own, as the library's are) - and makes its words visible here; failing a file, a vocabulary built into the library.
  `USE: a` does one.
- The library's (built in, every host's), laid out as Factor's: `core/prelude.fs` (the language), and in `basis/` a
  folder for each library - `basis/http/http.fs` is `http`, `basis/http/websocket/websocket.fs` is `http.websocket`.
  `tester` (tests, below), `json`, `base64`, `checksums.sha1`, and on the `net` vocabulary `http` and its parts
  (docs/http.md).
- A file sees only its own words, the vocabularies it lists, and the core (`forth`: the
  language itself, from the C files and the prelude). Nothing leaks in from files it didn't ask for.
- Its own definitions win over everything; a used vocabulary's win over the core. Two vocabularies may each have
  a word of one name - neither owns it: `vocab:name` says which is meant. If two a file uses have the name, the bare
  name is ambiguous: the one later in `USING:` is taken, and each use is warned of (below).
- `<PRIVATE ... PRIVATE>` puts helpers in `name.private`: visible in the file, not to its users
  (`name.private:word` still reaches one, for debugging).
- Vocabularies loading each other (a cycle of USING:s) is an error. So is a name that would reach outside the
  roots: one with `/` or `\`, or nothing between two dots.
- A file may define a word named as one of a vocabulary it uses: it is the file's own, and the one its bare name
  means there (nothing is said: a library adding a word doesn't break a program that had one of that name). What is
  warned of is a bare name that could be either of two the file uses - `split is ambiguous here: strings:split and
  text:split are both in sight (text:split taken, the later in USING:)` - at each use, as the definition using it is
  compiled; and a used vocabulary's word taken where the core has one of that name with another stack effect
  (`build here is app:build (1 in, 1 out), not forth:build ...`). `vocabulary:word` names the one meant, and is
  never warned of: `forth:build`, `strings:split`. (A host's `messages` aside; and a vocabulary and its own
  `<PRIVATE` half are one.)
- The prompt (`forth_eval`: a host's console, the `clankforth` tool) sees every loaded vocabulary; `USING:` there
  narrows nothing, it loads. `vocabs` lists them, `vocab-words name` lists one, `words` lists
  what the current source sees.
- A host's C words go in its own vocabularies (OpenHuey's and OpenMizzurna's: `engine`).

### Reloading: a word defined again, in a running system

`reload name ( -- )` reads vocabulary `name`'s file again (`forth_reload` from C; loaded the first time if it isn't
yet). As it ends, each word the vocabulary had - its `<PRIVATE` part's too - that the file defines anew **forwards**
to the new one: everything compiled for the old one, in any vocabulary, threaded or machine code, an actor's handler,
runs the new one from its next call. A call under way finishes as it began. So a running world takes a fix without
stopping:

```forth
reload town          \ town.fs changed: every caller of its words has the new ones
```

- A word the file no longer defines stays as it was. A variable, a `create`d word, a constant defined again forward
  too: the new one's storage (set as the file sets it) is what every caller reaches.
- One that calls the word it replaces (`: greet greet ." !" ;`, a wrapper) doesn't: it would call itself. What was
  compiled for the old one keeps it, and that is said.
- One defined again with another stack effect, with calls compiled for the old: a warning (`shape is ( 1 -- 1 ) now,
  was ( 0 -- 1 ): 1 call compiled for the old one goes to it`).
- A tuple class defined again with the same slots (and parent) is the same class: its tuples keep it, and the methods
  on it. A generic defined again keeps its methods from other vocabularies (its own vocabulary's go: they come with
  the file). Message kinds keep their number. An actor's behaviour starts afresh, and its state moves over field by
  field (docs/actors.md, State).
- Top-level code runs again: `reloading? ( -- flag )` is true meanwhile, so what must happen once (an actor spawned,
  a name registered) can ask. A file that fails part way leaves what it defined before the error, nothing forwarded:
  fixed, `reload` it again.
- At the prompt (`forth_eval`: the tool's, a host's console) a word defined again in its vocabulary forwards the same
  way once its definition is finished.

## Writing words

```forth
: square ( n -- n*n ) dup * ;
variable lives   3 lives !
10 constant max-items
0 value score   score 1+ to score
fvariable speed  2.5e speed f!
defer on-hit   ' noop is on-hit   action-of on-hit   \ (what it does now: a hook's chain)
: array ( n "name" -- ) create cells allot does> ( i -- addr ) swap cells + ;
```

`3s>f ( x y z -- ) ( F: -- x y z )` and `3f>s` move a point between the stacks. A deferred word runs its word in its place (no frame of its own). `xt jump` leaves the running word for xt -
a tail call: a chain of them never grows the return stack (a script's `goto`; not from a word with
locals - an error as it compiles - or a loop open). `flag abort" text"` is an error with that text when flag is true; `erase ( addr n -- )`
zeroes. A vocabulary's own word can shadow a core one of the same name: `forth:name` reaches the core's.

Control: `if else then`, `begin until`, `begin while repeat`, `begin again`,
`do loop`, `?do loop`, `+loop`, `i j leave unloop`, `case of endof endcase`, `exit`, `recurse`.

Locals (Forth-2012's, with floats): `{: a b F: x | c F: y -- comment :}` inside a definition names
its values - `a` and `b` taken from the stack (`b` from the top), `x` from the float stack (`F:`
makes the names after it floats, until `|`), `c` and `y` starting at 0, the rest a comment. A name
gives its value; `to name` stores into it. They last until the word returns (`exit` too), the
return stack still free for `>r` and loops; a quotation has its own, and sees its word's (below). A definition may
have several `{: :}`: each takes its values where it stands. The first makes the frame, so it can't be
inside an `if` or a loop; later ones can (`... do  i thing {: t :}  ... loop`) - in a quotation any can. A
quotation with locals (its own or its word's) can't `jump`. A local can't
take the name of an immediate word (`to`, `if`, `loop` ...): it would stop meaning itself. Prefer them
to stack juggling (`2 pick`, `rot`, `>r` chains) and to scratch variables:
`: centred ( addr len x y -- )  {: addr len x y :}  ... ;`.

Tuples (Factor's): objects with named slots, for the scripts' own data (`begin-structure`, below, is for a C
struct's or a file's layout).

```forth
USING: accessors ;                  \ (where their accessors are)
TUPLE: point  x y ;                 \ point ( -- class )  <point> ( x y -- p )  point? ( x -- flag )
TUPLE: point3 < point  z ;          \ a child: its parent's slots, then its own
TUPLE: ball  name  F: radius ;      \ F: the slots after it floats (to and from the float stack)
3 4 <point>  dup x>> .  10 >>x  [: 1+ ;] change-y   \ x>> ( p -- x )  >>x ( p x -- p )  change-x ( p xt -- p )

GENERIC: area ( shape -- n )        \ dispatched on the class of the tuple on top
M: rect area ( shape -- n )  dup w>> swap h>> * ;
M: object area ( shape -- n )  drop 0 ;   \ every tuple's, but those with their own
```

A child's tuple is its parent's kind too (`point?`, `instance? ( x class -- flag )`, a method). A slot's name is a
cell's or a float's everywhere: its accessors (in the vocabulary `accessors`, made with the first tuple that has it)
work on any tuple with that slot. A method's effect is its generic's: its comment and code are checked against it.
`class-of ( obj -- class )`, `.tuple ( obj -- )` prints one. A slot a tuple doesn't have, a number used as a tuple,
one disposed of, no method for its class: each an error.

Objects - lists, tuples and strings - are known by their address: a number, or one let go, where an object belongs is
an error, not a crash. Each lives until `dispose ( obj -- )`; `clone ( obj -- obj' )` copies one; `objects ( -- n )`
counts them, `.objects` by kind. Made inside `[: ... ;] with-scratch` they are freed as it ends (an error too; the code
in it may wait - `await`, `wait` - its region kept till it goes on and ends) - but
those `rescue ( obj -- obj )`d, kept by the region around it: so a frame's lists, a parse's pieces, need no freeing.
`rescue-all ( obj -- obj )` takes one out of every region round it, kept till `dispose`; `rescue-handler ( obj --
obj )` only as far as its handler's (or task's) own, so it lasts till the handler ends whatever `with-scratch` it was
made in - what a task-variable may hold. `permanent ( obj -- obj )`
makes one the system's for good - no region, rescue or message moves it, `dispose` refuses it: a constant every actor
may hold (json's `json-true`, `json-false`, `json-null` are). Each takes what the object
holds with it (a list's strings, a tuple's slots, a map's values made where it was). An actor's handler runs in a
scratch region of its own, and a message takes the objects it carries: docs/actors.md, An actor's objects.

Records: `begin-structure /point  ffield: p.x  ffield: p.y  end-structure` (`field:` a cell,
`ffield:` a float, `sffield:` / `lfield:` 32 bits, `cfield:` a byte, `n +field` n bytes).

Enumerations: `enum: mood  calm wary tense angry ;` makes the constants `calm` .. `angry` (0 up; a
number in the list sets the next: `enum: act  $36 hide  $52 lie-down ;`) and `mood>name ( n --
addr len )`, which names a value ("?" for one not listed) - for the debug words and traces. A long
list may run over several lines with comments (`\`, `( )`) between the names.

The dictionary's `find ( addr len -- xt | 0 )` is `find-word` once the prelude is in: its `find` is
the lists' (Factor's).

String lists: `strings: files  intro.sfd  DIR\menu.sfd  8 end.sfd ;` makes `files ( i -- addr len )`,
string i of the list - each a word as written, `\` and all - and "" for one not in it. As in
`enum:`, a number sets the next index (those skipped are ""), and comments may come between. The
rooms' string tables and the music's file names are written so (story/strings.fs).

Combinators (Factor's), to run code on values without shuffling them: `x [: .. ;] dip` (under
x), `x [: .. ;] keep` (x kept after), `2dip`, `2keep`, `x [: p ;] [: q ;] bi` (both on x), `tri`,
`x y [: q ;] bi@` (q on each), `flag [: .. ;] when` / `unless`, `flag a b ?` (a if the flag, else b).
Right after their quotations (`[: 1+ ;] dip`) these are compiled inline: the quotation called where it stands, no
`execute` - they cost no more than the code written out, and their stack effects are checked. Given a token from
elsewhere (`xt dip`) they run it as a word does.

Quotations: `[: ... ;]` leaves anonymous code as an execution token, inside definitions or at
the prompt (Factor's `[ ... ]`): `{ 1 2 3 } [: dup * ;] map`. A quotation sees the locals of the word it is in (and
of the quotations around it), and `to name` stores into them:

```forth
: add-to ( l n -- l' )  {: n :}  [: n + ;] map ;
: total ( l -- n )  {: l | sum :}  l [: sum + to sum ;] each  sum ;
```

One that does is a closure, made in its word's frame as it is reached: it costs nothing, and lasts as long as the
word runs - run while it does (`each`, `map`, `catch`, a combinator), not kept for later. Run after its word has
returned, or given to `start-task` (another task), it is an error saying so.

Sequences (Factor's protocol): a **list** (cells: `{ 1 2 3 }`, or `list` an empty one), a **string** (bytes - its
UTF-8 - its length its own: `" text"`, or what `>string` makes) and **bytes** (that grow: `<bytes>`) take the same
words, and what one makes is **of the kind it was given** - `map` over a string a string, `append` of lists a list.

- the protocol: `length ( seq -- n )`, `nth ( i seq -- x )` (negative counts from the end), `nth! ( x i seq -- )`,
  `push ( x seq -- )` (onto its end), `pop ( seq -- x )`, `clear ( seq -- )`, `like ( seq exemplar -- seq' )` (its
  items as the exemplar's kind: of it already, itself). A string's items are its bytes (0 to 255; one that isn't a
  byte, pushed or stored, is an error); characters are a walk of their own (below). **A string keeps its length** -
  its object is its address, which can't move: it doesn't take `push`, `pop`, `clear` or `append!`; one is built
  (`build`, below), or made of another (`append`, `like` ...). Lists and bytes grow.
- walking (prelude.fs): `each ( seq xt -- )`, `map ( seq xt -- seq' )`, `filter`, `reduce ( seq acc xt -- acc' )`,
  `find ( seq xt -- x true | false )`, `find-index ( -- i | -1 )`, `any? all? count`, `each-index ( seq xt -- )` and
  `map-index` (xt `( x i -- ... )`). The code runs on the stack as its caller left it, the item on top: `0 l [: + ;]
  each` adds the items to the 0. Right after a quotation in a definition (`[: dup * ;] map`), `each map filter
  reduce find` are compiled as the walk itself - no call of the word, none of `execute` - and their stack effects are
  checked, as the combinators'. A sequence made shorter or let go while it is walked is an error.
- looking: `index ( x seq -- i | -1 )`, `contains? ( x seq -- flag )`, `first last`, `empty?`, `subseq-index ( seq
  sub -- i | -1 )` (`subseq-index-from` with where to look from), `starts? ends? ( seq prefix -- flag )`, `seq= ( a b
  -- flag )` (the same items, whatever the kinds).
- making (each a new one, of its kind): `append ( a b -- c )`, `subseq ( seq from to -- seq' )`, `head tail ( seq n
  -- seq' )`, `reverse`, `concat ( seqs -- seq )` (the first's kind), `join ( seqs sep -- seq )` (sep's kind), `split (
  seq sep -- l )` (a list of pieces, sep a sequence: `" , "`), `replace ( seq old new -- seq' )`, `trim` (a string's
  spaces and control characters off its ends); in place: `append! ( a b -- a )` (a list or bytes).
- `range ( n -- l )` 0 .. n-1; `list-clear`, `list-free` (`dispose`), `.list`. Sorted in place: `sort ( l -- )`
  numbers, `sort-strings ( l -- )` strings by their bytes, `sort-by ( l xt -- )` as xt ( a b -- flag ) says (true: a
  goes first). Each is stable: items alike keep their order.

Lists made while loading live for good; free the ones made over and over at run time - or make them in a
`with-scratch` (below, Tuples).

**Building a string** - Factor's `make`, and how HTML is written: `build ( xt -- s )` runs xt with a string being
built, which `% ( seq -- )` (a string or bytes), `%s ( a n -- )`, `%n ( n -- )` (a number, in decimal) and `%c ( c -- )`
(a byte) add to; the string is the caller's. `build-at ( -- b n )` says which string is being built (one build's, while
it lasts) and how long it is so far - for a word that notes where in it what it wrote is.

```forth
: item ( name n -- s )  [: " <li>" %  %s  " </li>" % ;] build ;
```

xt runs in a scratch region of its own (as `with-scratch`): what it makes besides the string is freed as it ends.
Builds nest (an inner one is its own string); each task's are its own (one may wait in the middle of one); `%`
outside any is an error.

Maps: `<map> ( -- m )`, hash maps kept in the order their keys came (`keys`, `values`, `map-each` go in that order: a
run is the same each time). A key is a cell - a number, an object by identity - or a string, matched by its bytes:
a string object given as a key, or `addr len` to the `s-` words. The map keeps its own copy of a string key (`keys`
gives it; `dispose` refuses it: `delete-at`, `map-clear` or the map's `dispose` let it go).

```forth
<map> constant ages
42 s" ann" ages s-at!          s" ann" ages s-at   ( -- 42 true )
7 1001 ages at!                1001 ages at        ( -- 7 true )     99 ages at  ( -- false )
ages [: swap dup string? if string type else . then ." : " . cr ;] map-each
```

`fields>map ( a n max m -- flag )` reads an internet message's header fields into a map - HTTP's, a MIME part's:
lines `name: value` with CR LF between them, each name lower-cased, each value a new string without the spaces round
it, a name said again both values joined by `, `. False if there are more than `max` lines, a line has no name, or a
name holds what a header's may not (a space before the colon above all). It is http's reading of every request, in C.

`at ( key m -- value true | false )`, `at! ( value key m -- )`, `key? ( key m -- flag )`, `delete-at ( key m -- )`
(none there: nothing), the same with `s-` for `addr len` keys, `map-size`, `map-clear`, `keys ( m -- l )`, `values
( m -- l )` (new lists), `map-each ( m xt -- )` with xt ( key value -- ) - which may change values as it walks, not
add or take away entries (an error). `clone` copies one.

Bytes: `<bytes> ( -- b )`, bytes that grow - a file's, a message's, a protocol's. `b-push ( c b -- )`, `b-append ( a
n b -- )`, `b@ ( i b -- c )`, `b! ( c i b -- )`, `b-length`, `b-resize ( n b -- )` (cut, or grown with zeros),
`b-clear`, `b>string ( b -- s len )` a copy; `b-data ( b -- a n )` their address for the memory words (`w@ l@ move
type`), good until they grow.

Strings: `" text"` is a string object (one cell: a sequence, above), with `s\"`'s escapes - in a definition, a new
one each time the word runs, its own to keep or let go (`rescue`, `dispose`) as any string made then. Forth's own
`s" text"` gives `addr len`; `." text"` prints; `type`, `s=`, `compare`. At the prompt `s"` gives one of 16 rotating
buffers of 255 bytes (a longer one is an error: inside a definition it may be any length) - copy what must last.
`string ( s -- a n )` gives a string object's bytes as `addr len`, `>string ( a n -- s len )` an object of them.
Strings that last are objects (their addr; freed by `dispose`, or a scratch region's end - so a frame's text needs no
freeing in a `with-scratch`). The words on `addr len`:

- written with escapes: `s\" text"` as `s"`, with `\n \r \t \0 \e \" \\` and `\xHH` (a byte).
- made: `>string ( addr len -- s len )` a copy, `s+ ( a1 n1 a2 n2 -- a n )`, `n>s ( n -- a n )` (in the base),
  `f>string ( F: x -- ) ( -- a n )` (as `f.`, six digits), `f>exact ( F: x -- ) ( -- a n )` (the fewest that read
  back as x: what JSON writes), `s-upper` / `s-lower ( a n -- a' n )`, `s-split ( a n a2 n2 -- l )` (a list of
  strings: `string ( s -- a n )` gives one's bytes), `s-join ( l a n -- a' n' )` (the second between each).
- read back: `string>number ( a n -- n flag )` (the base, `$ # %` too; 0 false if not), `string>float ( a n --
  flag ) ( F: -- x )`. `list? map? bytes? string? ( x -- flag )`: an object of that kind.
- looked into, no copy: `s-find ( a n a2 n2 -- i )` (-1 if not there), `s-under ( a n c -- i )` (its first byte
  under c - `32 s-under` a control character - or -1: a scan for the few bytes that matter, without a loop over
  them all), `s-slice ( a n i len -- a' n' )` (kept to the string), `s-trim`, `s-starts? s-ends? ( a n a2 n2 --
  flag )`, `string? ( x -- flag )`. `s-marked ( a n table -- i )` is the same
  for any set of bytes: where the first one marked in a table is (256 bytes, `create t 256 allot`, not 0 for the
  bytes looked for), the length if none - the run before the next byte to escape, for text written out (json's
  strings, html's text). `crlf-lines? ( a n -- flag )`: its lines end CR LF and nothing else (no stray CR or LF,
  no NUL) - a request's head.
- formatted: `format ( args a n -- s len )` - each `{}` in the text an argument, taken from the stack in order (the
  first the deepest): `s" ann" 42 s" {s} is {} years" format`. `{}` or `{d}` a number (decimal, whatever the base),
  `{u}` unsigned, `{x}` `{X}` hex, `{c}` a character, `{s}` a string (`addr len`: two cells), `{f}` a float (from the
  float stack); before the letter `-` (to the left), `0` (zeros), a width, `.precision` (`{8.2f}`). `{{` `}}` are
  braces. Its effect depends on its text: not checked.
- characters (UTF-8): the words above count bytes; these count characters. `utf8-length ( a n -- count )`, `utf8-next
  ( a n -- c a' n' )` (the first character's code point, and the rest), `utf8-slice ( a n i len -- a' n' )`,
  `utf8-valid? ( a n -- flag )`, `utf8-clean ( a n -- s len )` (each byte out of place as U+FFFD), `char>string ( c -- s len )`, `b-push-char ( c b -- )`. A byte that begins no
  character (or an overlong form, a surrogate) reads as one character, U+FFFD.

Comments: `( ... )` and `\ to the end of the line`.

## The system vocabulary

A program's dealings with its machine, for hosts that let their scripts have them (`forth_system`: the `clankforth`
tool does; a game whose mods shouldn't touch the disk doesn't). `USING: system ;` In an actor's handler, what may
block - files read and written, `file-write`, `file-sync`, `dir-sync`, `shell`, `shell-output`, `sleep-ms` - is done on
a worker thread while the handler waits (docs/actors.md, Work that blocks).

- files: `read-file ( path n -- s len )` (a new string), `read-file-part ( path n from count -- s len )` (so many
  bytes from a place - fewer where the file ends: a long file a piece at a time), `read-bytes ( path n -- b )`, `write-file ( a n path pn --
  )`, `append-file`, `write-bytes ( b path pn -- )`, `file-exists? ( path n -- flag )`, `file-size`, `file-modified ( path n -- s )` (last
  written, seconds since 1970), `dir? ( path n -- flag )`, `delete-file` (a file, or an empty folder), `rename-file ( from n to n2 -- )` (a file
  there already replaced, as one step: what a write that must never be seen half done does - written to a temporary
  file, then renamed over), `make-dir`, `dir-list ( path n -- l )` (the names, sorted). One that can't be done is an
  error saying why (`catch` it): `s" a.txt" ' read-file catch`.
- files held open, made to last: `file-open-append ( path n -- file )` (made if it isn't there; 64 open at once),
  `file-write ( a n file -- )` at its end, `file-sync ( file -- )` - on the disk itself before it returns (fsync:
  fdatasync on Linux, F_FULLFSYNC on macOS, `_commit` on Windows) - and `file-close ( file -- )`; `dir-sync ( path
  n -- )` a folder's names made to last (a file made or renamed in it: POSIX; nothing on Windows). A log's words
  (docs/jsondb.md).
  `file-lock ( file -- flag )`: the open file taken as this process's alone if no other open of it has it (false:
  one has; nothing waited for) - the system's lock, gone when the file is closed or the process ends however it
  ends: how one process at a time has a folder (jsondb's).
- stopping when told to: `on-shutdown ( xt -- )` - xt run when the process gets SIGTERM or SIGINT (Ctrl-C), from this
  Forth's loop (`serve`, a node's), not in the signal; then that loop ends (`serve` returns; the main loop first waits
  - `shutdown-grace! ( ms -- )`, 30 s - for the nodes with words of their own to have run them). Until a script says
  `on-shutdown`, the signals do what they did (end the process at once); after, the first is noted for the loops, a
  second ends the process at once (Ctrl-C twice: a program not yet in `serve`). `shutting-down? ( -- flag )`: told
  to stop (a long loop of the script's asks). docs/http.md, Stopping.
- the program: `args ( -- l )` its arguments (the tool's: those after `--`, `clankforth run.fs -- a b`; every node's), `getenv (
  name n -- s len true | false )`, `read-line ( -- s len true | false )` (the input's next line, false at its end),
  `exit-with ( n -- )`.
- other programs: `shell ( command n -- status )`, `shell-output ( command n -- s len status )` (its output, a new
  string).
- the clock: `ms-now ( -- ms )`, `us-now ( -- us )` (from any start, never going back: for durations), `time-now ( --
  s )` (seconds since 1970), `sleep-ms ( ms -- )` (in a handler or a task the loop runs, it waits and the others go
  on; at the prompt the thread sleeps).
- the process's memory: `process-bytes ( -- n )` (what it holds, as the system counts it; 0 where it doesn't say),
  `trim-memory ( -- )` (what was freed and the allocator kept, handed back to the system - `serve` does it when
  idle), `memory-trims ( -- n )` (the times this node has) - docs/debugging.md.

The network - TCP sockets and clock timers, each an actor's - is the `net` vocabulary: docs/net.md; on it, a web
server of actors, `http`: docs/http.md.

## Pitfalls

Each of these has cost someone time:

- **Strict mode**: `CLANKFORTH_STRICT=1` (the tool), or `true strict!` at a program's top, makes every warning an
  error - a definition its stack comment doesn't fit stops the load there, not as garbage bytes far away later.
  `strict? ( -- flag )`. The library loads clean under it; a program meant to run unattended is best developed so.
- **An unknown word** says where it is, if anywhere: `unknown word sql-query (in vocabulary sql: add it to USING:)` -
  the vocabularies loaded, and the library's not loaded yet.

- **A word defined twice in one vocabulary** is warned of (`x is defined again in v`): the later one wins from
  there on, so a variable and a word of one name break whatever uses the first after the second.
- **Your own words shadow everything**, the core too: a file that defines `move` has lost the core
  `move` (copying bytes) for the rest of its words - and gets no warning. Name words after what
  they mean here. `see word` shows what a name compiles to.
- **No word can be defined while another is being compiled** (`: a [ 5 constant b ] ;`): its header would land
  in the middle of the definition's code. It is an error; define it before.
- **Control words compile only inside a definition**: `if`, `do`, `?do`, `case`, `begin` at the top
  of a file or at `--eval` are an error ("only inside a definition"). Wrap them: `: go ... ; go`.
- **A quotation using its word's locals lasts only while the word runs**: a closure, in the word's frame. Keep or
  start a task with a quotation that uses none, and pass it what it needs on the stack.
- **What `>r` puts on the return stack comes off before the word returns** (and before `exit`; `unloop` before an
  `exit` inside a `do` loop): `;` warns where a way out leaves something there.
- **`r@` inside a `do` loop is the loop's index**: the loop keeps its counters on the return
  stack, so what you `>r`'d before the loop is under them. Take it into a local instead.
- **The first `{: :}` of a definition** can't be inside an `if` or a loop; later ones can.
- **Floats** are on their own stack (`.s` shows both: the floats after `F:`). Memory floats are 32-bit
  (`sf@ sf!`); `f@ f!` are for an `fvariable`'s double.
- **Two vocabularies with one name**: the later in `USING:` wins; `vocab:name` reaches the other.

## Tests

`tester` (built in: `USING: tester`): `T{ code -> results }T` runs the code and checks the stack against the
results; `testing name` labels what follows; `test-summary` prints the totals and sets the exit status (the tool's
`exit-status`). `full-suite` divides a file into a quick run and a full one: with `quick-tests` set (the host
sets it, `-1 quick-tests !`) the file ends there, its summary given - the tests after it (long ones: a host's
frames simulated, waits) only in a full run. `checker-tests` divides one likewise: what follows it tests the stack
checker, and a system built without it ends the file there. `skip-file ( -- )` skips the rest of the source being read (the
file, or the string `evaluate`d); `source-dir ( -- s len )` is the folder of the file being read (a new string; "."
for none), for a script's own files beside it. `tests/test_*.fs` are the language's, run by `ctest` with the `clankforth` tool; a host runs its
own with its own tool (`forth_tool_main` with its words bound). `tests/host_call.c` tests the host's side, from C.

A fuzzer (`tests/fuzz_eval.c`, clang's libFuzzer) gives a fresh system random text to interpret, looking for a
mistake that crashes where it should be an error:

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCLANKFORTH_FUZZ=ON && ninja -C build-fuzz
mkdir -p corpus && build-fuzz/fuzz_eval -dict=tests/fuzz.dict -max_len=96 corpus
```

What it finds is to be read: a large number used as an address (`99999999 @`) crashes, and is the language. The
dictionary leaves out the words that take an address for that reason.

What crosses threads - the worker pool, nodes' messages, placement, the load table - is run hard under
ThreadSanitizer by `tests/tsan_stress.fs` (CI's tsan job). It raises no errors on purpose (TSan can't follow an
error's longjmp across the tasks' stacks), and runs threaded for the same reason:

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_C_FLAGS="-fsanitize=thread -g" && ninja -C build-tsan
cd build-tsan && CLANKFORTH_THREADED=1 ./clankforth ../tests/tsan_stress.fs   # "tsan stress: done", no WARNING
```

A second (`tests/fuzz_http.c`) is for what a server reads from the internet: an HTTP request's head and body
(chunked too) and the parameters, cookies and multipart parts read from it; JSON; percent-decoding; gzip and
DEFLATE; a JWT. The input's first byte picks which (0 a request, 1 JSON, 2 decoding, 3 gzip, 4 a JWT); its seeds are
`tests/fuzz-http/`. Here any crash is a fault - none of it takes an address from its input:

```sh
mkdir -p corpus-http && cp tests/fuzz-http/* corpus-http && build-fuzz/fuzz_http -max_len=2048 corpus-http
```

The native backend is checked against threaded code by `tools/native-diff.py`: random definitions of the words it
writes in line (literals of every size, the stack words, arithmetic, comparisons, `within`, `if`, the loops, `i` `j`,
`>r` `r@` `r>`, locals and their frame, `@` `!` `c@` `c!` of a buffer and of what is no address, `exit`, calls of small
definitions it writes into their callers) run four ways - machine code, machine code with each word of a run sent
round by its primitive (`CLANKFORTH_STUBS_ONLY`), machine code calling every word (`CLANKFORTH_CALLS_ONLY`),
threaded code (`CLANKFORTH_THREADED`) - and what each leaves compared (after an error, its message). A change to
the fast paths runs it; CI does, ten seeds:

```sh
cmake -S . -B build-min -G Ninja -DCLANKFORTH_CHECKER=OFF && ninja -C build-min
cd build-min && python3 ../tools/native-diff.py ./clankforth 1 50     # first seed, seeds (each 400 definitions)
```

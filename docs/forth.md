# ClankForth

A small Forth in the spirit of jonesforth, written in C (`src/forth.c`) with the rest of the language in Forth
(`lib/prelude.fs`, built into the library: `forth_prelude`). It is not ANS Forth; it is a scripting language for
games, embedded in a host (a game engine) that adds its own words. Names are case-insensitive.

Below, "the host" is the program that embeds it: its C words, its scripts' folder, its actors. OpenHuey and
OpenMizzurna are two.

## The model

- **Cells** are pointer-sized integers, and addresses are real addresses: `@` and `!` reach C
  structs directly. Memory words come in sizes: `c@ w@ sw@ l@ sl@ @` (8/16/32 bits, signed
  variants) and their stores. Sums, differences and products wrap round (`+ - * 1+ 1- negate abs +! +loop`):
  the largest cell `1+` is the smallest, as Forth has it; so does the smallest `-1 /`. `*/` multiplies to twice a
  cell's bits before it divides. A shift by a cell's width or more (or a negative count) shifts every bit out:
  `lshift` and `rshift` give 0, `arshift` 0 or -1 by the sign. A literal may be any cell's bits (`$ffffffffffffffff`
  is -1); one larger is an error. Cells are 64 bits: ClankForth builds only for 64-bit machines.
- **Mistakes are errors, not crashes**: a number used as an address (below 64 KiB, or negative) - fetched, stored,
  typed, compared, or executed - is an error, and so are a negative length (`type`, `compare`, `evaluate`,
  `included`, `find-word`) and a reach past the dictionary's end. `allot` gives space back with a negative count,
  but not past the dictionary's start; `unused ( -- n )` is what is still free. A definition an error leaves
  unfinished is dropped, its space given back. This doesn't make a script safe to run untrusted: real
  addresses can still reach anything.
- **Floats** have their own stack (doubles). A number with a `.` or an exponent is a float:
  `1.5`, `2e`, `-0.25e3`. In memory floats are 32-bit, as game data has them: `sf@ sf!` (and `f@ f!` for
  a Forth `fvariable`, which holds a double).
- **Words** are indirect threaded: each has a code field (a C function) and a body. `see word`
  decompiles a colon definition.
- **Tasks**: every running piece of Forth has its own stacks. `' word spawn` starts one (it gives
  an id); the host runs each ready task a little every tick (`forth_run_tasks`). Inside a task, `yield` gives way
  until the next step and `n wait` for n steps (a host may add `seconds`: OpenMizzurna's `0.5e seconds wait`). `id kill` stops one, `.tasks` lists them.
  `tasks-paused!` ( flag ) stops the scheduler: no task runs and their waits don't count (the host paused).
  `spawn-held` starts one the scheduler leaves alone: `id resume` gives it its turn (whoever drives
  it decides when; it answers whether the task is still alive). `xt restart` makes the running
  task start over in another word at once, its stacks emptied (a script's "go to").
- **A task that never yields** is stopped after about 20 million steps in one turn ("ran too
  long without yielding"), so one script can't freeze the host.
- **Errors** print a message with the file and line and unwind: at the prompt the stacks are
  emptied; a task that errs is stopped (and the host told: `task_failed`). `' word catch`
  runs a word and gives -1 instead of unwinding (`error-message` has the text).
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
  and the combinators - but `[: ... ;] execute`, `['] w catch` are), a loop whose turns take or leave items (n stores
  in an n-turn loop), a deferred word, `evaluate`, a host's C word with no effect given (`forth_set_effect`), and an
  immediate word (its comment is the code it lays down's).
- `warnings ( -- n )` counts the warnings given (a test can say a file loads without any).

## Vocabularies (as in Factor)

Words live in vocabularies. A file starts by saying where its definitions go and what it uses:

```forth
\ scripts/player.fs
IN: player
USING: engine keys vectors ;
```

- `USING: a b ;` loads each vocabulary that isn't loaded yet from its file - `a` is
  `a.fs` in a root (`forth_add_root`: the host's scripts' folder), `a.b` is `a/b.fs` - and makes its words visible
  here; failing a file, a vocabulary built into the library (`tester`). `USE: a` does one.
- A file sees only its own words, the vocabularies it lists, and the core (`forth`: the
  language itself, from forth.c and the prelude). Nothing leaks in from files it didn't ask for.
- Its own definitions win over everything; a used vocabulary's win over the core. If two used
  vocabularies define the same name, the one later in `USING:` wins (list the general ones -
  a host's `engine` - first, your own nearest last); `vocab:name` reaches the other.
- `<PRIVATE ... PRIVATE>` puts helpers in `name.private`: visible in the file, not to its users
  (`name.private:word` still reaches one, for debugging).
- Vocabularies loading each other (a cycle of USING:s) is an error. So is a name that would reach outside the
  roots: one with `/` or `\`, or nothing between two dots.
- A file defining a word that hides one of a vocabulary it uses gets a warning (`x hides vocab:x`) as it loads
  (the core's aside, and a host's `messages`): rename it, or keep a vocabulary's workings `<PRIVATE ... PRIVATE>` so
  only its face is seen.
- The prompt (`forth_eval`: a host's console, the `clankforth` tool) sees every loaded vocabulary; `USING:` there
  narrows nothing, it loads. `vocabs` lists them, `vocab-words name` lists one, `words` lists
  what the current source sees.
- A host's C words go in its own vocabularies (OpenHuey's and OpenMizzurna's: `engine`).

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
zeroes. A vocabulary's own word can shadow a core one of the same name (an actor kernel's `spawn`): `forth:spawn`
reaches the core's.

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
counts them, `.objects` by kind. Made inside `[: ... ;] with-scratch` they are freed as it ends (an error too) - but
those `rescue ( obj -- obj )`d, kept by the region around it: so a frame's lists, a parse's pieces, need no freeing.

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
returned, or `spawn`ed (another task), it is an error saying so.

Lists: growable arrays of cells. `{ 1 2 3 }` makes one (in a definition, each time it runs),
`list` an empty one; `push ( x l -- )`, `pop`, `nth ( i l -- x )` (negative counts from the end),
`nth!`, `length`, `list-clear`, `list-free` (`dispose`). Higher-order words (prelude.fs): `each`, `map`,
`filter`, `reduce ( l acc xt -- acc' )`, `find ( l xt -- x true | false )`, `any?`, `all?`, `count`,
`contains? ( x l -- flag )`, `range ( n -- l )`, `.list`. Lists made while loading live for good;
free the ones made over and over at run time - or make them in a `with-scratch` (below, Tuples).

Strings: `s" text"` gives `addr len`; `." text"` prints; `type`, `s=`, `compare`.

Comments: `( ... )` and `\ to the end of the line`.

## Pitfalls

Each of these has cost someone time:

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
  spawn a quotation that uses none, and pass it what it needs on the stack.
- **`r@` inside a `do` loop is the loop's index**: the loop keeps its counters on the return
  stack, so what you `>r`'d before the loop is under them. Take it into a local instead.
- **The first `{: :}` of a definition** can't be inside an `if` or a loop; later ones can.
- **Floats** are on their own stack (`.s` shows both: the floats after `F:`). Memory floats are 32-bit
  (`sf@ sf!`); `f@ f!` are for an `fvariable`'s double.
- **Two vocabularies with one name**: the later in `USING:` wins; `vocab:name` reaches the other.

## Tests

`tester` (built in: `USING: tester`): `T{ code -> results }T` runs the code and checks the stack against the
results; `testing name` labels what follows; `test-summary` prints the totals and sets the exit status (the tool's
`exit-status`). `tests/test_*.fs` are the language's, run by `ctest` with the `clankforth` tool; a host runs its
own with its own tool (`forth_tool_main` with its words bound).

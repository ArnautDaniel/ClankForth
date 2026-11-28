\ steps (docs/actors.md, The schedule): a handler's slice is counted in steps, and a word of C's is one however long it
\ works - so machine code counts the words of C's it calls (a loop's turn is its calls, not one), and a word whose
\ work grows with its data counts that work by its size. A handler doing either over and over gives way as often as
\ one doing as much in Forth: here by the turns it takes (the slice is in steps: the same on any machine).
IN: test-steps
USING: tester actors ;

slice-steps constant tool-slice
20000 slice-steps!
message work ( xt -- )
variable worked
behaviour worker  on work ( xt -- )  execute  true worked ! ;  end-behaviour
: turns ( xt -- n )   \ the rounds xt takes in a handler: one, and one more each time its slice ends
    worker -1 s" worker" spawn {: xt w | n :}  false worked !  xt w send work
    begin  deliver  n 1+ to n  worked @  n 100000 > or  until  w kill  deliver  n ;

: hundred-k ( -- s )  [: 10000 0 do  s" abcdefghi " %s  loop ;] build ;
hundred-k rescue-all constant text
: numbers ( n -- l )  list {: n l :}  n 0 do  n i - l push  loop  l ;
20000 numbers rescue-all constant many

testing words of C's called in a loop of machine code: each counted (a turn of the loop was one step whatever it
testing  called - a slice of them ran five times as long as its steps say)
: lengths ( -- )  0  400000 0 do  text length +  loop  drop ;
: nths ( -- )  0  400000 0 do  i 16383 and many nth +  loop  drop ;
T{ ' lengths turns 60 > -> true }T   \ (twenty turns, counted one a turn of the loop)
T{ ' nths turns 60 > -> true }T
\ ... and those of a word's straight code, called over and over: counted as the word begins
: three-lengths ( -- n )  text length  text length +  text length + ;
: called ( -- )  0  200000 0 do  three-lengths +  loop  drop ;
T{ ' called turns 60 > -> true }T

testing work that grows with its data, counted by its size: text looked through, copied, compared, changed, hashed -
testing  each 200 times over 100,000 bytes was one turn (200 steps), the node held for all of it
: finds ( -- )  200 0 do  text string s" zzzz" s-find drop  loop ;
: copies ( -- )  200 0 do  [: text string >string 2drop ;] with-scratch  loop ;
: compares ( -- )  200 0 do  text string text string compare drop  loop ;
: uppers ( -- )  200 0 do  [: text string s-upper 2drop ;] with-scratch  loop ;
: appends ( -- )  200 0 do  [: <bytes> {: b :}  text string b b-append ;] with-scratch  loop ;
: counts ( -- )  200 0 do  text string utf8-length drop  loop ;
: splits ( -- )  20 0 do  [: text string s"  " s-split drop ;] with-scratch  loop ;
T{ ' finds turns 5 > -> true }T
T{ ' copies turns 5 > -> true }T
T{ ' compares turns 5 > -> true }T
T{ ' uppers turns 50 > -> true }T    \ (a loop of C's over each byte: sixteen times a copy's steps)
T{ ' appends turns 5 > -> true }T
T{ ' counts turns 50 > -> true }T
T{ ' splits turns 5 > -> true }T

testing ... a sequence's items gone over: found, turned round, sorted (20,000 numbers, 50 times)
: indexes ( -- )  50 0 do  -1 many index drop  loop ;
: reverses ( -- )  50 0 do  [: many reverse drop ;] with-scratch  loop ;
: sorts ( -- )  50 0 do  [: many clone sort ;] with-scratch  loop ;
T{ ' indexes turns 5 > -> true }T
T{ ' reverses turns 5 > -> true }T
T{ ' sorts turns 40 > -> true }T    \ (a turn a sort: what one word did ends its slice, and no more than that)

testing little work isn't counted at all: a short text's words cost their call
: short-finds ( -- )  2000 0 do  s" a short text, looked through" s" zz" s-find drop  loop ;
T{ ' short-finds turns 3 < -> true }T

tool-slice slice-steps!
text dispose  many list-free
test-summary

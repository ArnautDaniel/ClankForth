\ strings: the words that make them (objects, freed by dispose or a scratch region) and those that look into them
IN: test-strings
USING: tester ;

: same? ( a n a2 n2 -- flag )  s= ;

testing strings made: a copy, two together, a number's digits
T{ s" abc" >string  s" abc" same? -> true }T
T{ s" abc" >string drop string? -> true }T
T{ s" ab" s" cd" s+  s" abcd" same? -> true }T
T{ -42 n>s s" -42" same?  2.5e f>string s" 2.5" same? -> true true }T
T{ s" ab" >string drop string  nip -> 2 }T

testing numbers read back: a whole one (in the base, $ # %), a float - and not one
T{ s" 123" string>number  s" $ff" string>number  s" nope" string>number -> 123 true 255 true 0 false }T
T{ s" 1.5" string>float f>s  s" x" string>float f>s -> true 1 false 0 }T

testing looking in: find, slice, trim, starts, ends
T{ s" hello world" s" world" s-find  s" hello" s" z" s-find  s" abc" s" " s-find -> 6 -1 0 }T
\ (found by its first byte, then compared: a start that isn't it, one that is - at the end, past it, longer than all)
T{ s" aaab" s" aab" s-find  s" abcabd" s" abd" s-find  s" xyz" s" z" s-find  s" xyz" s" zz" s-find -> 1 3 2 -1 }T
: piece-count ( a n a2 n2 -- count )  s-split {: l :}  l length  l [: dispose ;] each  l list-free ;
T{ s" ab" s" abc" s-find  s" " s" a" s-find  s" a,b,,c" s" ," piece-count  s" ,," s" ," piece-count  s" none" s" ," piece-count -> -1 -1 4 3 1 }T
: split-twos ( -- n flag len )  s" a--b-c--" s" --" s-split {: l :}
    l length  1 l nth string s" b-c" s=  2 l nth string nip  l [: dispose ;] each  l list-free ;
T{ split-twos -> 3 true 0 }T
\ s-under: where the first byte under a value is (a control character under 32), -1 if none; a byte is 0 .. 255
T{ s" plain" 32 s-under  s\" tab\there" 32 s-under  s\" \r\n" 14 s-under  s" " 32 s-under -> -1 3 0 -1 }T
T{ s" abc" 98 s-under  s" abc" 0 s-under  s\" \xff\x80" 128 s-under  s\" \xff\x80" 129 s-under -> 0 -1 -1 1 }T
T{ s" abc" -1 s-under -> -1 }T
T{ s" abc" drop -1 32 ' s-under catch nip nip nip  error-message s" s-under: a negative length" s-starts? -> -1 true }T
T{ s" hello world" 6 5 s-slice s" world" same?  s" abc" 1 99 s-slice s" bc" same? -> true true }T
T{ s"   pad  " s-trim s" pad" same?  s"    " s-trim nip -> true 0 }T
T{ s" filename.fs" s" file" s-starts?  s" filename.fs" s" .fs" s-ends?  s" a" s" abc" s-ends? -> true true false }T
T{ s" MiXed" s-upper s" MIXED" same?  s" MiXed" s-lower s" mixed" same? -> true true }T

testing split and join: a list of strings, and back
: pieces ( -- l )  s" a,bb,,c" s" ," s-split ;
T{ pieces length  pieces 1 swap nth string s" bb" same?  pieces 2 swap nth string nip -> 4 true 0 }T
T{ pieces s" -" s-join s" a-bb--c" same?  list s" ," s-join nip -> true 0 }T

testing a scratch region: what is made in it freed as it ends - an error too - but what is rescued
: made-in-scratch ( -- n )  objects {: before :}  [: s" x" >string 2drop  s" a b" s"  " s-split drop ;] with-scratch  objects before - ;
T{ made-in-scratch -> 0 }T
: fails-in-scratch ( -- )  [: s" y" >string 2drop  0 @ drop ;] with-scratch ;
: failed-count ( -- n )  objects {: before :}  ['] fails-in-scratch catch drop  objects before - ;
T{ failed-count -> 0 }T
: kept-from-scratch ( -- a n )  [: s" kept" >string over rescue drop ;] with-scratch ;
T{ kept-from-scratch s" kept" same? -> true }T
: nested ( -- n )  objects {: before :}  [: [: s" in" >string drop rescue drop ;] with-scratch ;] with-scratch  objects before - ;
T{ nested -> 0 }T   \ (rescued from the inner: the outer's, freed as it ends)
: nested-all ( -- s )  [: [: s" out" >string drop rescue-all ;] with-scratch ;] with-scratch ;
T{ nested-all  dup string s" out" s=  swap dispose -> true }T   \ (out of both: kept till disposed)
testing rescue takes what an object holds with it: a list's strings made in the same region
: deep-kept ( -- l )  [: { s" deep" >string drop } rescue ;] with-scratch ;
T{ deep-kept  0 over nth string s" deep" s=  swap dispose -> true }T
: deep-kept-all ( -- l )  [: [: { s" all" >string drop } rescue-all ;] with-scratch ;] with-scratch ;
T{ deep-kept-all  0 over nth string s" all" s=  swap dispose -> true }T
: let-go ( -- n )  objects {: before :}  s" z" >string drop dispose  objects before - ;
T{ let-go -> 0 }T
: stale ( -- )  s" z" >string drop dup dispose string 2drop ;
T{ ' stale catch -> -1 }T

testing n>str: every number, the most negative too
T{ -9223372036854775807 1- n>str s" -9223372036854775808" s= -> true }T
T{ 0 n>str s" 0" s= -45 n>str s" -45" s= 1230 n>str s" 1230" s= -> true true true }T
testing $+ with a negative length adds nothing
T{ $clear  s" abc" drop -1 $+  $@ nip -> 0 }T

testing format: each {} an argument, the first the deepest
: says ( a n a2 n2 -- flag )  {: a n a2 n2 :}  a n a2 n2 compare 0=  a dispose ;
T{ s" ann" 42 s" {s} is {} years" format  s" ann is 42 years" says -> true }T
T{ 255 255 -1 s" {x} {05X} {u}" format  s" ff 000FF 18446744073709551615" says -> true }T
T{ 3.14159e 2.5e s" pi={.2f} half={f}" format  s" pi=3.14 half=2.5" says -> true }T
T{ s" ab" s" cd" s" [{-4s}][{4s}] {{x}}" format  s" [ab  ][  cd] {x}" says -> true }T
T{ 233 s" caf{c}" format  s" café" says -> true }T
T{ 1 s" {} {}" ' format catch nip nip nip -> -1 }T   \ (two {}s, one argument: an error)
T{ s" {q" ' format catch nip nip -> -1 }T
testing UTF-8: characters, not bytes
: snowman ( -- a n )  s" naïve ☃" ;
T{ snowman utf8-length  snowman nip  snowman utf8-valid? -> 7 10 true }T
T{ snowman 2 3 utf8-slice s" ïve" compare -> 0 }T
T{ s" ☃x" utf8-next  s" x" compare -> 9731 0 }T
: bad-utf8 ( -- a n )  here 255 c, 192 c, 128 c,  3 ;   \ (a stray byte, an overlong form)
T{ bad-utf8 utf8-valid?  bad-utf8 utf8-length -> false 3 }T
T{ 9731 char>string  s" ☃" says -> true }T
T{ $D800 ' char>string catch nip -> -1 }T   \ (a surrogate is no character)
: chars-pushed ( -- flag )  <bytes> {: b :}  9731 b b-push-char  65 b b-push-char  b b-data s" ☃A" compare 0=  b dispose ;
T{ chars-pushed -> true }T

testing s\" : escapes for what a line can't hold
T{ s\" a\tb\r\n" nip  s\" \x41\x42" s" AB" compare  s\" q\"q\\" nip -> 5 0 4 }T
: escaped ( -- a n )  s\" \e[1m\n" ;
T{ escaped nip  escaped drop c@ -> 5 27 }T

testing s" at the prompt longer than its buffer: an error, not cut short (inside a definition: whole)
<bytes> constant long-line
: long-text ( -- )  s\" s\" " long-line b-append  300 0 do  [char] x long-line b-push  loop  [char] " long-line b-push ;
long-text
T{ long-line b-data ' evaluate catch nip nip -> -1 }T
: long-definition ( -- n )   \ the long string inside a definition: its length
    s" : long-word " long-line b-data s+  s"  ; long-word nip" s+ {: s n :}  s n evaluate  s dispose ;
T{ long-definition -> 300 }T

testing an object's block used again (those let go are kept, by size, for the next): what is made in one is as new
\ a string of n bytes, each its place's letter: made, read back, let go - at the sizes the blocks are kept by, and past
: lettered ( n -- s )  {: n :}  [: n 0 ?do  i 26 mod [char] a + %c  loop ;] build ;
: lettered? ( s n -- flag )  {: s n :}  s string nip n =  n 0 ?do  s i + c@  i 26 mod [char] a + = and  loop ;
: round ( n -- flag )  {: n :}  n lettered {: s :}  s n lettered?  s dispose ;
: rounds ( -- flag )  true  { 0 1 15 16 17 31 32 255 256 257 1000 } {: sizes :}
    3 0 do  sizes [: round and ;] each  loop  sizes list-free ;
T{ objects  rounds  swap objects = -> true true }T
\ a list, a map, bytes, each in a block a string full of letters just left: empty, as a new one is
: after-letters ( -- n1 n2 n3 )
    48 lettered dispose  list {: l :}  48 lettered dispose  <map> {: m :}  48 lettered dispose  <bytes> {: b :}
    l length  m map-size  b b-length  l list-free  m dispose  b dispose ;
: after-letters-kept ( -- n1 n2 n3 flag )  objects {: o :}  after-letters  objects o = ;
T{ after-letters-kept -> 0 0 0 true }T
\ more let go at once than are kept of a size, then as many made again: each its own, none another's
: many ( -- flag )
    list {: l :}  200 0 do  20 lettered l push  loop  l [: dispose ;] each  l list-clear
    200 0 do  i 26 mod [char] a + {: c :}  [: 20 0 do  c %c  loop ;] build l push  loop
    true  200 0 do  i l nth c@  i 26 mod [char] a + = and  loop
    l [: dispose ;] each  l list-free ;
T{ objects  many  swap objects = -> true true }T

test-summary

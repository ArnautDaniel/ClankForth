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
: let-go ( -- n )  objects {: before :}  s" z" >string drop dispose  objects before - ;
T{ let-go -> 0 }T
: stale ( -- )  s" z" >string drop dup dispose string 2drop ;
T{ ' stale catch -> -1 }T

test-summary

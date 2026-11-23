\ maps (keys cells or strings, kept in the order they came) and bytes
IN: test-maps
USING: tester ;

testing a map: string keys by their bytes, other keys by their cell
<map> constant m
1 s" one" m s-at!   2 s" two" m s-at!   99 42 m at!
T{ s" two" m s-at -> 2 true }T
T{ s" two" >string drop dup m at  rot dispose -> 2 true }T   \ (a string object as a key: by its bytes too)
T{ 42 m at  7 m at -> 99 true false }T
T{ m map-size  s" one" m s-key?  s" three" m s-key?  42 m key? -> 3 true false true }T
T{ 3 s" one" m s-at!  s" one" m s-at  m map-size -> 3 true 3 }T   \ (set again: replaced, no new entry)
testing their order: as they came, a key set again keeping its place
: key-text ( key -- x )  dup string? if string drop c@ then ;   \ (a string key: its first byte)
T{ m keys dup length  swap dup 0 swap nth key-text  swap dup 2 swap nth  swap list-free -> 3 char o 42 }T
T{ m values dup 0 swap nth  swap dup 1 swap nth  swap list-free -> 3 2 }T
variable seen
T{ 0 seen !  m [: nip seen +! ;] map-each  seen @ -> 104 }T
testing a map's string keys are its own: dispose refuses them, delete-at lets them go
T{ m keys 0 over nth  ' dispose catch  swap drop  swap list-free -> -1 }T
T{ objects  s" one" m s-delete-at  objects -  m map-size  s" one" m s-key? -> 1 2 false }T
T{ 12345 m delete-at  m map-size -> 2 }T   \ (no such key: nothing)
testing many entries: the map grows, its order kept, the keys taken away and put back
: fill ( n m -- )  {: n m :}  n 0 do  i dup m at!  loop ;
<map> constant big
T{ 10000 big fill  big map-size  5000 big at  9999 big at -> 10000 5000 true 9999 true }T
: thin ( m -- )  {: m :}  10000 0 do  i m delete-at  2 +loop ;
T{ big thin  big map-size  4 big key?  5 big key? -> 5000 false true }T
T{ 4 4 big at!  big keys dup length  swap dup -1 swap nth  swap list-free -> 5001 4 }T
testing map-each: values may change as it walks, entries not
T{ big [: drop dup 1+ swap big at! ;] map-each  5 big at -> 6 true }T
: grows-while-walked ( -- )  big [: 2drop  -1 -1 big at! ;] map-each ;
: emptied ( -- m )  <map> {: m :}  1 7 m at!  2 8 m at!  7 m delete-at  8 m delete-at  m ;   \ (every entry taken away)
T{ 0 seen !  emptied [: 2drop 1 seen +! ;] map-each  seen @ -> 0 }T   \ (none walked: not taken for a change)
: shrinks-while-walked ( -- )  <map> {: m :}  1 7 m at!  2 8 m at!  m [: drop drop  7 m delete-at ;] map-each ;
T{ ' shrinks-while-walked catch 0<> -> true }T
T{ ' grows-while-walked catch -> -1 }T
testing clone, clear, dispose: their string keys go with them
: cloned ( -- n value flag )  m clone {: c :}  c map-size  s" two" c s-at  c dispose ;
T{ cloned -> 2 2 true }T
T{ objects  m map-clear  objects -  m map-size -> 1 0 }T
T{ objects  big dispose  m dispose  objects - -> 2 }T

testing bytes: pushed, appended, read, grown and cut
<bytes> constant b
T{ 65 b b-push  66 b b-push  s" CD" b b-append  b b-length  0 b b@  3 b b@ -> 4 65 68 }T
T{ b b-data s" ABCD" compare -> 0 }T
T{ b b-data b b-append  b b-data s" ABCDABCD" compare -> 0 }T   \ (its own bytes appended)
T{ 90 1 b b!  1 b b@  6 b b-resize  b b-length  b b>string over >r s" AZCDAB" compare  r> dispose -> 90 6 0 }T
T{ 8 b b-resize  7 b b@  b b-clear  b b-length -> 0 0 }T
T{ 9 b ' b@ catch  nip nip -> -1 }T   \ (past its end: an error)
T{ b clone  dup b-length  swap dispose  b dispose -> 0 }T

test-summary

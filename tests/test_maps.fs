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
: key-text ( key -- x )  dup string? if  first  then ;   \ (a string key: its first byte)
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

testing an entry's place: the key after one (none after the last, or of one not there), one moved to before another
<map> constant o
: ordered ( -- a n )  o keys " ,"  join string ;
1 s" a" o s-at!  2 s" b" o s-at!  3 s" c" o s-at!  4 s" d" o s-at!
T{ s" a" o s-key-after  swap string s" b" s= -> true true }T
T{ s" d" o s-key-after  s" zz" o s-key-after -> false false }T
T{ s" b" o s-delete-at  s" a" o s-key-after  swap string s" c" s= -> true true }T   \ (over the hole a delete left)
T{ 2 s" b" o s-at!  ordered s" a,c,d,b" s= -> true }T
T{ s" b" s" c" o s-move-before  ordered s" a,b,c,d" s=  s" b" o s-at  s" d" o s-at -> true 2 true 4 true }T
T{ s" a" s" d" o s-move-before  ordered s" b,c,a,d" s=  s" a" o s-at  o map-size -> true 1 true 4 }T
T{ s" a" s" zz" o s-move-before  s" zz" s" a" o s-move-before  ordered s" b,c,a,d" s= -> true }T   \ (one not there: nothing)
T{ 5 s" e" o s-at!  s" e" o s-at  ordered s" b,c,a,d,e" s= -> 5 true true }T
o dispose

testing s-hash: a number of a text - the same text the same number, another text another, "" one too; of bytes
testing  anywhere (an object's, a literal's)
T{ s" a region's html" s-hash  s" a region's html" s-hash = -> true }T
T{ s" a region's html" s-hash  s" a region's htmk" s-hash =  s" " s-hash  s" " s-hash = -> false true }T
T{ s" abc" >string drop dup string s-hash  swap dispose  s" abc" s-hash = -> true }T
T{ s" ab" s-hash  s" ba" s-hash =  s" a" s-hash 0=  -> false false }T

testing fields>map: a message's header fields (name: value, CR LF between) in a map - names lower-cased, values
testing  without the spaces round them, a name said twice both; false for too many lines, no name, a name no token
\ (made in a scratch region: the values are strings of their own, freed as it ends - the objects as they were)
: field ( a n m -- a' n' )  s-at if  string  else  s" (none)"  then ;
: fields ( a n -- m flag )  <map> {: m :}  100 m fields>map  m swap ;
: two-fields ( -- flag )
    s\" Host: example.org\r\nX-Mixed-CASE:  padded \t\r\nEmpty:\r\nUrl: http://a:8/b" fields {: m ok :}
    ok  s" host" m field s" example.org" s=  and  s" x-mixed-case" m field s" padded" s=  and
    s" empty" m field nip 0=  and  s" url" m field s" http://a:8/b" s=  and  m map-size 4 =  and ;
T{ objects  ' two-fields with-scratch  swap objects = -> true true }T
: said-twice ( -- flag )
    s\" Accept: a\r\nX: 1\r\nACCEPT: b\r\naccept:c" fields {: m ok :}  ok  s" accept" m field s" a, b, c" s=  and ;
T{ ' said-twice with-scratch -> true }T
: long-name ( -- flag )   \ (longer than the primitive lower-cases on C's stack: 70 bytes)
    [: 70 0 do  [char] N %c  loop  s" : v" %s ;] build string fields {: m ok :}
    ok  [: 70 0 do  [char] n %c  loop ;] build string m field s" v" s=  and ;
T{ ' long-name with-scratch -> true }T
: reads? ( a n -- flag )  fields nip ;
: nothing-there ( -- flag n )  s" " fields {: m ok :}  ok  m map-size ;
T{ ' nothing-there with-scratch -> true 0 }T
T{ [: s" Name: v" reads?  s" :v" reads?  s" no colon" reads?  s" Na me: v" reads?  s" Name : v" reads? ;] with-scratch
   -> true false false false false }T
T{ [: s\" N\xc3\xa9: v" reads?  s" N(1): v" reads?  s\" A: 1\r\n" reads?  s\" A: 1\r\n\r\nB: 2" reads? ;] with-scratch
   -> false false false false }T
T{ [: s" aZ09!#$%&'*+-.^_`|~: v" reads? ;] with-scratch -> true }T
: three-of ( max -- flag )  {: max :}  <map> {: m :}  s\" A: 1\r\nB: 2\r\nC: 3" max m fields>map ;
T{ [: 3 three-of  2 three-of  0 three-of ;] with-scratch -> true false false }T
: not-text ( -- )  <map> {: m :}  7 s" a" m s-at!  s\" A: 1" 9 m fields>map drop ;   \ (a number there already)
T{ [: ['] not-text catch ;] with-scratch  error-message s" fields>map: not a string" s-starts? -> -1 true }T

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

\ json (basis/json): text read into maps, lists, strings and numbers, and written back
IN: test-json
USING: tester accessors actors json ;

: round-trip ( a n a2 n2 -- flag )   \ the first read and written is the second
    {: a n ea en :}  a n json> {: v :}  v >json {: s sn :}  s sn ea en s=  s dispose  v json-free ;
: fails? ( a n -- flag )  {: a n :}  [: a n json> json-free ;] catch 0<> ;

testing scalars: numbers, strings, literals
T{ s" 42" json> -> 42 }T
T{ s" -7" json> -> -7 }T
T{ s"  true " json> json-true = -> true }T
T{ s" null" json> json-null = -> true }T
T{ s" 2.5" json> value>> f>s -> 2 }T
T{ s" 1e3" json> dup json-float instance? swap value>> f>s -> true 1000 }T
T{ s" 123456789012345678901234" json> json-float instance? -> true }T
T{ s\" \"a\\nb\\u0041\\u00e9\"" json> string  s\" a\nbA\xc3\xa9" s= -> true }T
T{ s\" \"\\ud83d\\ude00\"" json> string  s\" \xf0\x9f\x98\x80" s= -> true }T
testing arrays and objects
: nested-lists ( -- n n2 )  s" [1, [2, 3], []]" json> {: l :}  l length  1 l nth length  l json-free ;
T{ nested-lists -> 3 2 }T
: ann ( -- v )  s\" {\"name\": \"ann\", \"age\": 7, \"tags\": [\"a\"]}" json> ;
: ann-fields ( -- age flag size )  ann {: m :}  s" age" m s-at drop  s" name" m s-at drop string s" ann" s=  m map-size  m json-free ;
T{ ann-fields -> 7 true 3 }T
testing written back: the order kept, strings escaped, floats with a point, a map's number keys as strings
T{ s\" {\"b\":1,\"a\":[true,false,null],\"c\":{}}" s\" {\"b\":1,\"a\":[true,false,null],\"c\":{}}" round-trip -> true }T
T{ s\" \"q\\\"\\\\\\n\\u0001\"" s\" \"q\\\"\\\\\\n\\u0001\"" round-trip -> true }T
T{ s" [3.0, 0.5]" s" [3.0,0.5]" round-trip -> true }T
: numbered ( -- flag )  <map> {: m :}  s" x" >string drop 5 m at!  m >json {: s n :}  s n s\" {\"5\":\"x\"}" s=  s dispose  m json-free ;
T{ numbered -> true }T
T{ 1 json-bool json-true = -> true }T
testing what isn't JSON: an error, nothing kept
objects constant before
T{ s" [1, 2" fails? -> true }T
T{ s\" {\"a\" 1}" fails? -> true }T
T{ s" 01" fails? -> true }T
T{ s" [1] x" fails? -> true }T
T{ s" tru" fails? -> true }T
T{ s\" \"\x01\"" fails? -> true }T
T{ s\" [\"a\", {\"b\": [1, 2, nope]}]" fails? -> true }T
T{ objects before - -> 0 }T
T{ s" [1, 2" fails? drop  error-message s" json: ']' expected at byte 5" s= -> true }T
3 to json-depth-max
T{ s" [[[[1]]]]" fails? -> true }T
T{ s" [[[1]]]" json> json-free -> }T
32 to json-depth-max
testing the depth at first: 32 read, copied, written and freed - 33 an error saying so (not the return stack's)
: deep ( d -- a n )  {: d :}  <bytes> {: b :}  d 0 ?do  [char] [ b b-push  loop  d 0 ?do  [char] ] b b-push  loop  b b>string ;
: deep-ok? ( d -- flag )
    {: d :}  d deep {: a n :}  [: a n json> {: v :}  v json-copy {: c :}  c >json 2drop  c json-free  v json-free ;] catch
    a dispose  0= ;
T{ 32 deep-ok?  33 deep-ok? -> true false }T
T{ error-message s" nested too deeply" s-find 0>= -> true }T

testing a value's fields of the kind wanted: false for none, another kind, or a value not an object
s\" {\"name\":\"ann\",\"age\":41,\"ok\":true,\"no\":false,\"tags\":[1],\"m\":{},\"f\":2.5,\"z\":null}" json> constant person
T{ s" name" person json-string@  -rot s" ann" s= -> true true }T
T{ s" age" person json-string@ -> false }T
T{ s" age" person json-int@ -> 41 true }T
T{ s" f" person json-int@ -> false }T
T{ s" name" person json-int@ -> false }T
T{ s" ok" person json-bool@  s" no" person json-bool@ -> true true false true }T
T{ s" z" person json-bool@ -> false }T
T{ s" tags" person json-list@ nip  s" m" person json-map@ nip -> true true }T
T{ s" tags" person json-map@  s" missing" person json-list@ -> false false }T
T{ s" name" 5 json-string@  s" name" s" x" >string drop json-string@ -> false false }T

testing json's true, false and null are the system's for good: dispose refuses them, no rescue takes them
T{ json-true ' dispose catch nip  json-null ' dispose catch nip -> -1 -1 }T
T{ json-false rescue-all json-false =  json-false object? -> true true }T

testing json-copy: a tree of its own - its strings, lists, maps, bytes and floats copied (the original let go after,
testing  the copy whole); numbers and the constants as they are
: original ( -- v )
    s\" {\"s\":\"text\",\"l\":[1,{\"x\":2.5}],\"t\":true,\"n\":7}" json> {: m :}
    <bytes> {: b :}  9 b b-push  b s" b" m s-at!  m ;
: copied-after-free ( -- copy )  original {: o :}  o json-copy  o json-free ;
T{ copied-after-free >json s\" {\"s\":\"text\",\"l\":[1,{\"x\":2.5}],\"t\":true,\"n\":7,\"b\":\"0x09\"}" s= -> true }T
T{ s" b" copied-after-free s-at drop bytes?  42 json-copy  json-null json-copy -> true 42 json-null }T

testing json-text, json-int, json-flag: a field with a default - no flag to test ("" / 0 / false: not there, null,
testing  another kind, or not an object at all)
s\" {\"s\":\"Bond\",\"n\":42,\"t\":true,\"f\":false,\"z\":null,\"l\":[1]}" json> constant row
T{ s" s" row json-text s" Bond" s=  s" n" row json-text s" 42" s= -> true true }T
T{ s" z" row json-text nip  s" l" row json-text nip  s" q" row json-text nip  s" s" 0 json-text nip -> 0 0 0 0 }T
T{ s" n" row json-int  s" s" row json-int  s" q" row json-int -> 42 0 0 }T
T{ s" t" row json-flag  s" f" row json-flag  s" n" row json-flag  s" q" 7 json-flag -> true false false false }T

testing json> in a handler: a tree of any depth the handler's (its region's, or kept with state!) - nothing of it
testing  left once the actor is gone (each level rescued again went a region further out: never freed)
message parse-deep ( -- )
behaviour deep-parser  on parse-deep ( -- )  s\" {\"a\":[[[\"x\",\"y\"]],{\"b\":{\"c\":[1,2.5]}}]}" json> drop ;  end-behaviour
: deep-cycle ( -- )  deep-parser -1 s" deep" spawn {: a :}  a send parse-deep  deliver  a kill  deliver ;
deep-cycle   \ (the actor kind's first: its own objects made once)
objects constant objects-deep
deep-cycle deep-cycle deep-cycle
T{ objects objects-deep - -> 0 }T

testing numbers written as they were read: a float's digits all (not %g's six); 19 digits a whole number if it fits
: round ( a n -- s len )  json> {: v :}  v >json  v json-free ;
T{ s" [1700000000.123,12345678.9,3.14159265358979,0.1]" round s" [1700000000.123,12345678.9,3.14159265358979,0.1]" s= -> true }T
T{ s" [9223372036854775807,-1234567890123456789]" round s" [9223372036854775807,-1234567890123456789]" s= -> true }T
T{ s" [9223372036854775808]" round s" e+18" s-find 0> -> true }T
testing a string not UTF-8 written with U+FFFD in place of each bad byte: what comes out is JSON
: bad-bytes-json ( -- a n )  <map> {: m :}  s\" a\xffb" >string drop s" k" m s-at!  m >json ;
T{ bad-bytes-json  2dup utf8-valid?  -rot s\" \xef\xbf\xbd" s-find 0> -> true true }T

test-summary

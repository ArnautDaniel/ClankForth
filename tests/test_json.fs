\ json (basis/json): text read into maps, lists, strings and numbers, and written back
IN: test-json
USING: tester accessors json ;

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
128 to json-depth-max

test-summary

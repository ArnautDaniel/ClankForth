\ json.fs - JSON (RFC 8259) read into the language's objects and written back.
\   s\" {\"name\": \"ann\", \"tags\": [1, 2]}" json> ( -- v )     v >json ( -- s len )
\ An object is a map (its keys strings), an array a list, a string a string, a whole number a cell; a number with a
\ fraction or an exponent (or past a cell) a json-float (`value>>` its float); true, false and null the objects
\ json-true, json-false and json-null. What `json>` gives is one tree: `json-free` lets it all go (made inside a
\ scratch region - a web handler's - it goes with the region).
IN: json
USING: accessors ;

TUPLE: json-literal  name ;
TUPLE: json-float  F: value ;
s" null" >string drop <json-literal> permanent constant json-null
s" true" >string drop <json-literal> permanent constant json-true
s" false" >string drop <json-literal> permanent constant json-false
32 value json-depth-max          \ arrays and objects inside each other, at most: deeper is an error (as deep as
                                 \ objects go inside objects: read, copied, written, sent and freed)

: json-bool ( flag -- v )  if  json-true  else  json-false  then ;

<PRIVATE
19 constant cell-digits          \ a whole number of more digits is past a cell: read as a float (19: if it fits)
s" 9223372036854775807" >string drop constant cell-max-digits   \ (the most a cell holds, its digits)
$d800 constant high-surrogate    $dc00 constant low-surrogate    $400 constant surrogate-span
$10000 constant past-bmp

task-variable src  task-variable src-n  task-variable pos  task-variable depth   \ (each handler its own)

: peek ( -- c )  pos @ src-n @ < if  src @ pos @ + c@  else  -1  then ;   \ -1 at the end
: advance ( -- )  1 pos +! ;
: ws? ( c -- flag )  {: c :}  c 32 =  c 9 = or  c 10 = or  c 13 = or ;
: rest ( -- a n )  src @ pos @ +  src-n @ pos @ - ;   \ (what is left to read)
: skip-ws ( -- )  rest skip-blanks pos +! ;
: digit? ( c -- flag )  [char] 0 [char] 9 1+ within ;
: fail ( a n -- )  pos @ s" json: {s} at byte {}" format drop throw ;
: expect ( c -- )  {: c :}  skip-ws  peek c <> if  c s" '{c}' expected" format fail  then  advance ;
: word-is ( a n -- )   \ the literal's letters there
    {: a n :}  src @ pos @ + src-n @ pos @ - n min  a n s= 0= if  s" a value expected" fail  then  n pos +! ;

: hex4 ( -- n )   \ \uXXXX's four digits
    {: | n :}
    4 0 do
        peek {: c :}
        c digit? if  c [char] 0 -
        else  c 32 or {: l :}  l [char] a [char] f 1+ within if  l [char] a - 10 +  else  s" \u wants four hex digits" fail 0  then
        then
        n 4 lshift or to n  advance
    loop  n ;
: escape ( b -- )   \ after the backslash
    {: b | c :}
    peek to c  advance
    c case
        [char] " of  [char] " b b-push  endof
        [char] \ of  [char] \ b b-push  endof
        [char] / of  [char] / b b-push  endof
        [char] b of  8 b b-push  endof
        [char] f of  12 b b-push  endof
        [char] n of  10 b b-push  endof
        [char] r of  13 b b-push  endof
        [char] t of  9 b b-push  endof
        [char] u of
            hex4 {: u :}
            u high-surrogate low-surrogate within if   \ (a pair: the low half next)
                peek [char] \ =  if  advance  peek [char] u = if  advance  else  s" a surrogate's other half expected" fail  then
                else  s" a surrogate's other half expected" fail  then
                hex4 {: lo :}
                lo low-surrogate dup surrogate-span + within 0= if  s" a surrogate's other half expected" fail  then
                u high-surrogate - 10 lshift  lo low-surrogate - or  past-bmp + to u
            then
            u b b-push-char
        endof
        s" a known escape expected" fail
    endcase ;
: parse-string ( -- s )   \ after the opening quote (its plain runs taken whole: scan-plain)
    {: | b c k :}
    rest scan-plain to k   \ (most strings: no escape - made from the bytes at once)
    k src-n @ pos @ - <  if  src @ pos @ + k + c@ [char] " =  else  false  then  if
        src @ pos @ + k >string drop  k 1+ pos +!  exit
    then
    <bytes> to b
    begin
        rest scan-plain to k  src @ pos @ + k b b-append  k pos +!
        peek to c  c [char] " <>
    while
        c 0< if  s" the string's end expected" fail  then
        c 32 < if  s" a control character in a string" fail  then
        advance  b escape
    repeat
    advance  b b>string drop  b dispose ;
: digits ( -- )   \ one or more
    rest scan-digits {: k :}  k 0= if  s" a digit expected" fail  then  k pos +! ;
: parse-number ( -- v )
    {: | start float? :}  pos @ to start
    peek [char] - = if  advance  then
    peek [char] 0 = if  advance  peek digit? if  s" no leading zeros" fail  then  else  digits  then
    peek [char] . = if  advance  digits  true to float?  then
    peek 32 or [char] e = if
        advance  peek [char] + =  peek [char] - = or if  advance  then  digits  true to float?
    then
    src @ start +  pos @ start - {: a n :}
    a c@ [char] - = if  1  else  0  then {: sign :}
    float? 0=  n sign - cell-digits =  and if   \ (19 digits: a whole number if they fit a cell, else a float)
        a sign + cell-digits cell-max-digits string compare 0> if  true to float?  a n s" e0" s+ to n to a  then
    then
    float? 0= n sign - cell-digits > and if   \ (digits past a cell: a float, said as one)
        a n s" e0" s+ {: e en :}  e en string>float  e dispose  0= if  s" a number expected" fail  then  <json-float>
    else float? if
        a n string>float 0= if  s" a number expected" fail  then  <json-float>
    else  a n string>number drop  then then ;

defer parse-value ( -- v )
: nested ( -- )  1 depth +!  depth @ json-depth-max > if  s" nested too deeply" fail  then ;
: parse-array ( -- l )   \ after the [
    {: | l :}  nested  list to l  skip-ws
    peek [char] ] = if  advance  -1 depth +!  l exit  then
    begin
        parse-value l push  skip-ws
        peek [char] , = if  advance  false  else  [char] ] expect  true  then
    until
    -1 depth +!  l ;
: parse-object ( -- m )   \ after the {
    {: | m :}  nested  <map> to m  skip-ws
    peek [char] } = if  advance  -1 depth +!  m exit  then
    begin
        [char] " expect  parse-string {: k :}
        [char] : expect  parse-value  k string m s-at!  k dispose  skip-ws
        peek [char] , = if  advance  false  else  [char] } expect  true  then
    until
    -1 depth +!  m ;
: (parse-value) ( -- v )
    skip-ws  peek {: c :}
    c [char] { = if  advance parse-object exit  then
    c [char] [ = if  advance parse-array exit  then
    c [char] " = if  advance parse-string exit  then
    c [char] t = if  s" true" word-is  json-true exit  then
    c [char] f = if  s" false" word-is  json-false exit  then
    c [char] n = if  s" null" word-is  json-null exit  then
    c [char] - =  c digit? or if  parse-number exit  then
    s" a value expected" fail  0 ;
' (parse-value) is parse-value

: literal? ( v -- flag )  {: v :}  v json-null =  v json-true = or  v json-false = or ;
\ the tree out of the scratch region it was made in, into the one around: once, at its root - rescue takes what it holds
\ with it (each object rescued again went a region further out, at last out of every one: never freed)
: keep-tree ( v -- )  {: v :}  v literal?  v object? 0= or if  exit  then  v rescue drop ;
PRIVATE>

\ the JSON text read: a value (an error, saying where, for text that isn't JSON - `catch` it)
: json> ( a n -- v )
    {: a n :}
    [:  a src !  n src-n !  0 pos !  0 depth !
        parse-value  skip-ws  pos @ src-n @ < if  s" the text's end expected" fail  then
        dup keep-tree
    ;] with-scratch ;

\ what json> gave let go: every object in it
: json-free ( v -- )
    {: v :}
    v json-null =  v json-true = or  v json-false = or  if  exit  then
    v object? 0= if  exit  then
    v list? if  v length 0 ?do  i v nth recurse  loop  then
    v map? if  v values {: l :}  l length 0 ?do  i l nth recurse  loop  l list-free  then
    v dispose ;

\ a value copied all the way down - a map's keys and values, a list's items, strings, bytes, json-floats - a tree of
\ its own (numbers and json's true, false and null as they are): what to keep of a value someone else will let go
\ (a page's state, a message's), without its JSON's text between (>json json> would turn bytes into "0x" text)
: json-copy ( v -- v' )
    {: v | c :}
    v object? 0= if  v exit  then
    v literal? if  v exit  then
    v map? if  <map> to c  v [: {: k x :}  x recurse  k string? if  k string  else  k n>str  then  c s-at! ;] map-each  c exit  then
    v list? if  v [: recurse ;] map exit  then
    v string? if  v string >string drop exit  then
    v clone ;   \ (bytes, a json-float: no objects inside)

\ ---- writing ----
<PRIVATE
\ a string's bytes go as they are, a run at a time, but those JSON writes otherwise: " \ and what is under a space
\ the bytes a string writes otherwise - a control character, the quote, the backslash - marked, for s-marked to find
\ the next (made once, read by every handler)
create escaped 256 allot
: mark-escaped ( -- )  escaped 256 erase  32 0 do  1 escaped i + c!  loop  1 escaped [char] " + c!  1 escaped [char] \ + c! ;
mark-escaped
: plain-run ( a n i -- j )   \ from i, the first byte written otherwise (n: none)
    {: a n i :}  a i +  n i -  escaped s-marked  i + ;
: write-escaped ( c b -- )
    {: c b :}
    c [char] " = if  s\" \\\"" b b-append  exit  then
    c [char] \ = if  s" \\" b b-append  exit  then
    c 10 = if  s" \n" b b-append  exit  then
    c 13 = if  s" \r" b b-append  exit  then
    c 9 = if  s" \t" b b-append  exit  then
    c s" \u{04x}" format {: s sn :}  s sn b b-append  s dispose ;
: write-string ( a n b -- )   \ (bytes not UTF-8 written as U+FFFD: what comes out is JSON, and a browser's)
    {: a n b | i j :}  a n utf8-valid? 0= if  a n utf8-clean to n to a  then  [char] " b b-push
    begin  i n <  while
        a n i plain-run to j
        j i > if  a i +  j i -  b b-append  then
        j n < if  a j + c@ b write-escaped  j 1+ to j  then
        j to i
    repeat
    [char] " b b-push ;
: write-float ( b -- ) ( F: x -- )
    {: b :}  fdup fdup f- 0e f= 0= if  fdrop  s" null" b b-append  exit  then   \ (infinite, or not a number)
    f>exact {: s sn :}   \ (its digits all: 1700000000.123, not %g's 1.7e+09)
    s sn b b-append
    s sn s" ." s-find 0<  s sn s" e" s-find 0< and if  s" .0" b b-append  then
    s dispose ;
defer write-value ( v b -- )
: hex-digit ( n -- c )  dup 10 < if  [char] 0 +  else  10 - [char] a +  then ;
: (write-value) ( v b -- )
    {: v b :}
    v literal? if  v name>> string b b-append  exit  then
    v string? if  v string b write-string  exit  then
    v list? if
        [char] [ b b-push
        v length 0 ?do  i if  [char] , b b-push  then  i v nth b write-value  loop
        [char] ] b b-push  exit
    then
    v map? if
        [char] { b b-push  true {: first :}
        v [:  {: k x :}  first 0= if  [char] , b b-push  then  false to first
            k string? if  k string  else  k n>str  then  b write-string  [char] : b b-push  x b write-value ;] map-each
        [char] } b b-push  exit
    then
    v bytes? if   \ (bytes - a binary column's: "0x" and their hex, as SQL Server writes them)
        s\" \"0x" b b-append  v b-data {: a n :}
        n 0 ?do  a i + c@ {: c :}  c 4 rshift hex-digit b b-push  c 15 and hex-digit b b-push  loop
        [char] " b b-push  exit
    then
    v object? if
        v json-float instance? if  v value>> b write-float  exit  then
        s" >json: a tuple that isn't JSON's" >string drop throw
    then
    v n>str b b-append ;
' (write-value) is write-value
PRIVATE>

\ the value as JSON text (a new string): maps, lists, strings, numbers, json-floats, json-true json-false json-null
: >json ( v -- s len )  {: v | b :}  <bytes> to b  v b write-value  b b>string  b dispose ;

\ ---- a value's fields, of the kind wanted: what came from a client checked as it is read ----
\ (each false if v isn't an object, it has no such field, or the field is of another kind)
<PRIVATE
: field ( key n v -- x true | false )  {: ka kn v :}  v map? 0= if  false exit  then  ka kn v s-at ;
: number? ( x -- flag )  object? 0= ;   \ (a whole number is a cell; all else json> gives is an object)
PRIVATE>
: json-string@ ( key n v -- a n true | false )  field 0= if  false exit  then  dup string? if  string true  else  drop false  then ;
: json-int@ ( key n v -- n true | false )  field 0= if  false exit  then  dup number? if  true  else  drop false  then ;
: json-bool@ ( key n v -- flag true | false )
    field 0= if  false exit  then
    dup json-true = if  drop true true exit  then
    json-false = if  false true  else  false  then ;
: json-list@ ( key n v -- l true | false )  field 0= if  false exit  then  dup list? if  true  else  drop false  then ;
: json-map@ ( key n v -- m true | false )  field 0= if  false exit  then  dup map? if  true  else  drop false  then ;
\ a field read with a default, no flag to test: what a page reads of a row, a form, its state a hundred times -
\ v not an object, the field not there or another kind: the default
: json-text ( key n v -- a n )   \ a string as it is; a whole number in decimal (a new string); else ""
    field 0= if  s" " exit  then  {: x :}
    x string? if  x string exit  then
    x number? if  x s" {}" format exit  then
    s" " ;
: json-int ( key n v -- n )  json-int@ 0= if  0  then ;            \ a whole number; else 0
: json-flag ( key n v -- flag )  json-bool@ 0= if  false  then ;   \ true; false, or none, or another kind: false

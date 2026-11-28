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
: literal? ( v -- flag )  {: v :}  v json-null =  v json-true = or  v json-false = or ;
\ the tree out of the scratch region it was made in, into the one around: once, at its root - rescue takes what it holds
\ with it (each object rescued again went a region further out, at last out of every one: never freed)
: keep-tree ( v -- )  {: v :}  v literal?  v object? 0= or if  exit  then  v rescue drop ;
PRIVATE>

\ the JSON text read: a value (an error, saying where, for text that isn't JSON - `catch` it). Read in C ((json>),
\ src/objects.c): it is given this vocabulary's three literals and its word for a float.
: json> ( a n -- v )
    {: a n :}
    [: a n  json-null json-true json-false ['] <json-float> json-depth-max  (json>)  dup keep-tree ;] with-scratch ;

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

\ the value as JSON text (a new string): maps, lists, strings, numbers, json-floats, json-true json-false json-null;
\ bytes as "0x" and their hex. Written in C ((>json), src/objects.c): given this vocabulary's three literals and its
\ float's class. A long value written in a handler gives way as it goes.
: >json ( v -- s len )  json-null json-true json-false json-float (>json) ;

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
\ a field written: the text as a new string of the map's (a number, a map, a list: s-at!)
: json-string! ( a n key n m -- )  {: a n ka kn m :}  a n >string drop ka kn m s-at! ;
: json-int ( key n v -- n )  json-int@ 0= if  0  then ;            \ a whole number; else 0
: json-flag ( key n v -- flag )  json-bool@ 0= if  false  then ;   \ true; false, or none, or another kind: false

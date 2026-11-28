\ references (docs/forth.md, Objects): an object is known by a cell that is no address, that names nothing once its
\ object is let go, and that no number read from text can be
IN: test-references
USING: tester json ;

objects constant objects-before
s" hello" >string drop constant greeting
: fails ( xt -- flag )  catch 0<> ;

testing a reference is no address: memory read or written through one is an error, not a fetch of wherever it says
T{ [: greeting c@ ;] fails  [: greeting @ ;] fails  [: 0 greeting c! ;] fails  [: greeting 1+ c@ ;] fails -> true true true true }T
T{ [: greeting c@ ;] fails drop  error-message s" an object where an address belongs" s-find 0>= -> true }T
: fetched ( a -- c )  c@ ;   \ (in a definition: the fetch written in line)
T{ [: greeting fetched ;] fails -> true }T

testing one let go names nothing, though its slot is another object's now (by its address it named whatever came to
testing  be there)
: let-go-and-made ( -- old? new? same? )
    s" one" >string drop {: a :}  a dispose  s" two" >string drop {: b :}  a object?  b object?  a b =  b dispose ;
T{ let-go-and-made -> false true false }T
T{ [: s" gone" >string drop dup dispose  string ;] fails -> true }T
: all-new ( -- flag )   \ a slot used a thousand times: each object's cell its own
    list {: seen :}  true  1000 0 do  s" x" >string drop {: s :}  s seen index 0< and  s seen push  s dispose  loop  seen list-free ;
T{ all-new -> true }T

testing a number added to a reference is no object's: the slot next door isn't named by its neighbour's cell
: neighbours ( -- flag )
    list {: l :}  64 0 do  s" x" >string drop l push  loop
    true  63 0 do  i l nth {: s :}  s 1+ object? 0=  s 1- object? 0= and and  loop
    l [: dispose ;] each  l list-free ;
T{ neighbours -> true }T

testing text given as a reference and a length: the string's first bytes, to every word that takes text; more than it
testing  has, an error
T{ greeting 5 s" hello" s=  greeting 3 s" hel" s=  greeting 0 s" " s= -> true true true }T
T{ [: greeting 6 s" hello!" s= ;] fails -> true }T
: list-as-text ( -- flag )  list {: l :}  [: l 0 s" " s= drop ;] fails  l list-free ;   \ (a list is no text)
T{ list-as-text -> true }T
T{ greeting string  swap greeting = -> 5 true }T                   \ (string: the object as text - itself, its length)

testing s-bytes: text's bytes by their address, for the words that read memory; move copies out of a string
T{ greeting 5 s-bytes  swap greeting <>  swap -> true 5 }T
T{ greeting 5 s-bytes drop c@  greeting 5 s-bytes drop 4 + c@ -> char h char o }T
T{ s" abc" 2dup s-bytes  rot =  -rot = -> true true }T   \ (by its address already: as it is)
create copied 8 allot
T{ greeting 5 copied swap move  copied 5 s" hello" s= -> true }T

testing no number read from text is a reference: its digits are no number to string>number, a float to json>
: its-digits ( -- flag )  greeting s" {}" format {: s n :}  s n string>number nip  s dispose ;
T{ its-digits -> false }T
T{ s" 4611686018427387903" string>number  s" 4611686018427387904" string>number nip -> 4611686018427387903 true false }T
T{ s" 5764607523034234879" string>number nip  s" 5764607523034234880" string>number -> false 5764607523034234880 true }T
T{ s" $4000000000000000" string>number nip  s" -4611686018427387904" string>number nip -> false true }T
T{ s" 4611686018427387903" json>  s" 4611686018427387904" json> dup object? swap json-free -> 4611686018427387903 true }T
: its-digits-in-json ( -- written-as-text? still-there? )
    greeting s" [{}]" format {: s n :}  s n json> {: v :}  v >json {: t tn :}  t tn s" hello" s-find 0>=  t dispose  v json-free  greeting object?  s dispose ;
T{ its-digits-in-json -> false true }T

greeting dispose
T{ objects -> objects-before }T

\ (three thousand left alive, in three chunks of the table: the end lets them go, and each chunk after its objects -
\  a chunk freed as its own were done was still named by the next one's)
: left-alive ( -- )  3000 0 do  s" left" >string 2drop  loop ;
left-alive
test-summary

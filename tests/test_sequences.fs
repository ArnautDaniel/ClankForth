\ sequences (Factor's protocol): lists, strings and bytes under the same words, what each makes the kind it was
\ given; " text" string objects; build and %
IN: test-sequences
USING: tester frames ;

: str ( s -- a n )  string ;   \ (a string object's bytes, to compare)
: is ( s a n -- flag )  rot string s= ;

testing the protocol on each kind: length nth (negative from the end) nth! push pop clear
T{ { 1 2 3 } length  " abc" length  <bytes> length -> 3 3 0 }T
T{ 1 { 10 20 30 } nth  -1 { 10 20 30 } nth  0 " abc" nth  -1 " abc" nth -> 20 30 97 99 }T
T{ " abc" dup  char X 1 rot nth!  " aXc" seq= -> true }T
T{ <bytes>  65 over push  66 over push  dup pop  swap length -> 66 1 }T
T{ { 1 2 } dup clear length -> 0 }T
testing what a string can't do: grow, shrink; a byte that isn't one; an index out of range
T{ 5 " ab" ' push catch nip nip -> -1 }T
T{ " ab" ' pop catch nip -> -1 }T
T{ 300 <bytes> ' push catch nip nip -> -1 }T
T{ 5 " ab" ' nth catch nip nip -> -1 }T
T{ 42 ' length catch nip -> -1 }T   \ (no sequence)
testing " text": a string object, with s\"'s escapes
T{ " hello" str s" hello" s= -> true }T
T{ " a\tb\"c\\" str s\" a\tb\"c\\" s= -> true }T
T{ " " length -> 0 }T
: literal-word ( -- s )  " made once" ;
T{ literal-word literal-word = -> false }T   \ (a new one each time the word runs: its to keep or let go)
T{ literal-word dispose  literal-word str s" made once" s= -> true }T   \ (one freed: the next as it was)
T{ [: literal-word drop ;] with-scratch  literal-word str s" made once" s= -> true }T   \ (its region ended: the next as it was)
T{ 88 0 literal-word nth!  literal-word str s" made once" s= -> true }T   \ (one changed: not the next)

testing map, filter: the kind they were given - inline in a definition and not
: up ( s -- s' )  [: 32 xor ;] map ;
: no-a ( s -- s' )  [: [char] a <> ;] filter ;
T{ " hello" up  s" HELLO" is -> true }T
T{ " banana" no-a  s" bnn" is -> true }T
T{ { 1 2 3 } [: 10 * ;] map  { 10 20 30 } seq= -> true }T
T{ " hello" ' 1+ map  s" ifmmp" is -> true }T   \ (map not inline: the same)
T{ <bytes> dup " xy" append! drop  [: 1+ ;] map  bytes? -> true }T
T{ " abc" [: 1000 + ;] ' map catch nip nip -> -1 }T   \ (a string's items are bytes)
testing reduce, each, find, find-index, count, any?, all?, each-index, map-index
T{ " ab" 0 [: + ;] reduce -> 195 }T
: chars-of ( s -- n )  0 swap [: drop 1+ ;] each ;
T{ " four" chars-of -> 4 }T
T{ " hello" [: [char] l = ;] find  " hello" [: [char] l = ;] find-index -> 108 true 2 }T
T{ " hello" [: [char] z = ;] find  " hello" [: [char] z = ;] find-index -> false -1 }T
T{ " hello" [: [char] l = ;] count  { 2 4 } [: 1 and 0= ;] all?  { 1 2 } [: 3 = ;] any? -> 2 true false }T
T{ { 5 6 } [: + ;] map-index  { 5 7 } seq= -> true }T
: index-sum ( l -- n )  0 swap [: nip + ;] each-index ;
T{ { 9 9 9 } index-sum -> 3 }T

testing like: a sequence as another's kind (of it already: itself)
T{ { 104 105 } " " like  s" hi" is -> true }T
T{ " hi" { } like  { 104 105 } seq= -> true }T
T{ " hi" <bytes> like bytes? -> true }T
T{ " hi" dup " " like = -> true }T
testing append (new, of the first's kind) and append! (in place); a string doesn't take append!
T{ " ab" " cd" append  s" abcd" is -> true }T
T{ { 1 } { 2 3 } append  { 1 2 3 } seq= -> true }T
T{ " ab" { 99 } append  s" abc" is -> true }T
T{ { 1 } dup { 2 } append! drop  { 1 2 } seq= -> true }T
T{ <bytes> dup dup append! drop length -> 0 }T
T{ " ab" " c" ' append! catch nip nip -> -1 }T
testing subseq, head, tail, reverse, first, last, empty?
T{ " hello" 1 4 subseq  s" ell" is -> true }T
T{ " hello" 2 head  s" he" is  " hello" 2 tail  s" llo" is -> true true }T
T{ " hi" 9 head  s" hi" is  " hi" 9 tail length -> true 0 }T
T{ " hello" 3 9 ' subseq catch nip nip nip -> -1 }T
T{ " abc" reverse  s" cba" is  { 1 2 3 } reverse  { 3 2 1 } seq= -> true true }T
T{ " abc" first  " abc" last  " " empty?  { 1 } empty? -> 97 99 true false }T
testing index, contains?, subseq-index, starts?, ends?, seq=
T{ 3 { 1 2 3 } index  9 { 1 2 3 } index  char c " abc" contains? -> 2 -1 true }T
T{ " hello" " ll" subseq-index  " hello" " lo!" subseq-index  " hello" " " subseq-index -> 2 -1 0 }T
T{ " a-b-c" " -" 2 subseq-index-from -> 3 }T
T{ " abc" " b" -1 1 rshift subseq-index-from -> -1 }T   \ (from past the end, however far: none, no sum past a cell)
T{ { 1 2 3 } { 2 } 9 subseq-index-from -> -1 }T
T{ 5 " ab" ' (like-done) catch nip nip -> -1 }T   \ (gathered into what isn't an object: an error, not a crash)
T{ " hello" " he" starts?  " hello" " lo" ends?  " he" " hello" starts? -> true true false }T
T{ " hi" { 104 105 } seq=  " hi" " ho" seq=  " hi" " h" seq= -> true false false }T
testing split, join, concat, replace, trim
: pieces ( l -- a n )  " |" join string ;
T{ " a,b,,c" " ," split pieces s" a|b||c" s= -> true }T
T{ " ,a," " ," split pieces s" |a|" s= -> true }T
T{ " a, b, c" " , " split length -> 3 }T
T{ " abc" " " split length -> 1 }T
T{ { " x" " y" " z" } " , " join  s" x, y, z" is -> true }T
T{ { } " , " join length -> 0 }T
T{ { " ab" " " " cd" } concat  s" abcd" is  { { 1 } { 2 3 } } concat { 1 2 3 } seq= -> true true }T
T{ " hello world" " o" " 0" replace  s" hell0 w0rld" is -> true }T
T{ " aaa" " a" " bb" replace  s" bbbbbb" is -> true }T
T{ s\" \t pad me \n" >string drop trim  s" pad me" is  " " trim length -> true 0 }T

testing build: % %s %n %c onto a string being built; builds nest; outside one an error
T{ [: " <p>" %  s" hi" %s  42 %n  [char] ! %c  " </p>" % ;] build  s" <p>hi42!</p>" is -> true }T
T{ [: " a" %  [: " b" % ;] build %  " c" % ;] build  s" abc" is -> true }T
T{ [: ;] build length -> 0 }T
T{ " x" ' % catch nip -> -1 }T
testing build's region: what it makes besides freed as it ends; an error in it leaves no build behind
: throwaway ( -- s )  [: { 1 2 3 } drop  " made" " here" append %  " !" % ;] build ;
objects constant objects-before   \ (after it: its literals made as it compiled)
T{ throwaway  s" madehere!" is -> true }T
T{ objects objects-before - -> 1 }T   \ (only the string built)
T{ [: [: " a" %  true abort" broken" ;] build drop ;] catch -> -1 }T
T{ " after" ' % catch nip -> -1 }T   \ (the broken build gone: % outside any)
testing builds in tasks that wait between: each task its own
variable built-a   variable built-b
: build-a ( -- )  [: " a1" %  2 wait  " a2" % ;] build built-a ! ;
: build-b ( -- )  [: " b1" %  1 wait  " b2" % ;] build built-b ! ;
' build-a start-task drop  ' build-b start-task drop
T{ 6 tick-tasks  built-a @ str s" a1a2" s=  built-b @ str s" b1b2" s= -> true true }T

test-summary

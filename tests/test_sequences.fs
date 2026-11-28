\ sequences (Factor's protocol): lists, strings and bytes under the same words, what each makes the kind it was
\ given; " text" string objects; build and %
IN: test-sequences
USING: tester frames ;

: str ( s -- a n )  string ;   \ (a string object's bytes, to compare)
: is ( s a n -- flag )  rot string s= ;

testing the protocol on each kind: length nth (negative from the end) nth! push pop clear
T{ { 1 2 3 } length  " abc" length  <bytes> length -> 3 3 0 }T
T{ 1 { 10 20 30 } nth  -1 { 10 20 30 } nth  0 " abc" nth  -1 " abc" nth -> 20 30 97 99 }T
: changed-bytes ( -- flag )   \ (a string doesn't change: its bytes as a buffer do)
    " abc" <bytes> like {: b :}  [char] X 1 b nth!  b " aXc" seq=  b dispose ;
: changed-string ( -- failed? )  " abc" {: s :}  [: [char] X 1 s nth! ;] catch 0<>  s dispose ;
T{ changed-bytes  changed-string -> true true }T
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

testing a walk reads its items where the sequence was found (no lookup an item) while no object is let go: the same
testing  items when objects are let go as it goes, when it grows as it goes (the items it had as the walk began),
testing  when what map gathers outgrows its room; of each kind
: sum ( seq -- n )  0 swap [: + ;] each ;
: sum-letting-go ( seq -- n )  0 swap [: +  " gone" dispose ;] each ;   \ (an object let go at every item)
: hundred ( -- l )  list {: l :}  100 0 do  i l push  loop  l ;
: hundred-bytes ( -- b )  <bytes> {: b :}  100 0 do  i b push  loop  b ;
: hundred-string ( -- s )  hundred-bytes {: b :}  b " " like  b dispose ;
: kinds-sum ( xt -- a b c )   \ what xt ( seq -- n ) makes of a list, bytes and a string of 0 .. 99
    {: xt :}  [: hundred xt execute  hundred-bytes xt execute  hundred-string xt execute ;] with-scratch ;
T{ ' sum kinds-sum -> 4950 4950 4950 }T
T{ ' sum-letting-go kinds-sum -> 4950 4950 4950 }T
: sum-growing ( seq -- n length )  {: s :}  0 s [: +  7 s push ;] each  s length ;   \ (as long as it was: 100 walked)
T{ [: hundred sum-growing  hundred-bytes sum-growing ;] with-scratch -> 4950 200 4950 200 }T
: doubled ( seq -- seq' )  [: 2 * 255 and ;] map ;
: doubled-ok? ( seq -- flag )  {: s :}  s doubled {: d :}  d length 100 =  50 d nth 100 = and  99 d nth 198 = and ;
T{ [: hundred doubled-ok?  hundred-bytes doubled-ok?  hundred-string doubled-ok? ;] with-scratch -> true true true }T
T{ [: hundred doubled list?  hundred-bytes doubled bytes?  hundred-string doubled string? ;] with-scratch -> true true true }T
: evens ( seq -- seq' )  [: 1 and 0= ;] filter ;
T{ [: hundred evens dup length swap 49 swap nth  hundred-string evens dup length swap 49 swap nth ;] with-scratch -> 50 98 50 98 }T
T{ [: hundred [: 60 > ;] find  hundred-bytes [: 60 > ;] find  hundred-string [: 200 > ;] find ;] with-scratch -> 61 true 61 true false }T
T{ [: hundred 0 [: + ;] reduce  hundred-string 1000 [: + ;] reduce ;] with-scratch -> 4950 5950 }T

testing ... a string's or bytes' map given what isn't a byte: an error saying so; a list made shorter as it is
testing  walked, with objects let go or none: an error; one walk inside another; nothing left of any of it
: not-a-byte ( -- flag )  [: " abc" [: 300 + ;] map drop ;] catch 0<>  error-message s" isn't a byte" s-find 0>= and ;
T{ objects  [: not-a-byte ;] with-scratch  objects rot = -> true true }T
: shortened ( -- flag )  hundred {: l :}  [: l [: drop  l pop drop  l pop drop ;] each ;] catch 0<>  error-message s" lost items" s-find 0>= and ;
: shortened-letting-go ( -- flag )
    hundred {: l :}  [: l [: drop  " gone" dispose  l pop drop  l pop drop ;] each ;] catch 0<>  error-message s" lost items" s-find 0>= and ;
: both-shortened ( -- flag flag left )  objects {: before :}  [: shortened  shortened-letting-go ;] with-scratch  objects before - ;
T{ both-shortened -> true true 0 }T
: table ( -- n )  0  hundred [: {: x :}  " abc" [: x * + ;] each ;] each ;   \ (each of 0..99 times a + b + c)
T{ [: table ;] with-scratch -> 4950 294 * }T
: gone-under ( -- flag )  hundred {: l :}  [: l [: drop  l list-free ;] each ;] catch 0<> ;   \ (let go as it is walked)
T{ [: gone-under ;] with-scratch -> true }T
\ ... let go, and another kind made where it was (the allocator gives the block again): an error at the next item.
\ The walk went on over the other one: map's cells written into bytes, a string read to the length the list had
: bytes-then-list ( -- flag )   \ bytes walked by map: at the second item let go, a list made in their place
    <bytes> {: b | turn :}  200 0 do  66 b b-push  loop
    [: b [: drop  turn 1+ to turn  turn 2 = if  b dispose  list drop  then  1 ;] map drop ;] catch 0<> ;
: list-then-string ( -- flag )   \ a list of 1000 walked: at the first item let go, a string of 20 in its place
    list {: l | turn :}  1000 0 do  7 l push  loop
    [: l [: drop  turn 0= if  l list-free  s" twenty bytes of text" >string 2drop  then  turn 1+ to turn ;] each ;] catch 0<> ;
: both-replaced ( -- flag flag left )  objects {: before :}  [: bytes-then-list  list-then-string ;] with-scratch  objects before - ;
T{ both-replaced -> true true 0 }T

testing translate: each item the table's item at its place - bytes through bytes, of its kind; a list's through a
testing  list; an item with no place in the table an error, nothing left
: as-string ( a n -- s )  >string drop ;
: text= ( seq a n -- flag )  {: s a n :}  a n s-bytes drop to a  s length n =  n 0 ?do  i s nth  a i + c@ = and  loop ;
T{ [: " abc" " ................................................................................................" " .ABC" append translate  s" ABC" text= ;] with-scratch -> true }T
T{ [: { 2 0 1 } { 10 20 30 } translate dup list? swap  dup length swap 0 swap nth ;] with-scratch -> true 3 30 }T
T{ [: " " " abc" translate dup string? swap length ;] with-scratch -> true 0 }T
: beyond ( -- flag left )
    objects {: before :}  [: [: " zzz" " abc" translate drop ;] catch 0<>  error-message s" no place in a table of 3" s-find 0>= and ;] with-scratch
    objects before - ;
T{ beyond -> true 0 }T
: bytes-of ( a n -- b )  {: a n | b :}  a n s-bytes drop to a  <bytes> to b  n 0 ?do  a i + c@ b push  loop  b ;
T{ [: s" abc" bytes-of " ................................................................................................" " .ABC" append translate dup bytes? swap s" ABC" text= ;] with-scratch -> true true }T

testing regroup: the same bits cut another way - bytes as sixes and back, as fours, as ones; the last bits filled
testing  out when the cut is finer, left out when coarser; what doesn't fit its bits, a list, bits past a byte: errors
: sixes ( a n -- l... )  as-string 8 6 regroup ;
T{ [: s" hel" sixes {: s :}  s length  0 s nth  1 s nth  2 s nth  3 s nth ;] with-scratch -> 4 26 6 21 44 }T
T{ [: s" h" sixes {: s :}  s length  0 s nth  1 s nth ;] with-scratch -> 2 26 0 }T          \ (two bits left: 00 0000)
T{ [: s" he" sixes {: s :}  s length  2 s nth ;] with-scratch -> 3 20 }T                    \ (four left: 0101 00)
T{ [: s" hello, sequences" as-string dup 8 6 regroup 6 8 regroup s" hello, sequences" text= nip ;] with-scratch -> true }T
T{ [: s\" \xab\x0f" as-string 8 4 regroup {: s :}  s length  0 s nth 1 s nth 2 s nth 3 s nth ;] with-scratch -> 4 10 11 0 15 }T
T{ [: s\" \x80" as-string 8 1 regroup {: s :}  s length  0 s nth  7 s nth ;] with-scratch -> 8 1 0 }T
T{ [: s" " as-string 8 6 regroup length  s" a" as-string 8 8 regroup length ;] with-scratch -> 0 1 }T
T{ [: s" abc" bytes-of 8 6 regroup dup bytes? swap length ;] with-scratch -> true 4 }T
: fails? ( xt -- flag left )  {: xt :}  objects {: before :}  [: xt catch 0<> ;] with-scratch  objects before - ;
: too-wide ( -- )  s\" \x40" as-string 6 8 regroup drop ;      \ (64: seven bits)
: a-list ( -- )  { 1 2 3 } 8 6 regroup drop ;
: nine-bits ( -- )  " abc" 9 6 regroup drop ;
: no-bits ( -- )  " abc" 8 0 regroup drop ;
T{ ' too-wide fails?  ' a-list fails?  ' nine-bits fails?  ' no-bits fails? -> true 0  true 0  true 0  true 0 }T

testing slices: a piece of a string that is no copy - read as a string by every word that reads one; what is made of
testing  one a string; the string let go, an error to read (its bytes are no more) - not a read of them
" hello, world" constant whole
: a-slice ( -- n slice? string? same? )  7 12 whole <slice> {: p :}  p length  p slice?  p string?  p string s" world" s=  p dispose ;
T{ a-slice -> 5 true true true }T
: piece ( -- slice )  2 7 whole <slice> ;
: read-slice ( -- first last sum )  piece {: p :}  0 p nth  -1 p nth  p 0 [: + ;] reduce  p dispose ;
T{ read-slice -> char l 32 char l 2* char o + char , + 32 + }T
: mapped-slice ( -- same? slice? )  piece {: p :}  p [: 1+ ;] map {: m :}  m string s" mmp-!" s=  m slice?  m dispose  p dispose ;
T{ mapped-slice -> true false }T
: compared-slice ( -- same? starts? )  piece {: p :}  p " llo, " seq=  p " llo" starts?  p dispose ;
T{ compared-slice -> true true }T
: sliced-slice ( -- head? tail? inner? )
    piece {: p :}  p 3 head-slice {: h :}  p 3 tail-slice {: t :}  1 4 p <slice> {: i :}   \ (a piece of a piece)
    h string s" llo" s=  t string s" , " s=  i string s" lo," s=  h dispose  t dispose  i dispose  p dispose ;
T{ sliced-slice -> true true true }T
: copied-slice ( -- same? slice? clone-slice? )   \ (subseq and clone: copies, strings of their own)
    piece {: p :}  p 1 3 subseq {: c :}  p clone {: d :}  c string s" lo" s=  c slice?  d slice?  c dispose  d dispose  p dispose ;
T{ copied-slice -> true false false }T
: slice-key ( -- v found? )  <map> piece {: m p :}  42 p m at!  " llo, " m at  m dispose  p dispose ;   \ (a key by its bytes)
T{ slice-key -> 42 true }T
: unchanged-slice ( -- flag )  piece {: p :}  [: 0 0 p nth! ;] catch 0<>  p dispose ;   \ (it doesn't change)
T{ unchanged-slice -> true }T
T{ [: 3 20 whole <slice> ;] catch 0<>  [: 0 1 list <slice> ;] catch 0<> -> true true }T
: orphaned ( -- flag )   \ the string let go under its slice: reading the slice is an error
    s" short-lived" >string drop {: s :}  0 5 s <slice> {: p :}  s dispose
    [: p length drop  p string type ;] catch 0<>  p slice? and  p dispose ;
T{ orphaned  error-message s" a slice of a string that was let go" s-find 0>= -> true true }T
: kept-with ( -- flag )   \ a slice rescued takes its string with it
    [: s" kept with its slice" >string drop {: s :}  5 9 s <slice> rescue ;] with-scratch {: p :}
    p string s" with" s=  p dispose ;
T{ kept-with -> true }T

testing words for what reads a protocol: span (a class of bytes run through), digits>number, be> le>, seq-xor,
testing  b-append-seq - each from a place in a sequence, no piece of it made
: digit? ( c -- flag )  [char] 0 [char] 9 1+ within ;   ' digit? byte-class digit-class   \ (a class of bytes: a table by its test)
T{ " 123abc" digit-class 0 span  " 123abc" digit-class 3 span  " 123" digit-class 0 span -> 3 3 3 }T
T{ " x1234y" 1 5 10 digits>number  " ff" 0 2 16 digits>number  " FF" 0 2 16 digits>number -> 1234 true 255 true 255 true }T
T{ " 12x" 0 3 10 digits>number  " -1" 0 2 10 digits>number  " 12" 0 0 10 digits>number  " 9" 0 1 8 digits>number -> false false false false }T
T{ " 9223372036854775807" 0 19 10 digits>number  " 9223372036854775808" 0 19 10 digits>number -> 9223372036854775807 true false }T
: four ( -- b )  <bytes> {: b :}  1 b b-push  2 b b-push  3 b b-push  4 b b-push  b ;
: four-read ( -- be le middle )  four {: b :}  b 0 4 be>  b 0 4 le>  b 1 3 be>  b dispose ;
T{ four-read -> $01020304 $04030201 $0203 }T
T{ [: four 0 9 be> ;] catch 0<> -> true }T
T{ " abcdef" "  " seq-xor dup string s" ABCDEF" s= swap dispose -> true }T
: four-xor-four ( -- n )  four four {: a b :}  a b seq-xor {: x :}  x 0 4 be>  x dispose  a dispose  b dispose ;
T{ four-xor-four -> 0 }T
: appended ( -- flag )  <bytes> {: b :}  " hello, world" 7 12 b b-append-seq  b 0 2 b b-append-seq  b b>string {: s n :}  s n s" worldwo" s=  s dispose  b dispose ;
T{ appended -> true }T
whole dispose

testing text as a sequence: >seq - a string's reference with all its length the string itself, with less a slice of
testing  it, text by its address a slice of that memory; string gives it back as text
: itself? ( -- flag )  s" whole text" >string drop {: s :}  s s length >seq s =  s dispose ;
T{ itself? -> true }T
: shorter ( -- flag )  s" whole text" >string drop {: s :}  s 5 >seq {: p :}  p slice?  p string s" whole" s= and  p dispose  s dispose ;
T{ shorter -> true }T
: by-address ( -- total )  [: 0  s" abc" >seq [: + ;] each ;] with-scratch ;   \ (walked as any sequence is)
T{ by-address -> 294 }T
: of-memory ( -- flag )  [: s" in memory" >seq {: p :}  p slice?  p string s" in memory" s= and ;] with-scratch ;
T{ of-memory -> true }T
T{ objects  [: s" freed with its region" >seq drop ;] with-scratch  objects = -> true }T

testing pieces by sequence words, no copy: split1 (cut at the first of a separator), trim-slice, seq-at?
: cut-at ( a n -- flag )   \ name=value: what is before the first =, and all after it
    [: >seq " =" split1 {: name value :}  name string s" name" s=  value string s" a=b" s= and  name slice? and ;] with-scratch ;
T{ s" name=a=b" cut-at -> true }T
: uncut ( -- flag )  [: " no separator" {: s :}  s " =" split1 0=  swap s = and ;] with-scratch ;   \ (none: itself, and 0)
T{ uncut -> true }T
: empty-sides ( -- before after )  [: " =" " =" split1 length swap length swap ;] with-scratch ;
T{ empty-sides -> 0 0 }T
: trimmed ( -- flag )  [: s\"  \t padded \r\n" >seq trim-slice {: p :}  p string s" padded" s=  p slice? and ;] with-scratch ;
T{ trimmed -> true }T
: all-blank ( -- n )  [: "    " trim-slice length ;] with-scratch ;
T{ all-blank -> 0 }T
: here? ( -- at-2 at-3 past start end )
    [: " abcdef" {: s :}  s " cd" 2 seq-at?  s " cd" 3 seq-at?  s " ef" 5 seq-at?  s " ab" starts?  s " ef" ends? ;] with-scratch ;
T{ here? -> true false false true true }T
: shorter-than ( -- starts ends neg )  [: " ab" " abc" starts?  " ab" " xab" ends?  " ab" " a" -1 seq-at? ;] with-scratch ;
T{ shorter-than -> false false false }T
: no-piece ( -- made )   \ (starts? makes no piece of the sequence: the two strings are all there is)
    [: " prefix and more" " prefix" objects {: before :}  starts? drop  objects before - ;] with-scratch ;
T{ no-piece -> 0 }T
: lists-too ( -- flag )  { 1 2 3 4 } {: l :}  { 2 3 } {: m :}  l m 1 seq-at?  l m 2 seq-at? 0= and  l list-free  m list-free ;
T{ lists-too -> true }T

testing where an item is: last-index, index-from (looking from a place on)
: places ( -- last none from-3 from-4 past )
    [: " a.b.c" {: s :}  [char] . s last-index  [char] x s last-index  [char] . s 2 index-from  [char] . s 4 index-from
       [char] . s 99 index-from ;] with-scratch ;
T{ places -> 3 -1 3 -1 -1 }T
: in-a-list ( -- last from )  { 7 8 7 9 } {: l :}  7 l last-index  7 l 1 index-from  l list-free ;
T{ in-a-list -> 2 2 }T

testing %part: a sequence's items from one place to another onto what is built - no piece made
: parts ( -- flag )  [: " hello, world" {: s :}  s 7 12 %part  s 0 5 %part  s 3 3 %part ;] build {: made :}
    made string s" worldhello" s=  made dispose ;
T{ parts -> true }T
T{ [: [: " abc" 2 9 %part ;] build drop ;] catch 0<> -> true }T

testing byte-class: a class of bytes by its test - a table for span and s-marked
: vowel? ( c -- flag )  " aeiou" contains? ;   ' vowel? byte-class vowels
T{ s" rhythm" vowels s-marked  s" strength" vowels s-marked -> 6 3 }T
T{ objects  vowels 256 + drop  objects = -> true }T   \ (what its test made as the table was filled: freed)

test-summary

\ compression (src/compress.c): DEFLATE both ways, gzip - round trips; streams made elsewhere (Python's zlib, RFC
\ 7692's example) read; a limit on what is made kept; what isn't DEFLATE or gzip refused. The same tests whether the
\ work is zlib's or the system's own (CI's builds are some of each). In a handler, what would take longer than a
\ slice is done on a worker while the node's other actors go on
IN: test-compression
USING: tester compression system actors ;

: hex-digit ( c -- n )  {: c :}  c [char] 9 <= if  c [char] 0 -  else  c 32 or [char] a - 10 +  then ;
: hex> ( a n -- s len )   \ "1F 8B ..." as bytes
    {: a n | b hi :}  a n s-bytes drop to a  <bytes> to b  -1 to hi
    n 0 ?do  a i + c@ {: c :}
        c 32 > if  hi 0< if  c hex-digit to hi  else  hi 4 lshift c hex-digit or b b-push  -1 to hi  then  then
    loop  b b>string  b dispose ;
: big ( -- s len )  [: 400 0 do  s" <tr class=" %s  i %n  s" ><td>row</td></tr>" %s  loop ;] build string ;

testing round trips: deflate / inflate, gzip / gunzip, deflate-sync (a WebSocket message's: its end put back)
T{ s" hello, hello, hello!" deflate 100 inflate  -rot s" hello, hello, hello!" s= -> true true }T
T{ big gzip 100000 gunzip  -rot big s= -> true true }T
T{ big deflate nip  big nip 8 / < -> true }T   \ (a page's repeats: an eighth or less)
: synced ( a n -- s len )  deflate-sync s\" \x00\x00\xff\xff" s+ ;
T{ big synced 100000 inflate  -rot big s= -> true true }T
T{ s" " gzip 10 gunzip  rot drop -> 0 true }T
T{ s" " deflate-sync nip -> 1 }T   \ (an empty message: one byte, 00 - RFC 7692's)

testing streams made elsewhere: Python's gzip, RFC 7692's "Hello" (7.2.3.1)
: py-gzip ( -- a n )  s" 1F 8B 08 00 00 00 00 00 02 FF CB 48 CD C9 C9 D7 51 C8 40 A2 14 01 A7 BB D2 FE 14 00 00 00" hex> ;
T{ py-gzip 100 gunzip  -rot s" hello, hello, hello!" s= -> true true }T
T{ s" F2 48 CD C9 C9 07 00 00 00 FF FF" hex> 100 inflate  -rot s" Hello" s= -> true true }T

testing what is made kept to the limit; what isn't DEFLATE or gzip refused; a gzip whose CRC doesn't match refused
T{ big gzip 100 gunzip -> false }T
T{ s" not deflate at all" 100 inflate -> false }T
T{ s" not gzip at all, really" 100 gunzip -> false }T
: bad-crc ( -- a n )  s" 1F 8B 08 00 00 00 00 00 02 FF CB 48 CD C9 C9 D7 51 C8 40 A2 14 01 A7 BB D2 FF 14 00 00 00" hex> ;
T{ bad-crc 100 gunzip -> false }T

T{ big gzip  big nip gunzip  nip nip -> true }T          \ (as long as the limit: made)
T{ big gzip  big nip 1- gunzip -> false }T               \ (a byte more than it: not)
T{ big deflate  big nip inflate  nip nip -> true }T
T{ big deflate  big nip 1- inflate -> false }T
T{ big gzip 5 - 100000 gunzip -> false }T                \ (cut short: its trailer's CRC isn't the bytes')
T{ big deflate 3 - 100000 inflate -> false }T            \ (... inside a block)
T{ s" " 100 inflate  rot drop -> 0 true }T                 \ (nothing: no block begun, none unfinished)

testing a gzip of several members (files put end to end, a log's rotations): the bytes of all of them, each one's CRC
testing  checked - and bytes after the last member that are no member, refused (they were passed over: a file cut
testing  off in a later member read as whole)
: two-members ( -- a n )  s" hello, " gzip  s" world" gzip  s+ ;
T{ two-members 100 gunzip  -rot s" hello, world" s= -> true true }T
T{ s" one" gzip  s" " gzip s+  s"  two" gzip s+  100 gunzip  -rot s" one two" s= -> true true }T
T{ big gzip  big gzip s+  big nip 2* gunzip  nip nip -> true }T
T{ big gzip  big gzip s+  big nip 2* 1- gunzip -> false }T   \ (the limit is of all of them)
T{ two-members 3 - 100 gunzip -> false }T                    \ (the second cut short)
T{ s" hello, " gzip  bad-crc s+  100 gunzip -> false }T      \ (... its CRC not its bytes')
T{ py-gzip s" junk" s+ 100 gunzip -> false }T
T{ py-gzip s" 00" hex> s+ 100 gunzip -> false }T
T{ s" F3 48 CD C9 C9 07 00 00 00 00 FF FF" hex> 100 inflate  -rot s" Hello" s= -> true true }T   \ (RFC 7692, 7.2.3.4:
                                                             \  a final block, an empty one after it - a message's)

testing every length there and back: under a window's size, at it, past the biggest - text that repeats, bytes that don't
: rows ( n -- s len )   \ n bytes of a table's rows
    {: n | b :}  <bytes> to b  begin  b b-length n <  while  s" <tr class=row><td>" b b-append  b b-length n>str b b-append  repeat
    b b>string drop n  b dispose ;
: noise ( n -- s len )   \ n bytes with nothing to find in them
    {: n | b x :}  <bytes> to b  12345 to x
    n 0 ?do  x 1103515245 * 12345 +  $7fffffff and to x  x 16 rshift b b-push  loop  b b>string  b dispose ;
: same? ( a n  z zn true | a n false -- flag )  if  s=  else  2drop false  then ;
: there-and-back? ( a n -- flag )   \ (each with as many bytes allowed as there were: no more are needed)
    {: a n :}
    a n  a n gzip n gunzip  same?
    a n  a n deflate n inflate  same? and
    a n  a n synced n inflate  same? and ;
create lengths  0 , 1 , 2 , 3 , 255 , 511 , 512 , 513 , 1023 , 1024 , 1025 , 4095 , 4096 , 4097 , 32767 , 32768 , 32769 , 70000 , 200000 ,
19 constant lengths-n
: each-length? ( xt -- flag )   \ xt ( n -- s len ): what it makes of each length, there and back
    {: xt | ok n :}  true to ok   \ (n: the loop's i isn't the quotation's)
    lengths-n 0 do  lengths i cells + @ to n  [: n xt execute there-and-back? ;] with-scratch  ok and to ok  loop  ok ;
T{ ' rows each-length? -> true }T
T{ ' noise each-length? -> true }T
T{ 4096 noise gzip nip  4096 4096 16 / + < -> true }T    \ (what won't compress grows little: stored)
T{ 200000 rows deflate nip  200000 6 / < -> true }T      \ (rows numbered: a sixth or less)

testing whose work it is: zlib's or the system's own, as the build was configured (it tells the test; by hand, either)
: by-as-built? ( -- flag )
    s" CLANKFORTH_EXPECT_COMPRESSED_BY" getenv 0= if  true exit  then  {: s n :}
    compressed-by {: w wn :}  w wn s n s-starts?  s dispose  w dispose ;
T{ by-as-built? -> true }T

testing nothing left behind: what a round trip makes goes with its scratch
T{ objects  [: big there-and-back? drop ;] with-scratch  objects = -> true }T
T{ objects  [: s" not gzip at all, really" 100 gunzip drop  bad-crc 100 gunzip drop  big gzip 100 gunzip drop ;] with-scratch  objects = -> true }T

testing in a handler: a long text is deflated and inflated on a worker - the handler waits, the node's other actors
testing  go on meanwhile; a short one where the word stands, the handler never waiting
message work ( xt -- )
message ping ( -- )
variable result   variable worked   variable pinged-meanwhile
behaviour worker  on work ( xt -- )  execute result !  true worked ! ;  end-behaviour
behaviour pinger  on ping ( -- )  worked @ 0= pinged-meanwhile ! ;  end-behaviour
worker -1 s" worker" spawn constant w
pinger -1 s" pinger" spawn constant p
: settle ( -- )  5000 0 do  deliver  worked @ if  leave  then  1 sleep-ms  loop ;   \ (at the prompt: a real sleep)
\ the handler begun, and another actor's message handled after: was the handler still at its work then?
: run-settled ( xt -- x )  false worked !  w send work  settle  result @ ;
: begun ( xt -- waiting? )  false worked !  false pinged-meanwhile !  w send work  deliver  p send ping  deliver  pinged-meanwhile @ ;
: long-rows ( -- s )   \ three million bytes: a hundred times what a word deflates where it stands
    {: | b :}  <bytes> to b  [: 30000 rows  100 0 do  2dup b b-append  loop  2drop ;] with-scratch  b b>string drop  b dispose ;
long-rows constant long-text
: long ( -- a n )  long-text string ;
long gzip drop constant long-gz          \ (at the prompt: made here)
: there-and-back ( -- flag )  long there-and-back? ;
T{ ' there-and-back begun -> true }T
settle
T{ worked @  result @ -> true true }T
: short-back ( -- flag )  s" hello, hello, hello!" there-and-back? ;
slice-steps constant slice-was
0 slice-steps!   \ (no slices: the handler stops only to wait - a slow build's clock doesn't end its turn for it)
T{ ' short-back begun  worked @  result @ -> false true true }T   \ (done before the next message: it never waited)
slice-was slice-steps!
: long-gunzipped ( -- flag )  long  long-gz string  long nip gunzip  same? ;
T{ ' long-gunzipped begun -> true }T
settle
T{ result @ -> true }T
testing ... short ones over and over: each where it stands, and counted against the handler's slice - it gives way
testing  as one doing as much in Forth would (a word of C's is one step, however long it works)
: rounds ( xt -- n )  {: | n :}  false worked !  w send work  begin  deliver  n 1+ to n  worked @  n 100000 > or  until  n ;
2480 rows drop constant page
: many-short ( -- 0 )  400 0 do  [: page string gzip 10000 gunzip drop 2drop ;] with-scratch  loop  0 ;
T{ ' many-short rounds 10 > -> true }T
page dispose
testing ... and what a worker refuses: more than the limit, a stream changed
: over-limit ( -- flag )  long-gz string 1000000 gunzip  if  2drop false  else  true  then ;
T{ ' over-limit begun  settle  result @ -> true true }T
long-gz string >string drop constant changed-gz
changed-gz string s-bytes 2/ +  dup c@ 85 xor  swap c!
: changed ( -- flag )  changed-gz string  long nip gunzip  if  2drop false  else  true  then ;
T{ ' changed run-settled -> true }T
testing ... through execute, and at the prompt: where the word stands, as long as it takes
: executed ( -- flag )  long ['] gzip execute  long nip gunzip  if  long s=  else  false  then ;
T{ ' executed run-settled -> true }T
T{ objects  [: long there-and-back? ;] with-scratch  objects rot = -> true true }T

testing killed as its long text is on a worker: its last words' own gzip is its own - it took the handler's finished
testing  job for its (they were known by the word's name: the long text's bytes for "tiny", cells left on its stack)
message squeeze ( -- )
variable last-n   variable last-ok   variable last-left
behaviour squeezer
  on squeeze ( -- )  long gzip 2drop ;
  on killed ( -- )
      depth {: before :}  s" tiny" gzip {: z zn :}  zn last-n !
      z zn 100 gunzip if  s" tiny" s= last-ok !  else  false last-ok !  then  depth before - last-left ! ;
end-behaviour
: killed-squeezing ( ms -- small? ok? left )
    squeezer -1 s" squeezer" spawn {: ms sq :}  0 last-n !  false last-ok !  -1 last-left !
    sq send squeeze  deliver  ms sleep-ms  sq kill  deliver   \ (its gzip on a worker: under way, then long done)
    last-n @ 100 <  last-ok @  last-left @ ;
T{ 0 killed-squeezing -> true true 0 }T
T{ 300 killed-squeezing -> true true 0 }T

w kill  p kill  long-text dispose  long-gz dispose  changed-gz dispose

test-summary

\ compression (src/compress.c): DEFLATE both ways, gzip - round trips; streams made elsewhere (Python's zlib, RFC
\ 7692's example) read; a limit on what is made kept; what isn't DEFLATE or gzip refused. The same tests whether the
\ work is zlib's or the system's own (CI's builds are some of each)
IN: test-compression
USING: tester compression system ;

: hex-digit ( c -- n )  {: c :}  c [char] 9 <= if  c [char] 0 -  else  c 32 or [char] a - 10 +  then ;
: hex> ( a n -- s len )   \ "1F 8B ..." as bytes
    {: a n | b hi :}  <bytes> to b  -1 to hi
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

test-summary

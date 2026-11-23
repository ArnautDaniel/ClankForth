\ compression (src/compress.c): DEFLATE both ways, gzip - round trips; streams made elsewhere (Python's zlib, RFC
\ 7692's example) read; a limit on what is made kept; what isn't DEFLATE or gzip refused
IN: test-compression
USING: tester compression ;

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

test-summary

\ base64.fs - RFC 4648's base64: bytes as text and back.
\   s" hi!" >base64 ( -- s len )  "aGkh"        s" aGkh" base64> ( -- s len true )
IN: base64

<PRIVATE
: digit ( n -- c )  s" ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" drop + c@ ;
: put ( v shift b -- )  {: v shift b :}  v shift rshift 63 and digit b b-push ;
: sextet ( c -- n )   \ a digit's value (+ / and the URL form's - _ alike), -1 for none
    {: c :}
    c [char] A [char] Z 1+ within if  c [char] A -  exit  then
    c [char] a [char] z 1+ within if  c [char] a - 26 +  exit  then
    c [char] 0 [char] 9 1+ within if  c [char] 0 - 52 +  exit  then
    c [char] + =  c [char] - = or if  62 exit  then
    c [char] / =  c [char] _ = or if  63 exit  then
    -1 ;
PRIVATE>

\ the bytes as base64 (a new string), padded with = to a whole number of 4 digits
: >base64 ( a n -- s len )
    {: a n | b i v :}  <bytes> to b
    begin  i 3 + n <=  while
        a i + c@ 16 lshift  a i + 1+ c@ 8 lshift or  a i + 2 + c@ or to v
        v 18 b put  v 12 b put  v 6 b put  v 0 b put
        i 3 + to i
    repeat
    n i - 1 = if
        a i + c@ 16 lshift to v  v 18 b put  v 12 b put  [char] = b b-push  [char] = b b-push
    then
    n i - 2 = if
        a i + c@ 16 lshift  a i + 1+ c@ 8 lshift or to v
        v 18 b put  v 12 b put  v 6 b put  [char] = b b-push
    then
    b b>string  b dispose ;

\ base64 (padded or not, + / or the URL form's - _) as its bytes, a new string; false for text that isn't base64
: base64> ( a n -- s len true | false )
    {: a n | b acc bits i c padded :}  <bytes> to b
    begin  i n <  while
        a i + c@ to c
        c [char] = = if
            true to padded
        else
            padded c sextet 0< or if  b dispose  false unloop exit  then
            acc 6 lshift  c sextet or to acc  bits 6 + to bits
            bits 8 >= if
                bits 8 - to bits
                acc bits rshift 255 and b b-push
                acc 1 bits lshift 1- and to acc
            then
        then
        i 1+ to i
    repeat
    b b>string  b dispose  true ;

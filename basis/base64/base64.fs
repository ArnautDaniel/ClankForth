\ base64.fs - RFC 4648's base64: bytes as text and back.
\   s" hi!" >base64 ( -- s len )  "aGkh"        s" aGkh" base64> ( -- s len true )
\   s" \xfb\xff" >base64url ( -- s len )  "-_8"   (the URL form: no padding)
\ Bytes are eight bits each and a digit says six: the same bits cut another way (regroup), each six then the digit
\ at its place (translate) - whole sequences at a time, in C. (A byte pushed at a time in Forth's words, it was 6%
\ of a page: a session's cookie and a form's token are signed and read at every request.)
IN: base64

<PRIVATE
" ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" permanent constant digits
" ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" permanent constant url-digits
255 constant none   \ (a byte that is no digit: its value)
\ each byte's value as a digit - of either form: + and - are 62, / and _ 63 - or none
: digit-values ( -- s )
    {: | b :}  <bytes> to b  256 0 do  none b push  loop
    64 0 do  i  i digits nth  b nth!   i  i url-digits nth  b nth!  loop
    b " " like  b dispose ;
digit-values permanent constant values
\ the text before its padding - if what follows the first = is = and no more
: unpadded ( in -- s true | false )
    {: in | p :}  [char] = in index to p
    p 0< if  in true exit  then
    in p tail [: [char] = <> ;] find if  drop false exit  then
    in p head true ;
\ the bytes as digits of that alphabet: their bits by sixes, each six its digit
: digits-by ( a n alphabet -- s )  {: alphabet :}  >string drop  8 6 regroup  alphabet translate ;
PRIVATE>

\ the bytes as base64 (a new string), padded with = to a whole number of 4 digits
: >base64 ( a n -- s len )
    {: a n :}
    [: a n digits digits-by {: d :}
       [: d %  d length negate 3 and 0 ?do  [char] = %c  loop ;] build rescue ;] with-scratch  string ;

\ base64 (padded or not, + / or the URL form's - _) as its bytes, a new string; false for text that isn't base64
: base64> ( a n -- s len true | false )
    {: a n | out :}
    [: a n >string drop unpadded if
         values translate {: v :}
         none v index 0< if  v 6 8 regroup rescue to out  then
       then ;] with-scratch
    out if  out string true  else  false  then ;

\ the bytes as the URL form of base64 (RFC 4648, 5): - and _ for + and /, no = padding - for a URL, a cookie, a
\ token (a new string; base64> reads it back)
: >base64url ( a n -- s len )  {: a n :}  [: a n url-digits digits-by rescue ;] with-scratch  string ;

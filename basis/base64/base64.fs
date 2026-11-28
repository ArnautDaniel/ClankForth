\ base64.fs - RFC 4648's base64: bytes as text and back.
\   s" hi!" >base64 ( -- s len )  "aGkh"        s" aGkh" base64> ( -- s len true )
\   s" \xfb\xff" >base64url ( -- s len )  "-_8"   (the URL form: no padding)
IN: base64

\ (Written in C - (>base64), (base64>): src/objects.c. A session's cookie and a form's token are signed and read at
\ every request: as Forth's words, a byte pushed at a time, they were 6% of a page's instructions.)

\ the bytes as base64 (a new string), padded with = to a whole number of 4 digits
: >base64 ( a n -- s len )  false (>base64) ;

\ base64 (padded or not, + / or the URL form's - _) as its bytes, a new string; false for text that isn't base64
: base64> ( a n -- s len true | false )  (base64>) ;

\ the bytes as the URL form of base64 (RFC 4648, 5): - and _ for + and /, no = padding - for a URL, a cookie, a
\ token (a new string; base64> reads it back)
: >base64url ( a n -- s len )  true (>base64) ;

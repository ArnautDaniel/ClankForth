\ encoding.fs - URLs' %-escapes (RFC 3986): text taken out of a path or a query, and put into one.
\   s" a%20b+c" url-decode ( -- s len )  "a b c"        s" a b&c" url-encode ( -- s len )  "a%20b%26c"
IN: urls.encoding

<PRIVATE
32 constant space-char
: hex-digit ( c -- n )   \ -1 for none
    {: c :}
    c [char] 0 [char] 9 1+ within if  c [char] 0 -  exit  then
    c 32 or {: l :}
    l [char] a [char] f 1+ within if  l [char] a - 10 +  exit  then
    -1 ;
\ a new string: each %HH the byte, and (plus? set) each + a space; a % with no two hex digits kept as it is
: decode ( a n plus? -- s len )
    {: a n plus? | b i c :}  <bytes> to b
    begin  i n <  while
        a i + c@ to c
        c [char] % =  i 2 + n <  and if
            a i + 1+ c@ hex-digit  a i + 2 + c@ hex-digit {: hi lo :}
            hi 0>= lo 0>= and if  hi 16 * lo +  b b-push  i 3 + to i
            else  c b b-push  i 1+ to i  then
        else
            c [char] + = plus? and if  space-char  else  c  then  b b-push  i 1+ to i
        then
    repeat
    b b>string  b dispose ;
: unreserved? ( c -- flag )   \ kept as it is in a URL: letters, digits, - . _ ~
    {: c :}  c [char] a [char] z 1+ within  c [char] A [char] Z 1+ within or  c [char] 0 [char] 9 1+ within or
    c [char] - = or  c [char] . = or  c [char] _ = or  c [char] ~ = or ;
PRIVATE>

: url-decode ( a n -- s len )  true decode ;     \ a query's: + is a space (a new string)
: path-decode ( a n -- s len )  false decode ;   \ a path's: + is itself
\ every byte but letters, digits and - . _ ~ as %HH (a new string): a query's value, or a path's segment
: url-encode ( a n -- s len )
    {: a n | b :}  <bytes> to b
    n 0 ?do
        a i + c@ {: c :}
        c unreserved? if  c b b-push  else  c s" %{02X}" format {: s sn :}  s sn b b-append  s dispose  then
    loop
    b b>string  b dispose ;

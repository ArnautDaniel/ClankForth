\ encoding.fs - URLs' %-escapes (RFC 3986): text taken out of a path or a query, and put into one.
\   s" a%20b+c" url-decode ( -- s len )  "a b c"        s" a b&c" url-encode ( -- s len )  "a%20b%26c"
IN: urls.encoding

<PRIVATE
32 constant space-char
\ what came after a %: its two hex digits the byte, the rest as it is - with no two, the % kept
: %escape ( piece b -- )
    {: p b :}
    p length 2 >= if  p 0 2 16 digits>number  else  false  then
    if  b b-push  p 2 p length b b-append-seq  else  [char] % b b-push  p 0 p length b b-append-seq  then ;
\ a new string: each %HH the byte, and (plus? set) each + a space; a % with no two hex digits kept as it is
: decode ( a n plus? -- s len )
    {: a n plus? | b :}
    a n s" %" s-find 0<  a n s" +" s-find 0<  plus? 0= or  and if  a n >string exit  then   \ (nothing to decode: most)
    <bytes> to b
    [: a n >seq  plus? if  [: dup [char] + = if  drop space-char  then ;] map  then
       " %" split [: {: p i :}  i if  p b %escape  else  p 0 p length b b-append-seq  then ;] each-index ;] with-scratch
    b b>string  b dispose ;
: unreserved? ( c -- flag )   \ kept as it is in a URL: letters, digits, - . _ ~
    {: c :}  c [char] a [char] z 1+ within  c [char] A [char] Z 1+ within or  c [char] 0 [char] 9 1+ within or
    c [char] - = or  c [char] . = or  c [char] _ = or  c [char] ~ = or ;
PRIVATE>

: url-decode ( a n -- s len )  true decode ;     \ a query's: + is a space (a new string)
: path-decode ( a n -- s len )  false decode ;   \ a path's: + is itself
\ every byte but letters, digits and - . _ ~ as %HH (a new string): a query's value, or a path's segment
: url-encode ( a n -- s len )
    {: a n :}  [: a n >seq [: {: c :}  c unreserved? if  c %c  else  c s" %{02X}" format %s  then ;] each ;] build string ;

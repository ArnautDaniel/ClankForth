\ tokens.fs - what a server gives out that no one may guess or forge: random tokens (a session's id, a CSRF token, a
\ link to reset a password), UUIDs, and values signed with a key (HMAC-SHA256) - a cookie that can't be changed.
\   random-token ( -- s len )  43 characters, 256 bits    uuid4 ( -- s len )  "3f2a...-4...-8..."
\   s" user=7" key sign ( -- s len )  "user=7.<mac>"    signed key unsign ( -- s len true | false )
IN: crypto.tokens
USING: crypto base64 checksums ;

<PRIVATE
32 constant token-bytes
16 constant uuid-bytes
6 constant version-at   8 constant variant-at   \ (a UUID's bytes holding its version, its variant)
46 constant dot
PRIVATE>

\ 256 random bits as the URL form of base64 (a new string): an id no one guesses - a session's, a CSRF token's
: random-token ( -- s len )  token-bytes random-bytes {: s n :}  s n >base64url  s dispose ;
\ so many random bytes as hex digits (a new string)
: random-hex ( n -- s len )  random-bytes {: s n :}  s n >hex  s dispose ;

\ a random UUID (RFC 9562, version 4): 8-4-4-4-12 hex digits (a new string)
: uuid-byte ( c i -- c' )   \ the version's and the variant's bits set in their bytes
    {: c i :}  i version-at = if  c $0f and $40 or  exit  then  i variant-at = if  c $3f and $80 or  exit  then  c ;
: uuid4 ( -- s len )
    {: | made :}
    [: uuid-bytes random-bytes drop [: uuid-byte ;] map-index string >hex {: h hn :}
       h hn 0 8 s-slice  h hn 8 4 s-slice  h hn 12 4 s-slice  h hn 16 4 s-slice  h hn 20 12 s-slice
       s" {s}-{s}-{s}-{s}-{s}" format drop rescue to made ;] with-scratch
    made string ;

\ the text with its MAC by the key after a dot (a new string): a value given out that comes back as it was, or is
\ known not to (a cookie's: the user it is for, an expiry). Not secret - signed: put nothing in it to hide.
: sign ( a n key n -- s len )
    {: a n ka kn :}
    ka kn a n hmac-sha256 {: m mn :}  m mn >base64url {: b bn :}
    a n b bn s" {s}.{s}" format  m dispose  b dispose ;
\ the text a sign gave, if its MAC is the key's (false: changed, or signed by another key)
: unsign ( s n key n -- a n true | false )
    {: sa sn ka kn :}  sa sn s" ." s-find-last {: at :}   \ (the MAC after the last dot: the text may have its own)
    at 0< if  false exit  then
    sa at ka kn sign {: t tn :}
    t tn sa sn same-bytes?  t dispose
    if  sa at true  else  false  then ;

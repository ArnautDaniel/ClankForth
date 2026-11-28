\ jwt.fs - JSON Web Tokens (RFC 7519), RS256 only: an OpenID Connect provider's ID tokens (Microsoft Entra's ...)
\ checked against its keys (a JWKS: what its jwks_uri gives), and tokens signed with a key of one's own.
\
\   token tn keys jwt-verify ( -- claims )        \ the claims (a map) - or an error saying why it isn't good
\   claims s" https://login.microsoftonline.com/<tenant>/v2.0" s" <client id>" jwt-check
\
\ Only RS256: a token saying "none", or HS256 (a public key taken for a secret), is refused, whatever it says.
IN: crypto.jwt
USING: crypto json base64 system ;

300 value jwt-skew-s   \ clocks that differ: a token this many seconds past its exp (or before its nbf) still good

<PRIVATE
: jwt-error ( a n -- )  true -rot (abort") ;
: unbase64 ( a n -- s len )   \ base64url, its padding left off
    base64> 0= if  s" jwt: a part that isn't base64url" jwt-error  then ;
: json-part ( a n -- v )   \ a part's JSON
    unbase64 {: s n :}  [: s n json> ;] catch if  s" jwt: a part that isn't JSON" jwt-error  then  s dispose ;
: dots ( a n -- at1 at2 )   \ where the two dots are
    {: a n :}  a n s-bytes drop to a  a n s" ." s-find {: one :}
    one 0< if  s" jwt: not header.payload.signature" jwt-error  then
    a one + 1+  n one - 1-  s" ." s-find {: two :}
    two 0< if  s" jwt: not header.payload.signature" jwt-error  then
    one  one 1+ two + ;
: text ( key n m -- a n | 0 0 )  json-string@ 0= if  0 0  then ;
: key-of ( kid n keys -- key )   \ the RSA key of that kid (none given: the set's one key)
    {: ka kn keys :}
    s" keys" keys json-list@ 0= if  s" jwt: a key set without keys" jwt-error  then {: l :}
    kn 0= if   \ (no kid: only a set of one RSA key says which)
        l [: {: k :}  s" kty" k text s" RSA" s= ;] filter {: rsa :}
        rsa length 1 <> if  s" jwt: a token without a kid, and a key set of more than one key" jwt-error  then
        0 rsa nth  rsa list-free exit
    then
    l [: {: k :}  s" kty" k text s" RSA" s=  s" kid" k text ka kn s= and ;] find
    0= if  ka kn s" jwt: no key {s} in the key set" format jwt-error  then ;
: whole-number ( key n claims -- n true | false )  json-int@ ;
PRIVATE>

\ the token's claims (a map), if it is good: RS256, signed by the key its kid names in the set (a JWKS's JSON, as
\ json> gives it), not expired (exp) nor not yet good (nbf) - jwt-skew-s allowed; else an error saying why
: jwt-verify ( token n keys -- claims )
    {: ta tn keys :}  ta tn s-bytes drop to ta
    ta tn dots {: d1 d2 :}
    ta d1 json-part {: header :}
    s" alg" header text s" RS256" s= 0= if  s" jwt: not RS256 (only RS256 is taken)" jwt-error  then
    s" kid" header text keys key-of {: key :}
    s" n" key text unbase64 {: n nn :}  s" e" key text unbase64 {: e en :}
    ta d2 + 1+  tn d2 - 1-  unbase64 {: sig sn :}
    ta d2  sig sn  n nn  e en  rsa-sha256-verify 0= if  s" jwt: the signature isn't the key's" jwt-error  then
    ta d1 + 1+  d2 d1 - 1-  json-part {: claims :}
    claims map? 0= if  s" jwt: its claims aren't an object" jwt-error  then
    time-now {: now :}
    s" exp" claims whole-number 0= if  s" jwt: no exp (a token has to end)" jwt-error  then
    jwt-skew-s +  now < if  s" jwt: expired" jwt-error  then
    s" nbf" claims whole-number if  jwt-skew-s -  now > if  s" jwt: not good yet (nbf)" jwt-error  then  then
    claims ;
\ the claims are the issuer's (iss) and for this audience (aud: a string, or a list holding it); else an error
: jwt-check ( claims iss n aud n -- )
    {: c ia in aa an :}
    s" iss" c text ia in s= 0= if  s" jwt: another issuer" jwt-error  then
    s" aud" c s-at 0= if  s" jwt: no audience" jwt-error  then {: aud :}
    aud string? if  aud string aa an s=
    else aud list? if  aud [: {: x :}  x string? if  x string aa an s=  else  false  then ;] any?
    else  false  then then
    0= if  s" jwt: for another audience" jwt-error  then ;

\ claims (a JSON object's text) as a token signed RS256 with the key (its kid; n and d big-endian bytes)
: jwt-sign ( claims n kid n modulus n d n -- token n )
    {: ca cn ka kn ma mn da dn | b :}
    ka kn s\" {{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"{s}\"}}" format {: h hn :}
    <bytes> to b
    h hn >base64url {: x xn :}  x xn b b-append  x dispose  h dispose
    s" ." b b-append
    ca cn >base64url {: y yn :}  y yn b b-append  y dispose
    b b-data ma mn da dn rsa-sha256-sign {: s sn :}
    s" ." b b-append  s sn >base64url {: z zn :}  z zn b b-append  z dispose  s dispose
    b b>string  b dispose ;
\ a key as a JWKS's entry would have it: {"kty": "RSA", "kid", "n", "e"} (n and e big-endian bytes) - a set of one:
\ {"keys": [...]}, as JSON text
: jwks-of ( kid n modulus n e n -- s len )
    {: ka kn ma mn ea en :}
    ma mn >base64url {: n nn :}  ea en >base64url {: e en2 :}
    ka kn n nn e en2 s\" {{\"keys\":[{{\"kty\":\"RSA\",\"use\":\"sig\",\"kid\":\"{s}\",\"n\":\"{s}\",\"e\":\"{s}\"}}]}}" format
    n dispose  e dispose ;

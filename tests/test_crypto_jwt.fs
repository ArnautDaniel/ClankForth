\ crypto.jwt (basis/crypto/jwt) and RSA (crypto's rsa-sha256-verify / -sign): a token OpenSSL signed verified; tokens
\ signed here verified; a changed one, "none", an expired one, another key, issuer or audience refused
IN: test-crypto-jwt
USING: tester crypto crypto.jwt json ;

: hex-digit ( c -- n )  {: c :}  c [char] 9 <= if  c [char] 0 -  else  c 32 or [char] a - 10 +  then ;
: unhex ( a n -- s len )   \ (a key's big-endian bytes, written in hex)
    {: a n | b :}  <bytes> to b  n 2/ 0 ?do  a i 2* + c@ hex-digit 4 lshift  a i 2* + 1+ c@ hex-digit or  b b-push  loop
    b b>string  b dispose ;
: n-hex ( -- a n )  s" e56120cc125bc604fcb1f3099756e2eb6a8c073679f93df185c601c9172d029cf7bb84b597189369e96ae794656f6a238323cd35fd4fc97385700bce4793b24e43e7b14310d568fb9a4b350d3285e5cd58fa8631c760a0a62d94ee03a8c1a040255777db0d12930a70e82b61abb43ff97f28459a8afd0f45b60b1f2e5606fca172ec125c46742a416068f11587382dde1a7908f98045d39c8cbda288bc5c1e86b1ade8a272cb211483ebfafa62231f08678a2280811c865fe3ceba2cfdcaa7a936750ba7ab91c2764270a1d5839b2e86db406a54275b1704b721a5a65df389cf70fd288f7951a718cf57e2f68f3967dc846e831ab24df4538ae3465d3be1f8a7" ;
: d-hex ( -- a n )  s" 484341a858ce45c39372623d2ecae62141474d0c69ffcdf6756cec6a141d8f79b10807e6876bec210d32ab335283779e2975d17405522a06754ed7c14c466901841e0d77b1e971b1f6296454bc8a775baa80fe5ed74f873ce3164ca7a5761e8ca6d6f800f00cde7b0717f576c737b981d9ffef71a100d95b5e5f019b40e9e1934895a181c5ea505e048d05c6dfea058b4ae0587e2be634eaf799997a8ecacbc0e91b70a512b238cdbff89a7101cd6be0e5c0e9a1e0ae3c4bd5acb471a530409768f3cc7808a1b287e2bbfa648089f48db8dfc8b700ca48e68fc2a60869a2dc3dfbdd2a3168463db79f1ec4a82e099b31fd45bb78ac1b4b3ce14d9a04371427cd" ;
n-hex unhex drop constant modulus   d-hex unhex drop constant private
: n ( -- a n )  modulus string ;   : d ( -- a n )  private string ;   : e ( -- a n )  s\" \x01\x00\x01" ;
: openssl-token ( -- a n )  s" eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImsxIn0.eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0L3YyLjAiLCJhdWQiOiJjbGllbnQtMSIsInN1YiI6InUxIiwibmFtZSI6IkFubiIsInJvbGVzIjpbIlN0YWZmIl0sImV4cCI6NDEwMjQ0NDgwMCwibmJmIjoxMDAwMDAwMDAwfQ.EBx-iS1NezUrXgZsz7pbGk3nr-UWYFVceQjzFb7aFzN-UsPZzIyW4teYmvYQGNDl7dgvPDTqa-HhA-HmcRgeEGyYcEyO2lZ9FJyMzxiL94vwBE9UhKpt1Gd5j-Gjs-ZdcE6Ho-T2D5SRwtVnAzBRaij0h5VEwvjTHW4LCg9-mTMkn2NBNDbSAF0cJXYOLXyUh1yROSNLuDXOTDTv0ThuD7kNTo8gyOenXKf5bJYktzVTEIcLJ8pJuTKXXJw4p9fLpriQ07UgUGrhqkXrwuzE4HxYRviH1DgBPB9e2tvKDQppXlJQoKj0uSf9gnmSq0rVqC18amwCpFMxPCAeteVJaQ" ;   \ (Python made it, OpenSSL signed it: exp 2100, kid k1)
s" k1" n e jwks-of json> constant keys

testing RSA: a signature made here verifies; it is OpenSSL's own (PKCS#1 v1.5 is the same every time)
T{ s" a message" n d rsa-sha256-sign  2dup  s" a message" 2swap n e rsa-sha256-verify -rot 2drop -> true }T
T{ s" a message" n d rsa-sha256-sign  s" a messagE" 2swap n e rsa-sha256-verify -> false }T

testing a token OpenSSL signed: verified, its claims read; its issuer and audience checked
: openssl-name? ( -- flag )  openssl-token keys jwt-verify {: c :}  s" name" c json-string@ drop s" Ann" s= ;
T{ openssl-name? -> true }T
: checked ( iss n aud n -- ok? )  {: ia in aa an :}  [: openssl-token keys jwt-verify ia in aa an jwt-check ;] catch 0= ;
T{ s" https://issuer.test/v2.0" s" client-1" checked -> true }T
T{ s" https://other.test/v2.0" s" client-1" checked -> false }T
T{ s" https://issuer.test/v2.0" s" client-2" checked -> false }T

testing a token signed here (jwt-sign): verified; changed, refused
: signed ( claims n -- a n )  s" k1" n d jwt-sign ;
: claims ( exp -- a n )  s\" {{\"sub\":\"u2\",\"exp\":{}}}" format ;
: good? ( a n -- flag )  {: a n :}  [: a n keys jwt-verify drop ;] catch 0= ;
T{ 4102444800 claims signed good? -> true }T
: changed ( a n -- a' n' )   \ (a byte of its claims changed: the signature no longer theirs)
    >string drop {: s :}  s string s" ." s-find 5 + {: at :}  [char] A  at s nth  = if  [char] B  else  [char] A  then  at s nth!  s string ;
T{ 4102444800 claims signed changed good? -> false }T
testing refused: expired; "none"; a kid the set hasn't; not a token at all
T{ 1000 claims signed good? -> false }T
: none-token ( -- a n )  s" eyJhbGciOiJub25lIn0.eyJzdWIiOiJ4In0." ;
T{ none-token good? -> false }T
T{ 4102444800 claims s" k9" n d jwt-sign good? -> false }T
T{ s" not a token" good? -> false }T
T{ s\" {\"sub\":\"u3\"}" signed good? -> false }T   \ (no exp: refused - a token has to end)

test-summary

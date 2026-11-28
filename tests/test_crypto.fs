\ crypto (src/crypto.c): SHA-256 by FIPS 180's vectors, HMAC-SHA256 by RFC 4231's, PBKDF2-HMAC-SHA256 by RFC 7914's;
\ SHA-1, HMAC-SHA1 and PBKDF2-HMAC-SHA1 by FIPS 180's, RFC 2202's and RFC 6070's;
\ the system's random bytes; a comparison all the way. crypto.passwords and crypto.tokens on them.
IN: test-crypto
USING: tester crypto checksums base64 crypto.passwords crypto.tokens ;

: hex= ( s len a n -- flag )  {: s sn ea en :}  s sn >hex {: h hn :}  h hn ea en s=  h dispose  s dispose ;

testing SHA-256: FIPS 180-4's vectors (one block, none, two)
T{ s" abc" sha256 s" ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" hex= -> true }T
T{ s" " sha256 s" e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" hex= -> true }T
T{ s" abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" sha256
   s" 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" hex= -> true }T
create million 1000000 allot  million 1000000 char a fill
T{ million 1000000 sha256 s" cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" hex= -> true }T
testing HMAC-SHA256: RFC 4231's cases 2 (a short key) and 6 (a key longer than a block)
T{ s" Jefe" s" what do ya want for nothing?" hmac-sha256
   s" 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" hex= -> true }T
create long-key 131 allot  long-key 131 $aa fill
T{ long-key 131 s" Test Using Larger Than Block-Size Key - Hash Key First" hmac-sha256
   s" 60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54" hex= -> true }T
testing PBKDF2-HMAC-SHA256: RFC 7914's vectors (1 iteration; 4096, a key longer than a digest); out of bounds an error
T{ s" passwd" s" salt" 1 64 pbkdf2-sha256
   s" 55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783" hex= -> true }T
T{ s" password" s" salt" 4096 32 pbkdf2-sha256
   s" c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a" hex= -> true }T
T{ s" p" s" s" 0 32 ' pbkdf2-sha256 catch nip nip nip nip nip nip -> -1 }T
T{ s" p" s" s" 1 0 ' pbkdf2-sha256 catch nip nip nip nip nip nip -> -1 }T
testing SHA-1: FIPS 180's vectors (one block, none, two, a million a's)
T{ s" abc" sha1 s" a9993e364706816aba3e25717850c26c9cd0d89d" hex= -> true }T
T{ s" " sha1 s" da39a3ee5e6b4b0d3255bfef95601890afd80709" hex= -> true }T
T{ s" abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" sha1
   s" 84983e441c3bd26ebaae4aa1f95129e5e54670f1" hex= -> true }T
T{ million 1000000 sha1 s" 34aa973cd4c4daa4f61eeb2bdbad27316534016f" hex= -> true }T
testing HMAC-SHA1: RFC 2202's cases 2 (a short key) and 6 (a key longer than a block)
T{ s" Jefe" s" what do ya want for nothing?" hmac-sha1 s" effcdf6ae5eb2fa2d27416d5f184df9c259a7c79" hex= -> true }T
T{ long-key 80 s" Test Using Larger Than Block-Size Key - Hash Key First" hmac-sha1
   s" aa4ae5e15272d00e95705637ce8a3b55ed402112" hex= -> true }T
testing PBKDF2-HMAC-SHA1: RFC 6070's vectors (1, 2 and 4096 iterations; a key longer than a digest)
T{ s" password" s" salt" 1 20 pbkdf2-sha1 s" 0c60c80f961f0e71f3a9b524af6012062fe037a6" hex= -> true }T
T{ s" password" s" salt" 2 20 pbkdf2-sha1 s" ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957" hex= -> true }T
T{ s" password" s" salt" 4096 20 pbkdf2-sha1 s" 4b007901b765489abead49d926f721d065a429c1" hex= -> true }T
T{ s" passwordPASSWORDpassword" s" saltSALTsaltSALTsaltSALTsaltSALTsalt" 4096 25 pbkdf2-sha1
   s" 3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038" hex= -> true }T
T{ s" p" s" s" 0 20 ' pbkdf2-sha1 catch nip nip nip nip nip nip -> -1 }T
testing an ASP.NET Identity 2 hash (made by Python's hashlib): its password, not another; not of the form, false
: v2-hash ( -- a n )  s" AAABAgMEBQYHCAkKCwwNDg/UWACWdUIHigknKcLi6xkd45Y0F6jeV7yCfhMjAjLM7Q==" ;
T{ s" Hunter2!" v2-hash aspnet-v2-ok? -> true }T
T{ s" hunter2!" v2-hash aspnet-v2-ok? -> false }T
T{ s" correct horse" s" AAABAgMEBQYHCAkKCwwNDg+d2FbDdriytxO2vgdAVMuIpGVW4BGF6o0NJn8znGUhaQ==" aspnet-v2-ok? -> true }T
T{ s" Hunter2!" s" AQAAAAEAACcQAAAAEJ4" aspnet-v2-ok? -> false }T   \ (a V3 hash's start: not V2)
T{ s" Hunter2!" s" not base64!" aspnet-v2-ok? -> false }T
T{ v2-hash rehash? -> true }T   \ (signed in: kept again as hash-password's)
testing random-bytes: so many, not the same twice; out of bounds an error
T{ 32 random-bytes nip -> 32 }T
T{ 16 random-bytes 16 random-bytes same-bytes? -> false }T
T{ 0 random-bytes nip -> 0 }T
T{ -1 ' random-bytes catch nip -> -1 }T
testing same-bytes?: equal, a byte off, a length off
T{ s" token" s" token" same-bytes? -> true }T
T{ s" token" s" tokeN" same-bytes? -> false }T
T{ s" token" s" toke" same-bytes? -> false }T
T{ s" " s" " same-bytes? -> true }T

testing passwords: hashed with a salt of their own, checked back, wrong ones refused, old hashes known
1000 to password-iterations
: hashed ( -- a n )  s" correct horse" hash-password ;
variable kept-a   variable kept-n   hashed kept-n ! kept-a !
: kept ( -- a n )  kept-a @ kept-n @ ;
T{ kept s" pbkdf2-sha256$1000$" s-starts? -> true }T
T{ s" correct horse" kept check-password -> true }T
T{ s" correct horsE" kept check-password -> false }T
T{ s" " kept check-password -> false }T
T{ hashed kept s= -> false }T   \ (each its own salt)
T{ s" x" s" pbkdf2-sha256$1000$nope" check-password -> false }T
T{ s" x" s" md5$1$AAAA$AAAA" check-password -> false }T
T{ s" x" s" pbkdf2-sha256$zz$AAAA$AAAA" check-password -> false }T
T{ s" x" s" pbkdf2-sha256$1000$!!$AAAA" check-password -> false }T
T{ kept rehash? -> false }T
2000 to password-iterations
T{ kept rehash? -> true }T
T{ s" correct horse" kept check-password -> true }T   \ (its own iterations: still right)
600000 to password-iterations

testing tokens: random tokens, UUIDs (version 4, its variant), values signed and checked
T{ random-token nip -> 43 }T
T{ random-token random-token s= -> false }T
T{ 8 random-hex nip -> 16 }T
: uuid-ok? ( a n -- flag )
    {: a n :}  n 36 <> if  false exit  then
    a n >seq {: u :}  8 u nth [char] - =  14 u nth [char] 4 = and
    19 u nth  " 89ab" contains?  and ;
T{ uuid4 uuid-ok? -> true }T
T{ uuid4 uuid4 s= -> false }T
variable signed-a   variable signed-n   s" user=7;exp=1700000000" s" server key" sign signed-n ! signed-a !
: signed ( -- a n )  signed-a @ signed-n @ ;
T{ signed s" user=7;exp=1700000000." s-starts? -> true }T
T{ signed s" server key" unsign  -rot s" user=7;exp=1700000000" s= -> true true }T
T{ signed s" other key" unsign -> false }T
: tampered ( -- a n )   \ (its sixth byte another)
    signed >seq [: {: c i :}  i 5 = if  [char] 8  else  c  then ;] map-index string ;
T{ tampered s" server key" unsign -> false }T
T{ s" no dot here" s" server key" unsign -> false }T
T{ s" " s" k" sign s" k" unsign rot drop -> 0 true }T

test-summary

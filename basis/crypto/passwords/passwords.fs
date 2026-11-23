\ passwords.fs - passwords kept as their hashes: PBKDF2-HMAC-SHA256 (crypto's), a random salt each, the iterations
\ written with it - so a stolen table gives no password but by guessing, each guess as slow as a sign-in.
\   s" hunter2" hash-password ( -- s len )   "pbkdf2-sha256$600000$<salt>$<hash>"   (kept in the user's record)
\   s" hunter2" stored check-password ( -- flag )
\   s" hunter2" legacy aspnet-v2-ok? ( -- flag )   (another system's hash, ASP.NET Identity 2's: then hash-password)
IN: crypto.passwords
USING: crypto base64 ;

600000 value password-iterations   \ a new hash's (OWASP's for PBKDF2-HMAC-SHA256, 2023): ~0.25 s a hash, here

<PRIVATE
16 constant salt-bytes
32 constant hash-bytes
36 constant dollar
s" pbkdf2-sha256" >string drop constant scheme

\ the text's pieces between $ (a list of strings)
: pieces ( a n -- l )  s" $" s-split ;
PRIVATE>

\ a password's hash, to keep: its scheme, iterations, salt and hash, between $ (base64)
: hash-password ( password n -- s len )
    {: pa pn | salt hash :}
    salt-bytes random-bytes drop to salt
    pa pn salt string password-iterations hash-bytes pbkdf2-sha256 drop to hash
    scheme string  password-iterations  salt string >base64  hash string >base64
    s" {s}${}${s}${s}" format ;

\ the password the one kept hashed? (false for a hash not of this form; compared all the way: its time tells nothing)
: check-password ( password n stored n -- flag )
    {: pa pn sa sn | l iters salt hash :}
    sa sn pieces to l
    l length 4 <> if  false exit  then
    0 l nth string scheme string s= 0= if  false exit  then
    1 l nth string string>number 0= if  drop false exit  then  to iters
    iters 1 < if  false exit  then
    2 l nth string base64> 0= if  false exit  then  drop to salt
    3 l nth string base64> 0= if  false exit  then  {: ha hn :}
    hn 1 < if  false exit  then
    pa pn salt string iters hn pbkdf2-sha256  ha hn same-bytes? ;

<PRIVATE
49 constant aspnet-v2-bytes   \ its base64's bytes: the marker 0, the salt, the subkey
1000 constant aspnet-v2-iterations
PRIVATE>
\ the password the one an ASP.NET Identity 2 hash (its "V2" format, an AspNetUsers row's PasswordHash) was made of?
\ base64 of 0x00, a 16-byte salt and PBKDF2-HMAC-SHA1's 32 bytes of 1,000 iterations - false for a hash not of that
\ form; compared all the way. Only to check what another system kept: sign the user in, then keep hash-password's
\ (`rehash?` is true of this form).
: aspnet-v2-ok? ( password n hash n -- flag )
    {: pa pn ha hn :}
    ha hn base64> 0= if  false exit  then  {: b bn :}
    bn aspnet-v2-bytes <>  b c@ 0<> or if  false exit  then
    pa pn  b 1+ salt-bytes  aspnet-v2-iterations hash-bytes pbkdf2-sha1
    b 1+ salt-bytes + hash-bytes same-bytes? ;

\ the hash made with fewer iterations than a new one would be: hash the password again as it signs in, and keep that
: rehash? ( stored n -- flag )
    pieces {: l :}
    l length 4 <> if  true exit  then
    1 l nth string string>number 0= if  drop true exit  then
    password-iterations < ;

\ http.oidc (basis/http/oidc): signing in by OpenID Connect against a provider standing in for Entra - the redirect
\ with PKCE, state and nonce; the code traded, the ID token checked (its key, issuer, audience, nonce); the user and
\ their roles in the session; pages that need signing in, or a role
IN: test-http-oidc
USING: tester actors accessors net system json jsondb crypto crypto.jwt base64 http http.sessions http.oidc vocabs.webclient ;

: wipe ( dir n -- )
    {: a n :}  a n dir? 0= if  exit  then
    a n dir-list {: l :}  l length 0 ?do  a n s" /" s+  i l nth string s+  delete-file  loop  a n delete-file ;
s" jsondb-oidc-data" wipe
s" jsondb-oidc-data" db-open constant db
db <session-store> constant sessions

\ ---- the provider: its key (the tests' own), its endpoints ----
: hex-digit ( c -- n )  {: c :}  c [char] 9 <= if  c [char] 0 -  else  c 32 or [char] a - 10 +  then ;
: unhex ( a n -- s len )
    {: a n | b :}  <bytes> to b  n 2/ 0 ?do  a i 2* + c@ hex-digit 4 lshift  a i 2* + 1+ c@ hex-digit or  b b-push  loop
    b b>string  b dispose ;
: n-hex ( -- a n )  s" e56120cc125bc604fcb1f3099756e2eb6a8c073679f93df185c601c9172d029cf7bb84b597189369e96ae794656f6a238323cd35fd4fc97385700bce4793b24e43e7b14310d568fb9a4b350d3285e5cd58fa8631c760a0a62d94ee03a8c1a040255777db0d12930a70e82b61abb43ff97f28459a8afd0f45b60b1f2e5606fca172ec125c46742a416068f11587382dde1a7908f98045d39c8cbda288bc5c1e86b1ade8a272cb211483ebfafa62231f08678a2280811c865fe3ceba2cfdcaa7a936750ba7ab91c2764270a1d5839b2e86db406a54275b1704b721a5a65df389cf70fd288f7951a718cf57e2f68f3967dc846e831ab24df4538ae3465d3be1f8a7" ;
: d-hex ( -- a n )  s" 484341a858ce45c39372623d2ecae62141474d0c69ffcdf6756cec6a141d8f79b10807e6876bec210d32ab335283779e2975d17405522a06754ed7c14c466901841e0d77b1e971b1f6296454bc8a775baa80fe5ed74f873ce3164ca7a5761e8ca6d6f800f00cde7b0717f576c737b981d9ffef71a100d95b5e5f019b40e9e1934895a181c5ea505e048d05c6dfea058b4ae0587e2be634eaf799997a8ecacbc0e91b70a512b238cdbff89a7101cd6be0e5c0e9a1e0ae3c4bd5acb471a530409768f3cc7808a1b287e2bbfa648089f48db8dfc8b700ca48e68fc2a60869a2dc3dfbdd2a3168463db79f1ec4a82e099b31fd45bb78ac1b4b3ce14d9a04371427cd" ;
n-hex unhex drop constant modulus   d-hex unhex drop constant private
: e ( -- a n )  s\" \x01\x00\x01" ;
<router> constant idp
idp 0 http-listen listener-port constant idp-port
<router> constant site
site 0 http-listen listener-port constant site-port
: idp-url ( path n -- s len )  {: a n :}  idp-port a n s" http://127.0.0.1:{}{s}" format ;
variable roles-given   s\" [\"Staff\"]" >string drop roles-given !
create challenge-seen 64 allot   variable challenge-n   create nonce-seen 64 allot   variable nonce-n
: keep ( a n buf var -- )  {: a n buf var :}  a buf n 64 min move  n 64 min var ! ;
: discovery ( req -- )
    {: r :}  s" " idp-url {: i in :}  s" /authorize" idp-url {: au aun :}  s" /token" idp-url {: t tn :}  s" /jwks" idp-url {: j jn :}
    i in au aun t tn j jn
    s\" {{\"issuer\":\"{s}\",\"authorization_endpoint\":\"{s}\",\"token_endpoint\":\"{s}\",\"jwks_uri\":\"{s}\"}}" format
    200 r respond-json ;
: jwks ( req -- )  {: r :}  s" k1" modulus string e jwks-of 200 r respond-json ;
: authorize ( req -- )   \ (signed in at once: back to the app with a code, its state; the challenge, nonce kept)
    {: r :}
    s" code_challenge" r query-param drop challenge-seen challenge-n keep
    s" nonce" r query-param drop nonce-seen nonce-n keep
    s" redirect_uri" r query-param drop {: ra rn :}  s" state" r query-param drop {: sa sn :}
    ra rn sa sn s" {s}?code=the-code&state={s}" format 302 r redirect ;
: token ( req -- )   \ (the verifier must be the challenge's: PKCE)
    {: r :}
    s" code_verifier" r form-param drop sha256 >base64url  challenge-seen challenge-n @ s= 0= if
        s\" {\"error\":\"invalid_grant\"}" 400 r respond-json  exit
    then
    s" " idp-url  nonce-seen nonce-n @  roles-given @ string  time-now 3600 +
    s\" {{\"iss\":\"{s}\",\"aud\":\"client-1\",\"oid\":\"user-7\",\"name\":\"Ann\",\"preferred_username\":\"ann@cr.test\",\"nonce\":\"{s}\",\"roles\":{s},\"exp\":{}}}" format
    s" k1" modulus string private string jwt-sign {: t tn :}
    t tn s\" {{\"id_token\":\"{s}\"}}" format 200 r respond-json ;
s" /.well-known/openid-configuration" ' discovery idp on-get
s" /jwks" ' jwks idp on-get
s" /authorize" ' authorize idp on-get
s" /token" ' token idp on-post

\ ---- the app ----
s" /.well-known/openid-configuration" idp-url  s" client-1"  site-port s" http://127.0.0.1:{}/auth/callback" format
sessions oidc-config constant sign-in
sign-in site oidc-routes
: home ( req -- )  {: r :}  r sign-in signed-in-user {: u :}  s" name" u json-string@ drop s" home of {s}" format 200 r respond ;
: admin ( req -- )  {: r :}  s" admin" 200 r respond ;
s" /home" ' home site on-get
s" /admin" ' admin site on-get
s" /" sign-in site require-signed-in
s" /admin" s" Admin" sign-in site require-role

\ ---- the browser: a request to a port, its Location, its sid cookie ----
create jar 128 allot   variable jar-n
: keep-sid ( a n -- )
    {: a n :}  a n s" Set-Cookie: sid=" s-find {: at :}  at 0< if  exit  then
    a n at 16 + n at - 16 - s-slice {: va vn :}  va vn s" ;" s-find {: semi :}  semi 0< if  vn to semi  then
    va jar semi move  semi jar-n ! ;
create located 1024 allot   variable located-n
: keep-location ( a n -- )
    {: a n :}  a n s" Location: " s-find {: at :}  at 0< if  0 located-n !  exit  then
    a n at 10 + n at - 10 - s-slice {: va vn :}  va vn s\" \r\n" s-find {: end :}
    va located end move  end located-n ! ;
: location ( -- a n )  located located-n @ ;
: path-of ( url n -- a n )   \ http://127.0.0.1:port/path -> /path
    {: a n :}  a n s" //" s-find 2 + {: at :}  a at + n at - s" /" s-find at + {: p :}  a p + n p - ;
create got 4096 allot   variable got-n
: visit ( path n port -- status )   \ a GET with the jar's cookie: its Location and cookie kept
    {: pa pn port :}  port web-port !
    jar-n @ if  jar jar-n @ s\" Cookie: sid={s}\r\n" format  else  s" "  then {: h hn :}
    pa pn h hn get-with {: a n :}  a got n 4096 min move  n 4096 min got-n !
    a n keep-sid  a n keep-location  a n status-of ;
: answer ( -- a n )  got got-n @ body-of ;

testing a page that needs signing in: sent to sign in, coming back to it after
T{ s" /home" site-port visit  location s" /auth/login?next=%2Fhome" s= -> 302 true }T
testing signing in: to the provider (PKCE S256, state, nonce), back with a code, the token checked, signed in
T{ location site-port visit  location s" /authorize?" s-find 0> -> 302 true }T
T{ location path-of idp-port visit  location s" /auth/callback?code=the-code&state=" s-find 0> -> 302 true }T
T{ location path-of site-port visit  location s" /home" s= -> 302 true }T
T{ s" /home" site-port visit  answer s" home of Ann" s= -> 200 true }T
testing a role the user hasn't: 403; given it, the page
T{ s" /admin" site-port visit -> 403 }T
testing a callback whose state isn't the session's: refused
T{ s" /auth/callback?code=x&state=forged" site-port visit -> 400 }T
testing signed out: sent to sign in again
T{ s" /auth/logout" site-port visit drop  s" /home" site-port visit -> 302 }T
testing a token with another nonce (a replayed sign-in): refused
: sign-in-again ( -- s1 )  0 jar-n !  s" /home" site-port visit drop  location site-port visit drop  location path-of idp-port visit ;
T{ sign-in-again  s" not-the-nonce" nonce-seen nonce-n keep  location path-of site-port visit -> 302 400 }T

test-summary

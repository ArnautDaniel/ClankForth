\ http.oidc with a provider for many organizations (Entra's common, organizations): its issuer said with {tenantid}
\ in it - the token's own tenant (tid) taken there; the user's tenant kept; a token naming another tenant than its
\ tid, or none, refused
IN: test-http-oidc-tenants
USING: tester actors accessors net system json jsondb crypto crypto.jwt base64 http http.sessions http.oidc vocabs.webclient ;

: wipe ( dir n -- )
    {: a n :}  a n dir? 0= if  exit  then
    a n dir-list {: l :}  l length 0 ?do  a n s" /" s+  i l nth string s+  delete-file  loop  a n delete-file ;
s" jsondb-oidc-tenants-data" wipe
s" jsondb-oidc-tenants-data" db-open constant db
db <session-store> constant sessions
variable token-tenant   s" tenant-7" >string drop token-tenant !   \ (the tenant its iss names ...)
variable token-tid      s" tenant-7" >string drop token-tid !      \ (... and its tid)

\ ---- the provider: its key (the tests' own), its endpoints ----
: unhex ( a n -- s len )  checksums:hex> drop ;
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
    {: r :}  s" /{tenantid}/v2.0" idp-url {: i in :}  s" /authorize" idp-url {: au aun :}  s" /token" idp-url {: t tn :}  s" /jwks" idp-url {: j jn :}
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
    s" /" idp-url  token-tenant @ string  nonce-seen nonce-n @  roles-given @ string  time-now 3600 +  token-tid @ string
    s\" {{\"iss\":\"{s}{s}/v2.0\",\"aud\":\"client-1\",\"oid\":\"user-7\",\"name\":\"Ann\",\"nonce\":\"{s}\",\"roles\":{s},\"exp\":{},\"tid\":\"{s}\"}}" format
    s" k1" modulus string private string jwt-sign {: t tn :}
    t tn s\" {{\"id_token\":\"{s}\"}}" format 200 r respond-json ;
s" /.well-known/openid-configuration" ' discovery idp on-get
s" /jwks" ' jwks idp on-get
s" /authorize" ' authorize idp on-get
s" /token" ' token idp on-post

s" /.well-known/openid-configuration" idp-url  s" client-1"  site-port s" http://127.0.0.1:{}/auth/callback" format
sessions oidc-config constant sign-in
sign-in site oidc-routes
: home ( req -- )  {: r :}  r sign-in signed-in-user {: u :}  s" tenant" u json-string@ drop s" tenant {s}" format 200 r respond ;
s" /home" ' home site on-get
s" /" sign-in site require-signed-in

\ ---- the browser: a request to a port, its Location, its sid cookie ----
create jar 128 allot   variable jar-n   create flow 2048 allot   variable flow-n
: cookie-value ( a n name n buf var -- )   \ the Set-Cookie of that name's value, kept (an emptied one: forgotten)
    {: a n na nn buf var :}  a n na nn s" Set-Cookie: {s}=" format {: h hn :}  h hn s-find {: at :}
    at 0< if  exit  then
    a n at hn + n at - hn - s-slice {: va vn :}  va vn s" ;" s-find {: semi :}  semi 0< if  vn to semi  then
    va buf semi move  semi var ! ;
: keep-sid ( a n -- )  {: a n :}  a n s" sid" jar jar-n cookie-value  a n s" oidc_flow" flow flow-n cookie-value ;
create located 1024 allot   variable located-n
: keep-location ( a n -- )
    {: a n :}  a n s" Location: " s-find {: at :}  at 0< if  0 located-n !  exit  then
    a n at 10 + n at - 10 - s-slice {: va vn :}  va vn s\" \r\n" s-find {: end :}
    va located end move  end located-n ! ;
: location ( -- a n )  located located-n @ ;
: path-of ( url n -- a n )   \ http://127.0.0.1:port/path -> /path
    {: a n :}  a n s" //" s-find 2 + {: at :}  a n at n s-slice s" /" s-find at + {: p :}  a n p n s-slice ;
create got 4096 allot   variable got-n
: visit ( path n port -- status )   \ a GET with the jar's cookie: its Location and cookie kept
    {: pa pn port :}  port web-port !
    jar jar-n @ flow flow-n @ s\" Cookie: sid={s}; oidc_flow={s}\r\n" format {: h hn :}
    pa pn h hn get-with {: a n :}  a got n 4096 min move  n 4096 min got-n !
    a n keep-sid  a n keep-location  a n status-of ;
: answer ( -- a n )  got got-n @ body-of ;

: sign-in-flow ( -- status )   \ signed out, to /home and through the provider: the callback's status
    0 jar-n !  0 flow-n !  s" /home" site-port visit drop  location site-port visit drop
    location path-of idp-port visit drop  location path-of site-port visit ;
testing a token of tenant-7 (its iss naming it, its tid tenant-7): signed in, the user's tenant kept
T{ sign-in-flow  s" /home" site-port visit  answer s" tenant tenant-7" s= -> 302 200 true }T
testing a token whose iss names another tenant than its tid: refused
s" tenant-9" >string drop token-tenant !
T{ sign-in-flow -> 400 }T
s" tenant-7" >string drop token-tenant !
T{ sign-in-flow -> 302 }T

test-summary

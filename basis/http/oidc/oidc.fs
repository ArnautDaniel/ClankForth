\ oidc.fs - signing in with OpenID Connect (Microsoft Entra ID, or any provider that speaks it): the authorization
\ code flow with PKCE, the ID token checked (crypto.jwt: RS256 against the provider's keys, its issuer, audience,
\ nonce and time), the user and their app roles kept in the session (http.sessions). Routes and live events then
\ ask for a role.
\
\   s" <tenant id>" entra  s" <client id>" s" https://app.example/auth/callback" sessions oidc-config
\       s" CLIENT_SECRET" getenv drop rot oidc-secret  constant sign-in    \ (a confidential client: a secret)
\   sign-in site oidc-routes                       \ GET /auth/login, /auth/callback, /auth/logout
\   s" /" sign-in site require-signed-in           \ every page: signed in, or sent to sign in
\   s" /admin" s" Admin" sign-in site require-role \ these: the Admin role too (403 without it)
\
\ The handlers wait (the provider's discovery, its keys, its token endpoint): a request's. docs/http.md, Signing in.
IN: http.oidc
USING: actors accessors http http.client http.cookies http.sessions json crypto crypto.tokens crypto.jwt base64 urls.encoding system ;

\ a provider and this app at it: its discovery document's URL, the app's client id, its secret (0: a public client,
\ PKCE alone), the URL the provider sends the user back to, the scopes asked for, the sessions kept in
\ - and the key the sign-in under way is signed with (in a cookie, till the provider sends the user back)
TUPLE: oidc-client  discovery client secret redirect scopes store key ;

\ Microsoft Entra ID's discovery URL for a tenant (its id or its domain)
: entra ( tenant n -- url n )
    s" https://login.microsoftonline.com/{s}/v2.0/.well-known/openid-configuration" format ;
: oidc-config ( discovery n client n redirect n store -- cfg )
    {: da dn ca cn ra rn st :}
    da dn >string drop  ca cn >string drop  0  ra rn >string drop  s" openid profile email" >string drop  st
    32 random-bytes drop rescue-all   \ (a key of this node's own, till one is said: several nodes need oidc-key)
    <oidc-client> rescue-all ;
: oidc-secret ( a n cfg -- cfg )   \ (its key the secret's hash too, unless oidc-key says another)
    {: a n c :}  c a n >string drop rescue-all >>secret  a n sha256 drop rescue-all >>key ;
\ the key a sign-in under way is signed with: the same on every node of the site (a login on one, its callback
\ maybe on another) - 32 random bytes, kept as the app's other secrets are
: oidc-key ( a n cfg -- cfg )  {: a n c :}  c a n >string drop rescue-all >>key ;
: oidc-scopes ( a n cfg -- cfg )  {: a n c :}  c a n >string drop rescue-all >>scopes ;

<PRIVATE
86400 constant keys-keep-s         \ the provider's keys fetched again after a day (sooner for a kid they haven't)
60 constant keys-refetch-min-s     \ ... but not more than once a minute
s" oidc_flow" >string drop constant flow-cookie   \ the sign-in under way (state, verifier, nonce, next), signed
600 constant flow-s                           \ ... good for ten minutes
s" oidc_user" >string drop constant user-key  \ ... the user signed in (name, email, roles ...)
\ what a provider says, each config's own (by its discovery's URL: two sites of a node, two providers or tenants):
\ its discovery document and keys (as their JSON text, read as they are used), when the keys were fetched
TUPLE: provider-cache  doc keys keys-at ;
<map> constant caches
: cache ( cfg -- pc )
    {: c :}  [: c discovery>> string caches s-at 0= if
        0 0 0 <provider-cache> rescue-all  dup c discovery>> string caches s-at!  then ;] atomically ;

: oidc-fail ( a n -- )  true -rot (abort") ;
: fetch-text ( url n -- s )   \ a GET's body, JSON checked (an error if it isn't 200, or isn't JSON)
    {: ua un :}  ua un http-get {: res :}
    res res-status 200 <> if  res res-status ua un s" oidc: {s} answered {}" format oidc-fail  then
    res res-body json> drop  res res-body >string drop ;
: endpoint ( name n cfg -- a n )   \ the config's provider's discovery document's
    {: a n c :}  a n c cache doc>> string json> json-string@ 0= if
        a n s" oidc: the provider's discovery has no {s}" format oidc-fail  then ;
: discovered ( cfg -- )   \ the discovery document, fetched once (each node, each provider its own)
    {: c :}  c cache doc>> if  exit  then
    c discovery>> string fetch-text rescue-all  c cache swap >>doc drop ;
: keys ( fresh? cfg -- keys )   \ the provider's keys (a JWKS), fetched as they are old, or wanted afresh
    {: fresh? c | now old pc :}  time-now to now  c cache to pc
    pc keys>> 0=  now pc keys-at>> - keys-keep-s > or
    fresh?  now pc keys-at>> - keys-refetch-min-s > and  or if
        pc now >>keys-at drop   \ (first: another asking while this one waits fetches none of its own)
        s" jwks_uri" c endpoint fetch-text rescue-all {: new :}   \ (fetched before the old is let go)
        pc keys>> to old  pc new >>keys drop  old ?dup if  dispose  then
    then
    [: pc keys>> string >string ;] atomically json> ;   \ (a copy read: another may let these go meanwhile)
\ the issuer a token must name: the provider's - Entra's common and organizations give it with {tenantid} in it, the
\ token's own tenant (its tid) taken there (a token of any organization's, then: signed-in-user's tenant says which)
: issuer-for ( claims cfg -- s len )
    {: cl c :}  s" issuer" c endpoint {: ia in :}
    ia in s" {tenantid}" s-find {: at :}
    at 0< if  ia in >string exit  then
    s" tid" cl json-string@ 0= if  s" oidc: a token for a multi-tenant sign-in without its tenant (tid)" oidc-fail  then
    {: ta tn :}  ia at  ta tn  ia in  at 10 +  in  s-slice  s" {s}{s}{s}" format ;   \ (10: {tenantid}'s length)
: verified ( token n cfg -- claims )   \ RS256 by the provider's keys (fetched again once if its kid is new to them)
    {: ta tn c | claims :}
    [: ta tn false c keys jwt-verify to claims ;] catch if  ta tn true c keys jwt-verify to claims  then
    claims c issuer-for {: ia in :}
    claims  ia in  c client>> string  jwt-check  ia dispose
    claims ;

: random-b64 ( -- s len )  32 random-bytes {: s n :}  s n >base64url  s dispose ;
: challenge ( verifier n -- s len )  sha256 {: h hn :}  h hn >base64url  h dispose ;   \ PKCE's S256
: encoded ( a n -- s len )  url-encode ;
: query-of ( req -- m )   \ the request's query as a map
    {: r | m :}  <map> to m
    " =" {: eq :}
    r req-query >seq " &" split [: eq split1 {: name value :}
        value 0<>  name empty? 0= and if  value string url-decode drop  name string url-decode drop string m s-at!  then ;] each
    m ;
: param ( name n m -- a n )  json-string@ 0= if  s" "  then ;
: path-char? ( c -- flag )   \ what a path to come back to may hold: letters, digits, RFC 3986's - . _ ~ / ? # [ ] @
    {: c :}  c [char] a [char] z 1+ within  c [char] A [char] Z 1+ within or  c [char] 0 [char] 9 1+ within or
    c  s" -._~/?=&%+,;:@!$'()*" >seq  contains? or ;   \ ! $ & ' ( ) * + , ; = : % - none a header or the page's script reads
[: path-char? 0= ;] byte-class not-path
: safe-next ( a n -- a n )   \ where to go after: a path of this site only (not //elsewhere, not \evil, no CR LF)
    {: a n :}  n 0=  n 2000 > or  if  s" /" exit  then
    a n s" /" s-starts? 0=  a n s" //" s-starts? or  if  s" /" exit  then
    a n not-path s-marked n <  if  s" /" exit  then
    a n ;
\ the sign-in under way as a cookie: its JSON, signed, good till then - written once the provider sends them back
: flow-out ( m req cfg -- )
    {: m r c :}
    time-now flow-s + s" exp" m s-at!
    m >json {: j jn :}  j jn >base64url {: b bn :}  b bn c key>> string sign {: v vn :}
    flow-s s" HttpOnly; Secure; SameSite=Lax; Path=/auth/; Max-Age={}" format {: at an :}
    v vn flow-cookie string at an r set-cookie!
    j dispose  b dispose  v dispose  at dispose ;
\ the sign-in under way the cookie says, if it is this site's (signed by its key) and not too old; 0 if not
: flow-in ( req cfg -- m | 0 )
    {: r c :}
    flow-cookie string r req-cookie 0= if  0 exit  then  {: sa sn :}
    sa sn c key>> string unsign 0= if  0 exit  then
    base64> 0= if  0 exit  then  {: ja jn :}
    [: ja jn json> ;] catch if  0 exit  then  {: m :}
    m map? 0= if  0 exit  then
    s" exp" m json-int@ 0= if  0 exit  then  time-now < if  0 exit  then
    m ;

\ GET /auth/login?next=/path: a new sign-in - state, PKCE verifier, nonce kept in the session - and the user sent
\ to the provider
: login-with ( req cfg -- )
    {: r c | m :}
    c discovered
    random-b64 drop {: state :}  random-b64 drop {: verifier :}  random-b64 drop {: nonce :}
    s" next" r query-param if  safe-next  else  s" /"  then >string drop {: next :}
    <map> to m  state s" state" m s-at!  verifier s" verifier" m s-at!  nonce s" nonce" m s-at!  next s" next" m s-at!
    m r c flow-out   \ (a signed cookie: nothing written to the store till someone has signed in)
    [:  s" authorization_endpoint" c endpoint %s
        s" ?response_type=code&client_id=" %s  c client>> string encoded %s
        s" &redirect_uri=" %s  c redirect>> string encoded %s
        s" &scope=" %s  c scopes>> string encoded %s
        s" &state=" %s  state string %s  s" &nonce=" %s  nonce string %s
        s" &code_challenge=" %s  verifier string challenge %s  s" &code_challenge_method=S256" %s
        s" &response_mode=query" %s ;] build {: url :}
    url string 302 r redirect ;
\ the token endpoint's answer for the code: its ID token
: token-for ( code n verifier n cfg -- token n )
    {: ca cn va vn c :}
    [:  s" grant_type=authorization_code&code=" %s  ca cn encoded %s
        s" &redirect_uri=" %s  c redirect>> string encoded %s
        s" &client_id=" %s  c client>> string encoded %s
        s" &code_verifier=" %s  va vn %s
        c secret>> ?dup if  s" &client_secret=" %s  string encoded %s  then ;] build {: body :}
    body string s" application/x-www-form-urlencoded" s" token_endpoint" c endpoint http-post {: res :}
    res res-status 200 <> if  res res-status s" oidc: the token endpoint answered {}" format oidc-fail  then
    s" id_token" res res-body json> json-string@ 0= if  s" oidc: no id_token in the token endpoint's answer" oidc-fail  then ;
\ the user as the session keeps them: their id (oid, else sub), name, email, roles
: user-of ( claims -- m id n )
    {: cl | m :}  <map> to m
    s" oid" cl json-string@ 0= if  s" sub" cl json-string@ drop  then {: ia in :}
    ia in >string drop s" id" m s-at!
    s" name" cl json-string@ if  >string drop s" name" m s-at!  then
    s" preferred_username" cl json-string@ 0= if  s" email" cl json-string@ 0= if  s" "  then  then
    >string drop s" email" m s-at!
    s" roles" cl json-list@ 0= if  list  then  s" roles" m s-at!
    s" tid" cl json-string@ if  >string drop s" tenant" m s-at!  then   \ (its organization: Entra's tenant id)
    m ia in ;
\ GET /auth/callback?code&state: the state the session's, the code traded for an ID token, it checked (the nonce
\ ours), the user signed in (a new session id); then where they were going
: callback-with ( req cfg -- )
    {: r c | s flow q :}
    c discovered
    r c flow-in ?dup 0= if  s" no sign-in under way (or it is too old)" 400 r respond  exit  then  to flow
    flow-cookie string s" /auth/" r expire-cookie!
    r c store>> session to s
    r query-of to q
    s" error" q param nip if  s" error_description" q param  400 r respond  exit  then
    s" state" q param  s" state" flow param  s= 0= if  s" sign-in: the state isn't this session's" 400 r respond  exit  then
    0 {: claims :}
    [: s" code" q param  s" verifier" flow param  c token-for  c verified to claims ;] catch if   \ (a token not good:
        error-message s" sign-in refused: {s}" format 400 r respond  exit                    \ the user told, 400)
    then
    s" nonce" claims json-string@ 0= if  s" "  then  s" nonce" flow param s= 0= if
        s" sign-in: the token's nonce isn't this sign-in's" 400 r respond  exit
    then
    s" next" flow param >string drop {: next :}
    claims user-of {: user ia in :}
    ia in s log-in
    user user-key string s session!
    next string 302 r redirect ;
\ GET /auth/logout: the session ended, then the provider's own sign-out (or the site's top)
: logout-with ( req cfg -- )
    {: r c :}
    r c store>> session log-out
    c cache doc>> ?dup if  s" end_session_endpoint" rot string json> json-string@  else  false  then
    if  302 r redirect  else  s" /" 302 r redirect  then ;
: login ( req -- )  dup req-data login-with ;   \ (a route's handler: its config the route's data)
: callback ( req -- )  dup req-data callback-with ;
: logout ( req -- )  dup req-data logout-with ;
PRIVATE>

\ GET /auth/login, /auth/callback (the redirect URL the provider is told of), /auth/logout
: oidc-routes ( cfg router -- )
    {: c rt :}
    s" GET" s" /auth/login" ['] login c rt route-with
    s" GET" s" /auth/callback" ['] callback c rt route-with
    s" GET" s" /auth/logout" ['] logout c rt route-with ;

\ the user signed in (a map: id, name, email, roles), or 0
: signed-in-user ( req cfg -- user | 0 )
    {: r c :}  user-key string r c store>> session session@ 0= if  0  then ;
\ the user has the app role (Entra's roles claim: the roles the app gives, assigned to them)
: user-role? ( role n user -- flag )
    {: a n u :}  u 0= if  false exit  then
    s" roles" u json-list@ 0= if  false exit  then  [: {: x :}  x string? if  x string a n s=  else  false  then ;] any? ;
: has-role? ( role n req cfg -- flag )  signed-in-user user-role? ;

<PRIVATE
\ the sign-in's own routes (left open by the guards: else signing in would need signing in)
: auth-route? ( req -- flag )
    req-path {: a n :}  a n s" /auth/login" s=  a n s" /auth/callback" s= or  a n s" /auth/logout" s= or ;
: to-sign-in ( req -- )   \ a GET sent to sign in (coming back here after, its query too), anything else 401
    {: r :}
    r req-method s" GET" s= if
        r req-query nip if  r req-path r req-query s" {s}?{s}" format  else  r req-path  then
        url-encode {: p pn :}  p pn s" /auth/login?next={s}" format 302 r redirect
    else  s" sign in first" 401 r respond  then ;
: signed-in-check ( req cfg -- continue? )
    {: r c :}  r c signed-in-user if  true exit  then  r to-sign-in  false ;
TUPLE: role-need  cfg role ;
: role-check ( req need -- continue? )
    {: r nd :}  r auth-route? if  true exit  then
    r nd cfg>> signed-in-user {: u :}
    u 0= if  r to-sign-in  false exit  then
    nd role>> string u user-role? if  true exit  then
    s" this needs a role you haven't" 403 r respond  false ;
PRIVATE>

\ requests under the prefix: signed in, or (a GET) sent to sign in and back - the sign-in's own three routes left open
: require-signed-in ( prefix n cfg router -- )
    {: pa pn c rt :}
    pa pn [: {: r c :}  r auth-route? if  true  else  r c signed-in-check  then ;] c rt before-under-with ;
\ requests under the prefix: signed in, with the role (403 without it)
: require-role ( prefix n role n cfg router -- )
    {: pa pn ra rn c rt :}  c ra rn >string drop <role-need> rescue-all {: nd :}
    pa pn ['] role-check nd rt before-under-with ;

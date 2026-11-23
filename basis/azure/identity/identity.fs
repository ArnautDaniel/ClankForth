\ identity.fs - Azure's managed identities: an access token for a resource (SQL Database, Key Vault, Storage ...)
\ from the identity Azure gives the machine - no secret in the program. A VM's from its metadata endpoint (IMDS);
\ App Service's and Container Apps' from IDENTITY_ENDPOINT (with IDENTITY_HEADER). Each token kept till five
\ minutes before it expires. Asked inside an actor (it waits for the endpoint's answer).
\
\   ['] managed-identity-sql-token  config sql-token    \ SQL logins with the machine's identity
\   s" https://vault.azure.net" managed-identity-token ( -- token n )
IN: azure.identity
USING: actors http.client json system urls.encoding ;

\ the VM's metadata endpoint (a test's stand-in may be put here)
s" http://169.254.169.254/metadata/identity/oauth2/token" >string drop value imds-url
variable identity-client-id   \ a user-assigned identity's client id (a string), or 0: the system-assigned one
300 value token-early-s       \ a token got again this long before it expires

<PRIVATE
<map> constant tokens     \ resource -> its token (a string)
<map> constant expiries   \ resource -> when it expires (seconds since 1970)
: fail ( a n -- )  true -rot (abort") ;
: env ( name n -- a n | 0 0 )  getenv 0= if  0 0  then ;
\ where to ask, and the header that says it is the machine asking
: request ( resource n -- url n headers n )
    {: ra rn :}
    s" IDENTITY_ENDPOINT" env {: ea en :}
    [:  en if  ea en %s  s" ?api-version=2019-08-01"  else  imds-url string %s  s" ?api-version=2018-02-01"  then  %s
        s" &resource=" %s  ra rn url-encode %s
        identity-client-id @ ?dup if  s" &client_id=" %s  string url-encode %s  then ;] build string
    en if  s" IDENTITY_HEADER" env s\" X-IDENTITY-HEADER: {s}\r\n" format  else  s\" Metadata: true\r\n"  then ;
\ expires_on: a number, or its decimal text (IMDS gives text)
: expires-of ( v -- s )
    {: v :}  s" expires_on" v s-at 0= if  time-now 3600 + exit  then {: x :}
    x string? if  x string string>number 0= if  drop time-now 3600 +  then  else  x  then ;
: fetch ( resource n -- )   \ a new token, kept
    {: ra rn :}
    ra rn request {: ua un ha hn :}
    s" GET" ua un ha hn s" " http-request {: res :}
    res res-status 200 <> if  res res-status res res-error  s" azure.identity: the identity endpoint answered {} {s}" format fail  then
    res res-body json> {: v :}
    s" access_token" v json-string@ 0= if  s" azure.identity: no access_token in the answer" fail  then
    >string drop rescue-all  ra rn tokens s-at if  dispose  then  ra rn tokens s-at!
    v expires-of ra rn expiries s-at! ;
PRIVATE>

\ an access token for the resource ("https://database.windows.net/" ...), from the machine's managed identity - kept
\ and given again till token-early-s before it expires. Inside an actor.
: managed-identity-token ( resource n -- token n )
    {: ra rn :}
    ra rn expiries s-at if  time-now token-early-s + <  else  true  then  if
        ra rn fetch   \ (the old one let go once the new one is kept: a fetch that fails leaves it, not a dead one)
    then
    ra rn tokens s-at drop string ;
\ a token for Azure SQL Database (TDS's FEDAUTH: sql's sql-token)
: managed-identity-sql-token ( -- token n )  s" https://database.windows.net/" managed-identity-token ;

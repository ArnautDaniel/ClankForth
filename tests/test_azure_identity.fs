\ azure.identity (basis/azure/identity): a managed identity's token from a stand-in for Azure's metadata endpoint -
\ asked with its header, kept till near its end; and SQL logins made with it (TDS's FEDAUTH) - no user nor password
IN: test-azure-identity
USING: tester actors net json system http sql.tds sql azure.identity ;
source-dir s" /sql/stand-in.fs" s+ included

\ ---- the metadata endpoint: a token, and how many times it was asked ----
variable asked-n   variable header-ok
<router> constant imds
: token-answer ( req -- )
    {: r :}  1 asked-n +!
    s" metadata" r req-header if  s" true" s= header-ok !  then
    s" resource" r query-param drop  s" https://database.windows.net/" s= 0= if  s" {}" 400 r respond-json exit  then
    time-now 3600 + s\" {{\"access_token\":\"eyJ.token.from-imds\",\"expires_on\":\"{}\",\"token_type\":\"Bearer\"}}" format
    200 r respond-json ;
s" /metadata/identity/oauth2/token" ' token-answer imds on-get
imds 0 http-listen listener-port constant imds-port
imds-port s" http://127.0.0.1:{}/metadata/identity/oauth2/token" format drop to imds-url

testing a token: asked of the endpoint (Metadata: true), then kept - asked once for two uses
create got 128 allot   variable got-n
: keep ( a n -- 0 )  dup got-n !  got swap move  0 ;
T{ [: managed-identity-sql-token keep ;] run  got got-n @ s" eyJ.token.from-imds" s=  header-ok @ -> 0 true true }T
T{ [: managed-identity-sql-token keep ;] run  asked-n @ -> 0 1 }T
testing another identity (a user-assigned one's client id): its own token, asked for - not the other's kept
s" client-b" >string drop identity-client-id !
T{ [: managed-identity-sql-token keep ;] run  asked-n @ -> 0 2 }T
T{ [: managed-identity-sql-token keep ;] run  asked-n @ -> 0 2 }T
0 identity-client-id !
T{ [: managed-identity-sql-token keep ;] run  asked-n @ -> 0 2 }T   \ (the machine's own: still kept)

testing a SQL login with it (FEDAUTH): the token the server got, no user nor password
: token-config ( -- c )
    s" 127.0.0.1" s" " s" " s" stagingdb" sql-config sql-plain  stand-in-port @ swap sql-port
    ['] managed-identity-sql-token swap sql-token ;
token-config 1 sql-pool constant db
T{ [: s" SELECT 42 AS answer" 0 db sql-query length ;] run -> 1 }T
T{ login-token login-token-n @ s" eyJ.token.from-imds" s= -> true }T
testing a token due again whose endpoint fails: an error - and the endpoint back, a token again (not a dead one)
imds-url constant working-url
100000 to token-early-s   \ (the token kept now near its end: asked again)
s" http://127.0.0.1:1/none" >string drop to imds-url
T{ [: managed-identity-sql-token keep ;] run -> -1 }T
working-url to imds-url
T{ [: managed-identity-sql-token keep ;] run  got got-n @ s" eyJ.token.from-imds" s= -> 0 true }T
T{ [: managed-identity-sql-token keep ;] run -> 0 }T
300 to token-early-s
testing an endpoint that can't give one: the asker told why, no login tried
s" http://127.0.0.1:1/none" >string drop to imds-url
: fresh-pool ( -- db )   \ (another resource's token: none kept for it)
    s" 127.0.0.1" s" " s" " s" stagingdb" sql-config sql-plain  stand-in-port @ swap sql-port
    [: s" https://other.example/" managed-identity-token ;] swap sql-token  1 sql-pool ;
fresh-pool constant failing
T{ [: s" SELECT 1" 0 failing sql-query ;] run  s" sql: no Entra token" failed-with? -> -1 true }T

test-summary

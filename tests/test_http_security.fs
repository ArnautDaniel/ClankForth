\ http's hooks (before-each, before-under, after-each), client-address; http.security (security headers, CORS);
\ http.limits (rate limits)
IN: test-http-security
USING: tester actors net system http http.security http.limits vocabs.webclient ;

: answer ( a n req -- )  200 swap respond ;
: listen-on ( router -- )  0 http-listen listener-port web-port ! ;

\ ---- hooks ----
<router> constant hooked
variable handled   variable afters   variable last-status
: page ( req -- )  {: r :}  1 handled +!  s" page" r answer ;
: tag ( req -- continue? )  {: r :}  s" yes" s" X-Hooked" r header!  true ;
: stop-secret ( req -- continue? )  {: r :}  s" no" 403 r respond  false ;
: count-after ( req -- )  1 afters +!  req-status last-status ! ;
: addr ( req -- )  {: r :}  r client-address r answer ;
s" /" ' page hooked on-get
s" /admin" ' page hooked on-get
s" /admin/users" ' page hooked on-get
s" /administrator" ' page hooked on-get
s" /addr" ' addr hooked on-get
' tag hooked before-each
s" /admin" ' stop-secret hooked before-under
' count-after hooked after-each
hooked listen-on

testing before-each: every request through it (a header on every answer)
T{ 0 handled !  s" /" get  2dup body-of s" page" s=  -rot s" X-Hooked: yes" has?  handled @ -> true true 1 }T
testing before-under: only the paths under its prefix; one giving false has answered, no handler run
T{ 0 handled !  s" /admin" get status-of  handled @ -> 403 0 }T
T{ 0 handled !  s" /admin/users" get status-of  handled @ -> 403 0 }T
T{ 0 handled !  s" /administrator" get status-of  handled @ -> 200 1 }T
T{ s" /admin" get  s" X-Hooked: yes" has? -> true }T   \ (the befores before it ran)
testing after-each: after every handler, the answer given
T{ 0 afters !  s" /" get drop drop  s" /admin" get drop drop  afters @ last-status @ -> 2 403 }T
testing client-address: the connection's other end; behind a proxy trusted, the last of X-Forwarded-For (the proxy's)
T{ s" /addr" get body-of s" 127.0.0.1" s= -> true }T
T{ s" /addr" s\" X-Forwarded-For: 203.0.113.9, 10.0.0.1\r\n" get-with body-of s" 127.0.0.1" s= -> true }T
true to http-trust-proxy
T{ s" /addr" s\" X-Forwarded-For: 203.0.113.9, 10.0.0.1\r\n" get-with body-of s" 10.0.0.1" s= -> true }T   \ (the proxy's: the last)
T{ s" /addr" s\" X-Forwarded-For: 198.51.100.7\r\n" get-with body-of s" 198.51.100.7" s= -> true }T
false to http-trust-proxy

\ ---- security headers ----
<router> constant guarded
s" /" ' page guarded on-get
guarded security-headers
guarded listen-on
testing security-headers: nosniff, no framing, a referrer policy, a CSP of its own site's; no HSTS
T{ s" /" get  2dup s" X-Content-Type-Options: nosniff" has?  -rot s" X-Frame-Options: DENY" has? -> true true }T
T{ s" /" get  s" Content-Security-Policy: default-src 'self'; frame-ancestors 'none'" has? -> true }T
T{ s" /" get  s" Referrer-Policy: strict-origin-when-cross-origin" has? -> true }T
T{ s" /" get  s" Strict-Transport-Security" has? -> false }T
T{ s" /nowhere" get  s" X-Frame-Options: DENY" has? -> true }T   \ (a 404 too)
<router> constant strict
s" /" ' page strict on-get
s" " 31536000 strict security-headers-with
strict listen-on
testing security-headers-with: no CSP, HSTS a year
T{ s" /" get  2dup s" Content-Security-Policy" has?  -rot s" Strict-Transport-Security: max-age=31536000; includeSubDomains" has? -> false true }T

\ ---- CORS ----
<router> constant shared
: data ( req -- )  {: r :}  1 handled +!  s" data" r answer ;
s" /api" ' data shared on-any
s" http://app.test" shared allow-origin
shared listen-on
testing allow-origin: its origin told it may read (with cookies); another not; a preflight answered, no handler
T{ s" /api" s\" Origin: http://app.test\r\n" get-with  2dup s" Access-Control-Allow-Origin: http://app.test" has?
   -rot s" Access-Control-Allow-Credentials: true" has? -> true true }T
T{ s" /api" s\" Origin: http://evil.test\r\n" get-with  s" Access-Control-Allow-Origin" has? -> false }T
T{ s" /api" get  s" Access-Control-Allow-Origin" has? -> false }T
T{ 0 handled !
   s\" OPTIONS /api HTTP/1.1\r\nHost: t\r\nConnection: close\r\nOrigin: http://app.test\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type\r\n\r\n" fetch
   2dup status-of  -rot s" Access-Control-Allow-Headers: content-type" has?  handled @ -> 204 true 0 }T
T{ s\" OPTIONS /api HTTP/1.1\r\nHost: t\r\nConnection: close\r\nOrigin: http://app.test\r\nAccess-Control-Request-Method: PUT\r\n\r\n" fetch
   s" Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE" has? -> true }T

\ ---- rate limits ----
<router> constant limited
s" /login" ' page limited on-any
s" /" ' page limited on-get
s" /login" 3 60 limited rate-limit-under
limited listen-on
testing rate-limit-under: so many from an address, then 429 with Retry-After; other paths not counted
T{ s" /login" get status-of  s" /login" get status-of  s" /login" get status-of -> 200 200 200 }T
T{ s" /login" get  2dup status-of  -rot s" Retry-After: " has? -> 429 true }T
T{ s" /" get status-of -> 200 }T
testing rate-ok?: each key its own count; its window over, counted afresh
2 1 <rate-limit> constant quick
T{ s" a" quick rate-ok?  s" a" quick rate-ok?  s" a" quick rate-ok?  s" b" quick rate-ok? -> true true false true }T
T{ s" a" quick retry-after 0> -> true }T
T{ 1100 sleep-ms  s" a" quick rate-ok? -> true }T

test-summary

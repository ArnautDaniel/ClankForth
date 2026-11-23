\ security.fs - what every answer of a site should say to the browser (security headers), and which other sites'
\ pages may call it (CORS) - each a hook before every handler (http's before).
\   site security-headers                           \ nosniff, no framing, a referrer policy, a CSP
\   s" http://localhost:5173" site allow-origin     \ a front end served from elsewhere (its dev server) may call it
IN: http.security
USING: http accessors ;

\ the headers an answer gets: its Content-Security-Policy ("" none) and Strict-Transport-Security's max-age (0 none)
TUPLE: security-policy  csp hsts ;

<PRIVATE
: add-headers ( req policy -- true )
    {: r p :}
    s" nosniff" s" X-Content-Type-Options" r header!
    s" DENY" s" X-Frame-Options" r header!
    s" strict-origin-when-cross-origin" s" Referrer-Policy" r header!
    s" same-origin" s" Cross-Origin-Opener-Policy" r header!
    p csp>> string nip if  p csp>> string s" Content-Security-Policy" r header!  then
    p hsts>> if  p hsts>> s" max-age={}; includeSubDomains" format {: s n :}  s n s" Strict-Transport-Security" r header!
        s dispose  then
    true ;
PRIVATE>

\ every answer with these headers: its Content-Security-Policy (the page's scripts, styles, frames ...) and HSTS's
\ seconds (the browser to use HTTPS only, that long: 0 for none - a site on HTTPS gives a year, 31536000)
: security-headers-with ( csp n hsts router -- )
    {: ca cn hsts rt :}  ['] add-headers  ca cn >string drop hsts <security-policy> rescue-all  rt before-each-with ;
\ every answer with X-Content-Type-Options nosniff, X-Frame-Options DENY, a Referrer-Policy, Cross-Origin-Opener-Policy
\ and a Content-Security-Policy that lets the page load from its own site only - no HSTS
: security-headers ( router -- )
    {: rt :}  s" default-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'" 0 rt
    security-headers-with ;

<PRIVATE
: method-is ( a n req -- flag )  req-method s= ;
<map> constant cors-sites   \ a router (its address, as text) -> the origins it allows (one hook each router)
: allows? ( a n origins -- flag )  {: a n os :}  os [: string a n s= ;] any? ;
\ a request from an allowed origin: told it may read the answer (with its cookies, but for "*"'s); a preflight
\ (OPTIONS asking which methods) answered here. An origin named wins over "*"; each header once
: cors ( req origins -- continue? )
    {: r os :}
    s" origin" r req-header 0= if  true exit  then  {: ra rn :}
    s" Origin" s" Vary" r header!   \ (allowed or not: a cache keeps the answers apart by it)
    ra rn os allows? {: named :}
    named 0=  s" *" os allows? 0= and if  true exit  then
    ra rn s" Access-Control-Allow-Origin" r header!
    named if  s" true" s" Access-Control-Allow-Credentials" r header!  then
    s" OPTIONS" r method-is  s" access-control-request-method" r req-header dup if  nip nip  then  and 0= if  true exit  then
    s" GET, POST, PUT, PATCH, DELETE" s" Access-Control-Allow-Methods" r header!
    s" access-control-request-headers" r req-header 0= if  s" Content-Type, X-CSRF-Token"  then
    s" Access-Control-Allow-Headers" r header!
    s" 600" s" Access-Control-Max-Age" r header!
    s" " 204 r respond  false ;
PRIVATE>

\ pages of that origin ("https://app.example.org", "*" any - not with cookies) may call the site from a script and
\ read its answers, with their cookies; its preflights answered. Once for each origin allowed (one hook does them all).
: allow-origin ( origin n router -- )
    {: oa on rt :}  rt s" {}" format {: ka kn :}
    ka kn cors-sites s-at 0= if
        list rescue-all  dup ka kn cors-sites s-at!  ['] cors swap rt before-each-with
        ka kn cors-sites s-at drop
    then {: os :}
    oa on >string drop rescue-all os push  ka dispose ;

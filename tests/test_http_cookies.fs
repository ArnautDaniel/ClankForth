\ http.cookies (basis/http/cookies): the cookies a request sends, the ones an answer sets
IN: test-http-cookies
USING: tester actors net http http.cookies vocabs.webclient ;

<router> constant site
: show-cookie ( req -- )  {: r | a n :}
    s" session" r req-cookie if  to n to a  else  s" none" to n to a  then  a n 200 r respond ;
: set-one ( req -- )  {: r :}  s" abc123" s" session" s" Path=/; HttpOnly" r set-cookie!
    s" 1" s" plain" s" " r set-cookie!  s" ok" 200 r respond ;
: forget ( req -- )  {: r :}  s" session" s" /" r expire-cookie!  s" ok" 200 r respond ;
: bad-value ( req -- )  {: r :}  [: s" a b" s" x" s" " r set-cookie! ;] catch 0<> if  s" refused" else s" set" then
    200 r respond ;
s" /show" ' show-cookie site on-get
s" /set" ' set-one site on-get
s" /forget" ' forget site on-get
s" /bad" ' bad-value site on-get
site 0 http-listen listener-port web-port !

testing req-cookie: the one of that name among them, its quotes off; none
T{ s" /show" s\" Cookie: theme=dark; session=xyz; n=1\r\n" get-with body-of s" xyz" s= -> true }T
T{ s" /show" s\" Cookie: session=\"q v\"\r\n" get-with body-of s" q v" s= -> true }T
T{ s" /show" s\" Cookie: sessions=no\r\n" get-with body-of s" none" s= -> true }T
T{ s" /show" get body-of s" none" s= -> true }T
T{ s" /show" s\" Cookie: theme=dark\r\nCookie: session=two\r\n" get-with body-of s" two" s= -> true }T   \ (two Cookie headers: joined by ", ")
testing set-cookie!: each its own Set-Cookie, attributes after; expire-cookie!
T{ s" /set" get  2dup s" Set-Cookie: session=abc123; Path=/; HttpOnly" has?  -rot s\" Set-Cookie: plain=1\r\n" has? -> true true }T
T{ s" /forget" get  s" Set-Cookie: session=; Max-Age=0; Path=/" has? -> true }T
testing a value a cookie can't hold: refused
T{ s" /bad" get body-of s" refused" s= -> true }T

test-summary

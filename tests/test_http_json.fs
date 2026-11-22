\ http.json (basis/http/json): a body read as JSON, a value sent as one, an error said as one
IN: test-http-json
USING: tester actors net http json http.json vocabs.webclient ;

<router> constant site
: make ( req -- )
    {: r :}  r req-json 0= if  s" not JSON" 400 r respond-error exit  then  {: v :}
    s" title" v json-string@ 0= if  s" a title, please" 422 r respond-error exit  then  {: ta tn :}
    <map> {: m :}  ta tn >string drop s" made" m s-at!  41 1+ s" id" m s-at!  m 201 r respond-value ;
s" /posts" ' make site on-post
site 0 http-listen listener-port web-port !
: post ( body n -- a n )
    {: ba bn :}  bn ba bn
    s\" POST /posts HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{s}" format
    {: q qn :}  q qn fetch  q dispose ;

testing a JSON body read; a value answered as JSON
T{ s\" {\"title\":\"hi\"}" post  2dup status-of  -rot body-of s\" {\"made\":\"hi\",\"id\":42}" s= -> 201 true }T
T{ s\" {\"title\":\"hi\"}" post  s" Content-Type: application/json" has? -> true }T
testing what isn't taken: no JSON (400), no title (422), each said as {"error": ...}
T{ s" {nope" post  2dup status-of  -rot body-of s\" {\"error\":\"not JSON\"}" s= -> 400 true }T
T{ s\" {\"name\":1}" post  2dup status-of  -rot body-of s\" {\"error\":\"a title, please\"}" s= -> 422 true }T
T{ s" " post status-of -> 400 }T

test-summary

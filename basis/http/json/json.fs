\ json.fs - a JSON API's requests and answers: a body read as JSON, a value sent as one, an error said as one.
\   : make-post ( req -- )  {: r :}  r req-json 0= if  s" not JSON" 400 r respond-error exit  then  {: v :}
\       s" title" v json-string@ 0= if  s" a title, please" 422 r respond-error exit  then  ... ;
IN: http.json
USING: http json ;

\ the request's body read as JSON (false: none, or not JSON - an error the client's)
: req-json ( req -- v true | false )
    {: r | v :}
    r req-body nip 0= if  false exit  then
    [: r req-body json> to v ;] catch if  false exit  then
    v true ;
\ the value as the answer's JSON body
: respond-value ( v status req -- )  {: v status r :}  v >json status r respond-json ;
\ an error as the answer: {"error": the text}, with that status
: respond-error ( a n status req -- )
    {: a n status r | m :}  <map> to m  a n >string drop s" error" m s-at!  m status r respond-value ;

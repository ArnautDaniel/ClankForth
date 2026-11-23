\ uploads.fs - a form's files and fields as a browser sends them with enctype="multipart/form-data" (RFC 7578): each
\ part's name, its file's name (as the browser gave it), its type and its bytes.
\   s" avatar" r req-part if  {: p :}  p part-data ( a n )  p part-type ...  then
\ The body is read whole first (http-body-max: 1 MiB - raise it for a site that takes files).
IN: http.uploads
USING: http accessors ;

\ a part: its field's name, its file's name (0: a plain field), its Content-Type, its bytes (strings)
TUPLE: upload  name filename type data ;

: part-name ( part -- a n )  name>> string ;
: part-data ( part -- a n )  data>> string ;
: part-type ( part -- a n )  type>> string ;
\ the file's name as the browser gave it - for showing only: never a path to write to (name the file yourself)
: part-filename ( part -- a n true | false )  filename>> ?dup if  string true  else  false  then ;

<PRIVATE
70 constant boundary-max
s" parts" >string drop constant parts-key

: unquote ( a n -- a' n' )   \ a value in double quotes without them
    {: a n :}  n 2 >=  a c@ [char] " = and  a n + 1- c@ [char] " = and if  a 1+ n 2 -  else  a n  then ;
\ the value of a parameter of a header's value (`name=...` in `form-data; name="x"; filename="a.png"`), unquoted
: params-split ( a n -- l )   \ split at each ; not inside double quotes (filename="a;b.txt" one)
    {: a n | l at quoted :}  list to l
    n 0 ?do
        a i + c@ {: c :}
        c [char] " = if  quoted 0= to quoted  then
        c [char] ; =  quoted 0= and if  a at + i at - >string drop l push  i 1+ to at  then
    loop
    a at + n at - >string drop l push  l ;
: param ( key n value n -- a n true | false )
    {: ka kn va vn | l found fa fn :}
    va vn params-split to l
    l length 1 ?do
        found 0= if
            i l nth string s-trim {: pa pn :}
            pa pn s" =" s-find {: eq :}
            eq 0> if  pa eq s-trim s-lower ka kn s= if  pa eq 1+ + pn eq - 1- s-trim unquote to fn to fa  true to found  then  then
        then
    loop
    found if  fa fn true  else  false  then ;
\ the boundary the Content-Type names (false: not a multipart body)
: boundary ( req -- a n true | false )
    {: r :}
    s" content-type" r req-header 0= if  false exit  then  {: ca cn :}
    ca cn s-lower s" multipart/form-data" s-starts? 0= if  false exit  then
    s" boundary" ca cn param 0= if  false exit  then  {: ba bn :}
    bn 1 boundary-max 1+ within 0= if  false exit  then
    ba bn true ;
\ a part's head (its lines) read: its name, file name and type
: head-of ( a n -- name n file|0 type n true | false )
    {: a n | l dname dn file type tn :}
    s" text/plain" to tn to type
    a n s\" \r\n" s-split to l
    l length 0 ?do
        i l nth string {: la ln :}
        la ln s" :" s-find {: colon :}
        colon 0> if
            la colon s-trim s-lower {: ka kn :}
            la colon 1+ + ln colon - 1- s-trim {: va vn :}
            ka kn s" content-disposition" s= if
                s" name" va vn param if  to dn to dname  then
                s" filename" va vn param if  >string drop to file  then
            then
            ka kn s" content-type" s= if  va vn to tn to type  then
        then
    loop
    dname 0= if  false exit  then
    dname dn file type tn true ;
\ the body's parts (a list of upload); false if it isn't a multipart body of the boundary, whole
: parse ( body n boundary n -- l true | false )
    {: a n ba bn | l delim dn p :}
    list to l
    ba bn s" --{s}" format to dn to delim
    a n delim dn s-find to p
    p 0<  if  false exit  then
    p 0>  if  a p + 2 - 2 s\" \r\n" s= 0= if  false exit  then  then
    begin
        p dn + to p
        p 2 + n > if  false exit  then
        a p + 2 s" --" s= if  l true exit  then
        a p + 2 s\" \r\n" s= 0= if  false exit  then
        p 2 + to p
        a p + n p - s\" \r\n\r\n" s-find {: he :}
        he 0< if  false exit  then
        a p + he 4 + +  n p - he - 4 -  {: rest rn :}
        rest rn s\" \r\n" delim dn s+ s-find {: nx :}
        nx 0< if  false exit  then
        a p + he head-of 0= if  false exit  then  {: na nn file ta tn :}
        na nn >string drop  file  ta tn >string drop  rest nx >string drop  <upload>  l push
        rest nx + 2 + a - to p   \ (at the next delimiter)
    again ;
PRIVATE>

\ the request's parts (a list of upload: read once, kept for the request); false if its body isn't
\ multipart/form-data, or isn't whole
: req-parts ( req -- l true | false )
    {: r :}
    parts-key string r req-attr if  dup if  true  else  drop false  then  exit  then
    r boundary 0= if  0 parts-key string r req-attr!  false exit  then  {: ba bn :}
    r req-body ba bn parse 0= if  0 parts-key string r req-attr!  false exit  then
    dup parts-key string r req-attr!  true ;
\ its part of that name (the first)
: req-part ( name n req -- part true | false )
    {: na nn r | found :}
    r req-parts 0= if  false exit  then
    [: {: p :}  found 0=  p part-name na nn s= and if  p to found  then ;] each
    found if  found true  else  false  then ;
\ a plain field's value (a multipart form's form-param)
: part-text ( name n req -- a n true | false )  req-part if  part-data true  else  false  then ;

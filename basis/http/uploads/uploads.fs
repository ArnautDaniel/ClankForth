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
s\" \r\n" >string drop permanent constant crlf
s\" \r\n\r\n" >string drop permanent constant blank-line
s" --" >string drop permanent constant dashes

: unquote ( a n -- a' n' )   \ a value in double quotes without them
    {: a n :}  n 2 >=  a n s\" \"" s-starts? and  a n s\" \"" s-ends? and  if  a n 1 n 2 - s-slice  else  a n  then ;
\ the value of a parameter of a header's value (`name=...` in `form-data; name="x"; filename="a.png"`), unquoted
: params-split ( a n -- l )   \ cut at each ; not inside double quotes (filename="a;b.txt" one): a list of slices
    >seq {: in | l at quoted :}  list to l
    in [: {: c i :}
        c [char] " = if  quoted 0= to quoted  then
        c [char] ; =  quoted 0= and if  at i in <slice> l push  i 1+ to at  then ;] each-index
    at in length in <slice> l push  l ;
: param ( key n value n -- a n true | false )
    {: ka kn va vn | found fa fn :}
    va vn params-split [: {: p i :}   \ (the first is the value itself: form-data)
        found 0=  i 0> and if
            p " =" split1 {: name value :}
            value 0<>  name empty? 0= and if
                name trim-slice string s-lower ka kn s= if  value trim-slice string unquote to fn to fa  true to found  then
            then
        then ;] each-index
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
    {: a n | dname dn file type tn :}
    s" text/plain" to tn to type
    a n >seq crlf split [: " :" split1 {: key value :}
        value 0<>  key empty? 0= and if
            key trim-slice string s-lower {: ka kn :}
            value trim-slice string {: va vn :}
            ka kn s" content-disposition" s= if
                s" name" va vn param if  to dn to dname  then
                s" filename" va vn param if  >string drop to file  then
            then
            ka kn s" content-type" s= if  va vn to tn to type  then
        then ;] each
    dname 0= if  false exit  then
    dname dn file type tn true ;
\ the body's parts (a list of upload); false if it isn't a multipart body of the boundary, whole
: parse ( body n boundary n -- l true | false )
    {: a n ba bn | in l delim dn ends p :}
    a n >seq to in  list to l
    ba bn s" --{s}" format to dn to delim
    delim dn s\" \r\n{s}" format drop to ends   \ (CR LF and the delimiter: where a part's data ends)
    in delim subseq-index to p
    p 0<  if  false exit  then
    p 1 =  if  false exit  then   \ (one byte before the first delimiter: no CR LF there - and none before the body)
    p 0>  if  in crlf p 2 - seq-at? 0= if  false exit  then  then
    begin
        p dn + to p
        p 2 + n > if  false exit  then
        in dashes p seq-at? if  l true exit  then
        in crlf p seq-at? 0= if  false exit  then
        p 2 + to p
        in blank-line p subseq-index-from {: he :}   \ (the part's head ends; its data begins after the blank line)
        he 0< if  false exit  then
        in ends he 4 + subseq-index-from {: nx :}
        nx 0< if  false exit  then
        p he in <slice> string head-of 0= if  false exit  then  {: na nn file ta tn :}
        na nn >string drop  file  ta tn >string drop  in he 4 + nx subseq  <upload>  l push
        nx 2 + to p   \ (at the next delimiter)
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

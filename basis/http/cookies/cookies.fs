\ cookies.fs - cookies (RFC 6265): the ones a request sends, and the ones an answer sets.
\   s" session" r req-cookie ( -- s len true | false )
\   s" abc123" s" session" s" Path=/; HttpOnly; SameSite=Lax" r set-cookie!
IN: http.cookies
USING: http ;

<PRIVATE
: cookie-octet? ( c -- flag )   \ what a cookie's value may hold: no space, " , ; \ or control characters
    {: c :}  c 33 =  c 35 44 within or  c 45 59 within or  c 60 92 within or  c 93 127 within or ;
: separator? ( c -- flag )   \ what a token (a cookie's name) can't hold
    s\" ()<>@,;:\\\"/[]?={}" >seq contains? ;
: token-char? ( c -- flag )  {: c :}  c 33 127 within  c separator? 0= and ;
\ the bytes a value, a name, may not hold - a class each (s-marked finds the first of a text's that is one)
[: cookie-octet? 0= ;] byte-class not-octet   [: token-char? 0= ;] byte-class not-token
: octets? ( a n -- flag )  {: a n :}  a n not-octet s-marked n = ;
: name? ( a n -- flag )  {: a n :}  n 0>  a n not-token s-marked n =  and ;
: unquoted ( a n -- a' n' )   \ a value in double quotes without them
    {: a n :}  n 2 >=  a n s\" \"" s-starts? and  a n s\" \"" s-ends? and  if  a n 1 n 2 - s-slice  else  a n  then ;
PRIVATE>

\ the value of the cookie of that name the request sent, as sent (its quotes taken off): a new string
: req-cookie ( name n req -- s len true | false )
    {: a n r | found :}
    s" cookie" r req-header 0= if  false exit  then  {: ha hn :}
    [: ha hn >seq " ," " ;" replace " ;" split   \ (several Cookie headers come joined by ", ": split there too)
       [: trim-slice " =" split1 {: name value :}
          found 0=  value 0<> and if
              name empty? 0=  name string a n s=  and if  value string unquoted >string drop rescue to found  then
          then ;] each ;] with-scratch
    found if  found string true  else  false  then ;

\ a cookie set by the answer: Set-Cookie name=value, and its attributes after ("" none; "Path=/; HttpOnly ...")
: set-cookie! ( value n name n attrs n req -- )
    {: v vn k kn at an r :}
    k kn name? 0= abort" set-cookie!: a cookie's name is a token: no spaces, no ( ) < > @ , ; : \ / [ ] ? = { }"
    v vn octets? 0= abort" set-cookie!: a cookie's value has no spaces, quotes, commas, semicolons or backslashes"
    an if  k kn v vn at an s" {s}={s}; {s}" format  else  k kn v vn s" {s}={s}" format  then {: s sn :}
    s sn s" Set-Cookie" r header!  s dispose ;

\ the browser told to forget the cookie (its path, "/" for most)
: expire-cookie! ( name n path n req -- )
    {: k kn p pn r :}  p pn s" Max-Age=0; Path={s}" format {: at an :}  s" " k kn at an r set-cookie!  at dispose ;

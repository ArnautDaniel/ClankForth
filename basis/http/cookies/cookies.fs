\ cookies.fs - cookies (RFC 6265): the ones a request sends, and the ones an answer sets.
\   s" session" r req-cookie ( -- s len true | false )
\   s" abc123" s" session" s" Path=/; HttpOnly; SameSite=Lax" r set-cookie!
IN: http.cookies
USING: http ;

<PRIVATE
: cookie-octet? ( c -- flag )   \ what a cookie's value may hold: no space, " , ; \ or control characters
    {: c :}  c 33 =  c 35 44 within or  c 45 59 within or  c 60 92 within or  c 93 127 within or ;
: separator? ( c -- flag )   \ what a token (a cookie's name) can't hold
    {: c :}  s\" ()<>@,;:\\\"/[]?={}" {: a n :}
    n 0 ?do  a i + c@ c = if  true unloop exit  then  loop  false ;
: octets? ( a n -- flag )  {: a n :}  n 0 ?do  a i + c@ cookie-octet? 0= if  false unloop exit  then  loop  true ;
: name? ( a n -- flag )
    {: a n :}  n 0= if  false exit  then
    n 0 ?do
        a i + c@ {: c :}
        c 33 127 within 0=  c separator? or if  false unloop exit  then
    loop  true ;
: unquoted ( a n -- a' n' )   \ a value in double quotes without them
    {: a n :}  n 2 >=  a c@ [char] " = and  a n + 1- c@ [char] " = and if  a 1+ n 2 -  else  a n  then ;
PRIVATE>

\ the value of the cookie of that name the request sent, as sent (its quotes taken off): a new string
: req-cookie ( name n req -- s len true | false )
    {: a n r | parts found :}
    s" cookie" r req-header 0= if  false exit  then   \ (several Cookie headers come joined by ", ": split there too)
    {: ha hn :}  [: hn 0 ?do  ha i + c@ dup [char] , = if  drop [char] ;  then  %c  loop ;] build string
    {: ca cn :}  ca cn s" ;" s-split to parts  ca dispose
    parts length 0 ?do
        found 0= if
            i parts nth string s-trim {: pa pn :}
            pa pn s" =" s-find {: eq :}
            eq 0> if  pa eq a n s= if
                pa pn eq 1+ pn eq - 1- s-slice unquoted >string drop to found
            then then
        then
    loop
    parts [: dispose ;] each  parts list-free
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

\ http.sessions (basis/http/sessions): a session's cookie, what it holds kept in a jsondb, a new id on signing in,
\ none after signing out, its CSRF token; one ended, gone
IN: test-http-sessions
USING: tester actors accessors net system json jsondb crypto base64 http http.cookies http.sessions vocabs.webclient ;

: wipe ( dir n -- )
    {: a n :}  a n dir? 0= if  exit  then
    a n dir-list {: l :}  l length 0 ?do  a n s" /" s+  i l nth string s+  delete-file  loop  a n delete-file ;
s" jsondb-sessions-data" wipe
s" jsondb-sessions-data" db-open constant db
db <session-store> constant sessions

<router> constant site
: answer ( a n req -- )  200 swap respond ;
: put-v ( req -- )  {: r :}  s" v" r query-param drop  s" v" r sessions session session-string!  s" ok" r answer ;
: get-v ( req -- )  {: r :}  s" v" r sessions session session@ if  string  else  s" none"  then  r answer ;
: login ( req -- )  {: r :}  s" u" r query-param drop  r sessions session log-in  s" ok" r answer ;
: who ( req -- )  {: r :}  r sessions session session-user 0= if  s" nobody"  then  r answer ;
: logout ( req -- )  {: r :}  r sessions session log-out  s" ok" r answer ;
: csrf ( req -- )  {: r :}  r sessions session csrf-token r answer ;
: form ( req -- )  {: r :}  r  r sessions session  csrf-ok? if  s" ok" 200  else  s" no" 403  then  r respond ;
s" /put" ' put-v site on-get
s" /get" ' get-v site on-get
s" /login" ' login site on-get
s" /who" ' who site on-get
s" /logout" ' logout site on-get
s" /csrf" ' csrf site on-get
s" /form" ' form site on-post
site 0 http-listen listener-port web-port !

\ ---- the client's side: the sid cookie an answer set, sent back ----
create jar 128 allot   variable jar-n
: keep-sid ( a n -- )   \ the sid an answer set, kept (none: the jar as it was)
    {: a n :}  a n s" Set-Cookie: sid=" s-find {: at :}
    at 0< if  exit  then
    a n at 16 + n at - 16 - s-slice {: va vn :}
    va vn s" ;" s-find {: semi :}  semi 0< if  vn to semi  then
    va jar semi move  semi jar-n ! ;
: sid ( -- a n )  jar jar-n @ ;
: with-sid ( path n -- a n )   \ a GET with the kept sid
    {: pa pn :}  sid s\" Cookie: sid={s}\r\n" format {: h hn :}  pa pn h hn get-with  h dispose ;
create old-sid 128 allot   variable old-n
: remember ( -- )  sid old-sid swap move  jar-n @ old-n ! ;
: with-old ( path n -- a n )
    {: pa pn :}  old-sid old-n @ s\" Cookie: sid={s}\r\n" format {: h hn :}  pa pn h hn get-with  h dispose ;

testing no session: nothing in it, no cookie set by a read
T{ s" /get" get  2dup body-of s" none" s=  -rot s" Set-Cookie" has? -> true false }T
testing a value put in it: written, its cookie set (HttpOnly, SameSite=Lax, Secure); read back by the cookie
T{ s" /put?v=hello" get  2dup keep-sid  s" Set-Cookie: sid=" has? -> true }T
T{ s" /put?v=x" get  s" HttpOnly; SameSite=Lax; Max-Age=1209600; Secure" has? -> true }T
T{ sid nip -> 43 }T
T{ s" /get" with-sid body-of s" hello" s= -> true }T
T{ s" /get" s\" Cookie: sid=forged\r\n" get-with body-of s" none" s= -> true }T
T{ s" sessions" db db-count -> 2 }T
testing kept by the token's hash: the database's id isn't the cookie
T{ s" sessions" sid db db-get -> false }T
T{ s" sessions" sid sha256 >base64url db db-get nip nip -> true }T
testing signing in: a new id (the old one gone), what it held kept; signing out: none
remember
T{ s" /login?u=7" with-sid  2dup keep-sid  s" Set-Cookie: sid=" has? -> true }T
T{ sid old-sid old-n @ s= -> false }T
T{ s" /who" with-sid body-of s" 7" s= -> true }T
T{ s" /who" with-old body-of s" nobody" s= -> true }T
T{ s" /get" with-sid body-of s" hello" s= -> true }T
T{ s" /logout" with-sid  s" Set-Cookie: sid=; Max-Age=0; Path=/" has? -> true }T
T{ s" /who" with-sid body-of s" nobody" s= -> true }T
testing CSRF: the session's token sent back by a form or a header; a wrong one, or none, refused
s" /put?v=again" get keep-sid
create token 64 allot   variable token-n
: take-token ( -- )  s" /csrf" with-sid body-of {: a n :}  a token n move  n token-n ! ;
take-token
T{ token-n @ -> 43 }T
: post-form ( field n -- a n )
    {: fa fn :}  fn 5 +  sid  fa fn
    s\" POST /form HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: {}\r\nCookie: sid={s}\r\n\r\ncsrf={s}" format
    {: q qn :}  q qn fetch  q dispose ;
T{ token token-n @ post-form status-of -> 200 }T
T{ s" wrong" post-form status-of -> 403 }T
: post-header ( -- a n )
    sid token token-n @
    s\" POST /form HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Length: 0\r\nCookie: sid={s}\r\nX-CSRF-Token: {s}\r\n\r\n" format
    {: q qn :}  q qn fetch  q dispose ;
T{ post-header status-of -> 200 }T
T{ s\" POST /form HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" fetch status-of -> 403 }T
testing a session that has ended: not read (and deleted); the ended ones swept
: ended ( token n -- )   \ a session for the token that ended long ago
    sha256 >base64url {: ia in :}  s\" {\"data\":{\"v\":\"old\"},\"expires\":1}"  s" sessions" ia in db db-put 2drop ;
s" endedtoken" ended
T{ s" /get" s\" Cookie: sid=endedtoken\r\n" get-with body-of s" none" s= -> true }T
T{ s" sessions" s" endedtoken" sha256 >base64url db db-get -> false }T
s" ended1" ended  s" ended2" ended
T{ sessions sweep-sessions -> 2 }T
T{ s" sessions" s" ended1" sha256 >base64url db db-get -> false }T
T{ s" /get" with-sid body-of s" again" s= -> true }T   \ (a live one kept)
testing a store not Secure: its cookie without it
db <session-store> false >>secure constant plain-sessions
: plain ( req -- )  {: r :}  s" 1" s" v" r plain-sessions session session-string!  s" ok" r answer ;
s" /plain" ' plain site on-get
T{ s" /plain" get  2dup s" Max-Age=1209600" has?  -rot s" Secure" has? -> true false }T

testing require-user: paths under it only for a user signed in (401 else); require-csrf: changes only with the token
<router> constant members
: inside ( req -- )  {: r :}  s" inside" r answer ;
: change ( req -- )  {: r :}  s" changed" r answer ;
s" /login" ' login members on-get
s" /csrf" ' csrf members on-get
s" /members" ' inside members on-get
s" /change" ' change members on-post
s" /members" sessions members require-user
sessions members require-csrf
members 0 http-listen listener-port web-port !
T{ s" /members" get status-of -> 401 }T
T{ s" /login?u=9" get keep-sid  s" /members" with-sid body-of s" inside" s= -> true }T
take-token
: post-change ( field n -- status )
    {: fa fn :}  fn 5 +  sid  fa fn
    s\" POST /change HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: {}\r\nCookie: sid={s}\r\n\r\ncsrf={s}" format
    {: q qn :}  q qn fetch status-of  q dispose ;
T{ token token-n @ post-change -> 200 }T
T{ s" nope" post-change -> 403 }T

test-summary

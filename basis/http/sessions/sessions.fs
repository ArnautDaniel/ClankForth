\ sessions.fs - sessions: a random id in a cookie (HttpOnly, SameSite=Lax, Secure), what the session holds kept in a
\ jsondb collection - so it outlives a restart, and every node (each its replica) sees it. Signing in gives a new id
\ (a session fixed by someone else isn't the one signed in); signing out deletes it. A CSRF token each.
\
\   db <session-store> constant sessions                   \ (db: a jsondb, or a node's replica of it)
\   : sign-in ( req -- )  {: r | s :}  r sessions session to s  ... s" 7" s log-in  ... ;
\   : home ( req -- )  {: r :}  r sessions session session-user if ... then ;
\
\ Its words wait for the database: they are a handler's. docs/http.md, Sessions.
IN: http.sessions
USING: actors accessors http http.cookies jsondb json crypto crypto.tokens base64 system ;

\ where sessions are kept, and how: the database, its collection's name, the cookie's name, how long one lasts unused
\ (seconds), whether the cookie is Secure (sent over HTTPS only - a browser takes it from localhost too)
TUPLE: session-keeping  db coll cookie max-age secure ;
\ a request's session: its store and request, its id in the database (0: none yet), the data it holds (a map),
\ when it ends (seconds since 1970)
TUPLE: req-session  store req id data expires ;

1209600 constant two-weeks
\ the database's sessions: in "sessions", the cookie "sid", two weeks, Secure (change one: >>max-age ...)
: <session-store> ( db -- store )
    s" sessions" >string drop  s" sid" >string drop  two-weeks  true  <session-keeping> rescue-all ;

<PRIVATE
s" session" >string drop constant attr-name
s" user" >string drop constant user-key
s" csrf" >string drop constant csrf-key
2 constant refresh-share   \ a session with less than 1/2 its time left lasts its whole time again, used

: now ( -- s )  time-now ;
: db-of ( sess -- db )  store>> db>> ;
: coll-of ( sess -- a n )  store>> coll>> string ;
\ the id a cookie's token is kept under: its SHA-256 (the database read gives no cookie to use)
: id-of ( token n -- s )  sha256 {: h hn :}  h hn >base64url drop  h dispose ;

: cookie-attrs ( store -- s len )
    {: st :}  st max-age>>  st secure>> if  s" ; Secure"  else  s" "  then
    s" Path=/; HttpOnly; SameSite=Lax; Max-Age={}{s}" format ;
\ a new id: its token in the cookie (the answer's: before it is sent), its hash the session's id
: new-id ( sess -- )
    {: s | t tn :}
    random-token to tn to t
    t tn id-of s swap >>id drop
    s store>> cookie-attrs {: aa an :}
    t tn  s store>> cookie>> string  aa an  s req>> set-cookie! ;
\ what it holds written (a new id first if it has none): {"data": ..., "expires": ...}
: save ( sess -- )
    {: s | m :}
    s id>> 0= if  s new-id  then
    s now s store>> max-age>> + >>expires drop
    <map> to m  s data>> s" data" m s-at!  s expires>> s" expires" m s-at!
    m >json  s coll-of  s id>> string  s db-of  db-put if  drop  else  true db-error (abort")  then ;
\ the request's session read from its cookie: the data it holds and when it ends (false: none, or ended)
: load ( sess -- )
    {: s | t tn v d e :}
    s store>> cookie>> string s req>> req-cookie 0= if  exit  then  to tn to t
    t tn id-of {: id :}
    s coll-of id string s db-of db-get 0= if  exit  then
    {: ja jn :}  [: ja jn json> to v ;] catch if  exit  then
    v map? 0= if  exit  then
    s" expires" v s-at 0= if  exit  then  to e
    s" data" v s-at 0= if  exit  then  to d
    d map? 0= if  exit  then
    e now <= if  s coll-of id string s db-of db-delete drop  exit  then
    s id >>id drop  s d >>data drop  s e >>expires drop
    e now -  s store>> max-age>> refresh-share /  < if  s save  then ;   \ (used: it lasts again)
PRIVATE>

\ the request's session (read once, kept for the request): one with nothing in it if it has none yet - written, and
\ its cookie set, the first time something is put in it
: session ( req store -- sess )
    {: r st :}
    attr-name string r req-attr if  exit  then
    st r 0 <map> 0 <req-session> {: s :}
    s load
    s attr-name string r req-attr!  s ;

: session@ ( key n sess -- v true | false )  data>> s-at ;   \ a value it holds (as json> gives it)
\ a value kept in it (a number, or an object json reads and writes: a string, a map ...): written now
: session! ( v key n sess -- )  {: v ka kn s :}  v ka kn s data>> s-at!  s save ;
: session-string! ( a n key n sess -- )  {: a n ka kn s :}  a n >string drop ka kn s session! ;
: session-forget ( key n sess -- )  {: ka kn s :}  ka kn s data>> s-delete-at  s id>> if  s save  then ;

\ the user signed in: the session given a new id (the old one deleted), the user's id kept in it
: log-in ( user n sess -- )
    {: ua un s :}
    s id>> if  s coll-of s id>> string s db-of db-delete drop  0 s swap >>id drop  then
    ua un user-key string s session-string! ;
\ the user signed out: the session deleted, its cookie forgotten
: log-out ( sess -- )
    {: s :}
    s id>> if  s coll-of s id>> string s db-of db-delete drop  0 s swap >>id drop  then
    <map> s swap >>data drop
    s store>> cookie>> string s" /" s req>> expire-cookie! ;
: session-user ( sess -- user n true | false )   \ the user signed in, if one is
    user-key string rot session@ if  dup string? if  string true exit  then  drop  then  false ;

\ the session's CSRF token (made the first time): what a form sends back (a field "csrf") or a script (a header
\ X-CSRF-Token)
: csrf-token ( sess -- a n )
    {: s :}
    csrf-key string s session@ if  string exit  then
    random-token csrf-key string s session-string!
    csrf-key string s session@ drop string ;
\ the request sent the session's CSRF token (compared all the way): a form's POST, a script's change
: csrf-ok? ( req sess -- flag )
    {: r s | ga gn :}
    csrf-key string s session@ 0= if  false exit  then  string {: ta tn :}
    csrf-key string r form-param if  to gn to ga
    else s" x-csrf-token" r req-header if  to gn to ga
    else  false exit  then then
    ga gn ta tn same-bytes? ;

\ the sessions that have ended deleted: how many (a timer's, now and then)
: sweep-sessions ( store -- n )
    {: st | m ids k :}
    0  st coll>> string st db>> db-count  st coll>> string st db>> db-list  json> to m
    m keys to ids
    ids length 0 ?do
        i ids nth string {: ia in :}
        ia in m s-at drop {: v :}
        v map? if  s" expires" v s-at if  now <= if
            st coll>> string ia in st db>> db-delete drop  k 1+ to k
        then  then  then
    loop
    k ;

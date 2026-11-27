\ sql.fs - SQL Server (Azure SQL Database, Managed Instance, SQL Server 2022) from actors: each connection an actor
\ speaking TDS on a socket, a pool handing them out. Asked from inside an actor (a handler, or `enter`ed): the asker
\ awaits its answer, its other messages kept meanwhile - nothing blocks the node.
\
\   s" myserver.database.windows.net" s" app_user" s" SQL_PASSWORD" getenv s" stagingCRSql1" sql-config
\       s" prodCRSql1" sql-forbid                       ( -- config )
\   4 sql-pool constant db                              \ up to 4 connections, opened as they are wanted
\   s" SELECT Id, Title FROM dbo.Tasks WHERE Status = @status" s\" {\"status\":\"open\"}" json> db sql-query
\       ( -- rows )                                     \ a list of maps: {"Id": 7, "Title": "..."}
\
\ Encrypted from the first byte (TDS 8.0, "strict": TLS first, the server's certificate checked against its name)
\ unless `sql-plain` says otherwise. Values go as parameters (sp_executesql), never into the SQL's text.
\ The database is the one named, or none: a login that can't use it fails (never the login's default), the
\ server's answer must say it, and a session whose database changes (USE ...) is closed before it runs anything
\ else. `sql-forbid` refuses SQL naming a database; `sql-dry-run` rolls every request back.
IN: sql
USING: actors net json accessors sql.tds supervisor ;

30000 value sql-timeout-ms          \ a request with no whole answer in this long fails (its connection closed)
60000 value sql-connect-ms          \ a connection not logged in in this long fails (a serverless database resuming takes 30-60 s)
1000 value sql-retry-ms             \ a login refused for now (below): tried again after this, doubling each time ...
10000 value sql-retry-max-ms        \ ... up to this between tries ...
8 value sql-retries                 \ ... this many times, before whoever waits is told
60000 value sql-idle-ms             \ a connection free this long is closed (0: kept) - a serverless database pauses
                                    \ only with no sessions open
2100 constant sql-params-max        \ SQL Server's limit on a request's parameters
67108864 value sql-response-max     \ an answer's bytes at most: more fails it

\ ---- what to connect to ----
\ host, port, user, password, database, the application's name (the server's sessions show it), TLS first (true) or
\ plain, the database names refused in SQL text (a list), every request rolled back (true), and a word giving an
\ Entra access token to log in with instead of the user and password (0: none)
TUPLE: connection-config  host port user password database app strict forbidden dry-run token ;
1433 constant sql-default-port
: sql-config ( host n user n password n database n -- config )
    {: ha hn ua un pa pn da dn :}
    ha hn >string drop  sql-default-port  ua un >string drop  pa pn >string drop  da dn >string drop
    s" clankforth" >string drop  true  list  false  0  <connection-config> ;
: sql-port ( port config -- config )  swap >>port ;
\ log in with an Entra access token (TDS's FEDAUTH) instead of a user and password: xt ( -- token n ) gives one for
\ the resource https://database.windows.net/ - azure.identity's managed-identity-sql-token, or one's own. Called as
\ each connection logs in (inside its actor: it may ask a server for it). TLS first only.
: sql-token ( xt config -- config )  swap in-handler >>token ;   \ (run in a connection's handler)
: sql-app ( name n config -- config )  {: a n c :}  c a n >string drop >>app ;
\ no TLS: a server on this machine or a test's (the password goes as it is) - a server that requires encryption
\ refuses it
: sql-plain ( config -- config )  false >>strict ;
\ SQL whose text names this database (in any case) refused before it is sent - a second lock beside the login's
\ own database: `prodCRSql1.dbo.Orders` can't be reached by mistake
: sql-forbid ( name n config -- config )  {: a n c :}  a n >string drop rescue-all c forbidden>> push  c ;
\ every request run in a transaction rolled back: what it would do, done and undone (a COMMIT in its text refused)
: sql-dry-run ( config -- config )  true >>dry-run ;

\ ---- the messages ----
message sql-open ( config pool -- )            \ to a new connection: log in, then tell the pool
message sql-ready ( conn -- )                  \ to its pool: logged in, free
message sql-gone ( conn error again? -- )      \ to its pool: closed (error: why, a string, or 0; again?: a login to try again)
message sql-run ( sql params types asker tag -- )   \ to a connection: run it (params, types: maps or 0; tag: the asker's)
message sql-answer ( reply error conn tag -- )      \ to the asker: the reply, or why there is none (error a string)
quiet-message sql-answer   \ (one that comes after its asker stopped waiting: dropped without a word)
message sql-want ( asker -- )                  \ to a pool: a connection, when one is free
message sql-lent ( conn error -- )             \ to the asker: the connection lent (or 0 and why none)
message sql-done-with ( conn -- )              \ to the pool: given back
message sql-close ( -- )                       \ to a connection: closed (free too long), the pool knowing
message sql-returned ( -- )                    \ to a connection given back: made clean (no transaction open), then free

<PRIVATE
s" tds/8.0" >string drop constant tds8-alpn
s" BEGIN TRANSACTION" >string drop constant begin-text
s" COMMIT TRANSACTION" >string drop constant commit-text
s" IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION" >string drop constant rollback-text

\ phases of a connection
0 constant connecting   1 constant prelogging   2 constant logging-in   3 constant ready   4 constant running
\ stages of a request run dry: its transaction begun, the request, rolled back
0 constant begin-stage   1 constant request-stage   2 constant rollback-stage

: same-text? ( a n a2 n2 -- flag )   \ equal, case ignored
    {: a n a2 n2 :}  n n2 <> if  false exit  then  a n s-lower  a2 n2 s-lower {: x xn y yn :}  x xn y yn s=
    x dispose  y dispose ;
: contains-text? ( a n a2 n2 -- flag )   \ a2 n2 inside a n, case ignored
    {: a n a2 n2 :}  a n s-lower  a2 n2 s-lower {: x xn y yn :}  x xn y yn s-find 0>=  x dispose  y dispose ;

state: sql-conn-state
    cell field sc-config
    cell field sc-pool
    cell field sc-conn       \ the socket
    cell field sc-phase
    cell field sc-packet     \ the packet size the server said
    cell field sc-txn        \ the transaction the session is in (its descriptor; 0 none)
    cell field sc-route      \ where the server routed it (a string, kept), or 0
    cell field sc-route-port
    cell field sc-timer
    cell field sc-asker      \ who waits for the request running (nobody: none - an actor's id may be 0)
    cell field sc-tag        \ ... and the tag it gave it (several requests of one asker's told apart)
    cell field sc-stage
    cell field sc-job        \ a request run dry: its SQL, parameters, types (kept till it is done) ...
    cell field sc-job-params
    cell field sc-job-types
    cell field sc-kept       \ ... and its reply's payload (bytes, kept) while it is rolled back
    cell field sc-again      \ its login refused for now (Azure's 40613 ...): the pool tries again
    cell field sc-failed     \ `fail` has told the pool (its `killed` after says nothing more)
    cell field sc-fed-echo   \ the server's FEDAUTHREQUIRED, said back in a token login
    cell field sc-cleaning   \ given back with a transaction open: rolling it back before it is free
    cell field sc-reset      \ lent again: its next request asks the server to reset the session first
end-state
8 constant reset-connection   \ (a packet's status bit: the session reset - SET options, temp tables - before it runs)

: cfg ( -- config )  sc-config @ ;
: stop-timer ( -- )  sc-timer @ ?dup if  cancel-timer  0 sc-timer !  then ;
: start-timer ( ms -- )  stop-timer  after sc-timer ! ;
: send-packets ( type b -- )   \ a message's payload, as packets of the size the server said
    {: type b :}  type b b-data sc-packet @ packets {: p :}
    sc-reset @ if   \ (the first request since it was lent again: the last borrower's session state not kept)
        p b-data drop 1+ {: st :}  st c@ reset-connection or st c!  false sc-reset !
    then
    p b-data sc-conn @ net-write  p dispose ;
\ the asker told - one gone (an actor killed mid-query) can't give the connection back: it goes back of itself
: tell-asker ( reply err tag asker -- )
    {: r e tag asker :}  [: r e self tag asker send sql-answer ;] catch if  self sc-pool @ send sql-done-with  then ;
: answer ( reply error -- )   \ the asker told; the request over
    {: reply err :}  stop-timer
    sc-asker @ {: asker :}
    asker nobody <> if  nobody sc-asker !  reply err sc-tag @ asker tell-asker  then
    sc-job @ ?dup if  dispose  0 sc-job !  then
    sc-job-params @ ?dup if  json-free  0 sc-job-params !  then
    sc-job-types @ ?dup if  json-free  0 sc-job-types !  then
    sc-kept @ ?dup if  dispose  0 sc-kept !  then
    sc-phase @ running = if  ready sc-phase !  then ;
\ the connection ended: whoever waits told why, the pool too; the socket closed, this actor gone
: fail ( a n -- )
    >string drop {: why :}  true sc-failed !
    sc-asker @ nobody <> if  0  why string >string drop  answer  then   \ (a copy each: a message takes what it carries)
    stop-timer
    self  sc-phase @ ready < if  why  else  0  then  sc-again @ sc-pool @ send sql-gone   \ (an error for a waiter
                                                                     \ only if it never was ready: else the pool opens another)
    sc-conn @ ?dup if  net-close  0 sc-conn !  then
    self kill ;

\ ---- logging in ----
: open-socket ( -- )   \ to the server (or where it routed us): TLS first, its name the one configured
    sc-route @ if  sc-route @ string sc-route-port @  else  cfg host>> string cfg port>>  then {: ha hn port :}
    cfg strict>> if
        s" net:tls-connect-as" find-word ?dup 0= if  s" sql: TLS (strict) needs a build with TLS - or sql-plain" fail exit  then
        {: xt :}  ha hn port  cfg host>> string  tds8-alpn string  xt execute
        default-packet-size over  s" net:tls-record-max!" find-word execute   \ (a TDS packet a TLS record: SQL Server reads them so)
    else  ha hn port connect  then
    sc-conn !
    connecting sc-phase !  default-packet-size sc-packet !
    sql-connect-ms start-timer ;
: send-prelogin ( -- )
    0 encrypt-not-supported 0 0  cfg token>> 0<>  prelogin-with {: b :}  prelogin-type b send-packets  b dispose
    prelogging sc-phase ! ;
: my-token ( -- s|0 )   \ the token to log in with (its word asked now), or 0 for the user and password
    cfg token>> ?dup 0= if  0 exit  then  {: xt :}
    [: xt execute >string drop ;] catch if  error-message s" sql: no Entra token: {s}" format fail 0 exit  then ;
: my-login ( -- info )
    my-token {: tok :}
    tds-7.4 default-packet-size 0 0
    cfg app>>  tok if  0 0  else  cfg user>>  cfg password>>  then  cfg app>>
    sc-route @ ?dup 0= if  cfg host>>  then
    s" clankforth" >string drop  0  cfg database>>  0  tok  sc-fed-echo @  <login-info> ;
: send-login ( -- )
    my-login  sc-failed @ if  drop exit  then   \ (no token got: the connection gone already)
    login7 {: b :}  login7-type b send-packets  b dispose
    logging-in sc-phase ! ;
: prelogin-answered ( payload -- )
    {: p :}
    cfg strict>> 0= if   \ (plain: only a server that doesn't encrypt at all)
        p b-data prelogin-encryption {: e :}
        e encrypt-not-supported <> if
            s" sql: the server requires encryption - leave the connection strict (TLS first), not sql-plain" fail exit
        then
    then
    p b-data prelogin-fedauth sc-fed-echo !
    send-login ;
\ Azure's errors that mean "not now": a serverless database paused (40613: resuming), a server moving or busy
create transient-errors  40613 , 40197 , 40501 , 49918 , 49919 , 49920 , 40540 ,
7 constant transient-count
: transient? ( number -- flag )
    {: n :}  false  transient-count 0 do  i cells transient-errors + @ n = if  drop true leave  then  loop ;
: right-database? ( reply -- flag )   \ the database the server says is the one configured
    database>> ?dup 0= if  false exit  then  string cfg database>> string same-text? ;
\ a database the reply changed to that isn't the configured one - any of them, not only the last (USE prod; ...;
\ USE staging ends where it began, its middle in prod) - else 0
: strayed ( reply -- name | 0 )
    databases>> [: string cfg database>> string same-text? 0= ;] find 0= if  0  then ;
: logged-in ( reply -- )
    {: r :}
    r reply-error ?dup if
        dup number>> transient? sc-again !
        error-text s" sql: login failed: {s}" format fail exit
    then
    r route-host>> if   \ (Azure's gateway: on to the server it names - once: a server routing on and on is refused)
        sc-route @ if  s" sql: routed again by the server it was routed to" fail exit  then
        r route-host>> rescue-actor sc-route !  r route-port>> sc-route-port !
        sc-conn @ net-close  0 sc-conn !  open-socket exit
    then
    r logged-in>> 0= if  s" sql: the server didn't log the connection in" fail exit  then
    r right-database? 0= if
        r database>> ?dup if  string  else  s" no database"  then  cfg database>> string
        s" sql: logged in to {s}, not {s} - closed" format fail exit
    then
    r packet-size>> ?dup if  sc-packet !  then
    cfg strict>> if  sc-packet @ sc-conn @ s" net:tls-record-max!" find-word execute  then
    stop-timer  ready sc-phase !
    self sc-pool @ send sql-ready ;

\ ---- requests ----
\ a list as a parameter: `IN (@ids)` with ids [3, 5, 8] sent as `IN (@ids0, @ids1, @ids2)`, each item a parameter of
\ its own (declared as `types` says of ids); an empty list as `SELECT NULL WHERE 1 = 0` - IN finds no row, NOT IN
\ every row. Names in quotes, brackets and comments are left.
: name-char? ( c -- flag )
    {: c :}  c [char] a [char] z 1+ within  c [char] A [char] Z 1+ within or  c [char] 0 [char] 9 1+ within or
    c [char] _ = or ;
: any-list? ( params -- flag )  {: p :}  false  p values [: list? or ;] each ;
: index-from ( a n at a2 n2 -- i )   \ where a2 n2 is at or after `at` (n: not there)
    {: a n at a2 n2 :}  a at +  n at -  a2 n2 s-find {: i :}  i 0< if  n  else  at i +  then ;
\ where a literal or comment beginning at `at` ends (past it) - 'text' ("it''s" its '' inside), "name", [name], -- to
\ the line's end, /* ... */ - or `at` if none begins there
: past-literal ( a n at -- at' )
    {: a n at :}
    a at + c@ {: c :}
    at 1+ n <  if  a at + 1+ c@  else  0  then {: d :}
    c [char] ' = if  at 1+ {: i :}
        begin  a n i s" '" index-from to i  i 1+ n <  if  a i + 1+ c@ [char] ' =  else  false  then  while  i 2 + to i  repeat
        i 1+ n min exit
    then
    c [char] " = if  a n at 1+ s\" \"" index-from 1+ n min exit  then
    c [char] [ = if  a n at 1+ s" ]" index-from 1+ n min exit  then
    c [char] - =  d [char] - = and if  a n at s\" \n" index-from exit  then
    c [char] / =  d [char] * = and if  a n at 2 + s" */" index-from 2 + n min exit  then
    at ;
: expanded ( sql params types -- sql' params' types' )
    {: s params types | p2 t2 :}
    params 0= if  s params types exit  then
    params any-list? 0= if  s params types exit  then
    <map> to p2  types if  <map>  else  0  then to t2
    params [: {: k v :}   \ (the others as they are)
        v list? 0= if  v k string p2 s-at!
            t2 if  k string types s-at if  k string t2 s-at!  then  then
        then ;] map-each
    s string {: a n :}
    [:  {: | at :}
        begin  at n <  while
            a at + c@ {: c :}
            a n at past-literal {: past :}
            past at > if  a at +  past at -  %s  past to at
            else c [char] @ =  at 1+ n < and if
                at 1+ {: from :}  from {: to-at :}
                begin  to-at n <  if  a to-at + c@ name-char?  else  false  then  while  to-at 1+ to to-at  repeat
                a from + to-at from - {: na nn :}
                nn if  na nn params s-at  else  false  then
                if  dup list? if
                        {: l :}  l empty? if  s" SELECT NULL WHERE 1 = 0" %s  then
                        l [: {: x i :}
                            i if  s" , " %s  then
                            na nn i s" {s}{}" format {: pa pn :}
                            [char] @ %c  pa pn %s
                            x pa pn p2 s-at!
                            t2 if  na nn types s-at if  pa pn t2 s-at!  then  then
                            pa dispose ;] each-index
                        to-at to at
                    else  drop  c %c  at 1+ to at  then
                else  c %c  at 1+ to at  then
            else  c %c  at 1+ to at  then then
        repeat ;] build
    p2 keys length sql-params-max > if
        p2 keys length sql-params-max s" sql: {} parameters, more than SQL Server takes ({}) - a long list belongs in a table" format
        true -rot (abort")
    then
    p2 t2 ;
PRIVATE>
\ SQL with each of its parameters' names given a suffix, and a map of them so named: `Id = @id` and {"id": 7} with
\ `_w3` are `Id = @id_w3` and {"id_w3": 7} - so several requests' statements, their parameters of one name, can go
\ as one. Names in quotes, brackets and comments are left, as a name that is no parameter's (@@ROWCOUNT). The
\ string and the map are new (the map's values the old one's own).
: sql-suffixed ( sql n params suffix n -- s params' )
    {: a n params xa xn | p2 :}
    <map> to p2
    params [: {: k v :}  k string xa xn s+ {: ka kn :}  v ka kn p2 s-at!  ka dispose ;] map-each
    [:  {: | at :}
        begin  at n <  while
            a n at past-literal {: past :}
            past at > if  a at +  past at -  %s  past to at
            else a at + c@ [char] @ = if
                at 1+ {: from :}
                from n <  if  a from + c@ [char] @ =  else  false  then  if  from 1+ to from  then   \ (@@name: the system's)
                from {: e :}
                begin  e n <  if  a e + c@ name-char?  else  false  then  while  e 1+ to e  repeat
                a at +  e at -  %s
                from at 1+ =  e from > and  if  a from +  e from -  params s-key?  else  false  then
                if  xa xn %s  then
                e to at
            else  a at + c@ %c  at 1+ to at  then then
        repeat ;] build
    p2 ;
<PRIVATE
: request-bytes ( sql params types -- type b )   \ a batch (no parameters) or sp_executesql
    {: s0 p0 t0 :}  s0 p0 t0 expanded {: s params types :}
    params 0=  params if  params keys length 0=  else  false  then  or if
        sql-batch-type  s string sc-txn @ sql-batch exit
    then
    params types param-decls {: d dn :}
    rpc-type  s string d dn params sc-txn @ rpc-executesql  d dispose ;
: send-request ( sql params types -- )
    request-bytes send-packets  running sc-phase !  sql-timeout-ms start-timer ;
: send-text ( s -- )  0 0 send-request ;
\ the session's changes a reply says: its transaction, its packet size - and its database, which mustn't change
: session-changes ( reply -- ok? )
    {: r :}
    r txn>> -1 <> if  r txn>> sc-txn !  then
    r packet-size>> ?dup if  sc-packet !  then
    r strayed ?dup if
        string s" sql: the session's database changed to {s} - its answer dropped, the connection closed" format fail
        false exit
    then
    true ;
: answer-reply ( reply -- )
    dup reply-error ?dup if  error-text drop  else  0  then  {: r err :}
    sc-cleaning @ if   \ (a returned connection's rollback answered: free if its transaction is gone, else closed)
        false sc-cleaning !  r err answer
        sc-txn @  err or if  s" sql: a transaction left open couldn't be rolled back - closed" fail exit  then
        self sc-pool @ send sql-ready exit
    then
    r err answer ;
: word-char? ( c -- flag )  {: c :}  c [char] a [char] z 1+ within  c [char] A [char] Z 1+ within or  c [char] 0 [char] 9 1+ within or  c [char] _ = or ;
\ a USE statement in it (USE as a word of its own, then a name): its database would be another's - never sent
: uses-database? ( a n -- flag )
    {: a n | found :}
    n 3 - 0 max 0 ?do
        found 0= if
            a i + 3 s" use" same-text?
            i 0= if  true  else  a i + 1- c@ word-char? 0=  then  and
            i 3 + n < if  a i + 3 + c@ {: c :}  c 32 =  c 9 = or  c 10 = or  c 13 = or  c [char] [ = or  else  false  then  and
            if  true to found  then
        then
    loop  found ;
: refused ( sql -- a n true | false )   \ SQL the configuration won't send: why
    {: s :}
    s string uses-database? if  s" sql: a USE statement - this configuration works in its own database only" true exit  then
    cfg forbidden>> [: {: name :}  s string name string contains-text? ;] find if
        string s" sql: the SQL names {s}, a database this configuration forbids" format true exit
    then
    cfg dry-run>> if  s string s" commit" contains-text?  s string commit-text string same-text? 0= and if
        s" sql: a COMMIT in a dry run (sql-dry-run): refused" true exit
    then then
    false ;
: copied ( map|0 -- map|0 )   \ (a copy of its own, kept by the connection)
    {: m :}  m 0= if  0 exit  then  m json-copy rescue-actor ;
: run-request ( sql params types -- )
    {: s params types :}
    cfg dry-run>>  s string commit-text string same-text? and if  rollback-text to s  then   \ (a transaction's end, dry: undone)
    cfg dry-run>>  sc-txn @ 0= and if   \ (its own transaction, rolled back after)
        s string >string drop rescue-actor sc-job !   \ (copies, kept till it is done: its own to free)
        params copied sc-job-params !  types copied sc-job-types !
        begin-stage sc-stage !  begin-text send-text exit
    then
    request-stage sc-stage !  s params types send-request ;
: request-answered ( payload -- )
    {: p :}
    p b-data tds-reply> {: r :}
    r session-changes 0= if  exit  then
    sc-stage @ begin-stage = if
        r reply-error if  r answer-reply exit  then
        request-stage sc-stage !  sc-job @ sc-job-params @ sc-job-types @ send-request exit
    then
    sc-stage @ request-stage =  sc-job @ and if   \ (dry: its reply kept while the transaction is undone)
        <bytes> {: b :}  p b-data b b-append  b rescue-actor sc-kept !
        rollback-stage sc-stage !  rollback-text send-text exit
    then
    sc-stage @ rollback-stage = if  sc-kept @ b-data tds-reply> answer-reply exit  then
    r answer-reply ;

\ ---- the bytes come ----
: answered ( payload -- )
    sc-phase @ case
        prelogging of  prelogin-answered  endof
        logging-in of  b-data tds-reply> logged-in  endof
        running of  request-answered  endof
        nip  s" sql: the server sent what wasn't asked for" fail
    endcase ;
: try-message ( -- )
    sc-conn @ net-input {: a n :}
    a n message-end {: used :}
    used 0< if  s" sql: what the server sent isn't TDS" fail exit  then
    used 0= if  n sql-response-max > if  s" sql: an answer longer than sql-response-max" fail  then  exit  then
    <bytes> {: p :}  a used p message-payload drop
    used sc-conn @ net-consume
    [: p answered ;] catch if   \ (a reply it can't read - a type it doesn't know: the asker told why, the connection closed)
        error-message fail
    then ;
PRIVATE>

behaviour sql-connection
    on sql-open ( config pool -- )  sc-pool !  sc-config !  nobody sc-asker !  open-socket ;
    on net-connected ( conn -- )  drop  send-prelogin ;
    on net-received ( conn -- )  drop  try-message ;
    on net-closed ( conn -- )
        net-why {: wa wn :}  0 sc-conn !
        sc-phase @ case
            connecting of  s" sql: no connection"  endof
            prelogging of  s" sql: the server closed the connection at PRELOGIN"  endof
            logging-in of  s" sql: the server closed the connection at the login"  endof
            s" sql: the connection closed" rot
        endcase
        wn if  wa wn s" {s}: {s}" format  then  fail ;   \ (and why, if an error ended it: refused, the TLS handshake ...)
    on timer-fired ( timer -- )
        drop  0 sc-timer !  sc-phase @ running = if  s" sql: no answer in sql-timeout-ms"  else  s" sql: not logged in in sql-connect-ms"  then  fail ;
    on sql-run ( sql params types asker tag -- )
        {: s params types asker tag :}
        sc-phase @ ready <> if  0  s" sql: the connection is busy or not logged in" >string drop  self tag asker send sql-answer  exit  then
        asker sc-asker !  tag sc-tag !
        s refused if  >string drop 0 swap answer exit  then
        [: s params types run-request ;] catch if   \ (a request that can't be made - too many parameters ...: said)
            0  error-message >string drop  answer
        then ;
    on sql-close ( -- )  true sc-failed !  sc-conn @ ?dup if  net-close  0 sc-conn !  then  self kill ;
    on sql-returned ( -- )   \ (given back: a transaction its user left open - a rollback that failed - undone first)
        true sc-reset !
        sc-txn @ 0= if  self sc-pool @ send sql-ready exit  then
        true sc-cleaning !  nobody sc-asker !  request-stage sc-stage !  rollback-text send-text ;
    on killed ( -- )   \ (failed in a handler: whoever waits told, and the pool - unless `fail` did)
        sc-failed @ if  exit  then
        sc-asker @ {: asker :}
        asker nobody <> if  nobody sc-asker !  0 s" sql: the connection's actor failed" >string drop sc-tag @ asker tell-asker  then
        self s" sql: the connection's actor failed" >string drop false sc-pool @ send sql-gone ;
    on start-over ( -- )  self kill ;   \ (a handler failed, the supervisor says: gone - its last words tell the pool)
end-behaviour

\ ---- the pool: connections opened as they are wanted (up to its size), lent one asker at a time ----
message sql-pool-init ( config size -- )
<PRIVATE
state: sql-pool-state
    cell field sp-config
    cell field sp-size
    cell field sp-count     \ connections open or opening
    cell field sp-idle      \ those free (a list, kept)
    cell field sp-waiting   \ askers waiting for one, first come first (a list, kept)
    cell field sp-all       \ every connection open or opening (a list, kept): one given back after it died isn't
    cell field sp-tries     \ logins refused for now in a row (Azure resuming a database): tried again after a wait
    cell field sp-idle-timers   \ each free connection's timer (a map, kept): closed when it fires, if still free
end-state
: kept-list ( l var -- )  {: l var :}  var @ ?dup if  dispose  then  l rescue var ! ;
: first-off ( var -- x )   \ a list's first item, taken off it
    {: var :}  var @ {: l :}  l first  l 1 tail var kept-list ;
: open-one ( -- )
    sql-connection sql-conn-state s" sql-connection" spawn {: c :}
    sp-config @ self c send sql-open  1 sp-count +!  c sp-all @ push ;
: forget ( conn -- )   \ out of the pool's lists
    {: c :}
    sp-idle @ [: c <> ;] filter sp-idle kept-list
    sp-all @ [: c <> ;] filter sp-all kept-list
    c sp-idle-timers @ key? if  c sp-idle-timers @ delete-at  then ;
: gone? ( addr -- flag )   \ an actor of this node no longer alive (one elsewhere taken to be)
    {: a :}  a 32 rshift 0=  a node-of node = or if  a $ffffffff and alive? 0=  else  false  then ;
\ a connection gone before the pool heard (its sql-gone on its way): out of the lists, the count down - once (its
\ sql-gone after finds it in none); another opened for whoever waits
: lost ( conn -- )
    {: c :}  c sp-all @ contains? 0= if  exit  then
    c forget  -1 sp-count +!
    sp-waiting @ empty? 0=  sp-count @ sp-size @ < and if  open-one  then ;
: give-out ( conn -- )   \ to the first waiting, or kept free (one gone: forgotten) - closed if free too long
    {: c :}
    c sp-all @ contains? 0= if  exit  then
    sp-waiting @ empty? if
        c sp-idle @ push
        sql-idle-ms if  sql-idle-ms after  c sp-idle-timers @ at!  then
        exit
    then
    begin  sp-waiting @ empty? 0=  while   \ (an asker gone while it waited: passed over, the connection not lost)
        sp-waiting first-off {: w :}
        w gone? 0= if  c 0 w send sql-lent exit  then
    repeat
    c sp-idle @ push
    sql-idle-ms if  sql-idle-ms after  c sp-idle-timers @ at!  then ;
PRIVATE>

: pool-timer ( timer -- )   \ (sql-pooler's timer-fired)
    {: t | idle :}  nobody to idle
    sp-idle-timers @ [: {: c tm :}  tm t = if  c to idle  then ;] map-each
    idle nobody <> if
        idle sp-idle-timers @ delete-at
        idle sp-idle @ contains? if  idle forget  -1 sp-count +!  idle gone? 0= if  idle send sql-close  then  then
        exit
    then
    sp-waiting @ empty? 0=  sp-count @ sp-size @ < and if  open-one  then ;

behaviour sql-pooler
    on sql-pool-init ( config size -- )
        sp-size !  rescue sp-config !   \ (a config made in a handler: the pool's from now, kept as long as it lives)
        list sp-idle kept-list  list sp-waiting kept-list  list sp-all kept-list
        <map> rescue sp-idle-timers ! ;
    on sql-want ( asker -- )
        {: asker :}
        begin  sp-idle @ empty? 0=  if  sp-idle @ last gone?  else  false  then  while   \ (free ones gone meanwhile: let go)
            sp-idle @ last lost
        repeat
        sp-idle @ empty? 0= if  sp-idle @ pop 0 asker send sql-lent exit  then
        asker sp-waiting @ push
        sp-count @ sp-size @ < if  open-one  then ;
    on sql-ready ( conn -- )  0 sp-tries !  give-out ;
    on sql-done-with ( conn -- )   \ (back to it to make clean: it says sql-ready when it is free)
        {: c :}  c sp-all @ contains? 0= if  exit  then
        c gone? if  c lost exit  then   \ (it ended, its sql-gone not here yet: forgotten now, not sent to)
        c send sql-returned ;
    on sql-gone ( conn error again? -- )
        {: c err again? :}
        c sp-all @ contains? if  -1 sp-count +!  then   \ (once: an idle one's timer may have let it go already)
        c forget
        sp-waiting @ empty? if  exit  then
        again?  sp-tries @ sql-retries < and if   \ (refused for now: tried again after a wait, doubling)
            sql-retry-ms sp-tries @ lshift sql-retry-max-ms min after drop  1 sp-tries +!  exit
        then
        0 sp-tries !
        again? if   \ (refused for now, too many times: everyone waiting told)
            sp-waiting @ [: {: w :}  w gone? 0= if  0 err string >string drop w send sql-lent  then ;] each
            list sp-waiting kept-list  exit
        then
        err if  sp-waiting first-off {: w :}  w gone? 0= if  0 err w send sql-lent  then  then   \ (one couldn't open: the first waiting told why)
        sp-waiting @ empty? 0=  sp-count @ sp-size @ < and if  open-one  then ;
    on timer-fired ( timer -- )   \ a free connection's time up (closed, if still free), or a wait before a login again
        {: t :}  [: t pool-timer ;] catch if  ." sql-pool: " error-message type cr  then ;   \ (a mistake said: the pool lives on)
end-behaviour

\ ---- asking ----
\ a pool (`sql-pool`'s: its actor, and a watcher told of the writes through it - sql.watch - or nobody), or one
\ connection lent for a transaction (`sql-transaction` gives its xt one: the connection, and its pool's db)
TUPLE: sql-db  pool watcher size ;
TUPLE: sql-session  conn db ;
message sql-touched ( table -- )   \ to a db's watcher: a table written through it (sql.model's words say)
\ the pool's db of a pool's or a session's
: sql-db-of ( db -- db )  dup sql-session instance? if  db>>  then ;

\ a pool of up to `size` connections to the configured server, opened as they are first wanted
: sql-pool ( config size -- db )
    {: c n :}  sql-pooler sql-pool-state s" sql-pool" spawn {: p :}
    p essential   \ (supervised: a mistake in it logged, the pool on - not the database gone till a restart)
    c n p send sql-pool-init  p nobody n <sql-db> ;

<PRIVATE
: sql-fail ( s -- )  true swap string (abort") ;   \ (its text the error's: the string goes with the catch round it)
: as-thrown ( code -- x )   \ what a catch gave, to throw on: an error (-1) as its text, a copy (made before more is caught)
    dup -1 = if  drop  error-message >string drop  then ;
: ?in-actor ( -- )  self 0< abort" sql: inside an actor (its answers are messages)" ;
: borrow ( db -- conn )
    {: db :}  ?in-actor  self db pool>> send sql-want
    await sql-lent {: c err :}  err if  err sql-fail  then  c ;
: give-back ( conn db -- )  pool>> send sql-done-with ;
: sent-copy ( map|0 -- map|0 )   \ a copy to send: a message takes what it carries, and what that holds -
    {: m :}  m 0= if  0 exit  then  m json-copy ;   \ the caller's map and its values would go with the request
: on-conn ( sql n params types conn -- reply )
    {: a n params types c :}
    a n >string drop  params sent-copy  types sent-copy  self 0 c send sql-run
    await sql-answer {: r err conn tag :}  err if  err sql-fail  then  r ;
PRIVATE>

\ SQL run, its parameters (a map of names - without @ - to values; 0 none) declared as `types` says (a map of names
\ to SQL types, "varchar(50)"; 0 or a name not there: by its value) - the whole reply: its result sets, counts,
\ messages, output parameters. A SQL error is thrown, its text the server's.
: sql-reply-typed ( sql n params types db -- reply )
    {: a n params types db | r :}
    db sql-session instance? if  a n params types db conn>> on-conn exit  then
    db borrow {: c :}
    [: a n params types c on-conn to r ;] catch as-thrown {: e :}
    c db give-back
    e if  e throw  then  r ;
: sql-reply ( sql n params db -- reply )  {: a n p db :}  a n p 0 db sql-reply-typed ;
\ its first result set's rows: a list of maps, each column's name to its value (none: an empty list)
: sql-query ( sql n params db -- rows )
    sql-reply sets>> {: sets :}  sets empty? if  list  else  sets first rows>>  then ;
\ its first row, or 0
: sql-one ( sql n params db -- row | 0 )  sql-query {: rows :}  rows empty? if  0  else  rows first  then ;
\ the rows it changed (its last statement's count; -1 if it counted none)
: sql-exec ( sql n params db -- n )  sql-reply affected>> ;
\ xt ( session -- ) run in a transaction on one connection: committed if it ends, rolled back if it throws (and the
\ error thrown on). Its requests are given the session as their db.
: sql-transaction ( xt db -- )
    {: xt db :}
    db borrow {: c :}  c db <sql-session> {: tx :}
    [: begin-text string 0 0 c on-conn drop  tx xt execute  commit-text string 0 0 c on-conn drop ;] catch as-thrown {: e :}
    e if  [: rollback-text string 0 0 c on-conn drop ;] catch drop  then
    c db give-back
    e if  e throw  then ;

\ ---- several at once ----
<PRIVATE
TUPLE: sql-request  sql params ;
task-variable collecting   \ (the requests sql-also adds, while sql-query-all's xt runs: no wait in between)
: first-rows ( reply -- rows )  sets>> {: sets :}  sets empty? if  list  else  sets first rows>>  then ;
PRIVATE>
\ inside sql-query-all's xt: one more request
: sql-also ( sql n params -- )
    {: a n p :}  collecting @ 0= abort" sql-also: only inside sql-query-all's xt"
    a n >string drop  p <sql-request> collecting @ push ;
<PRIVATE
: requests-of ( xt -- qs )  list collecting !  execute  collecting @  0 collecting ! ;
\ the requests run at once on up to the pool's size of its connections, all awaited: each one's reply in its place
\ (0 where it failed), and the first error (a string, or 0). With no connection to be had: thrown.
: run-all ( qs db -- replies err )
    {: qs db | results conns next out err :}
    qs [: drop 0 ;] map to results
    qs empty? if  results 0 exit  then
    ?in-actor  list to conns
    qs length db size>> min {: k :}
    k 0 do  self db pool>> send sql-want  loop
    k 0 do  await sql-lent {: lc le :}  le if  le to err  else  lc conns push  then  loop
    conns empty? if  err sql-fail  then
    [: {: c :}   \ (the next request on to a connection: tagged with its place)
        next qs nth {: q :}  q sql>> string >string drop  q params>> sent-copy  0  self next c send sql-run
        next 1+ to next  out 1+ to out ;] {: dispatch :}
    conns [: {: c :}  next qs length < if  c dispatch execute  else  c db give-back  then ;] each
    begin  out 0>  while
        await sql-answer {: r ae ac tag :}  out 1- to out
        ae if  err 0= if  ae to err  then  else  r tag results nth!  then
        ac gone? if   \ (its connection died with the error: nothing more sent to it, nothing given back)
        else next qs length < if  ac dispatch execute  else  ac db give-back  then then
    repeat
    results err ;
PRIVATE>
\ the requests xt adds (sql-also) run at once - on as many of the pool's connections as there are requests, up to
\ its size - their answers awaited together: each one's rows (as sql-query's), in the order they were added. A page's
\ queries that don't depend on each other take as long as the slowest, not their sum. One that fails: the first
\ error thrown, once all have answered. In a transaction (a session's one connection): one after another.
: sql-query-all ( xt db -- lists )
    {: xt db :}  xt requests-of {: qs :}
    db sql-session instance? if
        qs [: {: q :}  q sql>> string q params>> db sql-query ;] map exit
    then
    qs db run-all {: rs err :}  err if  err sql-fail  then
    rs [: first-rows ;] map ;
\ ... each one's whole reply (as sql-reply's) and nothing thrown for one that fails: 0 in its place, the others'
\ replies there all the same - for a caller that goes on with those that worked (sql.watch's rounds). No connection
\ to be had at all: thrown, as ever. Not in a transaction.
: sql-reply-all ( xt db -- replies )
    {: xt db :}  db sql-session instance? abort" sql-reply-all: of a pool, not in a transaction"
    xt requests-of db run-all drop ;

\ sql (basis/sql): connections and a pool against a stand-in server speaking TDS (plain, on this machine) - logging
\ in, queries with parameters, errors, the database locked to the one configured (a login to another refused, a
\ USE closing the session), forbidden names, dry runs rolled back, transactions, a route to another server followed
IN: test-sql
USING: tester actors net json accessors sql.tds sql ;

\ (a pool made first: the actor numbered 0 - a number, not "none")
s" 127.0.0.1" s" sa" s" pw" s" stagingdb" sql-config sql-plain  1 swap sql-port  1 sql-pool constant first-pool
source-dir s" /sql/stand-in.fs" s+ included

config 2 sql-pool constant db

testing a server not there (the pool the actor numbered 0): the asker told, not left waiting
T{ [: s" SELECT 1" 0 first-pool sql-query ;] run  s" sql: no connection" failed-with? -> -1 true }T
testing a query: logged in, the rows as maps; a parameter sent with its declaration (sp_executesql)
T{ [: s" SELECT 42 AS answer" 0 db sql-query  first s" answer" rot s-at drop ;] run -> 42 }T
T{ [: s" SELECT name FROM t WHERE id = @id" s\" {\"id\":7}" json> db sql-one  s" name" rot s-at drop string s" ann" s= ;] run -> true }T
T{ asked last string s" SELECT name FROM t WHERE id = @id|@id bigint" s= -> true }T
T{ [: s" UPDATE t SET x = 1" 0 db sql-exec ;] run -> 3 }T
testing a list as a parameter: IN (@ids) sent as a parameter for each item (a name in quotes left); none as NULL
: in-sql ( -- a n )  s" SELECT name FROM t WHERE Id IN (@ids) AND Note = '@ids' AND n = @n" ;
T{ [: in-sql s\" {\"ids\":[3,5,8],\"n\":1}" json> db sql-query length ;] run -> 1 }T
: in-sent ( -- a n )  s" SELECT name FROM t WHERE Id IN (@ids0, @ids1, @ids2) AND Note = '@ids' AND n = @n|@n bigint, @ids0 bigint, @ids1 bigint, @ids2 bigint" ;
T{ asked last string in-sent s= -> true }T
T{ [: in-sql s\" {\"ids\":[],\"n\":1}" json> db sql-query length ;] run -> 1 }T
T{ asked last string s" SELECT name FROM t WHERE Id IN (SELECT NULL WHERE 1 = 0) AND Note = '@ids' AND n = @n|@n bigint" s= -> true }T   \ (NOT IN: every row)
: odd-sql ( -- a n )  s\" SELECT [@ids], \"@ids\", 'it''s @ids' FROM t -- @ids\n WHERE x IN (@ids) /* @ids */" ;
T{ [: odd-sql s\" {\"ids\":[1]}" json> db sql-query length ;] run -> 1 }T
: odd-sent ( -- a n )  s\" SELECT [@ids], \"@ids\", 'it''s @ids' FROM t -- @ids\n WHERE x IN (@ids0) /* @ids */|@ids0 bigint" ;
T{ asked last string odd-sent s= -> true }T   \ (in brackets, quotes and comments: left)
: too-many ( -- m )  <map> {: m :}  list {: l :}  2101 0 do  i l push  loop  l s" ids" m s-at!  m ;
T{ [: s" SELECT 1 WHERE 1 IN (@ids)" too-many db sql-query ;] run  s" 2101 parameters, more than SQL Server takes (2100)" failed-with? -> -1 true }T
T{ [: s" SELECT 42 AS answer" 0 db sql-query length ;] run -> 1 }T   \ (the connection goes on)
: typed-in ( -- a n )  s" SELECT name FROM t WHERE Code IN (@c)" ;
T{ [: typed-in s\" {\"c\":[\"a\",\"b\"]}" json> s\" {\"c\":\"varchar(20)\"}" json> db sql-reply-typed drop 0 ;] run -> 0 }T
T{ asked last string s" SELECT name FROM t WHERE Code IN (@c0, @c1)|@c0 varchar(20), @c1 varchar(20)" s= -> true }T

testing a SQL error: thrown, its text the server's (the connection goes on)
T{ [: s" SELECT * FROM fail" 0 db sql-query ;] run  s" Invalid object name 'nope'. (208, line 1)" failed-with? -> -1 true }T
T{ [: s" SELECT 42 AS answer" 0 db sql-query length ;] run -> 1 }T

testing the database locked: a USE to another closes the session before anything else runs
T{ [: s" USE prodCRSql1" 0 db sql-exec ;] run  s" database changed to prodCRSql1 - closed" failed-with? -> -1 true }T
T{ [: s" SELECT 42 AS answer" 0 db sql-query length ;] run -> 1 }T   \ (a new connection, in the right database)
testing a login to another database than the one configured: refused, never used
s" master" says-database
config 1 sql-pool constant wrong-db
T{ [: s" SELECT 42 AS answer" 0 wrong-db sql-query ;] run  s" logged in to master, not stagingdb" failed-with? -> -1 true }T
s" stagingdb" says-database
testing a login refused: the server's words
true fail-login !
config 1 sql-pool constant refused-db
T{ [: s" SELECT 1" 0 refused-db sql-query ;] run  s" login failed: Login failed for user 'sa'." failed-with? -> -1 true }T
false fail-login !

testing a forbidden database: SQL naming it refused before it is sent (in any case)
config s" prodCRSql1" rot sql-forbid 1 sql-pool constant guarded
asked length constant before
T{ [: s" SELECT * FROM PRODCRSQL1.dbo.Orders" 0 guarded sql-query ;] run  s" forbids" failed-with? -> -1 true }T
T{ asked length -> before }T   \ (nothing sent)
T{ [: s" SELECT 42 AS answer" 0 guarded sql-query length ;] run -> 1 }T

testing a dry run: each request in a transaction rolled back after; a COMMIT refused
config sql-dry-run 1 sql-pool constant dry
asked list-clear
T{ [: s" UPDATE t SET x = 1" 0 dry sql-exec ;] run -> 3 }T
T{ asked length  0 asked nth string s" BEGIN TRANSACTION" s=  2 asked nth string s" ROLLBACK" s-find 0>= -> 3 true true }T
T{ [: s" UPDATE t SET x = 1; COMMIT" 0 dry sql-exec ;] run  s" COMMIT in a dry run" failed-with? -> -1 true }T

testing a transaction: begun, its requests on its connection, committed; one that throws rolled back
asked list-clear
: two-updates ( tx -- )  {: tx :}  s" UPDATE a SET x = 1" 0 tx sql-exec drop  s" UPDATE b SET y = 2" 0 tx sql-exec drop ;
T{ [: ['] two-updates db sql-transaction 0 ;] run -> 0 }T
T{ asked length  0 asked nth string s" BEGIN TRANSACTION" s=  3 asked nth string s" COMMIT TRANSACTION" s= -> 4 true true }T
asked list-clear
: then-fails ( tx -- )  {: tx :}  s" UPDATE a SET x = 1" 0 tx sql-exec drop  s" SELECT * FROM fail" 0 tx sql-exec drop ;
T{ [: ['] then-fails db sql-transaction 0 ;] run  s" Invalid object name" failed-with? -> -1 true }T
T{ asked last string s" ROLLBACK" s-find 0>= -> true }T

testing a database resuming (Azure serverless: 40613): its logins tried again after a wait, doubling; given up after
testing  sql-retries, the error said
20 to sql-retry-ms
2 busy-logins !  0 logins !
config 1 sql-pool constant resuming
T{ [: s" SELECT 42 AS answer" 0 resuming sql-query length ;] run  logins @ -> 1 3 }T
100 busy-logins !  2 to sql-retries
config 1 sql-pool constant paused
T{ [: s" SELECT 1" 0 paused sql-query ;] run  s" not currently available. (40613" failed-with? -> -1 true }T
0 busy-logins !  8 to sql-retries  1000 to sql-retry-ms

testing a connection free for sql-idle-ms: closed (a serverless database pauses only with none open); the next ask
testing  opens another
100 to sql-idle-ms
config 1 sql-pool constant idling
T{ [: s" SELECT 42 AS answer" 0 idling sql-query length ;] run -> 1 }T
0 logins !
T{ settle settle settle  [: s" SELECT 42 AS answer" 0 idling sql-query length ;] run  logins @ -> 1 1 }T
60000 to sql-idle-ms

testing a caller's values: given as parameters, still the caller's after (a message takes what it carries: a copy
testing  goes) - asked of a pool and in a transaction
: churn ( -- )  200 0 do  s" ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ" >string drop drop  <map> drop  loop ;   \ (freed memory used again)
: intact? ( v a n -- flag )  rot dup string? 0= if  drop 2drop false exit  then  string s= ;
: kept-value ( -- flag )
    s" ann" >string drop {: v :}  <map> {: m :}  v s" name" m s-at!
    s" SELECT name FROM t WHERE n = @name" m db sql-query drop  churn
    v s" ann" intact?  s" name" m s-at drop s" ann" intact? and ;
T{ ' kept-value run -> true }T
: kept-in-tx ( -- flag )
    s" ann" >string drop {: v :}  <map> {: m :}  v s" name" m s-at!
    [: {: tx :}  s" SELECT name FROM t WHERE n = @name" m tx sql-query drop ;] db sql-transaction  churn
    v s" ann" intact? ;
T{ ' kept-in-tx run -> true }T
testing a transaction whose rollback fails: its connection not lent again with the transaction open - rolled back on
testing  its return, or closed
true fail-rollback !
T{ [: ['] then-fails db sql-transaction 0 ;] run drop  -> }T
false fail-rollback !
T{ [: s" SELECT 42 AS answer" 0 db sql-query length ;] run  last-txn @ -> 1 0 }T
T{ [: s" SELECT 42 AS answer" 0 db sql-query length ;] run  last-txn @ -> 1 0 }T
testing a transaction begun by hand and left open: rolled back as its connection is given back
asked list-clear
T{ [: s" BEGIN TRANSACTION" 0 db sql-exec drop  s" SELECT 42 AS answer" 0 db sql-query length ;] run  last-txn @ -> 1 0 }T
T{ asked [: string s" ROLLBACK" s-find 0>= ;] any? -> true }T

testing several at once (sql-query-all): out together on as many connections as the pool has, the answers in order
config 3 sql-pool constant wide
: five ( -- )
    s" SELECT 42 AS answer" 0 sql-also  s" SELECT name FROM a" 0 sql-also  s" SELECT 42 AS answer" 0 sql-also
    s" SELECT name FROM b" 0 sql-also  s" SELECT 42 AS answer" 0 sql-also ;
: answers ( lists -- a n )   \ each list's first row's value, as text: "42 ann 42 ann 42"
    {: ls :}  [: ls [: {: rows :}  rows first {: r :}
        s" answer" r s-at if  %n  else  s" name" r s-at drop string %s  then  32 %c ;] each ;] build string ;
variable many
0 logins !
T{ [: ['] five wide sql-query-all rescue-all many !  0 ;] run  many @ answers s" 42 ann 42 ann 42 " s=  logins @ -> 0 true 3 }T
: one-fails ( -- )  s" SELECT 42 AS answer" 0 sql-also  s" SELECT * FROM fail" 0 sql-also  s" SELECT name FROM c" 0 sql-also ;
T{ [: ['] one-fails wide sql-query-all ;] run  s" Invalid object name" failed-with? -> -1 true }T
T{ [: ['] five wide sql-query-all length ;] run -> 5 }T   \ (its connections all given back)
: in-tx ( tx -- )  {: tx :}  ['] five tx sql-query-all rescue-all many ! ;
T{ [: ['] in-tx wide sql-transaction 0 ;] run  many @ answers s" 42 ann 42 ann 42 " s= -> 0 true }T   \ (one connection: in turn)

testing a route: the first login sent on to another server (Azure's gateway), followed
stand-in-port @ route-once !
0 logins !
config 1 sql-pool constant routed
T{ [: s" SELECT 42 AS answer" 0 routed sql-query length ;] run  logins @ -> 1 2 }T

testing TLS first (TDS 8.0's order): the handshake, its certificate checked by the host's name, then PRELOGIN, the
testing  login and a query inside it - a build with TLS (the stand-in behind tls-listen)
: tls-build? ( -- flag )  s" tls-listen" find-word 0<> ;
: start-tls ( -- )
    tls-build? 0= if  exit  then
    source-dir s" /tls/cert.pem" s+ drop {: cert :}  source-dir s" /tls/key.pem" s+ drop {: key :}
    cert key server send stand-in-start-tls  settle
    cert string s" tls-ca!" find-word execute ;
start-tls
: strict-config ( -- c )  s" localhost" s" sa" s" pw" s" stagingdb" sql-config  stand-in-tls-port @ swap sql-port ;
: strict-answer ( -- n )   \ (42: through TLS - or a build without it, which can't)
    tls-build? 0= if  42 exit  then
    [: s" SELECT 42 AS answer" 0 strict-config 1 sql-pool sql-query  first s" answer" rot s-at drop ;] run ;
T{ strict-answer -> 42 }T
: wrong-name ( -- flag )   \ (the certificate doesn't name 127.0.0.2: refused, said why)
    tls-build? 0= if  true exit  then
    [: s" 127.0.0.2" s" sa" s" pw" s" stagingdb" sql-config  stand-in-tls-port @ swap sql-port 1 sql-pool
        {: db :}  s" SELECT 1" 0 db sql-query ;] run drop  s" TLS: the certificate" failed-with? ;
T{ wrong-name -> true }T

test-summary

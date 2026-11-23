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

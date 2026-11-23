\ sql-check.fs - a first look at a SQL Server database, safely: connects, says where it is, what this login may open,
\ and (given a table) its columns as a model - every request in a transaction rolled back (sql-dry-run), nothing
\ written. It refuses a database whose name doesn't say "staging", and SQL naming any database whose name says
\ "prod".
\
\   SQL_PASSWORD=... clankforth $PWD/tools/sql-check.fs -- myserver.database.windows.net app_user stagingCRSql1 dbo.Orders
\
\ Exits 0 if all of it went, 1 if not (the reason said).
USING: actors net system json accessors sql sql.describe ;

: usage ( -- )
    ." usage: SQL_PASSWORD=... clankforth $PWD/tools/sql-check.fs -- HOST USER DATABASE [SCHEMA.TABLE] [PORT]" cr
    1 exit-with ;
: arg ( i -- a n )  args {: l :}  dup l length < if  l nth string  else  drop usage  s" "  then ;
: arg? ( i -- flag )  args length < ;
: contains-ci? ( a n a2 n2 -- flag )   \ a2 n2 inside a n, case ignored
    {: a n a2 n2 :}  a n s-lower  a2 n2 s-lower {: x xn y yn :}  x xn y yn s-find 0>= ;
: same-ci? ( a n a2 n2 -- flag )  {: a n a2 n2 :}  a n s-lower  a2 n2 s-lower s= ;

\ ---- the lock: staging only, before anything connects ----
: refuse ( a n -- )  ." refused: " type cr  1 exit-with ;
: preflight ( -- )
    2 arg? 0= if  usage  then
    2 arg s" staging" contains-ci? 0= if  s" the database's name doesn't say staging" refuse  then
    2 arg s" prod" contains-ci? if  s" the database's name says prod" refuse  then
    s" SQL_PASSWORD" getenv 0= if  s" SQL_PASSWORD isn't set" refuse  then  2drop ;
preflight
: make-config ( -- config )
    s" SQL_PASSWORD" getenv drop {: pa pn :}
    0 arg  1 arg  pa pn  2 arg  sql-config  sql-dry-run
    4 arg? if  4 arg string>number drop swap sql-port  then ;
make-config constant config
config 1 sql-pool constant db

\ the databases the server lists whose names say prod: forbidden in any SQL from here (a second lock)
: forbid-prod ( rows -- )
    [: {: r :}  s" name" r s-at drop string {: a n :}
        a n s" prod" contains-ci? if  a n config sql-forbid drop  ." (forbidden from here: " a n type ." )" cr  then ;] each ;
: show-access ( rows -- )
    ." databases this login may open:" cr
    [: {: r :}  ."   " s" name" r s-at drop string type  s" access" r s-at drop 1 = if  ."   yes"  else  ."   no"  then  cr ;] each ;

message check ( -- )
behaviour checker
    on check ( -- )
        [:
            ." connecting to " 0 arg type ."  as " 1 arg type ."  (TLS first, dry run) ..." cr
            s" SELECT DB_NAME() AS db, @@SERVERNAME AS server, SUSER_NAME() AS login, @@VERSION AS version" 0 db sql-one {: r :}
            ." database: " s" db" r s-at drop string type cr
            ." server:   " s" server" r s-at drop string type cr
            ." login:    " s" login" r s-at drop string type cr
            ." version:  " s" version" r s-at drop string 80 min type cr
            s" db" r s-at drop string 2 arg same-ci? 0= if  ." refused: the server says the database is another one" cr  1 exit-with  then
            db sql-database-access {: rows :}  rows forbid-prod  rows show-access
            3 arg? if  cr  3 arg db sql-describe type  then
            ." all went: nothing was written." cr  0 exit-with
        ;] catch if  ." failed: " error-message type cr  1 exit-with  then ;
end-behaviour

checker -1 s" checker" spawn constant checking
checking send check
serve

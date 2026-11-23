\ the sql vocabularies against a real SQL Server - CI's (SQL Server 2022 in a container: .github/workflows/ci.yml),
\ or one of your own you may write to. Not a ctest test: it needs a server, said in the environment -
\   SQL_HOST SQL_PORT SQL_USER SQL_PASSWORD   the server and a login that may create a table in SQL_DATABASE
\   SQL_DATABASE                              the database it works in (a table dbo.ClankThings made, dropped at the end)
\   SQL_OTHER                                 another database there (the lock's tests: never written)
\   SQL_CA                                    the certificates to trust (PEM; none: the system's)   SQL_PLAIN=1: no TLS
\   clankforth $PWD/tests/sql/live.fs
IN: test-sql-live
USING: tester actors net system json jsondb accessors sql sql.model sql.describe sql.watch ;
source-dir s" /asker.fs" s+ included
3000 run-ticks !   \ (a real server's login, over TLS: up to 30 s)

: env ( name n -- a n )  getenv 0= if  s" "  then ;
: need ( name n -- )  {: a n :}  a n getenv if  2drop  else  a n s" {s} isn't set: this test needs a server (see its first lines)" format type cr  1 exit-with  then ;
s" SQL_HOST" need  s" SQL_USER" need  s" SQL_PASSWORD" need  s" SQL_DATABASE" need  s" SQL_OTHER" need
: trust ( -- )  s" SQL_CA" getenv if  tls-ca!  then ;
trust
: live-config ( database n -- c )
    {: da dn :}  s" SQL_HOST" env  s" SQL_USER" env  s" SQL_PASSWORD" env  da dn  sql-config
    s" SQL_PORT" getenv if  string>number drop swap sql-port  then
    s" SQL_PLAIN" getenv if  2drop sql-plain  then ;
s" SQL_DATABASE" env live-config 2 sql-pool constant db
: col ( row a n -- v )  {: r a n :}  r map? 0= if  json-null exit  then  a n r s-at 0= if  json-null  then ;
: whole ( v scale -- n )   \ a float times scale, truncated (-1: no float)
    {: v sc :}  v json-float instance? if  v value>> sc s>f f* f>s  else  -1  then ;
: text= ( v a n -- flag )  rot dup string? if  string s=  else  drop 2drop false  then ;

testing logged in - TLS first (unless SQL_PLAIN), in the database configured
T{ [: s" SELECT DB_NAME() AS db" 0 db sql-one s" db" col s" SQL_DATABASE" env text= ;] run -> true }T
T{ [: s" IF OBJECT_ID('dbo.ClankThings') IS NOT NULL DROP TABLE dbo.ClankThings; CREATE TABLE dbo.ClankThings (Id int IDENTITY PRIMARY KEY, Name nvarchar(100) NOT NULL, Code varchar(20) NULL, Price decimal(10,2) NULL, Notes nvarchar(max) NULL, RowVer rowversion)" 0 db sql-exec drop 0 ;] run  failure type -> 0 }T

testing each type read: as the server sends it, as text where it is exact
: typed-sql ( -- a n )  s\" SELECT CAST(42 AS int) AS i, CAST(N'héllo €\xF0\x9F\x98\x80' AS nvarchar(20)) AS s, CAST(12.5 AS decimal(10,2)) AS d, CAST('2026-11-22T10:30:00.123' AS datetime2(3)) AS dt, CAST(1 AS bit) AS b, CAST(1.5 AS float) AS f, CAST('6F9619FF-8B86-D011-B42D-00C04FC964FF' AS uniqueidentifier) AS g, CAST(12.5 AS money) AS m, CAST('2026-11-22T10:30:00+01:00' AS datetimeoffset(0)) AS o, CAST(NULL AS int) AS n, CAST(0x0102 AS varbinary(10)) AS x, CAST('2026-11-22' AS date) AS dd, CAST('10:30:00' AS time(0)) AS t, CAST('2026-11-22 10:30:00' AS datetime) AS old, CAST('caf' + CHAR(233) AS varchar(10)) AS v, CAST(-7 AS bigint) AS big, CAST(200 AS tinyint) AS tiny, CAST(-3 AS smallint) AS small, CAST(1.25 AS real) AS r, CAST('2026-11-22 10:30' AS smalldatetime) AS sdt" ;
variable typed
: read-typed ( -- 0 )  typed-sql 0 db sql-one rescue-all typed !  0 ;
T{ ' read-typed run  failure type -> 0 }T
: typed? ( name n -- v )  typed @ -rot col ;
T{ s" i" typed? -> 42 }T
T{ s" s" typed? s\" héllo €\xF0\x9F\x98\x80" text= -> true }T
T{ s" d" typed? s" 12.50" text= -> true }T
T{ s" dt" typed? s" 2026-11-22T10:30:00.123" text= -> true }T
T{ s" b" typed? -> json-true }T
T{ s" f" typed? 2 whole  s" r" typed? 4 whole -> 3 5 }T
T{ s" g" typed? s" 6F9619FF-8B86-D011-B42D-00C04FC964FF" text= -> true }T
T{ s" m" typed? s" 12.5000" text= -> true }T
T{ s" o" typed? s" 2026-11-22T10:30:00+01:00" text= -> true }T
T{ s" n" typed? -> json-null }T
T{ s" x" typed? s" 0x0102" text= -> true }T
T{ s" dd" typed? s" 2026-11-22" text= -> true }T
T{ s" t" typed? s" 10:30:00" text= -> true }T
T{ s" old" typed? s" 2026-11-22T10:30:00.000" text= -> true }T
T{ s" v" typed? s\" café" text= -> true }T
T{ s" big" typed?  s" tiny" typed?  s" small" typed? -> -7 200 -3 }T
T{ s" sdt" typed? s" 2026-11-22T10:30:00" text= -> true }T

testing parameters of each kind; a long one (in pieces); a SQL error's text
: params-sql ( -- a n )  s" SELECT @i + 1 AS i, @s AS s, @f * 2 AS f, @b AS b, @n AS n" ;
: params-map ( -- m )  s\" {\"i\":41,\"s\":\"ünï\",\"f\":0.75,\"b\":true,\"n\":null}" json> ;
variable echoed
T{ [: params-sql params-map db sql-one rescue-all echoed !  0 ;] run  failure type -> 0 }T
T{ echoed @ s" i" col  echoed @ s" s" col s\" ünï" text=  echoed @ s" f" col 1 whole -> 42 true 1 }T
T{ echoed @ s" b" col  echoed @ s" n" col -> json-true json-null }T
: long-map ( -- m )   \ 5000 characters: 10000 bytes of UTF-16, past what goes in one piece
    <map> {: m :}  [: 5000 0 do  [char] y %c  loop ;] build s" s" m s-at!  m ;
T{ [: s" SELECT LEN(@s) AS n" long-map db sql-one s" n" col ;] run -> 5000 }T
T{ [: s" SELECT * FROM dbo.Nope" 0 db sql-query ;] run  s" Invalid object name 'dbo.Nope'." failed-with? -> -1 true }T

testing a model: insert (its key back), get, find, update checked by the rowversion (a stale one refused), delete
model thing dbo.ClankThings
    key: Id int identity
    column: Name nvarchar(100)
    column: Code varchar(20)
    column: Price decimal(10,2)
    column: Notes nvarchar(max)
    version: RowVer
end-model
variable id   variable read-row
T{ [: s\" {\"Name\":\"widget\",\"Code\":\"W-1\",\"Price\":\"9.99\"}" json> db thing-insert  dup id ! ;] run 0> -> true }T
T{ [: id @ db thing-get rescue-all read-row !  read-row @ s" Name" col s" widget" text= ;] run -> true }T
T{ [: s\" {\"where\":{\"Code\":\"W-1\",\"Price\":{\">=\":\"5\"}},\"sort\":[\"-Id\"],\"limit\":10}" db thing-find length ;] run -> 1 }T
T{ [: s\" {\"Name\":\"widget 2\"}" json> read-row @ db thing-update ;] run -> true }T
T{ [: s\" {\"Name\":\"stale\"}" json> read-row @ db thing-update ;] run -> false }T   \ (its version is the old one)
T{ [: id @ db thing-get s" Name" col s" widget 2" text= ;] run -> true }T
T{ [: s\" {\"where\":{\"Name\":{\"contains\":\"dget\"}}}" db thing-count ;] run -> 1 }T

testing sql-describe: the table as a model
create described 4000 allot   variable described-n
T{ [: s" dbo.ClankThings" db sql-describe {: a n :}  a described n move  n described-n !  0 ;] run -> 0 }T
: has? ( a n -- flag )  described described-n @ 2swap s-find 0>= ;
T{ s" key: Id int identity" has?  s" version: RowVer" has?  s" column: Notes nvarchar(max)" has?  s" column: Price decimal(10,2)" has? -> true true true true }T

testing a dry run: written and undone - the table as it was
s" SQL_DATABASE" env live-config sql-dry-run 1 sql-pool constant dry
T{ [: s\" {\"Name\":\"ghost\"}" json> dry thing-insert drop  s\" {\"where\":{\"Name\":\"ghost\"}}" db thing-count ;] run -> 0 }T
testing a transaction that throws: rolled back
: throws ( tx -- )  {: tx :}  s\" {\"Name\":\"rolled\"}" json> tx thing-insert drop  s" SELECT * FROM dbo.Nope" 0 tx sql-exec drop ;
T{ [: ['] throws db sql-transaction ;] run drop  [: s\" {\"where\":{\"Name\":\"rolled\"}}" db thing-count ;] run -> 0 }T

testing the lock: another database forbidden by name; a USE to it closes the session; a login to one not there fails
s" SQL_DATABASE" env live-config  s" SQL_OTHER" env rot sql-forbid 1 sql-pool constant guarded
: other-sql ( -- s len )  s" SQL_OTHER" env s" SELECT name FROM {s}.sys.tables" format ;
T{ [: other-sql 0 guarded sql-query ;] run  s" forbids" failed-with? -> -1 true }T
: use-other ( -- s len )  s" SQL_OTHER" env s" USE [{s}]" format ;
T{ [: use-other 0 db sql-exec ;] run  s" database changed" failed-with? -> -1 true }T
T{ [: s" SELECT DB_NAME() AS db" 0 db sql-one s" db" col s" SQL_DATABASE" env text= ;] run -> true }T
s" clank_no_such_database" live-config 1 sql-pool constant missing
T{ [: s" SELECT 1" 0 missing sql-query ;] run  s" login failed" failed-with? -> -1 true }T

testing a watcher: a write through the db told to a live view's kind of actor at once
db 500 sql-watcher constant watcher
variable told
message watch-things ( -- )
behaviour listener
    on watch-things ( -- )  thing watcher model-watch ;
    on db-changed ( c id rev json -- )  2drop 2drop  1 told +! ;
end-behaviour
listener -1 s" listener" spawn constant listening
listening send watch-things
: settle ( n -- )  0 ?do  10 net-poll drop  deliver  loop ;
100 settle
T{ told @ -> 0 }T
T{ [: s\" {\"Name\":\"watched\"}" json> db thing-insert drop 0 ;] run  100 settle  told @ -> 0 1 }T

testing done: the table dropped
T{ [: s" DROP TABLE dbo.ClankThings" 0 db sql-exec drop 0 ;] run -> 0 }T

test-summary

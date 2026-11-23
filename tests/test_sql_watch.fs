\ sql.describe and sql.watch (basis/sql): a table's catalog as a model declaration; tables watched - a change seen at
\ the next look, or at once when written through the db; db-changed as jsondb's, each watcher still alive told
IN: test-sql-watch
USING: tester actors net json jsondb accessors sql.tds sql sql.model sql.describe sql.watch ;
source-dir s" /sql/stand-in.fs" s+ included

: test-config ( -- c )  s" 127.0.0.1" s" sa" s" pw" s" stagingdb" sql-config sql-plain  stand-in-port @ swap sql-port ;
test-config 2 sql-pool constant db

testing sql-describe: a model to edit - the key (identity), types as SQL says them, NULL said, the rowversion
create described 2000 allot   variable described-n
: describe-it ( -- 0 )  s" dbo.Orders" db sql-describe {: a n :}  a described n move  n described-n !  0 ;
T{ ' describe-it run -> 0 }T
: has? ( a n -- flag )  described described-n @ 2swap s-find 0>= ;
T{ s" model orders dbo.Orders" has? -> true }T
T{ s" key: OrderId int identity" has? -> true }T
T{ s" column: Customer nvarchar(200)" has? -> true }T
T{ s" column: Code varchar(max)   \ NULL" has? -> true }T
T{ s" column: Total decimal(10,2)" has? -> true }T
T{ s" column: Placed datetime2(7)" has? -> true }T
T{ s" version: RowVer" has? -> true }T
T{ s" end-model" has? -> true }T
T{ asked last string s" OBJECT_ID(@t)" s-find 0>= -> true }T   \ (the table a parameter)

testing a watcher: told when a look finds the table changed (not at the first look); at once when written through the db
model order dbo.Orders
    key: OrderId int identity
    column: Customer nvarchar(200)
    version: RowVer
end-model
db 100 sql-watcher constant watcher
variable told   create told-table 40 allot   variable told-n
message watch-orders ( -- )
behaviour listener
    on watch-orders ( -- )  order watcher model-watch ;
    on db-changed ( c id rev json -- )
        2drop drop  string 40 min dup told-n !  told-table swap move  1 told +! ;
end-behaviour
listener -1 s" listener" spawn constant listening
: settle-ms ( n -- )  0 ?do  10 net-poll drop  deliver  10 +loop ;
1 checksum-answer !
listening send watch-orders
300 settle-ms
T{ told @ -> 0 }T   \ (looked at, unchanged)
T{ looks @ 1 > -> true }T
T{ asked last string s" MAX(CAST([RowVer] AS bigint)) AS c FROM [dbo].[Orders]" s-find 0>= -> true }T   \ (by the version)
2 checksum-answer !
300 settle-ms
T{ told @  told-table told-n @ s" dbo.Orders" s= -> 1 true }T
T{ 300 settle-ms  told @ -> 1 }T   \ (unchanged since: nothing more)
testing a write through the db: looked at at once
5000 to sql-timeout-ms
: write-one ( -- 0 )  3 checksum-answer !  s\" {\"Customer\":\"ann\"}" json> db order-insert drop  0 ;
T{ ' write-one run drop  50 settle-ms  told @ -> 2 }T
testing a watcher gone: forgotten, not sent to
listening kill
4 checksum-answer !
T{ 300 settle-ms  told @ -> 2 }T

test-summary

\ sql.describe and sql.watch (basis/sql): a table's catalog as a model declaration; tables watched - a change seen at
\ the next look, or at once when written through the db; db-changed as jsondb's, each watcher still alive told
IN: test-sql-watch
USING: tester actors net json jsondb accessors sql.tds sql sql.model sql.describe sql.watch vocabs.watchmsgs ;
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
testing a watch of some rows: its WHERE and parameters in the look (a seek on an index, not the table)
variable told-some
message watch-some ( -- )
behaviour some-listener
    on watch-some ( -- )  order s" CustomerId = @id" s\" {\"id\":7}" json> watcher model-watch-where ;
    on db-changed ( c id rev json -- )  2drop 2drop  1 told-some +! ;
end-behaviour
some-listener -1 s" some-listener" spawn constant some-listening
some-listening send watch-some
300 settle-ms
: some-look? ( -- flag )  asked [: string s" WHERE CustomerId = @id|@id bigint" s-find 0>= ;] any? ;
T{ some-look? -> true }T
5 checksum-answer !
T{ 300 settle-ms  told-some @ -> 1 }T
testing a round's looks go together: one request, a statement and a result set each; each watch's own parameters
variable told-items   variable told-lines
message watch-more ( -- )
behaviour more-listener
    on watch-more ( -- )
        s" dbo.Items" watcher sql-watch
        s" dbo.Lines" s" OrderId = @id AND Kind = @kind" s\" {\"id\":3,\"kind\":\"open\"}" json> watcher sql-watch-where ;
    on db-changed ( c id rev json -- )
        2drop drop {: c :}  c string s" dbo.Items" s= if  1 told-items +!  else  1 told-lines +!  then ;
end-behaviour
more-listener -1 s" more-listener" spawn constant more-listening
1 items-answer !
more-listening send watch-more
300 settle-ms
: hits ( a n a2 n2 -- count )   \ how many times the second is in the first
    {: a n a2 n2 | count at :}
    begin  a at +  n at -  a2 n2 s-find {: i :}  i 0>=  while  count 1+ to count  at i + n2 + to at  repeat  count ;
: statements ( s -- n )  string s" COUNT_BIG" hits ;
: round? ( s -- flag )  statements 4 = ;   \ (the four watches: Orders, Orders of a customer, Items, Lines of an order)
T{ asked [: round? ;] any? -> true }T
: a-round ( -- s )  asked [: round? ;] filter {: l :}  l last  l list-free ;
T{ a-round string s" FROM [dbo].[Items]" s-find 0>= -> true }T
\ (two watches' @id: two parameters, each declared, each in its own statement)
T{ a-round string s" WHERE CustomerId = @id_w1" s-find 0>=  a-round string s" WHERE OrderId = @id_w3 AND Kind = @kind_w3" s-find 0>= -> true true }T
T{ a-round string s" @id_w1 bigint" s-find 0>=  a-round string s" @id_w3 bigint" s-find 0>= -> true true }T
T{ a-round string s" @kind_w3 nvarchar" s-find 0>= -> true }T   \ (a text parameter: text still, as the request goes)
T{ told-items @ told-lines @  sql-watch-retries @ -> 0 0 0 }T   \ (and no request had to be made again, one look at a time)
testing each result set is its own watch's: the one that changed told, the others not
: only-items? ( -- flag )   \ a change of [Items] alone: its watcher told, no other
    told @ told-some @ told-lines @ told-items @ {: a b c d :}
    3 items-answer !  300 settle-ms
    told @ a =  told-some @ b = and  told-lines @ c = and  told-items @ d 1+ = and ;
T{ only-items? -> true }T
testing rounds leave nothing behind: the watcher holds what it held
\ (between rounds: mid-round it holds the request it waits on too - the fewest of some looks is it at rest)
: at-rest ( -- n )  watcher actor-objects  8 0 do  40 settle-ms  watcher actor-objects min  loop ;
: steady? ( -- flag )  at-rest {: n :}  500 settle-ms  at-rest n = ;
T{ steady? -> true }T
testing as many looks a request as sql-watch-batch says: the round in several
: after-here ( -- l )   \ the requests asked while 300 ms go by, those with a look in them
    asked length {: from :}  300 settle-ms
    asked from asked length subseq {: l :}  l [: statements 0> ;] filter  l list-free ;
: most-statements ( l -- n )  0 swap [: statements max ;] each ;
\ (a round on its way as the size changes lands first: its requests aren't the ones looked at)
: sizes-with ( batch -- most )  to sql-watch-batch  300 settle-ms  after-here {: l :}  l most-statements  l list-free ;
T{ 2 sizes-with  1 sizes-with  100 sizes-with -> 2 1 4 }T
testing a look that fails (no such table): its request's others looked at one at a time - told all the same
message watch-nope ( -- )
behaviour nope-listener
    on watch-nope ( -- )  s" dbo.Nope" watcher sql-watch ;
    on db-changed ( c id rev json -- )  2drop 2drop ;
end-behaviour
nope-listener -1 s" nope-listener" spawn constant nope-listening
nope-listening send watch-nope
300 settle-ms
: told-with-nope? ( -- flag )   \ (a round is now a request that fails and five that don't: longer)
    told-items @ {: d :}  4 items-answer !  1000 settle-ms  told-items @ d 1+ = ;
T{ told-with-nope?  sql-watch-retries @ 0> -> true true }T   \ (sql-watch-retries says so)
nope-listening kill  more-listening kill
300 settle-ms

testing sql-suffixed: a statement's parameters named apart - names in quotes, brackets and comments left, and @@names
: suffix-of ( sql n params-json n -- s )  json> {: p :}  p s" _w2" sql-suffixed drop ;
: suffixes-to? ( sql n json n want n -- flag )
    {: a n ja jn wa wn :}  [: a n ja jn suffix-of string wa wn s= ;] with-scratch ;
T{ s" Id = @id AND Kind = @kind" s\" {\"id\":7,\"kind\":2}" s" Id = @id_w2 AND Kind = @kind_w2" suffixes-to? -> true }T
T{ s" Id = @id OR Other = @id" s\" {\"id\":7}" s" Id = @id_w2 OR Other = @id_w2" suffixes-to? -> true }T
T{ s" Id = @ident AND N = @@ROWCOUNT AND X = @x" s\" {\"id\":7,\"ROWCOUNT\":1}" s" Id = @ident AND N = @@ROWCOUNT AND X = @x" suffixes-to? -> true }T
T{ s" Name = '@id' AND [@id] = @id -- @id" s\" {\"id\":7}" s" Name = '@id' AND [@id] = @id_w2 -- @id" suffixes-to? -> true }T
T{ s" a /* @id */ = @id" s\" {\"id\":7}" s" a /* @id */ = @id_w2" suffixes-to? -> true }T
: suffixed-params? ( -- flag )   \ the map: each name suffixed, its value the same
    [: s" Id = @id" s\" {\"id\":7,\"k\":\"x\"}" json> s" _w2" sql-suffixed nip {: p :}
       s" id_w2" p s-at drop 7 =  s" k_w2" p s-at drop string s" x" s= and  p map-size 2 = and ;] with-scratch ;
T{ suffixed-params? -> true }T

testing watchers gone: forgotten, not sent to - and a watch nobody watches dropped, not looked at
listening kill  some-listening kill
6 checksum-answer !
T{ 300 settle-ms  told @  told-some @ -> 3 1 }T   \ (the whole table's watch saw the 5 too, before)
looks @ constant looks-then
T{ 300 settle-ms  looks @ looks-then - -> 0 }T   \ (nothing watched: no looks - a serverless database may pause)

testing a round of several requests: out together, on as many of the pool's connections as it has - one whose
testing  answer is slow in coming holds up none of the others' asking (one after another, the round stopped at it);
testing  the round over when all have answered, each answer its own watch's
test-config 3 sql-pool constant wide
wide 500 sql-watcher constant wide-watcher
variable told-three
message watch-three ( -- )
behaviour three-listener
    on watch-three ( -- )   \ (the stand-in holds its answer to a request naming SLOW, till `release`)
        s" dbo.SLOW" wide-watcher sql-watch  s" dbo.A" wide-watcher sql-watch  s" dbo.Items" wide-watcher sql-watch ;
    on db-changed ( c id rev json -- )  2drop drop  string s" dbo.Items" s= if  1  else  100  then  told-three +! ;
end-behaviour
three-listener -1 s" three-listener" spawn constant three-listening
: slow? ( s -- flag )  string s" [SLOW]" s-find 0>= ;
: slow-asked ( -- n )  0  asked [: slow? if  1+  then ;] each ;
: until-slow ( n -- )  {: n :}  300 0 do  10 settle-ms  slow-asked n >= if leave then  loop  50 settle-ms ;
: release-it ( -- )  server send release  50 settle-ms ;
slow-asked constant slow-before
: looks-asked ( from -- n )  0  asked rot asked length subseq {: l :}  l [: statements if  1+  then ;] each  l list-free ;
asked length constant asked-before
1 to sql-watch-batch
three-listening send watch-three
slow-before 1+ until-slow  release-it   \ (its first look, as it is added: a request of its own)
slow-before 2 + until-slow              \ (a round: three requests - SLOW's held)
\ (six looks by now: one as each watch was added, and the round's three - the two others' asked while SLOW's is out;
\ one after another the round would have stopped at SLOW's: four)
T{ slow-asked slow-before -  asked-before looks-asked -> 2 6 }T
T{ told-three @ -> 0 }T
release-it  11 items-answer !
slow-before 3 + until-slow  release-it   \ (the next round, begun once that one was over)
T{ told-three @ -> 1 }T                 \ (the one that changed, and only it)
three-listening kill
100 to sql-watch-batch
600 settle-ms

: without-threads ( -- )  node 0= if  test-summary skip-file  then ;
without-threads
testing a watcher for another node's actors (one watcher, a server's nodes): theirs told across - not asked whether
testing  they are alive, which only their own node can say - and its table touched from there looked at at once
db 60000 sql-watcher constant far-watcher   \ (no round while this runs: only what is touched is looked at)
s" vocabs.nodewatch" start-node drop
: far-settle ( ms -- )  0 ?do  10 net-poll drop  deliver  10 +loop ;
: found ( a n -- addr )  {: a n :}  nobody  300 0 do  drop  a n whereis  dup nobody <> if leave then  10 far-settle  loop ;
s" far-page" found constant page
variable page-tolds
behaviour page-asker  on page-told ( n -- )  page-tolds ! ;  end-behaviour
page-asker -1 s" page-asker" spawn constant page-asking
: of-page ( watcher xt -- )   \ the page asked to (xt: send watch-it, or touch-it) about Items, through the watcher
    {: w xt :}  page-asking enter  s" dbo.Items" >string drop  w address  page xt execute  leave-actor ;
T{ far-watcher [: send watch-it ;] of-page  300 far-settle  9 items-answer !  300 far-settle  page-tolds @ -> 0 }T
T{ far-watcher [: send touch-it ;] of-page  300 far-settle  page-tolds @ -> 1 }T
testing ... and gone (its node ended): off every watch - a table only it watched is looked at no more
T{ looks @  300 far-settle  looks @ swap - -> 0 }T   \ (the quick watcher: nothing to look at, as before)
watcher [: send watch-it ;] of-page
T{ looks @  300 far-settle  looks @ swap - 0> -> true }T
T{ 10 items-answer !  300 far-settle  page-tolds @ -> 2 }T
page-asking enter  page send page-halt  leave-actor
500 far-settle
T{ looks @  300 far-settle  looks @ swap - -> 0 }T
T{ watcher alive?  far-watcher alive? -> true true }T

test-summary

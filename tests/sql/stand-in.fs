\ a stand-in for SQL Server: TDS on a port of this machine, plain - its answers made up from what it is asked (the
\ tests of sql and sql.model load it), and an actor that runs a test's code and keeps what it gave
USING: actors net json accessors sql.tds sql ;
\ ---- the stand-in server: what it was asked, kept to look at ----
list constant asked                       \ each request's SQL (and an RPC's declarations after a |)
variable login-database                   \ the database it says the login is in (a string)
variable fail-login                       \ true: the login refused
variable route-once                       \ a port: the first login routed there, once
variable logins
variable rowcount-answer  1 rowcount-answer !   \ what SELECT @@ROWCOUNT says
variable checksum-answer   \ a watcher's look: the c its count comes with
variable looks             \ how many looks a watcher made
: says-database ( a n -- )  >string drop rescue-all login-database ! ;
s" stagingdb" says-database

: le16, ( x b -- )  {: x b :}  x 255 and b b-push  x 8 rshift 255 and b b-push ;
: le32, ( x b -- )  {: x b :}  x b le16,  x 16 rshift b le16, ;
: le64, ( x b -- )  {: x b :}  x b le32,  x 32 rshift b le32, ;
: b-varchar, ( a n b -- )  {: a n b :}  a n utf16-length b b-push  a n b utf16, ;
: us-varchar, ( a n b -- )  {: a n b :}  a n utf16-length b le16,  a n b utf16, ;
: token-with-length ( kind xt b -- )   \ a token whose 16-bit length comes first: xt ( b -- ) writes its body
    {: kind xt b :}  <bytes> {: t :}  t xt execute  kind b b-push  t b-length b le16,  t b-data b b-append  t dispose ;
: done, ( count b -- )  {: n b :}  $fd b b-push  $10 b le16,  0 b le16,  n b le64, ;
: env-database, ( a n b -- )
    {: a n b :}  $e3 [: {: t :}  1 t b-push  a n t b-varchar,  0 t b-push ;] b token-with-length ;
: env-txn, ( kind descriptor b -- )
    {: kind d b :}  $e3 [: {: t :}  kind t b-push  kind 8 = if  8 t b-push  d t le64,  0 t b-push  else  0 t b-push  8 t b-push  d t le64,  then ;] b token-with-length ;
: loginack, ( b -- )
    $ad [: {: t :}  1 t b-push  $74000004 t le32,  s" stand-in" t b-varchar,  0 t le32, ;] rot token-with-length ;
: error, ( number a n b -- )
    {: num a n b :}  $aa [: {: t :}  num t le32,  1 t b-push  16 t b-push  a n t us-varchar,  s" srv" t b-varchar,  0 t b-push  1 t le32, ;] b token-with-length ;
: route, ( port b -- )
    {: port b :}  $e3 [: {: t :}  20 t b-push
        <bytes> {: v :}  0 v b-push  port v le16,  s" 127.0.0.1" v us-varchar,
        v b-length t le16,  v b-data t b-append  v dispose  0 t le16, ;] b token-with-length ;
: int-result, ( n name n b -- )   \ one int column, one row
    {: x na nn b :}
    $81 b b-push  1 b le16,  0 b le32,  1 b le16,  $26 b b-push  8 b b-push  na nn b b-varchar,
    $d1 b b-push  8 b b-push  x b le64,  1 b done, ;
: text-result, ( a n name n b -- )   \ one nvarchar column, one row
    {: a n na nn b :}
    $81 b b-push  1 b le16,  0 b le32,  1 b le16,  $e7 b b-push  100 b le16,
    $09 b b-push  $04 b b-push  $d0 b b-push  0 b b-push  $34 b b-push   \ (its collation)
    na nn b b-varchar,
    $d1 b b-push  a n utf16-length 2* b le16,  a n b utf16,  1 b done, ;

: le16@ ( a -- x )  {: a :}  a c@  a 1+ c@ 8 lshift or ;

\ a result set of any shape: its columns ( "name" kind ) - kind 0 int, 1 nvarchar, 2 bit - then its rows' values
0 constant int-col   1 constant text-col   2 constant bit-col
: column, ( a n kind b -- )
    {: a n kind b :}  0 b le32,  1 b le16,
    kind int-col = if  $26 b b-push  8 b b-push  then
    kind text-col = if  $e7 b b-push  400 b le16,  $09 b b-push  $04 b b-push  $d0 b b-push  0 b b-push  $34 b b-push  then
    kind bit-col = if  $68 b b-push  1 b b-push  then
    a n b b-varchar, ;
: value, ( v kind b -- )   \ (a text value a string object)
    {: v kind b :}
    kind int-col = if  8 b b-push  v b le64,  then
    kind text-col = if  v string utf16-length 2* b le16,  v string b utf16,  then
    kind bit-col = if  1 b b-push  v 0<> 1 and b b-push  then ;
\ the SQL a request carries: a batch's text; an RPC's statement and its declarations, "stmt|decls"
: request-text ( type a n -- s )
    {: type a n :}
    type sql-batch-type = if  a 22 + n 22 - utf16> drop exit  then
    a 22 + 6 + {: p :}   \ (past the headers, the procedure's number and flags)
    p 10 + le16@ {: sl :}  p 12 + sl utf16> drop {: stmt :}   \ (a parameter: name, status, type, length, collation, its length)
    p 12 + sl + {: q :}  q 10 + le16@ {: dl :}  q 12 + dl utf16> drop {: decls :}
    stmt string s" |" s+ decls string s+ drop ;


\ sys.columns of a made-up dbo.Orders: name type max_length precision scale nullable is_identity is_key
: describe-row, ( name n type n len prec scale null? ident? key? b -- )
    {: na nn ta tn len prec sc nul id key b :}
    $d1 b b-push  na nn >string drop text-col b value,  ta tn >string drop text-col b value,
    len int-col b value,  prec int-col b value,  sc int-col b value,  nul bit-col b value,  id bit-col b value,  key int-col b value, ;
: describe-answer, ( b -- )
    {: b :}  $81 b b-push  8 b le16,
    s" name" text-col b column,  s" type" text-col b column,  s" max_length" int-col b column,  s" precision" int-col b column,
    s" scale" int-col b column,  s" nullable" bit-col b column,  s" is_identity" bit-col b column,  s" is_key" int-col b column,
    s" OrderId" s" int" 4 10 0 false true 1 b describe-row,
    s" Customer" s" nvarchar" 400 0 0 false false 0 b describe-row,
    s" Code" s" varchar" -1 0 0 true false 0 b describe-row,
    s" Total" s" decimal" 9 10 2 false false 0 b describe-row,
    s" Placed" s" datetime2" 8 27 7 true false 0 b describe-row,
    s" RowVer" s" timestamp" 8 0 0 false false 0 b describe-row,
    5 b done, ;
: reply-to ( s b -- )   \ what the stand-in answers a request
    {: s b :}
    s string s" @@ROWCOUNT" s-find 0>= if  rowcount-answer @ s" n" b int-result, exit  then
    s string s" fail" s-find 0>= if  208 s" Invalid object name 'nope'." b error,  0 b done, exit  then
    s string s" USE " s-find 0>= if  s" prodCRSql1" b env-database,  0 b done, exit  then
    s string s" BEGIN TRANSACTION" s= if  8 $1122334455667788 b env-txn,  0 b done, exit  then
    s string s" COMMIT TRANSACTION" s= if  9 $1122334455667788 b env-txn,  0 b done, exit  then
    s string s" ROLLBACK" s-find 0>= if  10 $1122334455667788 b env-txn,  0 b done, exit  then
    s string s" UPDATE" s-find 0>= if  3 b done, exit  then
    s string s" SCOPE_IDENTITY" s-find 0>= if  99 s" id" b int-result, exit  then
    s string s" AS c FROM" s-find 0>= if   \ (a watcher's look: n and c)
        1 looks +!
        $81 b b-push  2 b le16,  s" n" int-col b column,  s" c" int-col b column,
        $d1 b b-push  5 int-col b value,  checksum-answer @ int-col b value,  1 b done, exit
    then
    s string s" sys.columns" s-find 0>= if  b describe-answer, exit  then
    s string s" COUNT_BIG" s-find 0>= if  5 s" n" b int-result, exit  then
    s string s" name" s-find 0>= if  s" ann" s" name" b text-result, exit  then
    42 s" answer" b int-result, ;

message stand-in-start ( -- )
variable stand-in-port
behaviour stand-in
    on stand-in-start ( -- )  0 listen-local listener-port stand-in-port ! ;
    on net-accepted ( listener conn -- )  2drop ;
    on net-received ( conn -- )
        {: c :}
        begin
            c net-input {: a n :}  a n message-end {: used :}
            used 0>
        while
            <bytes> {: p :}  a used p message-payload {: type :}  used c net-consume
            <bytes> {: out :}
            type prelogin-type = if  0 encrypt-not-supported 0 0 prelogin-payload b-data out b-append  then
            type login7-type = if
                1 logins +!
                fail-login @ if  18456 s" Login failed for user 'sa'." out error,  0 out done,
                else route-once @ if  route-once @ out route,  0 route-once !  out loginack,  0 out done,
                else  login-database @ string out env-database,  out loginack,  0 out done,  then then
            then
            type sql-batch-type =  type rpc-type = or if
                type p b-data request-text {: s :}  s string >string drop rescue-all asked push
                s out reply-to
            then
            reply-type out b-data 4096 packets {: pk :}  pk b-data c net-write
        repeat ;
    on net-closed ( conn -- )  drop ;
end-behaviour
stand-in -1 s" stand-in" spawn constant server
server send stand-in-start
: settle ( -- )  10 0 do  10 net-poll drop  deliver  loop ;
settle

source-dir s" /asker.fs" s+ included

: config ( -- c )   \ to the stand-in, plain, the database "stagingdb"
    s" 127.0.0.1" s" sa" s" pw" s" stagingdb" sql-config sql-plain  stand-in-port @ swap sql-port ;

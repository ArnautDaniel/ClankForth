\ model.fs - a table's words from its declaration (Factor's db.tuples, lighter): rows are maps, as `sql-query` gives
\ them; a model says the table's key, its columns and their types (the parameters are declared as the columns are,
\ so an index on a varchar column is used), and a rowversion column if it has one. It maps onto a schema that is
\ there - no tables made or dropped. `sql-describe` (sql.describe) writes one from the table itself.
\
\   model task dbo.Tasks
\       key: TaskId int identity
\       column: Title nvarchar(200)
\       column: Status varchar(20)
\       column: Priority int
\       version: RowVer
\   end-model
\
\   7 db task-get ( -- row|0 )
\   s\" {\"where\":{\"Status\":\"open\"},\"sort\":[\"-Priority\"],\"limit\":50}" db task-find ( -- rows )
\   changes row db task-update ( -- saved? )   \ false: someone else changed it since it was read
\
\ Queries are jsondb's shape (docs/jsondb.md): where - a column's value, null, or {"op": value} with = != < <= > >=
\ in contains exists - sort ("-" first: descending), offset, limit. Every column named is checked against the model:
\ a query can't reach a column (or a table) the model doesn't name.
IN: sql.model
USING: actors json accessors sql calendar ;

TUPLE: model-column  name type required not-null ;
\ a model: its name, its table ("dbo.Tasks"), its key column and whether the server numbers it (identity), its
\ columns (a list of model-column, the key first), its rowversion column (or 0); its hooks (xts, or 0): a form's
\ checks (validate:), and what follows a write (on-insert:, on-update:, on-delete: - an audit row's)
TUPLE: table-model  name table key identity columns version validate on-insert on-update on-delete ;

<PRIVATE
variable defining
: ?defining ( -- m )  defining @ ?dup 0= abort" sql.model: only between model and end-model" ;
: parsed ( -- s )  parse-name dup 0= abort" sql.model: a name or type expected" >string drop rescue-all ;
: add-column ( name type -- )  false false <model-column> rescue-all ?defining columns>> push ;
: hook-named ( "word" -- xt )   \ (a hook's word, defined before)
    parse-name 2dup find-word ?dup 0= if  s" sql.model: no word {s} (defined before the model)" format true -rot (abort")  then
    nip nip ;
: model-error ( a n -- )  true -rot (abort") ;
PRIVATE>

\ ---- declaring ----
: model ( "name" "table" -- )
    parsed {: name :}  parsed {: table :}
    name table 0 false list rescue-all 0  0 0 0 0  <table-model> rescue-all {: m :}
    m name string s" constant {s}" format evaluate
    m defining ! ;
: key: ( "column" "type" -- )  parsed {: c :}  parsed {: t :}  ?defining c >>key drop  c t add-column ;
: identity ( -- )  ?defining true >>identity drop ;     \ the key numbered by the server: an insert gives it back
: column: ( "column" "type" -- )  parsed {: c :}  parsed {: t :}  c t add-column ;
: version: ( "column" -- )  ?defining parsed >>version drop ;   \ a rowversion: updates and deletes checked by it
\ the column declared last can't be left empty in a form (model-form: "required")
: required ( -- )  ?defining columns>> last  true >>required drop ;
\ the text column declared last is NOT NULL: left empty in a form it is "" (else empty text is NULL)
: not-null ( -- )  ?defining columns>> last  true >>not-null drop ;
\ ( changes row errors -- ): a form's own checks, after its values' types - field-error says what is wrong
: validate: ( "word" -- )  ?defining hook-named >>validate drop ;
\ ( row key db -- ): after a row is put in - its db the write's (a transaction's: the audit row in it too)
: on-insert: ( "word" -- )  ?defining hook-named >>on-insert drop ;
\ ( changes row db -- ): after a row is saved (not when someone else's change refused it)
: on-update: ( "word" -- )  ?defining hook-named >>on-update drop ;
\ ( row db -- ): after a row is deleted
: on-delete: ( "word" -- )  ?defining hook-named >>on-delete drop ;

<PRIVATE
\ ---- SQL built (inside `build`) ----
: %name ( a n -- )   \ an identifier, bracketed ([Title]; a ] in it doubled)
    {: a n :}  a n s-bytes drop to a  [char] [ %c  n 0 ?do  a i + c@ {: c :}  c %c  c [char] ] = if  c %c  then  loop  [char] ] %c ;
: %table ( m -- )   \ dbo.Tasks as [dbo].[Tasks]
    table>> string {: a n :}  a n s-bytes drop to a
    a n s" ." s-find {: dot :}
    dot 0< if  a n %name exit  then
    a dot %name  [char] . %c  a dot + 1+  n dot - 1-  %name ;
: same-name? ( a n a2 n2 -- flag )   \ case ignored, as SQL Server's names
    {: a n a2 n2 :}  a n s-lower {: x xn :}  a2 n2 s-lower {: y yn :}  x xn y yn s=  x dispose  y dispose ;
: column-named ( a n m -- col )   \ the model's column of that name, else an error
    {: a n m :}
    m columns>> [: name>> string a n same-name? ;] find
    0= if  a n m table>> string s" sql.model: no column {s} in {s}" format model-error  then ;
: key-column ( m -- col )  {: m :}  m key>> string m column-named ;
: version-select ( m -- )   \ the version as a number: CAST([RowVer] AS bigint) AS [RowVer]
    version>> string {: a n :}  s" CAST(" %s  a n %name  s"  AS bigint) AS " %s  a n %name ;
: select-list ( m -- )
    {: m | first? :}  true to first?
    m columns>> [: name>> string  first? 0= if  s" , " %s  then  false to first?  %name ;] each
    m version>> if  s" , " %s  m version-select  then ;

\ parameters: named p1, p2 ... - each declared as its column is
task-variable params   task-variable types   task-variable pcount   \ (each handler its own)
: fresh ( -- )  <map> params !  <map> types !  0 pcount ! ;
: %param ( v type n -- )   \ a value as a parameter: @pN into the SQL, it and its type into the maps
    {: v ta tn :}  1 pcount +!  pcount @ s" p{}" format {: pa pn :}
    v pa pn params @ s-at!  ta tn >string drop rescue pa pn types @ s-at!   \ (made in the build: kept past it)
    [char] @ %c  pa pn %s  pa dispose ;
: base-type ( col -- a n )   \ its type's name before any (, lower-cased: "nvarchar"
    type>> string {: a n :}  a n s" (" s-find {: at :}  at 0< if  n to at  then  a at s-lower ;
: type-in? ( a n list n -- flag )  {: a n la ln :}  la ln s"  " s-split [: string a n s= ;] any? ;
: type-numbers ( col -- a b | -1 -1 )   \ the numbers in its type's (...): (200) 200 -1; (10,2) 10 2; (max) or none -1 -1
    type>> string {: a n :}  a n s-bytes drop to a  a n s" (" s-find {: open :}
    open 0< if  -1 -1 exit  then
    a open + 1+  n open - 1-  {: ia in :}  ia in s" )" s-find to in
    ia in s" ," s-find {: comma :}
    comma 0< if  ia in string>number 0= if  drop -1  then  -1 exit  then
    ia comma string>number drop  ia comma + 1+  in comma - 1-  string>number drop ;
\ a text's length as its column counts it: an n-type's in UTF-16 units (an emoji is two), another's in bytes
: text-units ( a n col -- u )
    {: a n col :}  a n s-bytes drop to a  col base-type {: t tn :}  t tn s-bytes drop to t  t c@ [char] n = if
        0  n 0 ?do  a i + c@ {: b :}  b $c0 and $80 <> if  1+  then  b $f0 >= if  1+  then  loop
    else  n  then ;
\ a value as a parameter of its column's type - but one the type would change (2.5 as an int, a text longer than
\ the column: the server would round or cut it, and match another row) by its own
: col-param ( v col -- )
    {: v col :}
    v json-float instance? if  col base-type s" int bigint smallint tinyint decimal numeric money smallmoney" type-in? if
        v s" float" %param exit
    then then
    v string? if  col type-numbers drop {: most :}  most 0> if  v string col text-units most > if
        v s" nvarchar(max)" %param exit
    then then then
    v col type>> string %param ;
\ a value to write: refused if its column can't hold it (the server would cut it, and say it saved)
: fits! ( v col -- )
    {: v col :}  v string? 0= if  exit  then  col type-numbers drop {: most :}
    most 0>  v string col text-units most > and if
        col name>> string col type>> string s" sql.model: {s} longer than its {s}" format model-error
    then ;
: like-pattern ( s -- s' )   \ contains: %text%, its % _ [ \ escaped by \
    string {: a n :}  a n s-bytes drop to a  [: [char] % %c  n 0 ?do  a i + c@ {: c :}
        c [char] % =  c [char] _ = or  c [char] [ = or  c [char] \ = or if  [char] \ %c  then  c %c  loop  [char] % %c ;] build ;
: op? ( a n op n -- flag )  s= ;

\ one condition on a column: its operator and value
: compare ( v op n col -- )
    {: v oa on col | first? :}
    col name>> string %name
    oa on s" exists" op? if  v json-true = if  s"  IS NOT NULL"  else  s"  IS NULL"  then  %s exit  then
    v json-null = if
        oa on s" =" op? 0=  oa on s" !=" op? 0= and if  oa on s" sql.model: {s} null - only = and != take null" format model-error  then
        oa on s" !=" op? if  s"  IS NOT NULL"  else  s"  IS NULL"  then  %s exit
    then
    oa on s" =" op? if  s"  = " %s  v col col-param exit  then
    oa on s" !=" op? if  s"  <> " %s  v col col-param exit  then
    oa on s" <" op?  oa on s" <=" op? or  oa on s" >" op? or  oa on s" >=" op? or if
        32 %c  oa on %s  32 %c  v col col-param exit
    then
    oa on s" in" op? if
        v list? 0= if  s" sql.model: in takes a list" model-error  then
        v empty? if  s"  IN (NULL)" %s exit  then   \ (true of no row)
        s"  IN (" %s  true to first?
        v [: {: x :}  first? 0= if  s" , " %s  then  false to first?  x col col-param ;] each
        [char] ) %c exit
    then
    oa on s" contains" op? if
        s"  LIKE " %s  v like-pattern rescue s" nvarchar(4000)" %param  s"  ESCAPE '\'" %s exit
    then
    oa on s" sql.model: no operator {s} (= != < <= > >= in contains exists)" format model-error ;
task-variable joined   \ a condition written already: the next after AND
: and-next ( -- )  joined @ if  s"  AND " %s  else  s"  WHERE " %s  then  true joined ! ;
: where-clause ( q m -- )
    {: q m :}  false joined !
    q map? 0= if  exit  then
    s" where" q s-at 0= if  exit  then {: w :}
    w map? 0= if  s" sql.model: where is an object" model-error  then
    w [: {: k v :}
        k string m column-named {: col :}
        v map? if  v [: {: op x :}  and-next  x op string col compare ;] map-each
        else  and-next  v s" =" col compare  then
    ;] map-each ;
: order-clause ( q m paged? -- )
    {: q m paged? | first? :}  true to first?
    q map? if  s" sort" q s-at  else  false  then
    if  {: l :}
        l [: string {: a n :}  a n s-bytes drop to a
            first? if  s"  ORDER BY " %s  else  s" , " %s  then  false to first?
            n 0>  a c@ [char] - = and {: desc? :}
            desc? if  a 1+ n 1-  else  a n  then  m column-named name>> string %name
            desc? if  s"  DESC" %s  then ;] each
    then
    first? paged? and if  s"  ORDER BY " %s  m key-column name>> string %name  then ;   \ (paging needs an order)
: number-of ( a n q -- n | -1 )   \ a whole number the query says (offset, limit), -1 if none
    {: a n q :}  q map? 0= if  -1 exit  then
    a n q s-at 0= if  -1 exit  then {: x :}
    x object? if  a n s" sql.model: {s} is a whole number" format model-error  then
    x 0< if  a n s" sql.model: {s} is 0 or more" format model-error  then
    x ;
: page-clause ( q -- )
    {: q :}  s" offset" q number-of {: off :}  s" limit" q number-of {: lim :}
    off 0< lim 0< and if  exit  then
    s"  OFFSET " %s  off 0 max %n  s"  ROWS" %s
    lim 0>= if  s"  FETCH NEXT " %s  lim %n  s"  ROWS ONLY" %s  then ;
: paged? ( q -- flag )  {: q :}  s" offset" q number-of 0>=  s" limit" q number-of 0>= or ;

: rows-of ( reply -- rows )  sets>> {: sets :}  sets empty? if  list  else  sets last rows>>  then ;
: the-value ( reply name n -- x )   \ a column of the reply's one row (an error if the server gave none)
    {: r a n :}  r sets>> empty? if  a n s" sql.model: the server's answer has no {s}" format model-error  then
    r sets>> last rows>> {: rows :}
    rows empty? if  a n s" sql.model: the server's answer has no {s}" format model-error  then
    a n rows first s-at 0= if  a n s" sql.model: the server's answer has no {s}" format model-error  then ;
: run-typed ( sql db -- reply )  {: s db :}  s string params @ types @ db sql-reply-typed ;
: touch ( m db -- )   \ the db's watcher told the table was written (sql.watch)
    {: m db | w :}  db sql-db-of watcher>> to w
    w nobody <> if  m table>> string >string drop w send sql-touched  then ;
: given ( a n row -- v )   \ a row's value of a column (null if it hasn't one)
    {: a n row :}  a n row s-at 0= if  json-null  then ;
: given-any-case ( a n row -- v )   \ ... its name in any case, as the columns' are
    {: a n row | v :}  json-null to v
    row [: {: k x :}  k string a n same-name? if  x to v  then ;] map-each  v ;
: version? ( a n m -- flag )  {: a n m :}  m version>> if  a n m version>> string same-name?  else  false  then ;
: writable? ( col m -- flag )   \ a column an insert or update may set: not the key the server numbers, not the version
    {: col m :}  col name>> string m key>> string same-name?  m identity>> and if  false exit  then
    m version>> if  col name>> string m version>> string same-name? 0=  else  true  then ;
: settable? ( col m -- flag )   \ ... an update: not the key at all
    {: col m :}  col m writable?  col name>> string m key>> string same-name? 0= and ;
\ the check that a row is as it was read: its key, and its version - or, without one, the old values of the columns
\ changed
: old-values ( row m -- map )   \ the columns the row has (the key and the version left to as-read): those checked
    {: row m | o :}  <map> to o   \ (a delete without a version: as it was read, not changed since)
    m version>> if  o exit  then
    row [: {: k x :}  m columns>> [: name>> string k string same-name? ;] any? if
        k string m key>> string same-name? 0= if  x k string o s-at!  then
    then ;] map-each  o ;
: as-read ( changes row m -- )
    {: changes row m :}
    s"  WHERE " %s  m key-column {: kc :}  kc name>> string %name  s"  = " %s  kc name>> string row given kc col-param
    m version>> if
        s"  AND CAST(" %s  m version>> string %name  s"  AS bigint) = " %s
        m version>> string row given s" bigint" %param exit
    then
    changes [: drop string m column-named {: col :}
        col name>> string row given {: old :}
        s"  AND " %s
        old json-null = if  col name>> string %name  s"  IS NULL" %s
        else  col name>> string %name  s"  = " %s  old col col-param  then ;] map-each ;
PRIVATE>

\ ---- the words a model has (and each its own, named after it: task-get ...) ----
\ the row of that key, or 0
: model-get ( key m db -- row | 0 )
    {: k m db :}  fresh
    [: s" SELECT " %s  m select-list  s"  FROM " %s  m %table
        s"  WHERE " %s  m key-column {: kc :}  kc name>> string %name  s"  = " %s  k kc col-param ;] build
    db run-typed rows-of {: rows :}  rows empty? if  0  else  rows first  then ;
\ the rows a query (jsondb's shape, its text) finds
: model-select ( q m db -- rows )   \ the query a map (json.query's)
    {: q m db :}  fresh
    s" limit" q number-of 0= if  list exit  then   \ (none wanted: FETCH NEXT 0 is no SQL)
    [: s" SELECT " %s  m select-list  s"  FROM " %s  m %table
        q m where-clause  q m q paged? order-clause  q page-clause ;] build
    db run-typed rows-of ;
: model-find ( query n m db -- rows )  {: qa qn m db :}  qa qn json> m db model-select ;   \ ... its JSON text
\ how many rows its where finds
: model-count-of ( q m db -- n )
    {: q m db :}  fresh
    [: s" SELECT COUNT_BIG(*) AS n FROM " %s  m %table  q m where-clause ;] build
    db run-typed s" n" the-value ;
: model-count ( query n m db -- n )  {: qa qn m db :}  qa qn json> m db model-count-of ;
\ a row put in: its columns the model has (an identity key and the version left to the server); its key
: model-insert ( row m db -- key )
    {: row m db | first? :}  fresh  true to first?
    [: s" INSERT INTO " %s  m %table
        list {: cols :}
        row [: drop string {: a n :}  a n m version? if  exit  then   \ (the version: the server's)
            a n m column-named {: col :}  col m writable? if  col cols push  then ;] map-each
        cols empty? if  s"  DEFAULT VALUES" %s
        else
            s"  (" %s  cols [: name>> string  first? 0= if  s" , " %s  then  false to first?  %name ;] each
            s" ) VALUES (" %s  true to first?
            cols [: {: col :}  first? 0= if  s" , " %s  then  false to first?
                col name>> string row given {: v :}  v col fits!  v col col-param ;] each
            [char] ) %c
        then
        m identity>> if  s" ; SELECT CAST(SCOPE_IDENTITY() AS bigint) AS id" %s  then ;] build
    db run-typed {: r :}
    m db touch
    m identity>> if  r s" id" the-value  else  m key>> string row given-any-case  then {: key :}
    m on-insert>> ?dup if  {: xt :}  row key db xt execute  then
    key ;
\ a row read before (its key and version, or the old values) changed as `changes` says (a map of columns to
\ values): true if it was saved - false if it had changed since it was read (or is gone)
: model-update ( changes row m db -- saved? )
    {: changes row m db | first? :}  fresh  true to first?
    changes [: drop string m column-named m settable? 0= if  s" sql.model: an update sets a column that isn't the key or the version" model-error  then ;] map-each
    changes keys empty? if  true exit  then
    [: s" UPDATE " %s  m %table  s"  SET " %s
        changes [: {: k v :}  k string m column-named {: col :}
            first? 0= if  s" , " %s  then  false to first?
            col name>> string %name  s"  = " %s  v col fits!  v col col-param ;] map-each
        changes row m as-read
        s" ; SELECT @@ROWCOUNT AS n" %s ;] build
    db run-typed s" n" the-value {: n :}
    m db touch
    n 0>  m on-update>> and if  changes row db m on-update>> execute  then
    n 0> ;
\ a row read before deleted: true if it was - false if it had changed since it was read (or is gone)
: model-delete ( row m db -- deleted? )
    {: row m db :}  fresh
    [: s" DELETE FROM " %s  m %table  row m old-values row m as-read  s" ; SELECT @@ROWCOUNT AS n" %s ;] build
    db run-typed s" n" the-value {: n :}
    m db touch
    n 0>  m on-delete>> and if  row db m on-delete>> execute  then
    n 0> ;

\ ---- a form: what a user typed, as the columns' values; what changed; what is wrong ----
\ a field's message in the errors (a map: field -> message), for a validate: word or the caller's own checks
: field-error ( msg n field n errors -- )  {: ma mn fa fn e :}  ma mn >string drop fa fn e s-at! ;

<PRIVATE
1 constant as-int   2 constant as-bit   3 constant as-decimal   4 constant as-float   5 constant as-text
6 constant as-date
4 constant money-scale
: class-of ( col -- class )
    base-type {: t tn :}
    t tn s" int bigint smallint tinyint" type-in? if  as-int exit  then
    t tn s" bit" s= if  as-bit exit  then
    t tn s" decimal numeric money smallmoney" type-in? if  as-decimal exit  then
    t tn s" float real" type-in? if  as-float exit  then
    t tn s" date datetime datetime2 smalldatetime datetimeoffset" type-in? if  as-date exit  then
    as-text ;
\ a date's text as seconds since 1970 (2026-11-23, 2026-11-23T10:00, the server's 2026-11-23T10:00:00.000): how two
\ dates are compared - the same moment however it is written
: date-seconds ( a n -- s true | false )  iso-date> ;
: empty-text ( col -- v )   \ an empty text field's value: "" for a NOT NULL column, else NULL
    not-null>> if  s" " >string drop  else  json-null  then ;
: digits? ( a n -- flag )   \ all decimal digits (none: true)
    {: a n :}  a n s-bytes drop to a  true  n 0 ?do  a i + c@ [char] 0 [char] 9 1+ within 0= if  drop false leave  then  loop ;
\ decimal text as one form ("-12.5": no +, no leading zeros, no trailing ones after the point) - for comparing
: decimal-form ( a n -- s len )
    {: a n | neg :}  a n s-bytes drop to a
    n if  a c@ [char] - =  a c@ [char] + = or if  a c@ [char] - = to neg  a 1+ to a  n 1- to n  then  then
    a n s" ." s-find {: dot :}  dot 0< if  n to dot  then
    a dot {: ia in :}  begin  in 1 >  if  ia c@ [char] 0 =  else  false  then  while  ia 1+ to ia  in 1- to in  repeat
    a dot + n dot - {: fa fn :}  fn if  fa 1+ to fa  fn 1- to fn  then
    begin  fn 0>  if  fa fn + 1- c@ [char] 0 =  else  false  then  while  fn 1- to fn  repeat
    [: neg  in 1 = ia c@ [char] 0 = and  fn 0= and  0= and if  [char] - %c  then
        ia in %s  fn if  [char] . %c  fa fn %s  then ;] build string ;
\ a decimal's text checked: its digits, how many after the point (scale) and before (precision - scale)
: decimal-ok? ( a n col -- msg n true | false )
    {: a0 n0 col | p sc a n dot ilen flen :}
    col base-type s" money smallmoney" type-in? if  19 to p  money-scale to sc  else  col type-numbers to sc to p  then
    sc 0< if  0 to sc  then
    a0 to a  n0 to n
    n if  a c@ [char] - =  a c@ [char] + = or if  a 1+ to a  n 1- to n  then  then   \ (its sign)
    a n s" ." s-find to dot  dot 0< if  n to dot  then
    dot to ilen  n dot - 1- 0 max to flen
    a ilen digits?  a dot + 1+ flen digits? and  ilen flen + 0> and
    dot n < if  flen 0> and  then
    0= if  s" a number" true exit  then
    flen sc > if  sc s" at most {} places after the point" format true exit  then
    p 0>  ilen p sc - > and if  p sc - s" at most {} digits before the point" format true exit  then
    false ;
\ the text as the column's value - trimmed; empty, NULL (a NOT NULL text column's: ""): the value, or the message
\ for what is wrong
: int-range ( col -- lo hi )   \ the whole numbers its type holds
    base-type {: t tn :}
    t tn s" tinyint" s= if  0 255 exit  then
    t tn s" smallint" s= if  -32768 32767 exit  then
    t tn s" int" s= if  -2147483648 2147483647 exit  then
    -9223372036854775807 1- 9223372036854775807 ;
\ a whole number as typed: a sign and digits only (not $10, %101, 'a': the Forth's), in its type's range
: whole-of ( a n col -- v true | msg n false )
    {: a n col | sa sn x :}  a n to sn to sa
    sn if  sa c@ [char] - =  sa c@ [char] + = or if  sa 1+ sn 1- digits?  sn 1 > and  else  sa sn digits?  then  else  false  then
    0= if  s" a whole number" false exit  then
    sn 20 > if  s" too large" false exit  then
    [: sa sn string>number drop to x ;] catch if  s" too large" false exit  then
    col int-range {: lo hi :}  x lo <  x hi > or if  lo hi s" from {} to {}" format false exit  then
    x true ;
: value-of ( a n col -- v true | msg n false )
    {: a0 n0 col | a n :}  a0 n0 s-trim to n to a
    col class-of {: cls :}
    n 0= if  col required>> if  s" required" false exit  then
        cls as-text = if  col empty-text  else  json-null  then  true exit
    then
    cls as-text = if   \ (its length checked)
        col type-numbers drop {: most :}
        most 0>  a n col text-units most > and if  most s" at most {} characters" format false exit  then
        a n >string drop true exit
    then
    cls as-date = if   \ (as typed: the server reads it - 2026-11-23, 2026-11-23T10:00)
        a n date-seconds 0= if  s" a date (2026-11-23)" false exit  then  drop
        a n >string drop true exit
    then
    cls as-int = if  a n col whole-of exit  then
    cls as-bit = if
        a n s-lower {: l ln :}
        l ln s" on true 1 yes" type-in? if  json-true true exit  then
        l ln s" off false 0 no" type-in? if  json-false true exit  then
        s" yes or no" false exit
    then
    cls as-decimal = if  a n col decimal-ok? if  false exit  then  a n >string drop true exit  then
    a n string>float 0= if  s" a number" false exit  then
    fdup fdup f- f0= 0= if  fdrop s" a number" false exit  then   \ (infinity, NaN: no number the server takes)
    <json-float> true ;
\ the form's value the same as the row's?
: same-value? ( new old col -- flag )
    {: v o col :}
    col class-of {: cls :}
    cls as-text = if   \ (NULL and "" the same: an empty field over a NULL changes nothing)
        v string? if  v string  else  s" "  then
        o string? if  o string  else  s" "  then  s= exit
    then
    cls as-bit = if   \ (NULL as no: "off" over a NULL changes nothing)
        v json-true =  o json-true = = exit
    then
    v json-null =  o json-null = or if  v o = exit  then
    cls as-date = if
        o string? 0= if  false exit  then
        v string date-seconds 0= if  false exit  then  o string date-seconds 0= if  drop false exit  then  = exit
    then
    cls as-decimal = if
        o string? 0= if  false exit  then
        v string decimal-form  o string decimal-form s= exit
    then
    cls as-float = if
        o json-float instance? 0= if  false exit  then
        v value>> o value>> f= exit
    then
    v string?  o string? and if  v string o string s= exit  then
    v o = ;
PRIVATE>

\ a form (a map of field names to the text typed: a live view's submit, form-params') against the row read before
\ (0: a new one): each column the form has - not the key the server numbers, nor the version - as its type's value;
\ the changes (those not the row's already) and the errors (field -> message: "a whole number", "required", "at most
\ 200 characters" ... and the model's validate: word's). No errors: the changes are what model-update takes.
: model-form ( form row m -- changes errors )
    {: form row m | changes errors :}  <map> to changes  <map> to errors
    m columns>> [: {: col :}
        col m writable? 0= if  exit  then
        col name>> string form s-at 0= if  exit  then {: text :}
        text string? 0= if  exit  then
        text string col value-of 0= if  col name>> string errors field-error  exit  then {: v :}
        row if  col name>> string row s-at  else  false  then  if  v swap col same-value? if  exit  then  then
        v col name>> string changes s-at!
    ;] each
    m validate>> ?dup if  {: xt :}  changes row errors xt execute  then
    changes errors ;

\ the model declared: its words, named after it - name-get name-find name-count name-insert name-update name-delete
: end-model ( -- )
    ?defining {: m :}
    m key>> 0= if  s" sql.model: a model needs a key: (its primary key)" model-error  then
    m name>> string {: a n :}
    a n a n s" : {s}-get ( key db -- row|0 ) {s} swap model-get ;" format evaluate
    a n a n s" : {s}-find ( query n db -- rows ) {s} swap model-find ;" format evaluate
    a n a n s" : {s}-count ( query n db -- n ) {s} swap model-count ;" format evaluate
    a n a n s" : {s}-select ( q db -- rows ) {s} swap model-select ;" format evaluate
    a n a n s" : {s}-count-of ( q db -- n ) {s} swap model-count-of ;" format evaluate
    a n a n s" : {s}-insert ( row db -- key ) {s} swap model-insert ;" format evaluate
    a n a n s" : {s}-update ( changes row db -- saved? ) {s} swap model-update ;" format evaluate
    a n a n s" : {s}-delete ( row db -- deleted? ) {s} swap model-delete ;" format evaluate
    0 defining ! ;

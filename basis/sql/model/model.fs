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
USING: actors json accessors sql ;

TUPLE: model-column  name type ;
\ a model: its name, its table ("dbo.Tasks"), its key column and whether the server numbers it (identity), its
\ columns (a list of model-column, the key first), its rowversion column (or 0)
TUPLE: table-model  name table key identity columns version ;

<PRIVATE
variable defining
: ?defining ( -- m )  defining @ ?dup 0= abort" sql.model: only between model and end-model" ;
: parsed ( -- s )  parse-name dup 0= abort" sql.model: a name or type expected" >string drop rescue-all ;
: add-column ( name type -- )  <model-column> rescue-all ?defining columns>> push ;
: model-error ( a n -- )  true -rot (abort") ;
PRIVATE>

\ ---- declaring ----
: model ( "name" "table" -- )
    parsed {: name :}  parsed {: table :}
    name table 0 false list rescue-all 0 <table-model> rescue-all {: m :}
    m name string s" constant {s}" format evaluate
    m defining ! ;
: key: ( "column" "type" -- )  parsed {: c :}  parsed {: t :}  ?defining c >>key drop  c t add-column ;
: identity ( -- )  ?defining true >>identity drop ;     \ the key numbered by the server: an insert gives it back
: column: ( "column" "type" -- )  parsed {: c :}  parsed {: t :}  c t add-column ;
: version: ( "column" -- )  ?defining parsed >>version drop ;   \ a rowversion: updates and deletes checked by it

<PRIVATE
\ ---- SQL built (inside `build`) ----
: %name ( a n -- )   \ an identifier, bracketed ([Title]; a ] in it doubled)
    {: a n :}  [char] [ %c  n 0 ?do  a i + c@ {: c :}  c %c  c [char] ] = if  c %c  then  loop  [char] ] %c ;
: %table ( m -- )   \ dbo.Tasks as [dbo].[Tasks]
    table>> string {: a n :}
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
variable params   variable types   variable pcount
: fresh ( -- )  <map> params !  <map> types !  0 pcount ! ;
: %param ( v type n -- )   \ a value as a parameter: @pN into the SQL, it and its type into the maps
    {: v ta tn :}  1 pcount +!  pcount @ s" p{}" format {: pa pn :}
    v pa pn params @ s-at!  ta tn >string drop rescue pa pn types @ s-at!   \ (made in the build: kept past it)
    [char] @ %c  pa pn %s  pa dispose ;
: col-param ( v col -- )  type>> string %param ;
: like-pattern ( s -- s' )   \ contains: %text%, its % _ [ \ escaped by \
    string {: a n :}  [: [char] % %c  n 0 ?do  a i + c@ {: c :}
        c [char] % =  c [char] _ = or  c [char] [ = or  c [char] \ = or if  [char] \ %c  then  c %c  loop  [char] % %c ;] build ;
: op? ( a n op n -- flag )  s= ;

\ one condition on a column: its operator and value
: compare ( v op n col -- )
    {: v oa on col | first? :}
    col name>> string %name
    oa on s" exists" op? if  v json-true = if  s"  IS NOT NULL"  else  s"  IS NULL"  then  %s exit  then
    v json-null = if
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
variable joined   \ a condition written already: the next after AND
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
        l [: string {: a n :}
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
: version? ( a n m -- flag )  {: a n m :}  m version>> if  a n m version>> string same-name?  else  false  then ;
: writable? ( col m -- flag )   \ a column an insert or update may set: not the key the server numbers, not the version
    {: col m :}  col name>> string m key>> string same-name?  m identity>> and if  false exit  then
    m version>> if  col name>> string m version>> string same-name? 0=  else  true  then ;
: settable? ( col m -- flag )   \ ... an update: not the key at all
    {: col m :}  col m writable?  col name>> string m key>> string same-name? 0= and ;
\ the check that a row is as it was read: its key, and its version - or, without one, the old values of the columns
\ changed
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
: model-find ( query n m db -- rows )
    {: qa qn m db :}  fresh  qa qn json> {: q :}
    [: s" SELECT " %s  m select-list  s"  FROM " %s  m %table
        q m where-clause  q m q paged? order-clause  q page-clause ;] build
    db run-typed rows-of ;
\ how many rows its where finds
: model-count ( query n m db -- n )
    {: qa qn m db :}  fresh  qa qn json> {: q :}
    [: s" SELECT COUNT_BIG(*) AS n FROM " %s  m %table  q m where-clause ;] build
    db run-typed s" n" the-value ;
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
            cols [: {: col :}  first? 0= if  s" , " %s  then  false to first?  col name>> string row given col col-param ;] each
            [char] ) %c
        then
        m identity>> if  s" ; SELECT CAST(SCOPE_IDENTITY() AS bigint) AS id" %s  then ;] build
    db run-typed {: r :}
    m db touch
    m identity>> if  r s" id" the-value  else  m key>> string row given  then ;
\ a row read before (its key and version, or the old values) changed as `changes` says (a map of columns to
\ values): true if it was saved - false if it had changed since it was read (or is gone)
: model-update ( changes row m db -- saved? )
    {: changes row m db | first? :}  fresh  true to first?
    changes [: drop string m column-named m settable? 0= if  s" sql.model: an update sets a column that isn't the key or the version" model-error  then ;] map-each
    changes keys empty? if  true exit  then
    [: s" UPDATE " %s  m %table  s"  SET " %s
        changes [: {: k v :}  k string m column-named {: col :}
            first? 0= if  s" , " %s  then  false to first?
            col name>> string %name  s"  = " %s  v col col-param ;] map-each
        changes row m as-read
        s" ; SELECT @@ROWCOUNT AS n" %s ;] build
    db run-typed s" n" the-value {: n :}
    m db touch
    n 0> ;
\ a row read before deleted: true if it was - false if it had changed since it was read (or is gone)
: model-delete ( row m db -- deleted? )
    {: row m db :}  fresh
    [: s" DELETE FROM " %s  m %table  <map> row m as-read  s" ; SELECT @@ROWCOUNT AS n" %s ;] build
    db run-typed s" n" the-value {: n :}
    m db touch
    n 0> ;

\ the model declared: its words, named after it - name-get name-find name-count name-insert name-update name-delete
: end-model ( -- )
    ?defining {: m :}
    m key>> 0= if  s" sql.model: a model needs a key: (its primary key)" model-error  then
    m name>> string {: a n :}
    a n a n s" : {s}-get ( key db -- row|0 ) {s} swap model-get ;" format evaluate
    a n a n s" : {s}-find ( query n db -- rows ) {s} swap model-find ;" format evaluate
    a n a n s" : {s}-count ( query n db -- n ) {s} swap model-count ;" format evaluate
    a n a n s" : {s}-insert ( row db -- key ) {s} swap model-insert ;" format evaluate
    a n a n s" : {s}-update ( changes row db -- saved? ) {s} swap model-update ;" format evaluate
    a n a n s" : {s}-delete ( row db -- deleted? ) {s} swap model-delete ;" format evaluate
    0 defining ! ;

\ describe.fs - what is in the database, read from its catalog: a table's columns as a model declaration to edit
\ (sql.model), the databases the login can open. Read-only: nothing here writes.
\
\   s" dbo.Tasks" db sql-describe type      \ model tasks dbo.Tasks / key: TaskId int identity / ... / end-model
\   db sql-database-access ( -- rows )      \ each database the server lists, and whether this login may open it
IN: sql.describe
USING: json accessors sql ;

<PRIVATE
: columns-sql ( -- a n )
    s" SELECT c.name AS name, TYPE_NAME(c.system_type_id) AS type, c.max_length AS max_length, c.precision AS precision, c.scale AS scale, c.is_nullable AS nullable, c.is_identity AS is_identity, CASE WHEN EXISTS (SELECT 1 FROM sys.indexes i JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id WHERE i.object_id = c.object_id AND i.is_primary_key = 1 AND ic.column_id = c.column_id) THEN 1 ELSE 0 END AS is_key FROM sys.columns c WHERE c.object_id = OBJECT_ID(@t) ORDER BY c.column_id" ;
: field ( row a n -- v )  rot s-at drop ;
: text-field ( row a n -- a n )  field string ;
: in? ( a n list n -- flag )   \ one of the names in a space-separated list
    {: a n la ln :}  la ln s"  " s-split [: string a n s= ;] any? ;
\ a column's type as SQL says it: nvarchar(200), varchar(max), decimal(10,2), datetime2(7) ...
: type-text ( row -- s len )
    {: r :}
    r s" type" text-field {: ta tn :}
    r s" max_length" field {: len :}
    ta tn s" char varchar binary varbinary" in? if
        len -1 = if  ta tn s" {s}(max)"  else  ta tn len s" {s}({})"  then  format exit
    then
    ta tn s" nchar nvarchar" in? if
        len -1 = if  ta tn s" {s}(max)"  else  ta tn len 2/ s" {s}({})"  then  format exit
    then
    ta tn s" decimal numeric" in? if  ta tn  r s" precision" field  r s" scale" field  s" {s}({},{})" format exit  then
    ta tn s" datetime2 time datetimeoffset" in? if  ta tn  r s" scale" field  s" {s}({})" format exit  then
    ta tn >string ;
: version-column? ( row -- flag )  s" type" text-field s" timestamp" s= ;
: key-column? ( row -- flag )  s" is_key" field 1 = ;
: model-name ( table n -- s len )   \ its last part, lower-cased: dbo.Tasks gives tasks
    {: a n :}  a n s" ." s-find {: dot :}  dot 0>= if  a dot + 1+  n dot - 1-  to n to a  then  a n s-lower ;
PRIVATE>

\ the table's columns as a model declaration (its first primary key column the key; a rowversion the version;
\ nullable columns said) - to paste into a file and edit. An error if there is no such table.
: sql-describe ( table n db -- s len )
    {: ta tn db | p keys keyed :}
    <map> to p  ta tn >string drop s" t" p s-at!
    columns-sql p db sql-query {: rows :}
    rows empty? if  ta tn s" sql.describe: no table {s} (or this login can't see it)" format true -rot (abort")  then
    rows [: key-column? ;] count to keys
    [:
        ta tn s" \ {s}, as the server describes it" format {: c cn :}  c cn %s  c dispose  10 %c
        ta tn model-name {: na nn :}  s" model " %s  na nn %s  32 %c  ta tn %s  10 %c
        keys 0= if  s"     \ no primary key: make the column that identifies a row key: (unique, never NULL)" %s  10 %c  then
        keys 1 > if  s"     \ a primary key of several columns: a model has one - the first is key: here" %s  10 %c  then
        rows [: {: r :}
            r version-column? if  s"     version: " %s  r s" name" text-field %s  10 %c  exit  then
            r key-column?  keyed 0= and if  true to keyed
                s"     key: " %s  r s" name" text-field %s  32 %c  r type-text %s
                r s" is_identity" field json-true = if  s"  identity" %s  then
                10 %c exit
            then
            s"     column: " %s  r s" name" text-field %s  32 %c  r type-text %s
            r s" nullable" field json-true = if  s"    \ NULL" %s  then
            10 %c
        ;] each
        s" end-model" %s  10 %c
    ;] build string ;

\ each database the server lists (an Azure SQL Database lists master and its own) and whether this login may open
\ it: rows of {"name": ..., "access": 1 or 0}
: sql-database-access ( db -- rows )
    {: db :}  s" SELECT name, HAS_DBACCESS(name) AS access FROM sys.databases ORDER BY name" 0 db sql-query ;

\ watch.fs - tables watched for changes, told as jsondb's are: `db-changed ( c id rev json -- )` to each watcher (c
\ the table's name; id, rev and json 0 - "something in it changed"), so a live view's changed: word works for either.
\ SQL Server pushes nothing a login without schema rights can turn on, so a watcher actor looks: every `ms` a
\ count and a checksum of each table watched (a model with a version: the highest version - its index, no scan),
\ and at once for a table written through the db it watches for (sql.model's words say so).
\
\   db 2000 sql-watcher constant watcher              \ (the db's writes told to it from now)
\   ... in a live view's mount:  task watcher model-watch      ( or: s" dbo.Tasks" watcher sql-watch )
\
\ Coarse by design: one notice per table per look, whatever changed in it - a view reloads what it shows.
\ Writes on another node (another process, the other project's) are seen at the next look.
IN: sql.watch
USING: actors net json jsondb accessors sql sql.model ;

message sql-watch-start ( db ms -- )
message sql-watch-add ( table version watcher -- )     \ table and version column: strings (version 0: none)
message sql-watch-remove ( table watcher -- )

<PRIVATE
\ a table watched: its name, its version column (or 0), what it looked like last ("" not yet), who watches it
TUPLE: watched  table version stamp watchers ;
state: sql-watch-state
    cell field sw-db
    cell field sw-ms
    cell field sw-tables   \ a list of watched (kept)
    cell field sw-timer
end-state
: %name ( a n -- )   \ an identifier, bracketed
    {: a n :}  [char] [ %c  n 0 ?do  a i + c@ {: c :}  c %c  c [char] ] = if  c %c  then  loop  [char] ] %c ;
: %table ( a n -- )   \ schema.table, each part bracketed
    {: a n :}  a n s" ." s-find {: dot :}
    dot 0< if  a n %name exit  then
    a dot %name  [char] . %c  a dot + 1+  n dot - 1-  %name ;
: look-sql ( w -- s )   \ what the table looks like now, as one row: n and c
    {: w :}
    [: s" SELECT COUNT_BIG(*) AS n, " %s
        w version>> if  s" MAX(CAST(" %s  w version>> string %name  s"  AS bigint)) AS c"
        else  s" CHECKSUM_AGG(BINARY_CHECKSUM(*)) AS c"  then  %s
        s"  FROM " %s  w table>> string %table ;] build ;
: stamp-of ( row -- s len )   \ "n:c" (c null for an empty table)
    {: r :}  s" n" r s-at drop  s" c" r s-at drop {: c :}  c json-null = if  0  else  c  then  s" {}:{}" format ;
: alive-watcher? ( id -- flag )  $ffffffff and alive? ;
: tell ( w -- )   \ each watcher still there told; those gone forgotten
    {: w :}
    w watchers>> [: alive-watcher? ;] filter {: live :}
    w watchers>> list-clear  live [: w watchers>> push ;] each
    w watchers>> [: {: id :}  w table>> string >string drop 0 0 0 id send db-changed ;] each ;
: look ( w -- )   \ the table looked at: its watchers told if it changed since the last look (not the first)
    {: w :}
    [: w look-sql string 0 sw-db @ sql-one ;] catch if  exit  then   \ (the server not there now: the next look)
    ?dup 0= if  exit  then
    stamp-of {: sa sn :}
    w stamp>> string sa sn s= if  sa dispose exit  then
    w stamp>> string nip 0> {: before? :}
    w stamp>> dispose  sa sn >string drop rescue-actor w swap >>stamp drop  sa dispose
    before? if  w tell  then ;
: watched-named ( a n -- w | 0 )
    {: a n :}  sw-tables @ [: table>> string a n s= ;] find 0= if  0  then ;
: look-again ( -- )  sw-ms @ after sw-timer ! ;
PRIVATE>

behaviour sql-watching
    on sql-watch-start ( db ms -- )  sw-ms !  sw-db !  list rescue sw-tables !  look-again ;
    on sql-watch-add ( table version watcher -- )
        {: t v id :}
        t string watched-named ?dup 0= if
            t rescue-actor  v ?dup if  rescue-actor  else  0  then  s" " >string drop rescue-actor  list rescue-actor
            <watched> rescue-actor  dup sw-tables @ push
            dup look   \ (how it looks now: what the next look is compared with)
        then
        {: w :}  id w watchers>> contains? 0= if  id w watchers>> push  then ;
    on sql-watch-remove ( table watcher -- )
        {: t id :}  t string watched-named ?dup if
            {: w :}  w watchers>> [: id <> ;] filter {: rest :}  w watchers>> list-clear  rest [: w watchers>> push ;] each
        then ;
    on sql-touched ( table -- )  string watched-named ?dup if  look  then ;
    on timer-fired ( timer -- )  drop  sw-tables @ [: look ;] each  look-again ;
end-behaviour

\ a watcher looking every ms at the tables watched through it, on the db's connections; the db's writes (sql.model)
\ told to it from now
: sql-watcher ( db ms -- watcher )
    {: db ms :}  sql-watching sql-watch-state s" sql-watch" spawn {: w :}
    db ms w send sql-watch-start
    db w >>watcher drop  w ;
\ this actor told of the table's changes (db-changed) - inside an actor (a live view's mount)
: sql-watch ( table n watcher -- )
    {: ta tn w :}  self 0< abort" sql-watch: inside an actor (its notices are messages)"
    ta tn >string drop 0 self w send sql-watch-add ;
: sql-unwatch ( table n watcher -- )  {: ta tn w :}  ta tn >string drop self w send sql-watch-remove ;
\ a model's table watched by its version (if it has one: its index read, not the table)
: model-watch ( m watcher -- )
    {: m w :}  self 0< abort" model-watch: inside an actor (its notices are messages)"
    m table>> string >string drop  m version>> ?dup if  string >string drop  else  0  then  self w send sql-watch-add ;

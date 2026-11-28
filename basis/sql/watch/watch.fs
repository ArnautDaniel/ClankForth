\ watch.fs - tables watched for changes, told as jsondb's are: `db-changed ( c id rev json -- )` to each watcher (c
\ the table's name; id, rev and json 0 - "something in it changed"), so a live view's changed: word works for either.
\ SQL Server pushes nothing a login without schema rights can turn on, so a watcher actor looks: every `ms` a
\ count and a checksum of what is watched (a model with a version: the highest version), and at once for a table
\ written through the db it watches for (sql.model's words say so).
\
\   db 2000 sql-watcher constant watcher              \ (the db's writes told to it from now)
\   ... in a live view's mount:
\   task watcher model-watch                          \ the whole table
\   company s" CompanyId = @id" params watcher model-watch-where   \ only its rows: an index's seek, not a scan
\
\ A round's looks go together: one request of many statements, a result set each (sql-watch-batch of them at most) -
\ a round trip for the round, not one a watch - and a round of several requests sends them at once, on as many of
\ the pool's connections as it has; the next round begins once all have answered. A watch whose look fails (a
\ table gone, a WHERE said wrong) fails the request it is in: those are then looked at one at a time, so only it
\ goes without.
\ A watch whose watchers are all gone is dropped: a database looked at by nobody can pause (Azure serverless).
\ A watcher may be on another node than the actors it tells (one watcher for a server's nodes: `self address` is
\ what they give it): theirs are monitored, and taken off every watch as they die or their node ends. Another
\ node's db tells it of its writes too: `watcher db >>watcher` there, with the watcher's address.
\ Coarse by design: one notice per watch per look, whatever changed in it - a view reloads what it shows. Writes
\ elsewhere (another node, another program) are seen at the next look.
IN: sql.watch
USING: actors net json jsondb accessors sql sql.model supervisor ;

message sql-watch-start ( db ms -- )
message sql-watch-add ( table version where params watcher -- )   \ strings (version, where, params' JSON: 0 none)
message sql-watch-remove ( table watcher -- )

\ a round's looks sent as one request, at most (a request takes 2100 parameters: each watch's WHERE has its own)
100 value sql-watch-batch
\ the requests of several looks that failed, their looks then made one at a time: it goes up every round while a
\ watch can't be looked at (and the round costs a round trip a watch again) - worth a look when a page is slow
variable sql-watch-retries
<PRIVATE
\ a watch: its table, its version column (or 0), its WHERE (or 0) and its parameters (JSON text, or 0), what it
\ looked like last ("" not yet), who watches it
TUPLE: watched  table version where params stamp watchers ;
state: sql-watch-state
    cell field sw-db
    cell field sw-ms
    cell field sw-watches   \ a list of watched (kept)
end-state
: %name ( a n -- )   \ an identifier, bracketed
    {: a n :}  a n s-bytes drop to a  [char] [ %c  n 0 ?do  a i + c@ {: c :}  c %c  c [char] ] = if  c %c  then  loop  [char] ] %c ;
: %table ( a n -- )   \ schema.table, each part bracketed
    {: a n :}  a n s-bytes drop to a  a n s" ." s-find {: dot :}
    dot 0< if  a n %name exit  then
    a dot %name  [char] . %c  a dot + 1+  n dot - 1-  %name ;
: look-sql ( w -- s )   \ what the watch looks like now, as one row: n and c
    {: w :}
    [: s" SELECT COUNT_BIG(*) AS n, " %s
        w version>> if  s" MAX(CAST(" %s  w version>> string %name  s"  AS bigint)) AS c"
        else  s" CHECKSUM_AGG(BINARY_CHECKSUM(*)) AS c"  then  %s
        s"  FROM " %s  w table>> string %table
        w where>> if  s"  WHERE " %s  w where>> string %s  then ;] build ;
: stamp-of ( row -- s len )   \ "n:c" (c null when nothing is there)
    {: r :}  s" n" r s-at drop  s" c" r s-at drop {: c :}  c json-null = if  0  else  c  then  s" {}:{}" format ;
: here? ( addr -- flag )  node-of node = ;
\ one on this node: asked; one on another can't be - it is monitored (its death told: `died`), and there till then
: alive-watcher? ( addr -- flag )  dup here? if  $ffffffff and alive?  else  drop true  then ;
: without ( addr l -- )   \ the list without it
    {: id l :}  l [: id <> ;] filter {: rest :}  l list-clear  rest [: l push ;] each ;
: prune ( w -- any? )   \ its watchers still there kept; any left?
    {: w :}
    w watchers>> [: alive-watcher? ;] filter {: live :}
    w watchers>> list-clear  live [: w watchers>> push ;] each
    w watchers>> empty? 0= ;
: tell ( w -- )
    {: w :}  w prune drop
    w watchers>> [: {: id :}  w table>> string >string drop 0 0 0 id send db-changed ;] each ;
: params-of ( w -- map|0 )  params>> ?dup if  string json>  else  0  then ;
: seen ( row w -- )   \ how it looks now: its watchers told if that isn't how it looked last (not the first time)
    {: r w :}
    r stamp-of {: sa sn :}
    w stamp>> string sa sn s= if  sa dispose exit  then
    w stamp>> string nip 0> {: before? :}
    w stamp>> dispose  sa sn >string drop rescue-actor w swap >>stamp drop  sa dispose
    before? if  w tell  then ;
: look ( w -- )   \ looked at, by a request of its own
    {: w :}
    [: w look-sql string  w params-of  sw-db @ sql-one ;] catch if  exit  then   \ (the server not there now: the next look)
    ?dup 0= if  exit  then
    w seen ;
\ the looks of some watches as one request: a statement each, in their order - each one's parameters named with its
\ place (two watches' @id are two parameters). (The statements are gathered, then joined: what is made inside
\ `build` goes as it ends, and the parameters' values must last till the request is sent.)
: looks-of ( ws -- sql params )
    {: ws | all parts :}  <map> to all  list to parts
    ws [: {: w k :}
        w params-of {: p :}
        p if
            k s" _w{}" format {: xa xn :}
            w look-sql string p xa xn sql-suffixed {: s p2 :}
            s parts push  p2 [: {: key v :}  v key string all s-at! ;] map-each
        else  w look-sql parts push  then ;] each-index
    parts s\" ;\n" s-join drop  all ;
: one-by-one ( ws -- )   \ (one alone has no others to tell from: it goes without till the next round)
    {: ws :}  ws length 1 > if  1 sql-watch-retries +!  ws [: look ;] each  then ;
: answered ( reply|0 ws -- )   \ a request's answer: a result set for each of its watches - or, it failed, one at a time
    {: r ws :}
    r 0= if  ws one-by-one exit  then
    r sets>> {: sets :}
    sets length ws length <> if  ws one-by-one exit  then
    ws [: {: w k :}  k sets nth rows>> {: rows :}  rows empty? 0= if  rows first w seen  then ;] each-index ;
: batches ( -- l )   \ the watches, sql-watch-batch of them to a request
    sw-watches @ {: all | at :}  list
    begin  at all length <  while
        all  at  at sql-watch-batch 1 max + all length min  subseq over push
        at sql-watch-batch 1 max + to at
    repeat ;
\ every watch looked at: the round's requests out together, on as many of the pool's connections as it has (one
\ after another they took a round trip each); over when all have answered - the handler waits, so the next round,
\ and a table touched meanwhile, come after. What the round made is freed with it. (No connection to be had: the
\ next round.)
: look-round ( -- )
    [: batches {: bs :}
       [: [: bs [: looks-of {: s p :}  s string p sql-also ;] each ;] sw-db @ sql-reply-all ;] catch if  exit  then
       {: rs :}  bs [: {: ws k :}  k rs nth ws answered ;] each-index ;] with-scratch ;
: same-text? ( s|0 s|0 -- flag )   \ both none, or the same text
    {: x y :}  x 0=  y 0= and if  true exit  then  x 0=  y 0= or if  false exit  then  x string y string s= ;
: watch-of ( table version where params -- w | 0 )
    {: t v wh p :}
    sw-watches @ [: {: w :}  w table>> t same-text?  w where>> wh same-text? and  w params>> p same-text? and ;] find
    0= if  0  then ;
: kept ( s|0 -- s|0 )  ?dup if  rescue-actor  else  0  then ;
: kept-watches ( l -- )  {: l :}  sw-watches @ ?dup if  dispose  then  l rescue sw-watches ! ;
: let-go ( w -- )   \ a watch dropped: what it kept freed
    {: w :}  w table>> dispose  w version>> ?dup if  dispose  then  w where>> ?dup if  dispose  then
    w params>> ?dup if  dispose  then  w stamp>> dispose  w watchers>> dispose  w dispose ;
PRIVATE>

behaviour sql-watching
    on sql-watch-start ( db ms -- )  sw-ms !  sw-db !  list rescue sw-watches !  sw-ms @ after drop ;
    on sql-watch-add ( table version where params watcher -- )
        {: t v wh p id :}
        t v wh p watch-of ?dup 0= if
            t kept  v kept  wh kept  p kept  s" " >string drop rescue-actor  list rescue-actor
            <watched> rescue-actor  dup sw-watches @ push
            dup look   \ (how it looks now: what the next look is compared with)
        then
        {: w :}
        id here? 0= if  [: id monitor ;] catch if  exit  then  then   \ (its node ended already: nobody to tell)
        id w watchers>> contains? 0= if  id w watchers>> push  then ;
    on sql-watch-remove ( table watcher -- )
        {: t id :}  sw-watches @ [: {: w :}  w table>> t same-text? if  id w watchers>> without  then ;] each ;
    on died ( id reason -- )   \ (a watcher on another node, or its node: off every watch)
        drop {: id :}  sw-watches @ [: {: w :}  id w watchers>> without ;] each ;
    on sql-touched ( table -- )
        {: t :}  sw-watches @ [: {: w :}  w table>> string t string s= if  w look  then ;] each ;
    on timer-fired ( timer -- )   \ (watches nobody watches dropped - a database looked at by none may pause)
        drop
        list {: keep :}  sw-watches @ [: {: w :}  w prune if  w keep push  else  w let-go  then ;] each
        keep kept-watches
        look-round
        sw-ms @ after drop ;
end-behaviour

\ a watcher looking every ms at what is watched through it, on the db's connections; the db's writes (sql.model)
\ told to it from now
: sql-watcher ( db ms -- watcher )
    {: db ms :}  sql-watching sql-watch-state s" sql-watch" spawn {: w :}  w essential
    db ms w send sql-watch-start
    db w >>watcher drop  w ;
<PRIVATE
: ?actor ( -- )  self 0< abort" sql.watch: inside an actor (its notices are messages)" ;
: params-text ( params|0 -- s|0 )  ?dup if  >json drop  else  0  then ;
: text-or-0 ( a n -- s|0 )  dup if  >string drop  else  2drop 0  then ;
PRIVATE>
\ this actor told of the table's changes (db-changed) - inside an actor (a live view's mount)
: sql-watch ( table n watcher -- )
    {: ta tn w :}  ?actor  ta tn >string drop 0 0 0 self address w send sql-watch-add ;
\ ... of its rows a WHERE finds - `CompanyId = @id`, its parameters a map (or 0): only those looked at (with an index
\ on CompanyId, a seek). Values in the WHERE go as parameters, as a query's.
: sql-watch-where ( table n where n params watcher -- )
    {: ta tn wa wn p w :}  ?actor  ta tn >string drop 0 wa wn text-or-0 p params-text self address w send sql-watch-add ;
: sql-unwatch ( table n watcher -- )  {: ta tn w :}  ta tn >string drop self address w send sql-watch-remove ;
\ a model's table watched by its version (if it has one: its index read, not the table)
: model-watch ( m watcher -- )
    {: m w :}  ?actor
    m table>> string >string drop  m version>> ?dup if  string >string drop  else  0  then  0 0 self address w send sql-watch-add ;
: model-watch-where ( m where n params watcher -- )
    {: m wa wn p w :}  ?actor
    m table>> string >string drop  m version>> ?dup if  string >string drop  else  0  then
    wa wn text-or-0 p params-text self address w send sql-watch-add ;

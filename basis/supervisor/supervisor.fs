\ supervisor.fs - supervision by default: an errors actor for the node, so an actor whose handler fails lives on (the
\ failure logged, as a JSON line to stdout) instead of dying of it - the way a server stays up when one request's
\ code has a bug. An actor that keeps failing (restart-max times in restart-window-ms) is killed - unless it is
\ essential (a pool, a watcher, a listener: one the program can't do without), which lives on whatever.
\
\   supervise                     \ (http-listen, sql-pool and sql-watcher do it themselves)
\   pool-actor essential          \ never killed for failing
\
\ Each failure is a line: {"time": ..., "level": "error", "event": "actor-failed", "actor": "sql-pool",
\ "message": "timer-fired", "error": "...", "chain": "...", "node": 0}; one killed for failing too often another,
\ "actor-killed". The failed actor is sent start-over (handled if it can put itself right; dropped if not).
IN: supervisor
USING: actors system json logging ;

5 value restart-max              \ failures within restart-window-ms an actor that isn't essential may have ...
10000 value restart-window-ms    \ ... before it is killed

message supervisor-start ( -- )
message essential-actor ( id -- )

<PRIVATE
state: supervisor-state
    cell field sv-recent      \ each actor's failures' times, recent ones (a map: id -> list of ms; kept)
    cell field sv-essential   \ the actors never killed for failing (a list; kept)
end-state

: failure-fields ( r -- m )   \ what a failure's record says, as a log line's fields
    {: r | m :}  <map> to m
    r failure-who >string drop s" actor" m s-at!
    r failure-what >string drop s" message" m s-at!
    r failure-error >string drop s" error" m s-at!
    r failure-chain >string drop s" chain" m s-at!
    node s" node" m s-at!
    m ;
: recent ( id -- l )   \ its failures' times, those within the window (a kept list, the map's)
    {: id | l now :}  ms-now to now
    id sv-recent @ at if  to l
    else  list rescue-actor to l  l id sv-recent @ at!  id monitor  then   \ (watched: forgotten as it goes)
    l [: now swap - restart-window-ms < ;] filter {: kept :}
    l list-clear  kept [: l push ;] each
    l ;
: forget ( id -- )  {: id :}  id sv-recent @ key? if  id sv-recent @ at drop dispose  id sv-recent @ delete-at  then ;
: start ( -- )  <map> rescue-actor sv-recent !  list rescue-actor sv-essential ! ;
: failed ( r -- )
    {: r | id :}  r failure-actor to id
    s" error" s" actor-failed" r failure-fields log-event
    id nobody = if  exit  then   \ (a task's: nothing to put right)
    id $ffffffff and alive? 0= if  id forget exit  then
    id sv-essential @ contains? if  id send start-over exit  then
    id recent {: times :}  ms-now times push
    times length restart-max > if
        <map> {: m :}  r failure-who >string drop s" actor" m s-at!  times length s" failures" m s-at!
        s" error" s" actor-killed" m log-event
        id forget  id kill exit
    then
    id send start-over ;
PRIVATE>

behaviour supervising
    on supervisor-start ( -- )  start ;
    on actor-failed ( record -- )  failed ;
    on died ( id reason -- )  drop  forget ;
    on essential-actor ( id -- )  {: id :}  id sv-essential @ contains? 0= if  id sv-essential @ push  then ;
end-behaviour

\ this node's errors actor, if it has none: from here an actor whose handler fails lives on, the failure logged
: supervise ( -- )
    errors-actor nobody <> if  exit  then
    supervising supervisor-state s" supervisor" spawn {: s :}
    self 0< if  s enter  start  leave-actor   \ (from the interpreter: ready at once)
    else  s send supervisor-start  then
    s errors-to ;
\ the actor never killed for failing (supervise'd first)
: essential ( id -- )  supervise  errors-actor send essential-actor ;

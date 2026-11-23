\ logging.fs - a log file that is an actor: lines sent it from any actor, on any node (register its name), each with
\ the time it came; those that come together written together; the file rotated as it grows (log, log.1 ... log.N).
\   s" logs/site.log" log-open constant logger        \ (at a program's top, a node's vocabulary's)
\   s" signed in: 7" logger log
\ (http.logging: a line for each request a router answers)
IN: logging
USING: actors system calendar json ;

10485760 value log-max-bytes   \ the file rotated once it is this long (10 MiB)
5 value log-keep               \ the old files kept: log.1 (the newest) to log.5

message log-line ( s -- )      \ to a logger: a line to write
message log-flush ( -- )       \ to itself: the lines come since written

<PRIVATE
10 constant newline
state: log-state
    cell field lg-path       \ its file's path (a string)
    cell field lg-file       \ the file, open to write at its end (system's), 0 while it isn't
    cell field lg-pending    \ lines not yet written (bytes)
    cell field lg-bytes      \ the file's length
    cell field lg-flushing   \ a log-flush sent itself, not yet come
end-state

: numbered ( i -- s len )  lg-path @ string rot s" {s}.{}" format ;   \ log.i
: rotate ( -- )   \ log.N let go, each log.i log.(i+1), the log log.1, a new log
    lg-file @ ?dup if  file-close  0 lg-file !  then
    log-keep numbered {: la ln :}  la ln file-exists? if  la ln delete-file  then
    log-keep 1- {: k :}
    begin  k 0>  while   \ (log.(N-1) to log.N first, down to log.1 to log.2)
        k numbered {: fa fn :}  fa fn file-exists? if  fa fn  k 1+ numbered  rename-file  then
        k 1- to k
    repeat
    log-keep 0> if  lg-path @ string  1 numbered  rename-file  else  lg-path @ string delete-file  then
    lg-path @ string file-open-append lg-file !  0 lg-bytes ! ;
: flush ( -- )   \ what came written (a write that fails: said, and the lines let go - a log doesn't stop the site)
    {: | b :}  false lg-flushing !  lg-pending @ to b
    b b-length 0= if  exit  then
    [: b b-data lg-file @ file-write ;] catch if  ." logging: " error-message type cr
    else  b b-length lg-bytes +!  then
    b b-clear
    lg-bytes @ log-max-bytes >  if  ['] rotate catch if  ." logging: rotating: " error-message type cr  then  then ;
: add ( s -- )   \ a line with its time; written once those come with it are in
    {: s | b :}  lg-pending @ to b
    time-now >iso-date {: t tn :}  t tn b b-append  t dispose
    32 b b-push  s string b b-append  newline b b-push
    lg-flushing @ 0= if  true lg-flushing !  self send log-flush  then ;
: last-words ( -- )  flush  lg-file @ ?dup if  file-close  0 lg-file !  then ;
PRIVATE>

behaviour logger
    on log-line ( s -- )  add ;
    on log-flush ( -- )  flush ;
    on killed ( -- )  last-words ;
end-behaviour

<PRIVATE
: folder-made ( path n -- )   \ the folder the file is in made, if it isn't there (one level)
    {: pa pn | at :}  -1 to at
    pn 0 ?do  pa i + c@ [char] / =  if  i to at  then  loop
    at 0> if  pa at dir? 0= if  pa at make-dir  then  then ;
: start ( path n -- )
    {: pa pn :}
    pa pn >string drop rescue-actor lg-path !  <bytes> rescue-actor lg-pending !
    pa pn folder-made
    pa pn file-exists? if  pa pn file-size  else  0  then  lg-bytes !
    pa pn file-open-append lg-file ! ;
PRIVATE>

\ a logger: its file (made if it isn't there - its folder too - written at its end) - an actor here. From the
\ interpreter. One that can't open its file: the error, no logger left.
: log-open ( path n -- logger )
    {: pa pn | id :}
    self 0< 0= abort" log-open: from the interpreter (a program's top, a node's vocabulary)"
    logger log-state s" logger" spawn to id
    id enter  [: pa pn start ;] catch  leave-actor
    if  error-message >string drop {: e :}  id kill  true e string (abort")  then
    id ;
\ a line to the log (its time put before it): from any actor, any node; from the interpreter too
: log ( a n logger -- )  {: a n l :}  a n >string drop  l send log-line ;

\ ---- JSON lines to stdout: what a container's log collector reads (Azure's Log Analytics, Docker's ...) ----
\ the map's fields as one JSON line, a "time" first (ISO 8601, UTC) - written whole, from any actor or the interpreter
: log-json ( m -- )
    {: m | out :}  <map> to out
    time-now >iso-date drop  s" time" out s-at!
    m [: {: k v :}  v k string out s-at! ;] map-each
    out >json {: s sn :}  s sn s\" \n" s+ {: line ln :}  line ln type
    s dispose  line dispose  out dispose ;
\ a line saying what happened: its level ("info", "warning", "error"), its event's name, more fields (a map, or 0)
: log-event ( level n event n fields -- )
    {: la ln ea en fields | m :}  <map> to m
    la ln >string drop s" level" m s-at!  ea en >string drop s" event" m s-at!
    fields if  fields [: {: k v :}  v k string m s-at! ;] map-each  then
    m log-json  m dispose ;

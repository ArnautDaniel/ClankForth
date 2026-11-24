\ logging (basis/logging): lines with their times, those that come together written together, the file rotated
IN: test-logging
USING: tester actors system logging ;

: wipe ( a n -- )  {: a n :}  a n file-exists? if  a n delete-file  then ;
: logs ( -- a n )  s" clankforth-test.log" ;
: old ( i -- a n )  logs rot s" {s}.{}" format ;
logs wipe  1 old wipe  2 old wipe  3 old wipe
logs log-open constant the-log
: settle ( -- )  20 0 do  deliver  1 sleep-ms  loop ;   \ (the log's writes done on a worker: time for them)

testing lines: each with its time (ISO 8601, UTC), written as they come
s" first" the-log log  s" second" the-log log
settle
: has? ( a n -- flag )  {: a n :}  logs read-file {: s sn :}  s sn a n s-find 0>  s dispose ;
: time-first? ( -- flag )  logs read-file {: s sn :}  s sn s" T" s-find 10 =  s dispose ;
T{ s" Z first" has?  s" Z second" has?  time-first? -> true true true }T
T{ logs read-file nip -> 20 1 + 5 + 1 +  20 1 + 6 + 1 + + }T   \ (each line: the time, a space, the text, a newline)
testing the file rotated as it grows: the old ones numbered, so many kept
60 to log-max-bytes  2 to log-keep
: lines ( n -- )  0 ?do  s" a line of some length" the-log log  settle  loop ;
6 lines
T{ 1 old file-exists?  2 old file-exists?  3 old file-exists? -> true true false }T
T{ logs file-size 60 < -> true }T
testing a logger killed: what came before written, the file closed
10485760 to log-max-bytes
s" last" the-log log  settle  the-log kill
T{ s" last" has? -> true }T
logs wipe  1 old wipe  2 old wipe

testing a log in a folder not there: made; one whose file can't be opened: the error, no logger left
: in-folder ( -- a n )  s" clankforth-test-logs/site.log" ;
in-folder log-open constant foldered
T{ s" clankforth-test-logs" dir? -> true }T
foldered kill  in-folder delete-file  s" clankforth-test-logs" delete-file
actors-alive constant alive-before
T{ s" no-such-dir/deeper/x.log" ' log-open catch nip nip  actors-alive alive-before - -> -1 0 }T
T{ self -> -1 }T   \ (the interpreter not left in the actor)

test-summary

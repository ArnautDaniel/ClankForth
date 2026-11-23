\ an actor that runs a test's code (sql's words wait for their answers: inside an actor) and keeps what it gave
USING: actors net ;
\ ---- an asker: runs code in an actor, keeps what it gave (or the error) ----
message ask ( xt -- )
variable result   variable done   create why 200 allot   variable why-n
behaviour asker
    on ask ( xt -- )
        catch ?dup if  drop  error-message 200 min  dup why-n !  why swap move  -1 result !
        else  result !  0 why-n !  then  true done ! ;
end-behaviour
asker -1 s" asker" spawn constant asking
variable run-ticks  300 run-ticks !   \ how long `run` waits, in 10 ms turns
: run ( xt -- x )   \ xt ( -- x ) run in the asker, its answer waited for (-1 if it threw: `failure` says why)
    0 done !  asking send ask
    run-ticks @ 0 do  10 net-poll drop  deliver  done @ if  leave  then  loop  result @ ;
: failure ( -- a n )  why why-n @ ;
: failed-with? ( a n -- flag )  failure 2swap s-find 0>= ;

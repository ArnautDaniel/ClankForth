\ logging.fs - the requests a router answers, a line each in a log (logging's): the client's address, the method, the
\ path, the status, how long it took.
\   s" logs/access.log" log-open constant access   access site access-log
IN: http.logging
USING: http logging system ;

<PRIVATE
s" log-started" >string drop constant started-key
: note-start ( req logger -- true )  drop {: r :}  us-now started-key string r req-attr!  true ;
: log-request ( req logger -- )
    {: r l | us :}
    started-key string r req-attr if  us-now swap - 1000 /  else  0  then  to us
    r client-address  r req-method  r req-path  r req-status  us  s" {s} {s} {s} {} {}ms" format {: s n :}
    s n l log  s dispose ;
PRIVATE>

\ a line for each request the router answers: the client's address, the method, the path, the status, the time it
\ took (its befores' and handler's) - the first hook added, so it times the rest
: access-log ( logger router -- )
    {: l rt :}  ['] note-start l rt before-each-with  ['] log-request l rt after-each-with ;

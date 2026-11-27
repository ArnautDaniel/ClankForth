\ test.fs - a live view tested without a browser or a socket: its page mounted here, events sent it as the
\ browser's would be, and what it shows read back - each region's HTML, the whole body, its state, a redirect.
\
\   USING: tester json http.live http.live.test ;
\   0 counter live-open                          \ mounted (params: a map, or 0)
\   T{ s" count" live-region s" <p>0</p>" s= -> true }T
\   json-null s" inc" live-do                    \ an event, its value (json-null, a form's map ...)
\   T{ s" count" live-region s" <p>1</p>" s= -> true }T
\   T{ s" count" tested-state json-int@ -> 1 true }T
\   live-close
\
\ One page at a time. A push (live-tell, a timer, a database's change) is seen with live-look after it.
IN: http.live.test
USING: actors json http.live ;

<PRIVATE
message lt-open ( params view -- )
message lt-do ( name value -- )
message lt-look ( -- )
shared-variable seen   \ the last answer (a string: its JSON), kept (one page tested at a time: the test's own)
variable the-page  \ the page actor
variable done
2000 constant turns-max   \ (deliveries to wait for an answer, at most)
: keep-seen ( s -- )  seen @ ?dup if  dispose  then  string >string drop rescue-all seen ! ;
behaviour lt-driver
    on lt-open ( params view -- )
        {: p v :}  live-page live-state s" live-test" spawn {: pg :}  pg the-page !
        p v pg send live-start  await live-html drop
        self pg send live-test-look  await live-test-seen keep-seen  true done ! ;
    on lt-do ( name value -- )
        {: s v :}  s v self the-page @ send live-test-event  await live-test-seen keep-seen  true done ! ;
    on lt-look ( -- )  self the-page @ send live-test-look  await live-test-seen keep-seen  true done ! ;
end-behaviour
lt-driver -1 s" live-test-driver" spawn constant driver
: wait-answer ( -- )
    turns-max 0 do  done @ if  unloop exit  then  deliver  loop
    true abort" live.test: no answer from the page (its handler failed? the error is above)" ;
: answer ( -- m )   \ the last answer, read (a map, in the caller's region)
    seen @ 0= abort" live.test: no page open (live-open)"  seen @ string json> ;
PRIVATE>

\ the view's page mounted with those params (a map - as a route's :names and query give them - or 0), drawn
: live-open ( params view -- )
    {: p v :}  false done !  p 0= if  <map> to p  then  p v driver send lt-open  wait-answer ;
\ an event the page's script would send: its name, its value (json-null, a string, a form's map of fields)
: live-do ( value name n -- )
    {: v na nn :}  false done !  na nn >string drop  v driver send lt-do  wait-answer ;
\ the page drawn again as it is now (after a push: live-tell, a timer, a watched collection's change)
: live-look ( -- )  false done !  driver send lt-look  wait-answer ;
\ what the page shows: a region's HTML (inside its <div id=...>), the whole body, its state, where it was sent
: live-region ( id n -- a n )
    {: ia in :}  s" regions" answer json-map@ 0= if  s" " exit  then  {: r :}  ia in r json-string@ 0= if  s" "  then ;
: live-body ( -- a n )  s" html" answer json-string@ 0= if  s" "  then ;
: tested-state ( -- m )  s" state" answer json-map@ 0= if  <map>  then ;
: live-redirected ( -- a n )  s" redirect" answer json-string@ 0= if  s" "  then ;   \ ("" none)
: tested-page ( -- addr )  the-page @ address ;   \ (for live-tell)
\ the page gone (its actor killed: what it watched let go)
: live-close ( -- )  the-page @ ?dup if  dup alive? if  kill  else  drop  then  then  0 the-page !
    seen @ ?dup if  dispose  0 seen !  then ;

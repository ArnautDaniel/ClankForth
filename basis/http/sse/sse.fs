\ sse.fs - server-sent events: an answer that stays open, an event sent down it whenever there is one - what an
\ actor hears, pushed to a browser (`new EventSource(url)`).
\   : news ( req -- )  {: r :}  r respond-events
\       begin  1000 await-within-ms headline if  {: s :}  s string s" headline" r send-event  s dispose  then
\       r req-gone? until ;
IN: http.sse
USING: http ;

\ the answer begun: 200, text/event-stream, not cached; events after it (send-event), the handler waiting between
: respond-events ( req -- )
    {: r :}  s" text/event-stream" r content-type!  s" no-cache" s" Cache-Control" r header!  200 r respond-start ;

\ an event: its name ("" for a plain message) and its data, each line of it a data: line
\ the data's line ends as LF, each: CR LF and a CR alone are line ends to a browser too (else data: could end early
\ and a line after it read as a field - an event: of the sender's)
: lf-lines ( a n -- s len )
    {: a n :}  [: n 0 ?do  a i + c@ {: c :}
        c 13 = if  i 1+ n <  if  a i + 1+ c@ 10 <>  else  true  then  if  10 %c  then  else  c %c  then  loop ;] build string ;
: send-event ( data n name n req -- )
    {: a n k kn r | b lines :}
    k kn crlf-free? 0= abort" send-event: a CR, LF or NUL in the event's name"
    <bytes> to b
    kn if  s" event: " b b-append  k kn b b-append  10 b b-push  then
    a n lf-lines {: la ln :}
    10 char>string {: nl nln :}  la ln nl nln s-split to lines  nl dispose  la dispose
    lines [: string  s" data: " b b-append  b b-append  10 b b-push ;] each
    10 b b-push
    b b-data r respond-chunk
    lines [: dispose ;] each  lines list-free  b dispose ;

\ a comment line: nothing to the browser, but it keeps the connection from looking idle to what is between
: send-ping ( req -- )  {: r :}  s\" : ping\n\n" r respond-chunk ;

\ files.fs - static files: a folder served under a path, each file with its type, Last-Modified, and a 304 for a
\ browser that has it already.
\   s" public" s" /static" site serve-files      \ GET /static/css/site.css: public/css/site.css
\   s" report.pdf" r respond-file                 \ (in a handler)
IN: http.files
USING: net http system calendar accessors ;

\ a file's content type, by its name's ending (application/octet-stream for one not known)
: mime-type ( path n -- a n )
    {: p pn | dot :}  -1 to dot
    pn 0 ?do  p i + c@ [char] . = if  i to dot  then  loop
    dot 0< if  s" application/octet-stream" exit  then
    p pn dot 1+ pn dot - 1- s-slice s-lower {: e en :}
    e en s" html" s=  e en s" htm" s= or if  s" text/html; charset=utf-8"
    else e en s" css" s= if  s" text/css; charset=utf-8"
    else e en s" js" s=  e en s" mjs" s= or if  s" text/javascript; charset=utf-8"
    else e en s" json" s= if  s" application/json"
    else e en s" txt" s=  e en s" md" s= or  e en s" fs" s= or if  s" text/plain; charset=utf-8"
    else e en s" svg" s= if  s" image/svg+xml"
    else e en s" png" s= if  s" image/png"
    else e en s" jpg" s=  e en s" jpeg" s= or if  s" image/jpeg"
    else e en s" gif" s= if  s" image/gif"
    else e en s" webp" s= if  s" image/webp"
    else e en s" ico" s= if  s" image/x-icon"
    else e en s" wasm" s= if  s" application/wasm"
    else e en s" woff2" s= if  s" font/woff2"
    else e en s" woff" s= if  s" font/woff"
    else e en s" pdf" s= if  s" application/pdf"
    else e en s" xml" s= if  s" application/xml"
    else e en s" mp4" s= if  s" video/mp4"
    else e en s" webm" s= if  s" video/webm"
    else e en s" mp3" s= if  s" audio/mpeg"
    else  s" application/octet-stream"
    then then then then then then then then then then then then then then then then then then then
    e dispose ;

\ the file as the answer: its bytes, its type by its name, Last-Modified; 304 (no body) to a request whose
\ If-Modified-Since is as late; 404 for no file there
: respond-file ( path n req -- )
    {: p pn r | mt b :}
    p pn file-exists? 0=  p pn dir? or if  r not-found exit  then
    p pn file-modified to mt
    mt >http-date {: d dn :}  d dn s" Last-Modified" r header!  d dispose
    s" if-modified-since" r req-header if
        http-date> if  mt >= if   \ (the browser's copy as late as the file)
            304 r answer-head to b  s\" \r\n" b b-append  b b-data r req-conn net-write  b dispose  exit
        then then
    then
    p pn mime-type r content-type!
    p pn read-file {: s n :}  s n 200 r respond  s dispose ;

<PRIVATE
\ what the path asks for, within the folder: no segment . or .., no backslash or NUL - false for one that would
\ reach outside it. Nor a hidden one (.git, .env: a deploy's leavings, not the site's), but .well-known (RFC 8615:
\ ACME's challenges, security.txt)
: hidden? ( a n -- flag )  {: a n :}  n 0>  if  a c@ [char] . =  else  false  then  a n s" .well-known" s= 0= and ;
: bad-segment? ( a n -- flag )  {: a n :}  a n s" .." s=  a n s" ." s= or  a n s" \" s-find 0>= or  a n hidden? or ;
: safe? ( a n -- flag )
    {: a n | parts ok :}  true to ok
    n 0 ?do  a i + c@ 0= if  false to ok  then  loop
    a n s" /" s-split to parts
    parts [: string bad-segment? if  false to ok  then ;] each
    parts [: dispose ;] each  parts list-free  ok ;
: serve-from-folder ( req -- )   \ serve-files' route: the folder its data, the rest of the path its *
    {: r | b :}
    s" *" r req-param drop {: a n :}
    a n safe? 0= if  r not-found exit  then
    <bytes> to b  r req-data string b b-append  [char] / b b-push  a n b b-append
    b b-data dir? if  n if  [char] / b b-push  then  s" index.html" b b-append  then
    b b-data r respond-file  b dispose ;
PRIVATE>

\ GET (and HEAD) of prefix/... answered by the file of that path in the folder (a folder's index.html)
: serve-files ( dir n prefix n router -- )
    {: d dn p pn rt :}
    p pn s" /*" s+ {: pat patn :}
    s" GET" pat patn ['] serve-from-folder  d dn >string drop rescue-all  rt route-with
    pat dispose ;

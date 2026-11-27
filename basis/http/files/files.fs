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

1048576 value http-file-whole-max   \ a file this long or shorter is read whole, and gzipped as any answer is (text,
                                    \ for a client that takes it); a longer one goes a piece at a time, as it is

<PRIVATE
65536 constant piece-bytes   \ a file sent a piece at a time: each this long
0 constant range-none        \ no Range said - or one not of bytes, or several: the whole file (as it may be)
1 constant range-part        \ these bytes of it
2 constant range-beyond      \ none of them in the file: 416
: digits? ( a n -- flag )
    {: a n | ok :}  n 0> to ok  n 0 ?do  a i + c@ [char] 0 [char] 9 1+ within 0= if  false to ok  then  loop  ok ;
999999999999999 constant past-any   \ a place past the end of any file (the longest number a length is read as)
\ a number of a Range header: false if it isn't digits; one of more digits than any length has is past-any - not
\ a cell wrapped round to some other place, nor an error (a request's header is no reason to fail)
: place ( a n -- x true | false )
    {: a n :}  a n digits? 0= if  false exit  then
    a n digits>length if  true exit  then  past-any true ;
\ a Range header's bytes of a file so long: "bytes=first-last", "bytes=first-" (to its end), "bytes=-n" (its last n)
: range-of ( a n size -- first last kind )
    {: a n size | dash first last :}
    a n s" bytes=" s-starts? 0= if  0 0 range-none exit  then
    a 6 +  n 6 -  s-trim to n to a
    a n s" ," s-find 0>= if  0 0 range-none exit  then
    a n s" -" s-find to dash  dash 0< if  0 0 range-none exit  then
    a dash s-trim {: fa fn :}  a dash 1+ +  n dash - 1-  s-trim {: la ln :}
    fn 0= if   \ (its last so many)
        la ln place 0= if  0 0 range-none exit  then  to last
        last 0=  size 0= or if  0 0 range-beyond exit  then
        size last - 0 max  size 1-  range-part exit
    then
    fa fn place 0= if  0 0 range-none exit  then  to first
    ln if
        la ln place 0= if  0 0 range-none exit  then  to last
        last first < if  0 0 range-none exit  then   \ (not a range at all: passed over)
    else  size 1- to last  then
    first size >= if  0 0 range-beyond exit  then
    first  last size 1- min  range-part ;
\ If-Range: the range only if the file is the one the client has a part of - its date this file's (an entity tag:
\ none are given here, so never) - else the whole file
: range-holds? ( req modified -- flag )
    {: r mt :}  s" if-range" r req-header 0= if  true exit  then
    http-date> if  mt =  else  false  then ;
\ is the file there still the one of that length, written then? (One put in its place as it was being sent - a
\ deploy's rename - would go out as the start of the old and the end of the new, with nothing to say so.)
: same-file? ( path n size modified -- flag )
    {: p pn size mt :}  p pn file-exists? if  p pn file-size size =  p pn file-modified mt = and  else  false  then ;
\ bytes first to last of the file (so long, written then) as the answer's body, a piece at a time: no more of it in
\ memory than a piece and what the client hasn't taken yet (respond-drained)
: send-part ( path n size modified first last status req -- )
    {: p pn size mt first last status r | at :}
    last first - 1+  status r respond-start-sized
    r head? if  exit  then
    first to at
    begin  at last <=  r req-gone? 0= and  while
        p pn size mt same-file? 0= abort" respond-file: the file changed as it was sent"
        p pn at  last at - 1+ piece-bytes min  read-file-part {: s n :}
        n 0= if  s dispose  true abort" respond-file: the file got shorter as it was sent"  then
        s n r respond-chunk  s dispose
        at n + to at
        r respond-drained
    repeat ;
PRIVATE>

\ the file as the answer: its bytes, its type by its name, Last-Modified; 304 (no body) to a request whose
\ If-Modified-Since is as late; 404 for no file there. A part of it to a request that asks for one (Range: 206 - a
\ download gone on with, a video sought in; 416 if the file has none of it). A file longer than http-file-whole-max
\ is never read whole: it goes a piece at a time, as fast as the client takes it.
: respond-file ( path n req -- )
    {: p pn r | mt b size first last kind :}
    p pn file-exists? 0=  p pn dir? or if  r not-found exit  then
    p pn file-modified to mt
    mt >http-date {: d dn :}  d dn s" Last-Modified" r header!  d dispose
    s" if-modified-since" r req-header if
        http-date> if  mt >= if   \ (the browser's copy as late as the file)
            304 r answer-head to b  s\" \r\n" b b-append  b b-data r req-conn net-write  b dispose  exit
        then then
    then
    p pn mime-type r content-type!
    s" bytes" s" Accept-Ranges" r header!
    p pn file-size to size
    s" range" r req-header if  size range-of to kind to last to first  then
    kind range-none <> if  r mt range-holds? 0= if  range-none to kind  then  then   \ (a part of another file
                                                \  asked for: the whole of this one - even where it has no such part)
    kind range-beyond = if
        size s" bytes */{}" format {: c cn :}  c cn s" Content-Range" r header!  c dispose
        s" " 416 r respond exit
    then
    kind range-part = if
        first last size s" bytes {}-{}/{}" format {: part pn2 :}  part pn2 s" Content-Range" r header!  part dispose
        p pn size mt first last 206 r send-part exit
    then
    size http-file-whole-max <= if  p pn read-file {: s n :}  s n 200 r respond  s dispose  exit  then
    p pn size mt 0 size 1- 200 r send-part ;

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

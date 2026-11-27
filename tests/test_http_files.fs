\ http.files (basis/http/files): a folder served, each file with its type and date; a 304; nothing outside it
IN: test-http-files
USING: tester actors net system calendar http http.files vocabs.webclient ;

[: 300000 0 do  i 251 mod %c  loop ;] build constant big-file   \ (300 KB, every byte there is in it)
: big ( -- a n )  big-file string ;
: setup ( -- )
    s" http-files-test" dir? 0= if  s" http-files-test" make-dir  then
    s" http-files-test/css" dir? 0= if  s" http-files-test/css" make-dir  then
    s" <h1>index</h1>" s" http-files-test/index.html" write-file
    s" body{}" s" http-files-test/css/site.css" write-file
    big s" http-files-test/big.bin" write-file
    s" secret" s" http-files-secret.txt" write-file
    s" http-files-test/.git" dir? 0= if  s" http-files-test/.git" make-dir  then
    s" [core]" s" http-files-test/.git/config" write-file  s" KEY=1" s" http-files-test/.env" write-file
    s" http-files-test/.well-known" dir? 0= if  s" http-files-test/.well-known" make-dir  then
    s" Contact: x" s" http-files-test/.well-known/security.txt" write-file ;
: cleanup ( -- )
    s" http-files-test/.git/config" delete-file  s" http-files-test/.git" delete-file  s" http-files-test/.env" delete-file
    s" http-files-test/.well-known/security.txt" delete-file  s" http-files-test/.well-known" delete-file
    s" http-files-test/css/site.css" delete-file  s" http-files-test/index.html" delete-file
    s" http-files-test/big.bin" delete-file
    s" http-files-test/css" delete-file  s" http-files-test" delete-file  s" http-files-secret.txt" delete-file ;
setup

<router> constant site
s" http-files-test" s" /static" site serve-files
: one-file ( req -- )  {: r :}  s" http-files-test/css/site.css" r respond-file ;
s" /one" ' one-file site on-get
site 0 http-listen listener-port web-port !

: modified-line ( -- s n )   \ an If-Modified-Since of the file's own date
    s" http-files-test/css/site.css" file-modified >http-date {: d dn :}
    d dn s\" If-Modified-Since: {s}\r\n" format  d dispose ;
: get-since ( path n -- a' n' )  modified-line {: m mn :}  m mn get-with  m dispose ;

testing a file of the folder: its bytes, its type by its name, its date
T{ s" /static/css/site.css" get  2dup status-of -rot body-of s" body{}" s= -> 200 true }T
T{ s" /static/css/site.css" get  s" Content-Type: text/css" has? -> true }T
T{ s" /static/css/site.css" get  s" Last-Modified: " has? -> true }T
testing a folder: its index.html
T{ s" /static/" get body-of s" <h1>index</h1>" s= -> true }T
T{ s" /static" get  s" Content-Type: text/html" has? -> true }T
testing none there: 404; nothing outside the folder, however it is asked for
T{ s" /static/nope.txt" get status-of -> 404 }T
T{ s" /static/../http-files-secret.txt" get status-of -> 404 }T
T{ s" /static/%2e%2e/http-files-secret.txt" get status-of -> 404 }T
T{ s" /static/css/..%2f..%2fhttp-files-secret.txt" get status-of -> 404 }T
testing a hidden file or folder (.git, .env: a deploy's leavings) not served - but .well-known is
T{ s" /static/.git/config" get status-of  s" /static/.env" get status-of  s" /static/%2eenv" get status-of -> 404 404 404 }T
T{ s" /static/.well-known/security.txt" get status-of -> 200 }T
testing If-Modified-Since: 304 with no body when the browser's copy is as late; an older one gets the file
T{ s" /static/css/site.css" get-since  2dup status-of -rot body-of nip -> 304 0 }T
T{ s" /static/css/site.css" s\" If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n" get-with status-of -> 200 }T
testing respond-file in a handler; mime-type
T{ s" /one" get body-of s" body{}" s= -> true }T
T{ s" a/b.PNG" mime-type s" image/png" s= -> true }T
T{ s" README" mime-type s" application/octet-stream" s= -> true }T
T{ s" app.js" mime-type s" text/javascript; charset=utf-8" s= -> true }T

testing a file longer than http-file-whole-max: sent a piece at a time, as it is - its bytes, its length said, the
testing  connection kept for the request after
100000 to http-file-whole-max
T{ s" /static/big.bin" get  2dup status-of -rot body-of big s= -> 200 true }T
: big-head ( -- a n )  s" /static/big.bin" get  2dup s\" \r\n\r\n" s-find 0 swap s-slice ;
T{ big-head s" Content-Length: 300000" has?  big-head s" Accept-Ranges: bytes" has?  big-head s" Transfer-Encoding" has? -> true true false }T
T{ s\" GET /static/big.bin HTTP/1.1\r\nHost: t\r\n\r\nGET /static/css/site.css HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n" fetch
   2dup s" body{}" s-ends?  -rot nip 300000 > -> true true }T
T{ s\" HEAD /static/big.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n" fetch
   2dup status-of -rot  2dup s" Content-Length: 300000" has? -rot  body-of nip -> 200 true 0 }T

testing a part of a file (Range): 206, the bytes asked for and where they are of how many - from a place to another,
testing  from a place on, its last so many, more than it has (what it has); none of it there: 416
: part-of ( range n -- a n )  s\" Range: {s}\r\n" format {: h hn :}  s" /static/big.bin" h hn get-with  h dispose ;
T{ s" bytes=0-9" part-of  2dup status-of -rot  2dup body-of big drop 10 s= -rot  s" Content-Range: bytes 0-9/300000" has? -> 206 true true }T
T{ s" bytes=100000-199999" part-of body-of  big 100000 100000 s-slice s= -> true }T   \ (more than a piece)
T{ s" bytes=250000-" part-of  2dup status-of -rot body-of  big 250000 50000 s-slice s= -> 206 true }T
T{ s" bytes=-100" part-of  2dup body-of big 299900 100 s-slice s= -rot  s" Content-Range: bytes 299900-299999/300000" has? -> true true }T
T{ s" bytes=299990-400000" part-of  2dup body-of nip -rot  s" Content-Range: bytes 299990-299999/300000" has? -> 10 true }T
T{ s" bytes=300000-" part-of  2dup status-of -rot  s" Content-Range: bytes */300000" has? -> 416 true }T
T{ s" bytes=-0" part-of status-of -> 416 }T
testing what isn't one range of bytes - last before first, several, lines - is passed over: the whole file
T{ s" bytes=5-2" part-of status-of  s" bytes=0-1,5-6" part-of status-of  s" lines=1-2" part-of status-of -> 200 200 200 }T
T{ s" bytes=x-9" part-of body-of nip -> 300000 }T
testing a part of a short file (read whole, asked whole)
T{ s" /static/css/site.css" s\" Range: bytes=1-3\r\n" get-with  2dup status-of -rot body-of s" ody" s= -> 206 true }T
testing If-Range: the part only of the file the client has a part of (by its date); another's, the whole file
: if-range-of ( date n -- a n )
    s\" If-Range: {s}\r\nRange: bytes=0-9\r\n" format {: h hn :}  s" /static/big.bin" h hn get-with  h dispose ;
: big-date ( -- s n )  s" http-files-test/big.bin" file-modified >http-date ;
T{ big-date if-range-of status-of  s" Sun, 06 Nov 1994 08:49:37 GMT" if-range-of  2dup status-of -rot body-of nip -> 206 200 300000 }T
T{ s\" \"an-etag\"" if-range-of status-of -> 200 }T   \ (an entity tag: none are given here)
testing read-file-part (system's): so many bytes from a place - fewer where the file ends, none past it
T{ s" http-files-test/css/site.css" 1 3 read-file-part s" ody" s= -> true }T
T{ s" http-files-test/css/site.css" 4 100 read-file-part s" {}" s=  s" http-files-test/css/site.css" 6 10 read-file-part nip
   s" http-files-test/css/site.css" 100 10 read-file-part nip -> true 0 0 }T
T{ [: s" http-files-test/nope" 0 10 read-file-part ;] catch 0<>  [: s" http-files-test/css/site.css" -1 10 read-file-part ;] catch 0<> -> true true }T
1048576 to http-file-whole-max

cleanup
test-summary

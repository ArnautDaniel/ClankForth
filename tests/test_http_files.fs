\ http.files (basis/http/files): a folder served, each file with its type and date; a 304; nothing outside it
IN: test-http-files
USING: tester actors net system calendar http http.files vocabs.webclient ;

: setup ( -- )
    s" http-files-test" dir? 0= if  s" http-files-test" make-dir  then
    s" http-files-test/css" dir? 0= if  s" http-files-test/css" make-dir  then
    s" <h1>index</h1>" s" http-files-test/index.html" write-file
    s" body{}" s" http-files-test/css/site.css" write-file
    s" secret" s" http-files-secret.txt" write-file ;
: cleanup ( -- )
    s" http-files-test/css/site.css" delete-file  s" http-files-test/index.html" delete-file
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
testing If-Modified-Since: 304 with no body when the browser's copy is as late; an older one gets the file
T{ s" /static/css/site.css" get-since  2dup status-of -rot body-of nip -> 304 0 }T
T{ s" /static/css/site.css" s\" If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n" get-with status-of -> 200 }T
testing respond-file in a handler; mime-type
T{ s" /one" get body-of s" body{}" s= -> true }T
T{ s" a/b.PNG" mime-type s" image/png" s= -> true }T
T{ s" README" mime-type s" application/octet-stream" s= -> true }T
T{ s" app.js" mime-type s" text/javascript; charset=utf-8" s= -> true }T

cleanup
test-summary

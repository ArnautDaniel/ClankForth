\ http.uploads (basis/http/uploads): a multipart/form-data body's parts - fields and files, their names, types and
\ bytes (binary too); a body that isn't one, or isn't whole
IN: test-http-uploads
USING: tester actors net http http.uploads vocabs.webclient ;

<router> constant site
\ each part as name:file:type:length; - or "none"
: list-parts ( req -- )
    {: r | b :}  <bytes> to b
    r req-parts if
        [: {: p :}  p part-name b b-append  [char] : b b-push
            p part-filename if  b b-append  else  s" -" b b-append  then  [char] : b b-push
            p part-type b b-append  [char] : b b-push
            p part-data nip s" {}" format {: s n :}  s n b b-append  s dispose  [char] ; b b-push ;] each
    else  s" none" b b-append  then
    b b-data 200 r respond ;
: title-field ( req -- )  {: r :}  s" title" r part-text 0= if  s" none"  then  200 r respond ;
: echo-file ( req -- )  {: r :}  s" file" r req-part if  part-data  else  s" none"  then  200 r respond ;
s" /parts" ' list-parts site on-post
s" /field" ' title-field site on-post
s" /file" ' echo-file site on-post
site 0 http-listen listener-port web-port !

\ ---- a body as a browser sends one: a field, a file (with a byte 0 and a CR LF inside), the end ----
<bytes> constant body
: line ( a n -- )  body b-append  s\" \r\n" body b-append ;
: make-body ( -- )
    s" --XyZ" line
    s\" Content-Disposition: form-data; name=\"title\"" line  s" " line
    s" my photo" line
    s" --XyZ" line
    s\" Content-Disposition: form-data; name=\"file\"; filename=\"a b.png\"" line
    s" Content-Type: image/png" line  s" " line
    s\" \x89PNG\r\n\x00\x01end" line
    s" --XyZ--" line ;
make-body
: post ( path n type n -- a n )   \ the body to the path, with that Content-Type
    {: pa pn ta tn :}  pa pn ta tn body b-length
    s\" POST {s} HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Type: {s}\r\nContent-Length: {}\r\n\r\n" format
    {: h hn :}  sending b-clear  h hn sending b-append  body b-data sending b-append  h dispose
    sending b-data >string {: s n :}  s n fetch  s dispose ;
: multipart ( -- a n )  s" multipart/form-data; boundary=XyZ" ;

testing a body's parts: a field (no file name, text/plain), a file (its name, type and bytes, binary)
T{ s" /parts" multipart post body-of s" title:-:text/plain:8;file:a b.png:image/png:11;" s= -> true }T
T{ s" /field" multipart post body-of s" my photo" s= -> true }T
T{ s" /file" multipart post body-of s\" \x89PNG\r\n\x00\x01end" s= -> true }T
testing a quoted boundary; a Content-Type in another case
T{ s" /field" s\" Multipart/Form-Data; boundary=\"XyZ\"" post body-of s" my photo" s= -> true }T
testing what isn't one: another type, a boundary not in the body, a body cut short
T{ s" /parts" s" text/plain" post body-of s" none" s= -> true }T
T{ s" /parts" s" multipart/form-data; boundary=other" post body-of s" none" s= -> true }T
body b-clear  s" --XyZ" line  s\" Content-Disposition: form-data; name=\"x\"" line  s" " line  s" never ends" line
T{ s" /parts" multipart post body-of s" none" s= -> true }T
body b-clear  s" --XyZ--" line
T{ s" /parts" multipart post  2dup status-of -rot body-of nip -> 200 0 }T   \ (no parts: an empty list)

test-summary

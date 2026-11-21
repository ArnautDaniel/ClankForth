\ TLS (net's tls-listen / tls-connect, a build with OpenSSL) and HTTPS (http.tls): the certificate checked, by name and
\ by address; one not trusted, or not the host's, refused; a client that isn't TLS turned away
IN: test-tls
USING: tester actors net ;

: skip-without-tls ( -- )
    s" tls-listen" find-word 0= if  ." (a build without TLS: skipped)" cr  test-summary skip-file  then ;
skip-without-tls

USING: http http.tls vocabs.webclient ;
source-dir s" /tls/cert.pem" s+ drop constant cert   \ (a test's own: localhost and 127.0.0.1, self-signed)
source-dir s" /tls/key.pem" s+ drop constant key

<router> constant site
: secure ( req -- )  {: r :}  s" secure" 200 r respond ;
create big 200000 allot  big 200000 char x fill
: large ( req -- )  {: r :}  big 200000 200 r respond ;
s" /" ' secure site on-get
s" /large" ' large site on-get
site 0 cert string key string https-listen listener-port web-port !

: by-address ( port -- conn )  s" 127.0.0.1" rot tls-connect ;
: by-name ( port -- conn )  s" localhost" rot tls-connect ;
: by-other-address ( port -- conn )  s" 127.0.0.2" rot tls-connect ;   \ (a loopback address the certificate doesn't name)
cert string tls-ca!

testing HTTPS: the server's certificate trusted, checked by address and by name
' by-address is connect-to
T{ s" /" get  2dup status-of -rot body-of s" secure" s= -> 200 true }T
' by-name is connect-to
T{ s" /" get body-of s" secure" s= -> true }T
testing a long answer (sent in many writes) and keep-alive, over TLS
T{ s" /large" get body-of nip -> 200000 }T
T{ s\" GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nConnection: close\r\n\r\n" fetch s" secure" hits -> 2 }T
testing a certificate not trusted, or not the host's: no connection (net-closed), nothing read
' by-other-address is connect-to
T{ s" /" get nip  closed @ -> 0 1 }T
' by-address is connect-to
s" " tls-ca!
T{ s" /" get nip  closed @ -> 0 1 }T
cert string tls-ca!
testing a client that isn't TLS: turned away; the server goes on
' plain-connect is connect-to
T{ s" /" get  s" HTTP/1.1" has? -> false }T
' by-address is connect-to
T{ s" /" get status-of -> 200 }T
testing tls-listen with no certificate there: an error
: listens-without ( -- flag )  client enter  [: 0 s" no-such-cert.pem" key string tls-listen drop ;] catch  leave-actor  0<> ;
T{ listens-without -> true }T

test-summary

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
\ tls-connect-as: reached at an address the certificate doesn't name, checked against a name it does; ALPN offered
: as-localhost ( port -- conn )  s" 127.0.0.2" rot s" localhost" s" tds/8.0,http/1.1" tls-connect-as ;
: as-other ( port -- conn )  s" 127.0.0.1" rot s" example.com" s" " tls-connect-as ;
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
testing net-why: why a connection ended - a certificate not the host's
message probe ( -- )
create why-said 200 allot   variable why-n
behaviour why-prober
    on probe ( -- )  s" 127.0.0.2" web-port @ tls-connect drop ;
    on net-closed ( conn -- )  net-why 200 min s-bytes dup why-n !  why-said swap move ;
end-behaviour
why-prober -1 s" why-prober" spawn constant why-probing
: probe-why ( -- a n )  0 why-n !  why-probing send probe  100 0 do  10 net-poll drop  deliver  why-n @ if  leave  then  loop  why-said why-n @ ;
T{ probe-why s" TLS: the certificate" s-find 0>= -> true }T
testing tls-connect-as: the certificate checked against the name given, not the address reached
' as-localhost is connect-to
T{ s" /" get body-of s" secure" s= -> true }T
' as-other is connect-to
T{ s" /" get nip  closed @ -> 0 1 }T
' by-address is connect-to
testing a client that isn't TLS: turned away; the server goes on
' plain-connect is connect-to
T{ s" /" get  s" HTTP/1.1" has? -> false }T
' by-address is connect-to
T{ s" /" get status-of -> 200 }T
testing http.client over https: the server's certificate checked (one not the host's: no response)
USING: http.client ;
message ask-https ( xt -- )
variable https-status   variable https-done   create https-body 16 allot
behaviour https-asker
    on ask-https ( xt -- )
        execute {: res :}  res res-status https-status !  res res-body 16 min https-body swap move  true https-done ! ;
end-behaviour
https-asker -1 s" https-asker" spawn constant https-asking
: ask-https-of ( xt -- status )
    0 https-done !  https-asking send ask-https
    500 0 do  10 net-poll drop  deliver  https-done @ if  leave  then  loop  https-status @ ;
T{ [: web-port @ s" https://localhost:{}/" format http-get ;] ask-https-of  https-body 6 s" secure" s= -> 200 true }T
T{ [: web-port @ s" https://127.0.0.2:{}/" format http-get ;] ask-https-of -> 0 }T
testing tls-listen with no certificate there: an error
: listens-without ( -- flag )  client enter  [: 0 s" no-such-cert.pem" key string tls-listen drop ;] catch  leave-actor  0<> ;
T{ listens-without -> true }T

test-summary

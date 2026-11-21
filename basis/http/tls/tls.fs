\ tls.fs - HTTPS: the http server's routes on a TLS listener (net's tls-listen: a build with OpenSSL).
\   site 8443 s" cert.pem" s" key.pem" https-listen drop
IN: http.tls
USING: net http ;

\ requests on this port over TLS, answered by the router's routes: the certificate (its chain, PEM) and its key from
\ these files; the listener
: https-listen ( router port cert n key n -- listener )
    {: rt port c cn k kn :}  rt [: port c cn k kn tls-listen ;] http-listen-with ;

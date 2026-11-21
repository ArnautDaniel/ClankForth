\ A web server in a page: every connection its own actor, each request answered "hello".
\   clankforth examples/hello-server.fs -- 8080      then   curl localhost:8080/
USING: actors net system ;

: port ( -- n )  args {: l :}  l length if  0 l nth string string>number drop  else  8080  then  l list-free ;

\ one connection: waits for a whole request (its blank line), answers, closes
behaviour visitor
  on net-received ( conn -- )  {: c :}
      c net-input  s\" \r\n\r\n" s-find 0< if exit then   \ (not all of it yet)
      s" Hello from ClankForth" {: body n :}
      n s\" HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nConnection: close\r\n\r\n" format
      over >r  c net-write  r> dispose
      body n c net-write  c net-close  self kill ;
  on net-closed ( conn -- )  drop  self kill ;
end-behaviour

\ the listener: each connection handed to a visitor of its own
behaviour front-door
  on spawned ( -- )  port listen drop ;
  on net-accepted ( listener conn -- )  nip  visitor -1 s" visitor" spawn swap net-owner! ;
end-behaviour

front-door -1 s" front-door" spawn drop
.( listening) cr
serve

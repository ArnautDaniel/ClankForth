\ The web benchmark's server: its routes (site/routes.fs) on this node and one more for each node asked for.
\   BENCH_PORT=8101 clankforth bench/http/server.fs -- 4      (nodes; 1 if none said)
USING: actors net system http site.routes ;
: nodes ( -- n )  args {: l :}  l length if  0 l nth string string>number drop  else  1  then  l [: dispose ;] each  l list-free ;
: start-nodes ( -- )  nodes 1- 0 ?do  s" site.routes" start-node drop  loop ;
start-nodes
serve

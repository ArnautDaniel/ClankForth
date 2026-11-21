\ A web app on every core: its routes (webapp/site.fs) on this node and on a node of their own for each other core,
\ all listening on one port; and here, an actor keeping the count of visits that every node's handlers ask.
\   clankforth examples/web-app.fs -- 8080 4      (port, nodes)   then   curl localhost:8080/
USING: actors net system http webapp.site ;

state: counting-state  cell field visits  end-state
behaviour counting
    on visit ( -- )  1 visits +!  visits @ sender send visits-now ;
end-behaviour
counting counting-state s" visits" spawn  s" visits" rot register-name

: nodes ( -- n )  1 4 arg ;
: start-nodes ( -- )  nodes 1- 0 ?do  s" webapp.site" start-node drop  loop ;
start-nodes
site-port nodes s" listening on port {}, {} nodes" format type cr
serve

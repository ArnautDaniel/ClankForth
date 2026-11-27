\ A web server whose nodes are processes: a supervisor that keeps them, each node the whole program - its own Forth,
\ its own actors, its own database folder - all listening on one port. A node that crashes is one node gone for a
\ fifth of a second; the others answer meanwhile.
\   clankforth examples/nodes-server.fs -- 8080 4     (port, nodes: a node a core without)
\   curl localhost:8080/            which node answered, its life, how many it has answered
\   curl localhost:8080/crash       that node killed outright: ask again - the others answer, then it is back
USING: actors net system json jsondb http ;

: arg ( i default -- n )   \ the program's argument i as a number, or the default
    {: i default | l n :}  args to l  default to n
    i l length < if  i l nth string string>number if  to n  else  drop  then  then  n ;

1 cores arg as-nodes   \ from here: one of that many nodes (the supervisor never comes back from this)

\ this node's own data: a folder a node (two processes can't open one - the second is told so)
s" nodes-data" make-dir
node-index s" nodes-data/node-{}" format db-open constant db
: counted ( -- )   \ (its count's record, made the first time: a node started again goes on from what it had)
    s" counts" s" all" db db-get if  2drop  else  s" {}" s" counts" s" all" db db-put 2drop  then ;
counted

<router> constant site
: home ( req -- )
    {: r :}  1 s" answered" s" counts" s" all" db db-add 0= if  0  then {: n :}
    node-index node-count node-life n s\" node {} of {}, its life {}: {} answered by it\n" format 200 r respond ;
: crash ( req -- )  drop  s" kill -SEGV $PPID" shell drop ;   \ (as a fault in the system's own C would end it)
s" /" ' home site on-get
s" /crash" ' crash site on-get

site 0 8080 arg http-listen drop
: announce ( -- )   \ (said once: by the first node, the first time it runs)
    node-index 1 =  node-life 1 = and if
        0 8080 arg node-count s" http://localhost:{}/ - {} nodes, each a process" format type cr
    then ;
announce
serve

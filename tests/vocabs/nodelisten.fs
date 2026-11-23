\ (test_nodes.fs) run in a node of its own by start-node: an actor listening on a port the main node listens on too,
\ counting the connections it gets - registered as "listener<node>"
IN: vocabs.nodelisten
USING: actors net vocabs.listenmsgs ;
variable got
behaviour node-listener
  on listen-on ( port -- )  listen drop ;
  on net-accepted ( listener conn -- )  nip net-close  1 got +! ;
  on how-many ( -- )  got @ sender send counted ;
end-behaviour
node-listener -1 s" node-listener" spawn  node s" listener{}" format rot register-name

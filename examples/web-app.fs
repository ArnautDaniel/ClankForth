\ A web app on every core: its routes (webapp/site.fs) on this node and on a node of their own for each other core,
\ all listening on one port; and here, the actors every node's handlers talk to - a count of visits, and a chat room
\ that owns every chat socket, on whichever node it was accepted.
\   clankforth examples/web-app.fs -- 8080 4      (port, nodes)   then   open http://localhost:8080/
USING: actors net system http http.websocket webapp.site ;

state: counting-state  cell field visits-so-far  end-state
behaviour counting
    on visit ( -- )  1 visits-so-far +!  visits-so-far @ sender send visits-now ;
end-behaviour
counting counting-state s" visits" spawn  s" visits" rot register-name

\ the room: its members the sockets open on it, each message to every one of them
state: room-state  cell field members  end-state
behaviour room
    on spawned ( -- )  list members ! ;
    on ws-opened ( ws -- )  members @ push ;
    on ws-text ( s ws -- )
        drop {: s :}  members @ [: {: ws :}  s string ws ws-send ;] each  s dispose ;
    on ws-closed ( ws -- )
        {: gone | kept :}  list to kept
        members @ [: dup gone = if  drop  else  kept push  then ;] each
        members @ list-free  kept members ! ;
end-behaviour
room room-state s" room" spawn  s" room" rot register-name

: nodes ( -- n )  1 4 arg ;
: start-nodes ( -- )  nodes 1- 0 ?do  s" webapp.site" start-node drop  loop ;
start-nodes
site-port nodes s" listening on port {}, {} nodes" format type cr
serve

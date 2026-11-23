\ (test_http_live.fs) run in a node of its own by start-node: a site whose /live takes a socket for a page on another
\ node - its port asked of the actor registered as "livenode"
IN: vocabs.livenode
USING: actors net http html http.live vocabs.livemsgs ;
: n-render ( -- )  <p  s" elsewhere" text  p> ;
live-view elsewhere
    render: n-render
end-live-view
<router> constant node-site
s" /e" elsewhere node-site live-route
variable node-port
behaviour live-node
  on live-port? ( -- )
    node-port @ 0= if  node-site 0 http-listen listener-port node-port !  then
    node-port @ sender send live-port-is ;
end-behaviour
live-node -1 s" livenode" spawn  s" livenode" rot register-name

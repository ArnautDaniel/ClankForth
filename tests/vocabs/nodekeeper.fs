\ (test_nodes.fs) run in a node of its own by start-node: "keeper" makes actors for the other node to watch, link to
\ and kill
IN: vocabs.nodekeeper
USING: actors vocabs.nodemsgs ;
behaviour mortal
  on ping ( n -- )  1+ sender send pong ;
  on fail ( -- )  0 @ drop ;   \ (an error: a null address)
end-behaviour
behaviour keeper
  on make ( -- )  mortal -1 s" mortal" spawn address  sender send made ;
  on halt ( -- )  bye ;
end-behaviour
keeper -1 s" keeper" spawn  s" keeper" rot register-name

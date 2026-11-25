\ (test_nodes.fs) run in a node of its own by start-node: "churner" makes objects its handler's end frees - what its
\ node's loop, idle after, hands back to the system
IN: vocabs.nodechurn
USING: actors system vocabs.nodemsgs ;
behaviour churner
  on churn ( n -- )  0 ?do  s" gone soon" >string 2drop  loop ;
  on count-trims ( -- )  memory-trims sender send carried ;
  on halt ( -- )  bye ;
end-behaviour
churner -1 s" churner" spawn  s" churner" rot register-name

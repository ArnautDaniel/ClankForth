\ (test_nodes.fs) run in a node of its own by start-node: "idler" says how many rounds its node's loop has run - few
\ while nothing there can run, one after another while a handler yields over and over
IN: vocabs.nodeidle
USING: actors system vocabs.nodemsgs ;
behaviour idler
  on count-rounds ( -- )  rounds sender send carried ;
  on spin ( n -- )  0 ?do  yield  loop ;
  on nap-ms ( ms -- )  sender {: asker :}  sleep-ms  rounds asker send carried ;
  on halt ( -- )  bye ;
end-behaviour
idler -1 s" idler" spawn  s" idler" rot register-name

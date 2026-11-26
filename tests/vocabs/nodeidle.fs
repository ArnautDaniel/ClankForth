\ (test_nodes.fs) run in a node of its own by start-node: "idler" says how many frames its node's loop has run - few
\ while nothing there waits on frames, one every few ms once something does (a tick's listener, a handler's `wait`)
IN: vocabs.nodeidle
USING: actors system vocabs.nodemsgs ;
behaviour idler
  on count-frames ( -- )  actors-frames sender send carried ;
  on listen-ticks ( flag -- )  if  self subscribe tick  else  self unsubscribe tick  then ;
  on tick ( -- ) ;
  on nap ( frames -- )  sender {: asker :}  wait  actors-frames asker send carried ;
  on nap-ms ( ms -- )  sender {: asker :}  sleep-ms  actors-frames asker send carried ;
  on halt ( -- )  bye ;
end-behaviour
idler -1 s" idler" spawn  s" idler" rot register-name

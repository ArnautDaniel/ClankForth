\ (test_shutdown.fs) a node with words of its own for when the process is told to stop: it tells the main node's
\ "witness", which the main loop waits for
IN: vocabs.shutnode
USING: actors system vocabs.shutmsgs ;
: tell ( -- )  7  s" witness" whereis send noted ;
' tell on-shutdown

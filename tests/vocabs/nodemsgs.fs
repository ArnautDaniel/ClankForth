\ (test_nodes.fs) what two nodes' actors say to each other: each node loads it, so the kinds and the class match
IN: vocabs.nodemsgs
USING: actors ;
message ping ( n -- )
message pong ( n -- )
message carry ( s l p -- )   \ a string, a list, a tuple: copied to the other node
message carried ( n -- )
message carry-on ( s l p -- )   \ the same, nothing done with them: the handler's, freed after it
message carry-map ( m b -- )     \ a map (a string key, a number key) and bytes: copied as the rest
message count-objects ( -- )    \ the objects there, told back (carried)
message halt ( -- )
TUPLE: parcel  label weight ;
message make ( -- )        \ the keeper's: a new mortal, its address told back
message made ( addr -- )
message fail ( -- )        \ a mortal's handler fails (no errors actor there: it dies of it)
message churn ( n -- )      \ the churner's: so many objects made and (the handler over) freed
message count-trims ( -- )  \ the times its node handed free memory back, told back (carried)
message count-frames ( -- )  \ the frames its node has run, told back (carried)
message listen-ticks ( flag -- )   \ the idler subscribes to the tick, or no more
message nap ( frames -- )   \ the idler waits so many frames, then says so (carried: the frames run)
message nap-ms ( ms -- )    \ ... or so many ms, asleep

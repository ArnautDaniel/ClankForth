\ (test_nodes.fs) what two nodes' actors say to each other: each node loads it, so the kinds and the class match
IN: vocabs.nodemsgs
USING: actors ;
message ping ( n -- )
message pong ( n -- )
message carry ( s l p -- )   \ a string, a list, a tuple: copied to the other node
message carried ( n -- )
message carry-on ( s l p -- )   \ the same, nothing done with them: the handler's, freed after it
message count-objects ( -- )    \ the objects there, told back (carried)
message halt ( -- )
TUPLE: parcel  label weight ;
message make ( -- )        \ the keeper's: a new mortal, its address told back
message made ( addr -- )
message fail ( -- )        \ a mortal's handler fails (no errors actor there: it dies of it)

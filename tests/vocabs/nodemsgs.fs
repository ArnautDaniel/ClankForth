\ (test_nodes.fs) what two nodes' actors say to each other: each node loads it, so the kinds and the class match
IN: vocabs.nodemsgs
USING: actors ;
message ping ( n -- )
message pong ( n -- )
message carry ( s l p -- )   \ a string, a list, a tuple: copied to the other node
message carried ( n -- )
message halt ( -- )
TUPLE: parcel  label weight ;

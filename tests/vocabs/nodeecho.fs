\ (test_nodes.fs) run in a node of its own by start-node: an actor that answers, registered as "echo"
IN: vocabs.nodeecho
USING: actors accessors vocabs.nodemsgs ;
behaviour echo
  on ping ( n -- )  1+ sender send pong ;
  on carry ( s l p -- )  {: s l p :}
      s string nip  l length +  p label>> string nip +  p weight>> +  sender send carried
      s dispose  l dispose  p label>> dispose  p dispose ;
  on carry-on ( s l p -- )  drop 2drop ;
  on count-objects ( -- )  objects sender send carried ;
  on halt ( -- )  bye ;
end-behaviour
echo -1 s" echo" spawn  s" echo" rot register-name

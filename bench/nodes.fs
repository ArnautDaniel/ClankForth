\ Nodes on cores: the same jobs shared among n worker nodes.  echo "4 run bye" | clankforth bench/nodes.fs -i
USING: actors benchmsgs ;
64 constant jobs
30 constant size
variable answered
behaviour boss
  on done ( r -- )  drop 1 answered +! ;
end-behaviour
boss -1 s" boss" spawn constant me
list constant workers
: started ( n -- )  0 ?do  s" benchworker" start-node drop  loop ;
\ (each node's worker is its first actor: id 0 there)
: worker-of ( node -- addr )  32 lshift ;
: run ( n -- )  {: n | first :}
    node 1+ to first  n started
    0 answered !  me enter
    jobs 0 do  size  i n mod first + worker-of  send job  loop  leave-actor
    begin  actors-frame  answered @ jobs <  while  5 node-wait drop  repeat ;

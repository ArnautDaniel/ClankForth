\ what an actor no longer names is let go (docs/actors.md, An actor's objects): as a handler ends, once its region has
\ grown to twice what was last in use, what the actor's state record names - and what that holds - is kept, the rest
\ of the region let go. Here at every handler's end (collect-always!), so what is kept and what isn't is seen at once.
IN: test-collect
USING: tester actors ;

objects constant objects-before
true collect-always!
message put ( n -- )
message put-many ( n -- )
message into-table ( -- )
message orphan ( -- )
message ask ( -- )
message look ( -- )
variable seen   variable ok   variable told
state: keeping  cell field current  cell field table  cell field piece  end-state
: settle ( -- )  30 0 do  deliver  loop ;
: numbered ( n -- s )  s" value {}" format drop ;
list rescue-all constant shared   \ (the program's own: a table an actor's handlers put what they keep in)

behaviour keeper
  on put ( n -- )  numbered rescue current !  1 seen +! ;                  \ (in another's place: that one named no more)
  on put-many ( n -- )  0 do  i numbered rescue current !  loop  1 seen +! ;
  on into-table ( -- )
      table @ 0= if  <map> rescue table !  shared table @ 7 swap at!  then
      s" kept in a map" >string drop rescue  s" k" table @ s-at!           \ a map's value, its key the map's
      s" kept in the program's list" >string drop rescue  shared push     \ in the program's own list, named by the map
      s" a string and its slice" >string drop rescue  0 8 rot <slice> rescue piece !   \ a slice: its string with it
      1 seen +! ;
  on orphan ( -- )  s" named by nothing" >string drop rescue drop  1 seen +! ;
  on look ( -- )
      current @ string s" value 3" s=
      s" k" table @ s-at drop string s" kept in a map" s= and
      0 shared nth string s" kept in the program's list" s= and
      piece @ string s" a string" s= and  ok !  1 seen +! ;
end-behaviour
keeper keeping s" keeper" spawn constant k
: after ( -- kept )  settle  k actor-objects ;

testing a state field given another object: the one before is named by nothing, and let go as the handler ends
T{ 1 k send put  after -> 1 }T
T{ 2 k send put  after  3 k send put  after -> 1 1 }T
T{ objects-collected 2 >= -> true }T
T{ 500 k send put-many  3 k send put  after -> 1 }T   \ (five hundred in one handler: all but the last, after it)

testing what the state names is kept, and what that holds: a map's value and key, what the program's own list holds
testing  (reached through the map), a slice and the string it is of - and an object rescued that nothing names is not
T{ k send into-table  after -> 6 }T   \ (the current one; the map, its value; the list's; the slice, its string)
T{ k send orphan  after -> 6 }T
T{ false ok !  k send look  settle  ok @ -> true }T
T{ k send into-table  after -> 7 }T   \ (again: the first's value and slice named no more - its list item is, by the list)
T{ shared length -> 2 }T

testing a handler that waits keeps what it has: its region is looked through only as a handler ends
message hold-on ( -- )   message answer ( -- )
behaviour waiter
  on hold-on ( -- )  s" held across a wait" >string drop rescue {: s :}  await answer  s string s" held across a wait" s= told ! ;
end-behaviour
waiter keeping s" waiter" spawn constant w
T{ false told !  w send hold-on  settle  k send orphan  settle  w send answer  settle  told @ -> true }T

testing not at every handler's end by default: once the region has grown to twice what was last in use
false collect-always!
keeper keeping s" slow-keeper" spawn constant k2
: put-times ( n -- )  0 do  i k2 send put  loop  settle ;
T{ 100 put-times  k2 actor-objects -> 100 }T                \ (under 256: not looked through)
T{ 200 put-times  k2 actor-objects 256 < -> true }T         \ (grown past it: all but the one named let go, and on)

k kill  k2 kill  w kill  deliver
shared list-free   \ (what it held was the keeper's: gone with it)
T{ objects -> objects-before }T
test-summary

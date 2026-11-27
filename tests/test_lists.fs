\ lists, quotations and the higher-order words
IN: test-lists
USING: tester frames ;

testing quotations
T{ 3 [: 1+ ;] execute -> 4 }T
: twice ( xt -- n ) dup execute swap execute + ;
T{ [: 5 ;] dup drop twice -> 10 }T
: make-adder-test 10 [: 2 * ;] execute ;
T{ make-adder-test -> 20 }T
: nested [: [: 7 ;] execute 1+ ;] execute ;
T{ nested -> 8 }T

testing list literals
T{ { 1 2 3 } length -> 3 }T
T{ { 10 20 30 } 1 swap nth -> 20 }T
T{ { 10 20 30 } -1 swap nth -> 30 }T
: inside { 4 5 } ;
T{ inside length inside 0 swap nth -> 2 4 }T
T{ { } length -> 0 }T
T{ { { 1 } { 2 3 } } 1 swap nth length -> 2 }T

testing push pop nth!
list constant l
T{ 1 l push 2 l push l length -> 2 }T
T{ 9 0 l nth! 0 l nth -> 9 }T
T{ l pop l length -> 2 1 }T

testing each map filter reduce
variable total
T{ 0 total ! { 1 2 3 } [: total +! ;] each total @ -> 6 }T
T{ { 1 2 3 } [: dup * ;] map { 1 4 9 } [: + ;] 0 swap reduce swap [: + ;] 0 swap reduce -> 14 14 }T
T{ { 1 2 3 4 5 6 } [: 2 mod 0= ;] filter dup length swap 0 swap nth -> 3 2 }T
T{ { 3 4 5 } 0 [: + ;] reduce -> 12 }T

testing find any? all? count contains? range
T{ { 1 8 3 9 } [: 5 > ;] find -> 8 true }T
T{ { 1 2 } [: 5 > ;] find -> false }T
T{ { 1 2 7 } [: 5 > ;] any? { 1 2 } [: 5 > ;] any? -> true false }T
T{ { 6 7 } [: 5 > ;] all? { 6 2 } [: 5 > ;] all? -> true false }T
T{ { 1 6 7 2 9 } [: 5 > ;] count -> 3 }T
T{ 7 { 1 7 3 } contains? 4 { 1 7 3 } contains? -> true false }T
T{ 4 range 0 [: + ;] reduce -> 6 }T

testing a list that isn't one (a number, one disposed of): an error, not a crash
: not-a-list ( -- )  12345 length drop ;
T{ ' not-a-list catch -> -1 }T
: disposed-list ( -- )  { 1 } dup dispose length drop ;
T{ ' disposed-list catch -> -1 }T

testing a task's objects are its own: one started inside a scratch region keeps what it makes
variable kept-list
: makes-one ( -- )  { 1 2 3 } kept-list ! ;
: starts-in-scratch ( -- )  [: ['] makes-one start-task drop ;] with-scratch ;
T{ starts-in-scratch  1 tick-tasks  kept-list @ length -> 3 }T

testing each map filter reduce find in a definition (the walk laid down where they stand)
: summed ( l -- n )  0 swap [: + ;] each ;
: squares ( l -- l' )  [: dup * ;] map ;
: evens ( l -- l' )  [: 2 mod 0= ;] filter ;
: totalled ( l -- n )  0 [: + ;] reduce ;
: first-over ( l n -- x true | false )  {: n :}  [: n > ;] find ;
T{ { 1 2 3 4 } summed  { } summed -> 10 0 }T
T{ { 1 2 3 } squares totalled  { } squares length -> 14 0 }T
T{ { 1 2 3 4 5 6 } evens dup length swap totalled -> 3 12 }T
T{ { 5 6 7 } 5 first-over  { 5 6 7 } 9 first-over  { } 0 first-over -> 6 true false false }T
T{ { 1 2 3 } 2 first-over  { 3 } 2 first-over -> 3 true 3 true }T
testing the quotation runs on the stack as its word left it: what is under the item is the caller's
: scaled-sum ( l k -- n )  0 rot [: 2 pick * + ;] each  nip ;
T{ { 1 2 3 } 10 scaled-sum -> 60 }T
T{ 0 { 1 2 3 } ' + each  { 1 2 3 } 0 ' + reduce -> 6 6 }T
testing walks one in another, and an exit from the quotation (its own)
: pairs ( l -- n )  {: l | n :}  l [: drop  l [: drop  n 1+ to n ;] each ;] each  n ;
T{ { 1 2 3 } pairs -> 9 }T
: odd-sum ( l -- n )  0 swap [: dup 2 mod 0= if  drop exit  then  + ;] each ;
T{ { 1 2 3 4 5 } odd-sum -> 9 }T
testing a list made shorter, or let go, as it is walked: an error
: shrinks ( l -- )  dup [: drop  dup pop drop ;] each  drop ;
: lets-go ( l -- )  dup [: drop  dup dispose ;] each  drop ;
T{ { 1 2 3 4 } ' shrinks catch nip -> -1 }T
T{ { 1 2 3 4 } ' lets-go catch nip -> -1 }T
T{ { 1 2 3 } summed -> 6 }T   \ (the walk the error left on the return stack is gone with it)
testing see: a walk shown as the word it was written as
: seen ( l -- l' )  [: 1+ ;] map ;
T{ s" see seen" ' evaluate catch -> 0 }T

testing count and all?: their code sees the stack as the caller left it
: count-fives ( -- n n )  { 5 1 5 } {: l :}  5  l [: over = ;] count  l list-free ;
T{ count-fives -> 5 2 }T
T{ { 2 4 } dup [: 1 and 0= ;] all? swap list-free -> true }T
testing range under 1: none
T{ -1 range dup length swap list-free  0 range dup length swap list-free -> 0 0 }T
T{ 3 { 1 2 3 } dup >r contains?  9 r@ contains?  r> list-free -> true false }T

testing sort: numbers, strings by their bytes, or as code says - alike items keep their order
T{ { 5 3 9 1 3 } dup sort  dup .list  list-free -> }T
: sorted? ( l -- flag )  {: l :}  true  l length 1 ?do  i 1- l nth  i l nth  > if drop false then  loop ;
T{ { 5 3 9 -1 3 } dup sort  dup sorted?  swap dup 0 swap nth  swap list-free -> true -1 }T
T{ { 5 3 9 1 } dup [: > ;] sort-by  dup 0 swap nth  swap dup 3 swap nth  swap list-free -> 9 1 }T
: pairs ( -- l )  { 1 10  0 20  1 30  0 40 } ;   \ (by the key: the tens keep their order)
: by-key ( l -- l' )  {: l | made :}  list to made  l length 2 / 0 ?do  i 2 * l nth 100 * i 2 * 1+ l nth +  made push  loop  l list-free  made ;
T{ pairs by-key dup [: 100 / swap 100 / > ;] sort-by  dup 0 swap nth  swap dup 1 swap nth  swap list-free -> 20 40 }T
: words-list ( -- l )  { s" pear" >string drop  s" apple" >string drop  s" app" >string drop } ;
T{ words-list dup sort-strings  dup 0 swap nth string s" app" compare  over 2 swap nth string s" pear" compare  rot dup [: dispose ;] each list-free -> 0 0 }T
T{ { 1 2 } dup [: 2drop 0 @ ;] ' sort-by catch nip nip  swap list-free -> -1 }T   \ (an error in its code: passed on)

testing an object asked for again and again (the one asked for last is kept, to be found without the table): let
testing  go, it is no object - not the list it was; the table grown round it (thousands made), it is found still;
testing  another kind made where one was is what it is now
list constant kept-l   7 kept-l push
: many-made ( -- )  [: 5000 0 do  s" x" >string 2drop  loop ;] with-scratch ;
T{ kept-l length  many-made  kept-l length  0 kept-l nth -> 1 1 7 }T
: gone-list? ( -- before after-failed? )  list {: l :}  l length  l list-free  l ['] length catch 0<> nip ;
T{ gone-list? -> 0 true }T
: churned ( -- wrong )   \ lists and maps made and let go in turn, each asked what it is: never the other's kind
    {: | wrong :}
    200 0 do
        list {: l :}  l length drop  l list?  l map? 0= and 0= if  wrong 1+ to wrong  then  l list-free
        <map> {: m :}  m map-size drop  m map?  m list? 0= and 0= if  wrong 1+ to wrong  then
        m ['] length catch 0= if  wrong 1+ to wrong  then  drop  m dispose
    loop  wrong ;
T{ churned -> 0 }T

test-summary

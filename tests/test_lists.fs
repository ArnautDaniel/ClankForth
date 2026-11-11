\ lists, quotations and the higher-order words
IN: test-lists
USING: tester ;

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

test-summary

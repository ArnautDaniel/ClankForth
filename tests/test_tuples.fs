\ tuples (Factor's): classes with slots, accessors, generic words and their methods
IN: test-tuples
USING: tester accessors ;

TUPLE: point  x y ;
TUPLE: point3 < point  z ;
TUPLE: ball  name  F: radius ;

testing a tuple made, its slots read and written
T{ 3 4 <point> dup x>> swap y>> -> 3 4 }T
T{ 3 4 <point> 10 >>x x>> -> 10 }T
T{ 1 2 3 <point3> dup x>> over y>> rot z>> -> 1 2 3 }T
T{ 3 4 <point> [: 1+ ;] change-x x>> -> 4 }T

testing float slots: from and to the float stack
T{ 7 2.5e <ball> dup radius>> f>s swap name>> -> 2 7 }T
T{ 0 1.5e <ball> 0.5e >>radius [: 2e f* ;] change-radius radius>> f>s -> 1 }T

testing predicates and classes: a child is its parent's kind too
T{ 1 2 <point> point?  1 2 3 <point3> point?  1 2 <point> point3? -> true true false }T
T{ 5 point?  0 point? -> false false }T
T{ 1 2 3 <point3> point instance?  1 2 3 <point3> object instance? -> true true }T
T{ 1 2 <point> class-of point = -> true }T

testing a slot a class doesn't have, or no tuple at all: an error
: no-z ( -- )  1 2 <point> z>> drop ;
T{ ' no-z catch -> -1 }T
: no-tuple ( -- )  12345 x>> drop ;
T{ ' no-tuple catch -> -1 }T
: freed-one ( -- )  1 2 <point> dup dispose x>> drop ;
T{ ' freed-one catch -> -1 }T

GENERIC: area ( shape -- n )
TUPLE: rect  w h ;
TUPLE: square < rect ;
TUPLE: circle  r ;
M: rect area ( shape -- n )  dup w>> swap h>> * ;
M: circle area ( shape -- n )  r>> dup * 3 * ;

testing generic words: each class its method, a child its parent's
T{ 3 4 <rect> area  2 <circle> area  5 5 <square> area -> 12 12 25 }T
M: square area ( shape -- n )  w>> dup * 1+ ;   \ (its own now)
T{ 5 5 <square> area  3 4 <rect> area -> 26 12 }T

testing no method: an error; `object`'s method is every class's
: point-area ( -- n )  1 2 <point> area ;
T{ ' point-area catch -> -1 }T
M: object area ( shape -- n )  drop 0 ;
T{ point-area -> 0 }T

testing a method's effect is its generic's: a comment that says otherwise, or code that does, is a warning
: warns ( addr len -- n )  warnings {: before :}  evaluate  warnings before - ;
T{ s" M: rect area ( a b -- n ) 2drop 0 ;" warns -> 1 }T
T{ s" M: rect area ( shape -- n ) drop ;" warns -> 1 }T
T{ s" M: rect area ( shape -- n ) w>> ;" warns -> 0 }T
T{ s" : uses-area ( shape -- ) area ;" warns -> 1 }T
T{ ' <point3> stack-effect -> 3 1 0 0 1 }T
T{ ' <ball> stack-effect -> 1 1 1 0 1 }T

testing objects: kept till dispose (clone a copy); a scratch region frees what is made in it
: count-made ( -- n )  objects {: before :}  [: 1 2 <point> drop  { 1 2 } drop ;] with-scratch  objects before - ;
T{ count-made -> 0 }T
: rescued ( -- p )  [: 7 8 <point> rescue ;] with-scratch ;
T{ rescued x>> -> 7 }T
T{ 1 2 <point> dup clone  2dup = >r  x>> swap x>> r> -> 1 1 false }T

test-summary

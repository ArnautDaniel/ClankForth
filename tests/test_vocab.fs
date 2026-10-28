\ vocabularies: IN:, USING:, USE:, <PRIVATE PRIVATE>, vocab:name, shadowing, cycles
IN: test-vocab
USING: tester vocabs.va ;

testing a used vocabulary
T{ greet tell -> 1 99 }T

testing private words stay private
: try-secret s" secret" evaluate ;
T{ ' try-secret catch -> -1 }T
T{ vocabs.va.private:secret -> 99 }T

testing a vocabulary loads once
USE: vocabs.vb
T{ greet2 loads @ -> 2 1 }T

testing qualified names
T{ vocabs.vb:shared vocabs.va:shared -> 20 10 }T

testing two vocabularies with one name: the one used later wins
T{ shared -> 20 }T

testing our own definitions win
: shared 30 ;
T{ shared -> 30 }T

testing vocabularies using each other
: try-circle s" USING: vocabs.vc ;" evaluate ;
T{ ' try-circle catch -> -1 }T

testing a missing vocabulary
: try-missing s" USING: no.such.thing ;" evaluate ;
T{ ' try-missing catch -> -1 }T

testing lookup: case doesn't matter, the newest of a name wins, a word isn't found while it is compiled
: Mixed-Case 7 ;
T{ mixed-case MIXED-CASE -> 7 7 }T
: again1 1 ;  : again1 2 ;
T{ again1 -> 2 }T
: again1 again1 10 + ;   \ (the older one: the new one is hidden until its ;)
T{ again1 -> 12 }T

testing a definition an error leaves unfinished is not found
: half-made  s" : unfinished 1 no-such-word ;" evaluate ;
T{ ' half-made catch -> -1 }T
T{ s" unfinished" find-word -> 0 }T

testing many words in one vocabulary: each found (its table grown many times)
: nth-name ( i -- addr len )  $clear s" many-" $+ n>str $+ $@ ;
: def-many ( n -- )  0 ?do  $clear s" : many-" $+ i n>str $+ s"  " $+ i n>str $+ s"  ;" $+ $@ evaluate  loop ;
3000 def-many
: all-found? ( n -- flag )  true swap 0 ?do  i nth-name find-word dup if execute i = else drop false then and  loop ;
T{ 3000 all-found? -> true }T
T{ s" MANY-2999" find-word execute -> 2999 }T

test-summary

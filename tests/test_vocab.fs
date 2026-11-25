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

testing a word named as a used vocabulary's is one's own to define: nothing is said, and one's own is the one meant
: warns ( addr len -- n )  warnings {: before :}  evaluate  warnings before - ;
T{ s" : greet ( -- n )  5 ;  : greets ( -- n )  greet ;" warns  greets  vocabs.va:greet -> 0 5 1 }T
testing a bare name two used vocabularies each have is ambiguous: said where it is used (the later in USING: taken);
testing  the qualified name says which, and nothing is said
T{ s" IN: test-vocab-two  USING: vocabs.va vocabs.vb ;  : takes ( -- n )  shared ;  IN: test-vocab" warns
   test-vocab-two:takes -> 1 20 }T
T{ s" IN: test-vocab-two  : says ( -- a b )  vocabs.va:shared vocabs.vb:shared ;  IN: test-vocab" warns
   test-vocab-two:says -> 0 10 20 }T
: ambiguity-said ( -- flag )   \ (strict: the warning an error, its words to read)
    true strict!
    s" IN: test-vocab-two  : takes-again ( -- n )  shared ;  IN: test-vocab" ['] evaluate catch nip nip
    false strict!  s" IN: test-vocab" evaluate
    0<>  error-message s" shared is ambiguous here: vocabs.vb:shared and vocabs.va:shared" s-find 0>= and ;
T{ ambiguity-said -> true }T
testing a vocabulary and its own <PRIVATE half are one source: a name in both is no ambiguity
T{ s" IN: test-vocab-three  : twice ( -- n )  1 ;  <PRIVATE  : twice ( -- n )  2 ;  : inner ( -- n )  twice ;  PRIVATE>  : outer ( -- n )  twice  test-vocab-three.private:inner + ;  IN: test-vocab" warns
   test-vocab-three:outer -> 0 3 }T

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

testing a name only the core has is the core's; taken by a vocabulary it is that one's, the core's still reached
T{ -3 0<=  3 0<= -> true false }T
: 0<= ( n -- n' )  drop 42 ;
T{ 3 0<=  3 forth:0<= -> 42 false }T
testing a number's name taken by a word: the word
T{ 123 -> 123 }T
: 123 ( -- n )  7 ;
T{ 123  124 -> 7 124 }T
testing a used vocabulary's word hiding a core one's name is found before it
T{ s" USING: vocabs.ve ;  2 2*" evaluate -> 22 }T
T{ 2 forth:2* -> 4 }T

testing a vocabulary in a folder of its own (a/b/b.fs, as the library's basis/ is laid out)
USING: vocabs.nested.inner ;
T{ inner-answer -> 42 }T

testing source-dir: the folder of the file being read
: here-folder? ( -- flag )  source-dir {: s n :}  s n s" tests" s-ends?  s dispose ;
T{ here-folder? -> true }T

test-summary

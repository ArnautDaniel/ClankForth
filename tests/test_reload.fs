\ reload: a vocabulary's file read again - every caller of its words gets the new ones from its next call
IN: test-reload
USING: tester system actors accessors ;

\ the vocabulary's file: one of tests/reload/ copied to vocabs/generated/rl.fs (vocabs.generated.rl)
: generated ( -- a n )  source-dir s" /vocabs/generated" s+ ;
: ready ( -- )  generated dir? 0= if  generated make-dir  then ;
: install ( a n -- )  {: a n :}  source-dir s" /reload/" s+ a n s+ read-file  generated s" /rl.fs" s+ write-file ;
ready  s" one.fs" install

USING: vocabs.generated.rl ;
: user ( -- n )  twice greeting + ;
: use-wrapped ( -- n )  wrapped ;
: use-shape ( -- a b )  5 shape ;
: use-thready ( -- n )  thready ;
TUPLE: rl-other  z ;
M: rl-other rl-size ( obj -- n )  z>> ;     \ (a method of another vocabulary's: kept through its generic's reload)
message t-ask ( -- )
variable t-heard
behaviour t-asking
  on t-ask ( -- )  greeting t-heard ! ;
end-behaviour

testing as first loaded
T{ user  was-reloading  use-wrapped  show-secret  gone -> 3 false 5 100 9 }T
rl-asked rl-state s" r" spawn constant ra
t-asking -1 s" t" spawn constant ta
T{ ra enter  5 rl-a !  7 rl-b !  rl-a @ rl-b @  leave-actor -> 5 7 }T
3 4 <rl-point> constant p1
T{ p1 rl-size  7 <rl-other> rl-size -> 3 7 }T

testing reloaded: the words defined anew are what everything compiled for the old ones runs
s" two.fs" install
warnings constant warned
reload vocabs.generated.rl
T{ user  was-reloading -> 40 true }T
T{ ra send rl-ask  deliver  rl-heard @ -> 11 }T   \ (its behaviour's handler, the new one)
T{ ta send t-ask  deliver  t-heard @ -> 10 }T     \ (a handler compiled for the old greeting)
T{ use-wrapped  wrapped -> 5 6 }T                 \ (the one calling what it replaces: the old callers keep the old)
T{ show-secret  gone -> 200 9 }T                  \ (a <PRIVATE word too; a word not defined again stays)
T{ use-thready  ' thready native? -> 8 false }T

testing reloaded: an actor's state moved over field by field, by name; a tuple class the same if its slots are
T{ ra enter  rl-a @ rl-b @ rl-c @  leave-actor -> 5 7 0 }T
T{ p1 rl-point?  p1 rl-size  7 <rl-other> rl-size -> true 4 7 }T

testing a reload that fails: what came before the error stays, nothing forwarded
s" broken.fs" install
T{ s" reload vocabs.generated.rl" ' evaluate catch nip nip -> -1 }T
T{ user -> 40 }T
s" two.fs" install
reload vocabs.generated.rl
T{ user  greeting -> 40 10 }T

testing reload of a vocabulary not loaded yet: loaded; of a name with no file, an error
T{ s" reload base64" evaluate  s" base64:>base64" find-word 0<> -> true }T
T{ s" reload no-such-vocabulary" ' evaluate catch nip nip -> -1 }T

checker-tests
testing a word reloaded with another effect: a warning, for the calls compiled for the old one
T{ warnings warned - -> 1 }T   \ (shape's, and use-shape compiled for it)

test-summary

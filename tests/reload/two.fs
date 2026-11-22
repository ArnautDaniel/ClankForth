\ (test_reload.fs) the vocabulary changed: reloaded over one.fs
IN: vocabs.generated.rl
USING: actors accessors ;
: greeting ( -- n )  10 ;
: twice ( -- n )  greeting 3 * ;
: wrapped ( -- n )  wrapped 1+ ;    \ (calls the one it replaces: what was compiled for that one keeps it)
: shape ( n -- n )  1+ ;            \ (another effect: a warning, a call was compiled for the old one)
: eight ( -- n )  8 ;
: thready ( -- n )  ['] eight jump ;   \ (left threaded: machine code compiled for the old one gets here too)
reloading? constant was-reloading
state: rl-state  cell field rl-c  cell field rl-b  cell field rl-a  end-state   \ (reordered, one more)
TUPLE: rl-point  x y ;              \ (the same: the same class, its tuples still of it)
GENERIC: rl-size ( obj -- n )
M: rl-point rl-size ( obj -- n )  y>> ;
message rl-ask ( -- )
variable rl-heard
behaviour rl-asked
  on rl-ask ( -- )  greeting 1+ rl-heard ! ;
end-behaviour
<PRIVATE
: secret ( -- n )  200 ;            \ (show-secret isn't defined again: the old one calls the new secret)
PRIVATE>

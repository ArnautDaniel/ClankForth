\ (test_reload.fs) a vocabulary as it first is: copied to vocabs/generated/rl.fs, then loaded
IN: vocabs.generated.rl
USING: actors accessors ;
: greeting ( -- n )  1 ;
: twice ( -- n )  greeting 2 * ;
: wrapped ( -- n )  5 ;
: shape ( -- n )  1 ;
: gone ( -- n )  9 ;
: thready ( -- n )  7 ;
reloading? constant was-reloading
state: rl-state  cell field rl-a  cell field rl-b  end-state
TUPLE: rl-point  x y ;
GENERIC: rl-size ( obj -- n )
M: rl-point rl-size ( obj -- n )  x>> ;
message rl-ask ( -- )
variable rl-heard
behaviour rl-asked
  on rl-ask ( -- )  greeting rl-heard ! ;
end-behaviour
<PRIVATE
: secret ( -- n )  100 ;
PRIVATE>
: show-secret ( -- n )  secret ;

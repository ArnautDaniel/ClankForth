\ (test_reload.fs) the vocabulary with an error in it: reloaded, it fails part way
IN: vocabs.generated.rl
: greeting ( -- n )  99 ;
no-such-word
: twice ( -- n )  greeting 4 * ;

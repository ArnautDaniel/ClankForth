\ A quick run (tester's `quick-tests` set): the file ends at `full-suite`, its summary given.
USING: tester ;
-1 quick-tests !
testing before full-suite: run
T{ 1 1 + -> 2 }T
full-suite
testing after it: not in a quick run (this would fail)
T{ 1 -> 2 }T
test-summary

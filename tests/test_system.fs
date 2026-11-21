\ the system vocabulary (the tool has it): files, arguments and environment, other programs, the clock
IN: test-system
USING: tester system ;

: tmp ( -- a n )  s" clankforth-test-system.tmp" ;
: tmp2 ( -- a n )  s" clankforth-test-system2.tmp" ;
: says ( s n a2 n2 -- flag )  {: s n a2 n2 :}  s n a2 n2 compare 0=  s dispose ;

testing a file written, read, appended to, renamed and deleted
T{ s" hello" tmp write-file  tmp file-exists?  tmp file-size -> true 5 }T
T{ tmp read-file s" hello" says -> true }T
T{ s"  there" tmp append-file  tmp read-file s" hello there" says -> true }T
T{ tmp tmp2 rename-file  tmp file-exists?  tmp2 file-exists? -> false true }T
T{ tmp2 delete-file  tmp2 file-exists? -> false }T
T{ tmp ' read-file catch nip nip -> -1 }T   \ (none there: an error saying why)
testing bytes to a file and back
: bytes-round-trip ( -- flag )  <bytes> {: b :}  0 b b-push  255 b b-push  10 b b-push
    b tmp write-bytes  b dispose  tmp read-bytes {: c :}  c b-length 3 =  1 c b@ 255 = and  c dispose  tmp delete-file ;
T{ bytes-round-trip -> true }T
testing a folder: made, listed (sorted), its files deleted
: dir ( -- a n )  s" clankforth-test-dir.tmp" ;
: in-dir ( a n -- a' n' )  {: a n :}  dir s" /" s+  a n s+ ;
T{ dir make-dir  dir make-dir  s" b" s" b" in-dir write-file  s" a" s" a" in-dir write-file  dir file-exists? -> true }T
T{ dir dir-list  dup length  over 0 swap nth string s" a" compare  rot dup [: dispose ;] each list-free -> 2 0 }T
T{ s" a" in-dir delete-file  s" b" in-dir delete-file  dir delete-file  dir file-exists? -> false }T
testing the environment, the arguments, other programs
T{ s" CLANKFORTH_NO_SUCH_VARIABLE" getenv -> false }T
T{ args dup length swap list-free -> 0 }T
T{ s" exit 3" shell -> 3 }T
: echo-hi ( -- flag status )  s" echo hi" shell-output {: s n status :}  s n s" hi" s-starts?  s dispose  status ;
T{ echo-hi -> true 0 }T
testing the clock
T{ ms-now  10 sleep-ms  ms-now swap -  9 > -> true }T
T{ time-now 1700000000 > -> true }T

test-summary

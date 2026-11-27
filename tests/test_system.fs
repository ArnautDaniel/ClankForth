\ the system vocabulary (the tool has it): files, arguments and environment, other programs, the clock
IN: test-system
USING: tester system actors ;

: tmp ( -- a n )  s" clankforth-test-system.tmp" ;
: tmp2 ( -- a n )  s" clankforth-test-system2.tmp" ;
: says ( s n a2 n2 -- flag )  {: s n a2 n2 :}  s n a2 n2 compare 0=  s dispose ;

testing a file written, read, appended to, renamed and deleted
T{ s" hello" tmp write-file  tmp file-exists?  tmp file-size -> true 5 }T
T{ tmp read-file s" hello" says -> true }T
T{ s"  there" tmp append-file  tmp read-file s" hello there" says -> true }T
T{ tmp tmp2 rename-file  tmp file-exists?  tmp2 file-exists? -> false true }T
T{ tmp2 delete-file  tmp2 file-exists? -> false }T
testing rename-file over a file there already: replaced
T{ s" old" tmp2 write-file  s" new" tmp write-file  tmp tmp2 rename-file  tmp2 read-file s" new" says -> true }T
T{ tmp2 delete-file  tmp file-exists? -> false }T
testing a file held open: written at its end, made to last, closed (a handle not open an error)
s" one" tmp write-file
tmp file-open-append constant held
T{ s"  two" held file-write  held file-sync  tmp read-file s" one two" says -> true }T
T{ s"  three" held file-write  held file-close  tmp read-file s" one two three" says -> true }T
T{ held ' file-sync catch nip -> -1 }T
T{ 0 ' file-close catch nip -> -1 }T
testing file-lock: an open file taken as one's own - no other open of it takes it meanwhile; closed, it is free
tmp file-open-append constant mine   tmp file-open-append constant theirs
T{ mine file-lock  theirs file-lock -> true false }T
T{ mine file-close  theirs file-lock  theirs file-close -> true }T
T{ 0 ' file-lock catch nip -> -1 }T
T{ tmp delete-file  tmp file-exists? -> false }T
T{ s" ." dir-sync -> }T
T{ s" no-such-folder-here" ' dir-sync catch nip nip -> -1 }T
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
T{ s" a" in-dir delete-file  s" b" in-dir delete-file  dir dir-list dup length swap list-free -> 0 }T   \ (an empty one: no names)
T{ dir delete-file  dir file-exists? -> false }T
testing the environment, the arguments, other programs
T{ s" CLANKFORTH_NO_SUCH_VARIABLE" getenv -> false }T
T{ args dup length swap list-free -> 0 }T
T{ s" exit 3" shell -> 3 }T
: echo-hi ( -- flag status )  s" echo hi" shell-output {: s n status :}  s n s" hi" s-starts?  s dispose  status ;
T{ echo-hi -> true 0 }T
testing the clock
T{ ms-now  50 sleep-ms  ms-now swap -  30 > -> true }T   \ (Windows' clock ticks in ~16 ms steps: room for one)
T{ time-now 1700000000 > -> true }T

testing memory: what the process holds, the allocator's free memory handed back
\ 64 MB made and freed, an object made after it kept (the allocator can't just shrink its heap's end): held still,
\ until a trim - where the system says what a process holds, and nothing keeps freed memory itself (a sanitizer
\ does: CI's runs under one set ASAN_OPTIONS)
: sanitized? ( -- flag )  s" ASAN_OPTIONS" getenv dup if  -rot drop dispose  then ;
: buffer ( -- b )  <bytes> {: b :}  65536 0 do  i b b-push  loop  b ;
: churn ( -- pin )  list {: l :}  1024 0 do  buffer l push  loop  s" pin" >string drop  l [: dispose ;] each  l list-free ;
: held-mb ( -- n )  process-bytes 20 rshift ;
T{ process-bytes 0< -> false }T
T{ memory-trims  trim-memory  memory-trims swap - -> 1 }T
: trimmed ( -- flag )   \ the trim gave back at least half of what the churn left held (or there is no telling)
    objects held-mb {: o before | pin after :}  churn to pin  held-mb to after
    trim-memory  pin dispose
    cr ." (held: " before . ." MB before, " after . ." MB after 64 MB made and freed, " held-mb . ." MB trimmed)"
    objects o <> if  false exit  then
    sanitized? if  true exit  then
    after before - 32 < if  true exit  then   \ (not held in the first place: nothing to hand back)
    held-mb before -  after before - 2/  <= ;
T{ objects  sanitized? drop  objects = -> true }T
T{ trimmed -> true }T
\ a burst of objects (half a million alive at once, then none): their table and their region's list are given back
\ too - not kept for good at the burst's size (42 MB of table, were it kept)
: burst ( -- )  [: 500000 0 do  s" x" >string 2drop  loop ;] with-scratch ;
: burst-given-back? ( -- flag )
    objects held-mb {: o before :}  burst  trim-memory
    cr ." (held: " before . ." MB before, " held-mb . ." MB after half a million objects came and went)"
    objects o <> if  false exit  then
    sanitized? if  true exit  then
    held-mb before - 16 < ;
T{ burst-given-back? -> true }T
\ idle actors: a task - 11 KB of stacks, the machine stack it runs on - is a handler's while it runs or waits, not
\ an actor's: 2000 actors that each handled a message hold a few hundred bytes each, not 30 MB between them
message poke ( -- )
variable poked
behaviour sleeper  on poke ( -- )  1 poked +! ;  on killed ( -- )  -1 poked +! ;  end-behaviour
: sleepers ( n -- l )  list {: l :}  0 ?do  sleeper -1 s" sleeper" spawn  dup send poke  l push  loop  l ;
: idle-actors-small? ( -- flag )
    trim-memory  held-mb {: before | l grew :}  0 poked !  2000 sleepers to l  deliver  held-mb before - to grew
    cr ." (held: " before . ." MB before, " grew . ." MB more with 2000 idle actors)"
    poked @ 2000 <> if  false exit  then
    l [: kill ;] each  deliver  l list-free   \ (an idle one's last words: on a task lent for them)
    poked @ if  false exit  then
    sanitized? if  true exit  then
    grew 8 < ;
T{ objects  idle-actors-small?  swap objects = -> true true }T

test-summary

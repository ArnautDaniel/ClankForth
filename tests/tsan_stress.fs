\ tsan_stress.fs - what crosses threads, run hard, for ThreadSanitizer (CI's tsan job; not one of ctest's tests): the
\ worker pool (files written, read, synced; sleeps; PBKDF2; a C library's function) from many actors at once; actors made on other nodes
\ (spawn-on), messages between nodes, each node's load read and written; text gzipped and back on every node at once
\ (zlib's work, where the build has it: a call's state its own). No errors raised on purpose: TSan can't
\ follow an error's longjmp across the tasks' stacks - so a build with -fsanitize=thread runs it threaded:
\   CLANKFORTH_THREADED=1 build-tsan/clankforth tests/tsan_stress.fs     (it says "tsan stress: done")
IN: tsan-stress
USING: actors system crypto c vocabs.placework ;

3 constant workers-n
40 constant churners-n
create worker-nodes workers-n cells allot
: start-workers ( -- )  workers-n 0 do  s" vocabs.placework" start-node  worker-nodes i cells + !  loop ;
start-workers
200 sleep-ms

message churn ( i -- )
message farm ( i -- )
variable churned   variable farmed   variable crunched-sum   variable unsqueezed
: path ( i -- a n )  s" tsan-stress-{}.txt" format ;

\ a C function called on the pool (SLOW-FUNCTION:), where the tests' library is beside the tool (CMake builds it
\ there; elsewhere a sum made here stands in - no error raised to find out)
: use-library ( -- )   \ (slow-sum calls it where it stands: threaded, a word that waits can't be reached by execute)
    s" ./libclanktest.so" file-exists? if
        s\" s\" ./libclanktest.so\" c-library SLOW-FUNCTION: slow_add ( a b -- int )  : slow-sum ( a b -- n )  slow_add ;" evaluate
    else  s" : slow-sum ( a b -- n )  + ;" evaluate  then ;
use-library
variable summed

\ the pool, from a handler: a file made, read, appended to and synced, a sleep, a key
behaviour churner
  on churn ( i -- )
    {: i :}  i n>str  i path write-file  i path read-file 2drop
    i path file-open-append {: h :}  s" more" h file-write  h file-sync  h file-close
    3 sleep-ms  s" password" s" salt" 200 32 pbkdf2-sha256 2drop
    i 1 slow-sum summed +!
    i 20 * 1+ squeezes? 0= if  1 unsqueezed +!  then   \ (here, while the other nodes' crunchers do)
    i path delete-file  1 churned +! ;
end-behaviour

\ another node, from a handler: a cruncher made there, asked, its answer awaited; the loads read
behaviour farmer
  on farm ( i -- )
    {: i :}  cruncher work-state s" far" worker-nodes i workers-n mod cells + @ spawn-on {: c :}
    1000 c send crunch  await crunched {: sum nd :}
    i 20 * 1+ c send squeeze  await squeezed {: same nd2 :}  same 0= if  1 unsqueezed +!  then
    sum crunched-sum +!  least-busy-node drop  nd node-load drop  c kill  1 farmed +! ;
end-behaviour

: go ( -- )
    churners-n 0 do  churner -1 s" churner" spawn  i swap send churn  loop
    churners-n 0 do  farmer -1 s" farmer" spawn  i swap send farm  loop ;
: finished? ( -- flag )  churned @ churners-n =  farmed @ churners-n = and ;
: settle ( -- )  20000 0 do  deliver  finished? if  leave  then  1 sleep-ms  loop ;
3 constant rounds   \ (over a second or two: each node's load written a few times while others read it)
: round ( -- ok? )  0 churned !  0 farmed !  0 crunched-sum !  0 summed !  0 unsqueezed !  go  settle
    finished? 0= if  ." tsan stress: not finished" cr  false exit  then
    unsqueezed @ if  ." tsan stress: what was gzipped came back another" cr  false exit  then
    summed @ churners-n dup 1+ * 2/ <> if  ." tsan stress: wrong sums from C" cr  false exit  then   \ (each i + 1)
    crunched-sum @ churners-n 499500 * = dup 0= if  ." tsan stress: wrong sums" cr  then ;
: rounds-all ( -- )  rounds 0 do  round 0= if  unloop exit  then  loop  ." tsan stress: done" cr ;
rounds-all
: stop-workers ( -- )  workers-n 0 do  worker-nodes i cells + @ stop-node  loop ;
stop-workers

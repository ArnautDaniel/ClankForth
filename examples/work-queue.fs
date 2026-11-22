\ A work queue - a business app in ClankForth end to end: tasks filtered and sorted by their fields, claimed with a
\ click, edited in a form with its revision (a save someone else's came between is said, not lost); every open
\ screen updating as tasks change. The database (jsondb) here; the pages (examples/queue/site.fs) on this node and
\ on a node of their own for each other core, each reading through its node's replica, all listening on one port.
\   clankforth examples/work-queue.fs -- 8080 2      (port, nodes)   then open http://localhost:8080/  (ann / queue)
USING: actors net system json jsondb crypto.passwords ;

s" queue-data" db-open constant leader
leader s" db" rot register-name

: user ( name n -- )   \ (the password: queue)
    {: na nn | m :}  <map> to m
    s" queue" hash-password >string drop s" password" m s-at!
    m >json  s" users" na nn leader db-put 2drop ;
: task ( title n status n priority assignee n notes n -- )
    {: ta tn sa sn p aa an na nn | m :}  <map> to m
    ta tn >string drop s" title" m s-at!  sa sn >string drop s" status" m s-at!  p s" priority" m s-at!
    an if  aa an >string drop s" assignee" m s-at!  then  na nn >string drop s" notes" m s-at!
    m >json  s" tasks" leader db-insert 2drop drop ;
: seed ( -- )   \ (a first run: a few of each)
    s" users" leader db-count if  exit  then
    s" ann" user  s" bob" user
    s" Fix the login timeout"          s" open"        5 s" "    s" Users are signed out after a minute." task
    s" Write the onboarding guide"     s" open"        2 s" "    s" For new staff." task
    s" Renew the TLS certificate"      s" in progress" 4 s" bob" s" It runs out next week." task
    s" Archive last year's invoices"   s" done"        1 s" ann" s" " task
    s" Answer the supplier's quote"    s" open"        3 s" "    s" They want an answer by Friday." task
    s" Replace the office printer"     s" open"        1 s" "    s" " task
    s" Plan the quarterly review"      s" open"        4 s" ann" s" " task ;
seed
s" status" s" tasks" leader db-index drop

USING: queue.site ;   \ (the pages: this node's)
: nodes ( -- n )  1 1 arg ;
: start-nodes ( -- )  nodes 1- 0 ?do  s" queue.site" start-node drop  loop ;
start-nodes
site-port nodes s" the work queue on http://localhost:{}/ - {} node(s)" format type cr
serve

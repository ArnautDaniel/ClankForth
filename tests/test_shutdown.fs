\ stopping when told to (system's on-shutdown, http's http-drain): SIGTERM while a request is being answered - its
\ answer still given (the connection closed after), the listener closed, a node's words run too, serve returning
IN: test-shutdown
USING: tester actors net system http vocabs.webclient vocabs.shutmsgs ;

: skip-without-kill ( -- )   \ (a system with no kill to send the signal with: Windows)
    s" kill -0 $PPID" shell 0<> if  ." (no kill here: skipped)" cr  test-summary skip-file  then ;
skip-without-kill

variable noted-by-node   variable drained   variable left-running
behaviour witness  on noted ( n -- )  noted-by-node ! ;  end-behaviour
witness -1 s" witness" spawn  s" witness" rot register-name

<router> constant site
: slow ( req -- )   \ the process told to stop as it is answered; it goes on
    {: r :}  s" kill -TERM $PPID" shell drop  500 sleep-ms  s" done" 200 r respond ;
s" /slow" ' slow site on-get
site 0 http-listen constant listener
listener listener-port web-port !

: stop ( -- )  listener net-close  5000 http-drain  true drained !  http-in-flight left-running ! ;
' stop on-shutdown
s" vocabs.shutnode" start-node drop
: settled ( -- )  50 0 do  10 node-wait drop  loop ;   \ (the node's vocabulary loaded)
settled

s\" GET /slow HTTP/1.1\r\nHost: t\r\n\r\n" open-with
serve   \ (till the signal, its words, the node's)
20 polls

testing the request being answered as the signal came: answered, its connection closed after (draining)
T{ reply b-data status-of -> 200 }T
T{ reply b-data body-of s" done" s= -> true }T
T{ reply b-data s" Connection: close" has? -> true }T
testing the words run: this one's (drained, nothing left running), the node's (the main loop waited for it)
T{ drained @  left-running @  shutting-down? -> true 0 true }T
T{ noted-by-node @ -> 7 }T
testing the listener closed: no more connections
T{ s" /slow" get nip -> 0 }T

test-summary

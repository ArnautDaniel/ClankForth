\ (test_sql_watch.fs) what the test and the page on its other node say to each other: each node loads it
IN: vocabs.watchmsgs
USING: actors ;
message watch-it ( table watcher -- )   \ the page's: the table (a string) watched through the watcher (its address)
message touch-it ( table watcher -- )   \ ... the watcher told the table was written, as a db's write there tells it
message page-told ( n -- )              \ to whoever asked the page to watch: db-changed has come to it n times
message page-halt ( -- )                \ the page's node ends

\ (test_sql_watch.fs) run in a node of its own by start-node: "far-page" watches a table through a watcher on the
\ node that asks it to, and says so each time it is told of a change
IN: vocabs.nodewatch
USING: actors jsondb sql sql.watch vocabs.watchmsgs ;
state: page-state
    cell field pg-asker    \ who asked it to watch: told of each notice
    cell field pg-told
end-state
behaviour far-page
    on watch-it ( table watcher -- )  {: t w :}  sender pg-asker !  t string w sql-watch ;
    on touch-it ( table watcher -- )  {: t w :}  t string >string drop w send sql-touched ;
    on db-changed ( c id rev json -- )  2drop 2drop  1 pg-told +!  pg-told @ pg-asker @ send page-told ;
    on page-halt ( -- )  bye ;
end-behaviour
far-page page-state s" far-page" spawn  s" far-page" rot register-name

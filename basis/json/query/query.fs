\ json.query - a query written in Forth: the map jsondb's db-select and sql.model's name-select take
\
\   [:  s" open" s" status" where$=              \ status = "open"
\       2 s" >=" s" priority" where              \ priority >= 2
\       s" assignee" where-null                  \ no assignee
\       s" -priority" sorted-by  s" title" sorted-by
\       0 50 page ;] query   ( -- q )
\
\ The values go in as values - a number, a string, what a form sent - never into JSON text by hand: nothing a user
\ typed can end a string early and add a condition of its own. The map is the caller's (a handler's: gone with it).
IN: json.query
USING: json ;

<PRIVATE
task-variable made   \ the query being written (a map); 0: none is
: the-query ( -- q )  made @ dup 0= abort" a query's word (where, sorted-by, page) outside [: ... ;] query" ;
: wheres ( -- m )   \ its conditions, by field
    s" where" the-query s-at if  exit  then
    <map> dup s" where" the-query s-at! ;
\ the field's conditions as operators (made if it has none; one said plain - equal to a value - made "=" of it)
: operators ( field n -- m )
    {: fa fn | m :}
    fa fn wheres s-at if  dup map? if  exit  then  <map> to m  s" =" m s-at!  else  <map> to m  then
    m fa fn wheres s-at!  m ;
PRIVATE>

\ xt's words say the query; the map they made
: query ( xt -- q )
    {: xt | was q failed :}
    made @ to was  <map> made !
    xt catch to failed
    made @ to q  was made !
    failed if  failed throw  then  q ;

\ a condition on a field (all of a query's must hold). v: a number, a string, json-true json-false json-null, a list
\ (for "in"). op: = != < <= > >= in contains exists
: where ( v op n field n -- )  {: v oa on fa fn :}  v oa on fa fn operators s-at! ;
: where= ( v field n -- )   \ equal to the value
    {: v fa fn :}
    fa fn wheres s-at if  drop  v s" =" fa fn where  else  v fa fn wheres s-at!  then ;   \ (alone: plain - an index's)
: where$= ( a n field n -- )  {: a n fa fn :}  a n >string drop fa fn where= ;   \ ... to the text
: where-null ( field n -- )  {: fa fn :}  json-null fa fn wheres s-at! ;         \ null, or not there
: where-contains ( a n field n -- )  {: a n fa fn :}  a n >string drop s" contains" fa fn where ;   \ the text in it
: where-in ( l field n -- )  {: l fa fn :}  l s" in" fa fn where ;               \ equal to one of the list's

\ sorted by the field ("-field": descending); said again, ties by the next
: sorted-by ( field n -- )
    {: fa fn | l :}
    s" sort" the-query s-at if  to l  else  list to l  l s" sort" the-query s-at!  then
    fa fn >string drop l push ;
\ so many of them, from the offset
: page ( offset limit -- )  {: from k :}  from s" offset" the-query s-at!  k s" limit" the-query s-at! ;

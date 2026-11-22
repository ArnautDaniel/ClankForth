\ site.fs - the work queue's web side (examples/work-queue.fs runs it): its pages, on each node that loads it - each
\ reading the database through its node's replica, each open page an actor (http.live) that updates as tasks change.
IN: queue.site
USING: actors net system json jsondb html html.templates http http.sessions http.security http.files http.live
       crypto.passwords ;

: arg ( i default -- n )   \ the program's argument i as a number, or the default
    {: i default | l n :}  args to l  default to n
    i l length < if  i l nth string string>number if  to n  else  drop  then  then  n ;
: site-port ( -- n )  0 8080 arg ;

s" db" db-replica constant db             \ (the database's name: work-queue.fs registered it)
db <session-store> constant sessions
templates views                           \ views/task-row.html: task-row ( record -- )

\ ---- JSON, read and written a little more easily ----
: field$ ( key n v -- a n )  json-string@ 0= if  s" "  then ;   \ a string field, "" if none
: field# ( key n v -- n )  json-int@ 0= if  0  then ;          \ a whole number field, 0 if none
: put$ ( a n key n m -- )  {: a n ka kn m :}  a n >string drop ka kn m s-at! ;
: flash! ( a n -- )  s" flash" state-string! ;
: state$ ( key n -- a n )  state field$ ;

\ ---- every page: its document, its header, its flash ----
: page-layout ( content-xt -- )
    {: xt :}
    doctype  <html  <head
        <meta s" utf-8" charset=
        <meta s" viewport" name=  s" width=device-width, initial-scale=1" content=
        <title s" Work queue" text title>
        <link s" stylesheet" rel=  s" /static/style.css" href=
        live-script
    head>  <body  xt execute  body>  html> ;
: top ( -- )
    <header
        <h1  <a s" /" href=  s" Work queue" text  a>  h1>
        <a s" /new" href=  s" New task" text  a>
        <span s" muted" class=  s" user" state$ text  span>
        <form s" /logout" action=  s" post" method=
            <input s" hidden" type=  s" csrf" name=  s" csrf" state$ value=
            <button s" submit" type=  s" Sign out" text  button>
        form>
    header> ;
: flash ( -- )   \ (a region: what the last event said)
    s" flash" state$ dup if  <div s" flash" class=  text  div>  else  2drop  then ;

\ every page's request: its user and CSRF token (from its session), or the way to sign in
: signed-in ( req params -- continue? )
    {: r p | s :}  r sessions session to s
    s session-user 0= if  s" /login" 303 r redirect  false exit  then  s" user" p put$
    s csrf-token s" csrf" p put$
    true ;
: who-from ( params -- )  {: p :}  s" user" p field$ s" user" state-string!  s" csrf" p field$ s" csrf" state-string! ;

\ ---- the queue: tasks filtered (status, who, a search), sorted by priority; a click claims one ----
: queue-query ( -- a n )   \ the filters as a jsondb query
    {: | q w :}  <map> to q  <map> to w
    s" status" state$ {: sa sn :}  sa sn s" any" s= 0= if  sa sn s" status" w put$  then
    s" who" state$ {: wa wn :}
    wa wn s" me" s= if  s" user" state$ s" assignee" w put$  then
    wa wn s" nobody" s= if  json-null s" assignee" w s-at!  then
    s" q" state$ {: qa qn :}  qn if  <map> {: c :}  qa qn s" contains" c put$  c s" title" w s-at!  then
    w s" where" q s-at!
    { " -priority" " title" } s" sort" q s-at!
    100 s" limit" q s-at!
    q >json ;
: load-tasks ( -- )
    queue-query s" tasks" db db-query json> {: r :}
    s" records" r json-list@ drop  s" tasks" state!
    s" total" r field# s" total" state! ;
: results ( -- )   \ (a region: the tasks)
    s" total" state field# {: n :}
    <p s" muted" class=  n number  n 1 = if  s"  task"  else  s"  tasks"  then  text  p>
    <table
        <thead  <tr  <th s" Task" text th>  <th s" Status" text th>  <th s" Priority" text th>  <th s" Who" text th>  tr>  thead>
        <tbody  s" tasks" state json-list@ drop [: task-row ;] each  tbody>
    table> ;
: choices ( list name n current n -- )   \ a select of the choices, the current one selected
    {: l na nn ca cn :}
    <select  na nn name=  na nn id=
        l [: {: o :}  <option  o string value=  o string ca cn s= selected=  o string text  option> ;] each
    select> ;
: filters ( -- )
    <form  s" filters" class=  s" filter" live-change=
        { " any" " open" " in progress" " done" } s" status" s" status" state$ choices
        { " anyone" " me" " nobody" } s" who" s" who" state$ choices
        <input  s" search" type=  s" q" name=  s" q" id=  s" Search titles" placeholder=  s" q" state$ value=
    form> ;
: queue-render ( -- )  top  <main  s" flash" ['] flash region  filters  s" results" ['] results region  main> ;
: queue-mount ( params -- )
    who-from
    s" open" s" status" state-string!  s" anyone" s" who" state-string!  s" " s" q" state-string!  s" " flash!
    load-tasks  s" tasks" db db-watch drop ;
: filter ( form -- )
    {: f :}  s" status" f field$ s" status" state-string!  s" who" f field$ s" who" state-string!
    s" q" f field$ s" q" state-string!  s" " flash!  load-tasks ;
: claim ( id -- )   \ the task given to the user - unless someone was first (its revision: put-if)
    {: id | rec rev :}
    id string? 0= if  exit  then
    s" tasks" id string db db-get-rev 0= if  db-error flash! exit  then  to rev  json> to rec
    s" assignee" rec field$ nip if  s" Someone claimed it first." flash!  load-tasks exit  then
    s" user" state$ s" assignee" rec put$  s" in progress" s" status" rec put$
    rec >json rev s" tasks" id string db db-put-if if  drop  s" Claimed." flash!  else  db-error flash!  then
    load-tasks ;
: queue-changed ( c id rev json -- )  2drop 2drop  load-tasks ;
live-view queue-view
    request: signed-in
    mount: queue-mount
    render: queue-render
    layout: page-layout
    event: filter filter
    event: claim claim
    changed: queue-changed
end-live-view

\ ---- a task: its fields in a form, checked, saved with its revision; told if someone else changes it meanwhile ----
: task$ ( key n -- a n )  {: ka kn :}  s" task" state json-map@ if  ka kn rot field$  else  s" "  then ;
: task# ( key n -- n )  {: ka kn :}  s" task" state json-map@ if  ka kn rot field#  else  0  then ;
: error-for ( key n -- )   \ what is wrong with the field, if anything
    {: ka kn :}  s" errors" state json-map@ if  {: e :}  ka kn e json-string@ if  <span s" error" class= text span>  then  then ;
: labelled ( label n key n -- )  {: la ln ka kn :}  <label  ka kn for=  la ln text  label> ;
\ a record from a form's fields: checked (each field's error in errors); false if any is wrong
: checked ( form -- rec true | false )
    {: f | errs rec :}  <map> to errs  <map> to rec
    s" title" f field$ s-trim {: ta tn :}  tn 0= if  s" Give it a title." s" title" errs put$  then
    s" priority" f field$ string>number 0= if  drop 0  then {: p :}
    p 1 6 within 0= if  s" From 1 to 5." s" priority" errs put$  then
    errs s" errors" state!
    s" errors" state json-map@ drop map-size if  false exit  then
    ta tn s" title" rec put$  p s" priority" rec s-at!
    s" status" f field$ s" status" rec put$  s" notes" f field$ s" notes" rec put$
    s" assignee" f field$ s-trim {: aa an :}  an if  aa an s" assignee" rec put$  then
    rec true ;
: priorities ( -- l )  { " 1" " 2" " 3" " 4" " 5" } ;
: task-fields ( -- )   \ the form's fields, from the state's task
    <div s" field" class=  s" Title" s" title" labelled
        <input  s" title" id=  s" title" name=  s" title" task$ value=  s" title" error-for  div>
    <div s" field" class=  s" Status" s" status" labelled
        { " open" " in progress" " done" } s" status" s" status" task$ choices  div>
    <div s" field" class=  s" Priority" s" priority" labelled
        priorities s" priority" s" priority" task# s" {}" format choices  s" priority" error-for  div>
    <div s" field" class=  s" Who" s" assignee" labelled
        <input  s" assignee" id=  s" assignee" name=  s" assignee" task$ value=  div>
    <div s" field" class=  s" Notes" s" notes" labelled
        <textarea  s" notes" id=  s" notes" name=  s" notes" task$ text  textarea>  div> ;
: task-form ( -- )   \ (a region)
    <form  s" save" live-submit=  task-fields  <button s" submit" type=  s" Save" text  button>  form> ;
: banner ( -- )   \ (a region: someone else changed it)
    s" stale" state field# if
        <div s" flash warn" class=  s" Someone else changed this task. " text
            <button  s" reload" live-click=  s" Show their version" text  button>  div>
    then ;
: load-task ( -- )
    {: | rev :}
    s" tasks" s" id" state$ db db-get-rev if  to rev  json> s" task" state!  rev s" rev" state!  0 s" stale" state!
    else  s" That task isn't there." flash!  then
    <map> s" errors" state! ;
: task-mount ( params -- )
    {: p :}  p who-from  s" id" p field$ s" id" state-string!  s" " flash!
    load-task  s" tasks" db db-watch drop ;
: heading ( -- )  <h2  s" title" task$ text  h2> ;
: task-render ( -- )
    top  <main  s" banner" ['] banner region  s" flash" ['] flash region
        s" heading" ['] heading region  s" form" ['] task-form region  main> ;
: save ( form -- )   \ with the revision it was read at: someone else's save between is said, not overwritten
    checked 0= if  s" " flash! exit  then  {: rec :}
    rec >json  s" rev" state field#  s" tasks" s" id" state$ db db-put-if if
        drop  load-task  s" Saved." flash!
    else  s" Someone else saved this task first - your changes weren't saved." flash!  1 s" stale" state!  then ;
: reload ( v -- )  drop  load-task  s" " flash! ;
: task-changed ( c id rev json -- )   \ a later revision of this task than the page's: someone else's
    {: c i r j :}  i string s" id" state$ s=  r s" rev" state field# >  and if  1 s" stale" state!  then ;
live-view task-view
    request: signed-in
    mount: task-mount
    render: task-render
    layout: page-layout
    event: save save
    event: reload reload
    changed: task-changed
end-live-view

\ ---- a new task ----
: new-mount ( params -- )
    who-from  <map> s" task" state!  <map> s" errors" state!  s" " flash! ;
: new-render ( -- )  top  <main  <h2 s" New task" text h2>  s" form" ['] task-form region  main> ;
: create ( form -- )
    checked 0= if  exit  then  {: rec :}
    s" status" rec field$ nip 0= if  s" open" s" status" rec put$  then
    rec >json s" tasks" db db-insert if  s" /tasks/{s}" format live-redirect  else  db-error flash!  then ;
live-view new-view
    request: signed-in
    mount: new-mount
    render: new-render
    layout: page-layout
    event: save create
end-live-view

\ ---- signing in and out: plain pages ----
: login-page ( req error n -- )
    {: r ea en | s :}  r sessions session to s
    [:  doctype  <html  <head
            <meta s" utf-8" charset=  <title s" Sign in" text title>
            <link s" stylesheet" rel=  s" /static/style.css" href=
        head>
        <body  <form  s" login" class=  s" /login" action=  s" post" method=
            <h2 s" Sign in" text h2>
            en if  <p s" error" class=  ea en text  p>  then
            <input  s" hidden" type=  s" csrf" name=  s csrf-token value=
            <div s" field" class=  s" Name" s" username" labelled
                <input  s" username" id=  s" username" name=  true autofocus=  div>
            <div s" field" class=  s" Password" s" password" labelled
                <input  s" password" id=  s" password" name=  s" password" type=  div>
            <button s" submit" type=  s" Sign in" text  button>
            <p s" muted" class=  s" ann or bob; the password: queue" text  p>
        form>  body>  html> ;] html
    string 200 r respond-html ;
: login-get ( req -- )  s" " login-page ;
: login-post ( req -- )
    {: r | s ok :}  r sessions session to s
    s" username" r form-param 0= if  s" "  then  {: ua un :}
    s" password" r form-param 0= if  s" "  then  {: pa pn :}
    s" users" ua un db db-get if  json> {: u :}  pa pn  s" password" u field$  check-password to ok  then
    ok if  ua un s log-in  s" /" 303 r redirect
    else  r s" That name and password don't match." login-page  then ;
: logout ( req -- )  {: r :}  r sessions session log-out  s" /login" 303 r redirect ;

\ ---- the site ----
<router> constant site
site security-headers
sessions site require-csrf
source-dir s" /public" s+ s" /static" site serve-files
s" /login" ' login-get site on-get
s" /login" ' login-post site on-post
s" /logout" ' logout site on-post
s" /" queue-view site live-route
s" /tasks/:id" task-view site live-route
s" /new" new-view site live-route
site site-port http-listen drop

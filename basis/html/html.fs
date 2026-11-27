\ html.fs - HTML written as Forth: a word for each element, its attributes, its text (escaped) - each adding to the
\ string being built (build's %), so a page or a part of one is a word, and a component a word that calls others.
\
\   : task-row ( task -- )
\       {: t :}
\       <tr  s" row" class=
\           <td  s" title" t json-string@ drop text  td>
\           <td  <button  s" claim" class=  s" Claim" text  button>  td>
\       tr> ;
\   [: tasks [: task-row ;] each ;] html  ( -- s )
\
\ <tag opens an element (attributes come after it, before its content: id= class= attr= ...); tag> closes it; a void
\ element (<input <br <img ...) has no closing word. text is escaped; raw isn't - the only way markup goes in as it
\ is. Attribute values are escaped too.
IN: html

<PRIVATE
\ the bytes written as entities - & < > " ' - marked, for s-marked to find the next (made once, read by every handler)
create special 256 allot
: mark-special ( -- )  special 256 erase  s\" &<>\"'" {: a n :}  n 0 do  1 special a i + c@ + c!  loop ;
mark-special
: entity ( c -- )   \ its entity onto what is being built
    case
        [char] & of  s" &amp;" %s  endof
        [char] < of  s" &lt;" %s  endof
        [char] > of  s" &gt;" %s  endof
        [char] " of  s" &quot;" %s  endof
        s" &#39;" %s
    endcase ;
PRIVATE>

\ the text onto what is being built, escaped (the runs with nothing to escape added whole: found by s-marked, not
\ looked for a byte at a time)
: %escaped ( a n -- )
    {: a n | i j :}
    begin
        a i +  n i -  special s-marked  i + to j
        a i +  j i -  %s
        j n <
    while
        a j + c@ entity  j 1+ to i
    repeat ;

\ the text with & < > " and ' as their entities (a new string): for an element's text and an attribute's quoted value
: html-escape ( a n -- s len )  {: a n :}  [: a n %escaped ;] build string ;

\ ---- elements ----
<PRIVATE
\ (the build's mark: true while an element's start tag is open - its attributes still to come)
: open? ( -- flag )  build-mark@ ;
PRIVATE>
\ the start tag open closed (its attributes over): what any content, child or end does first
: close-start ( -- )  open? if  [char] > %c  false build-mark!  then ;
\ an element of that name begun: <name, its attributes to come
: open-element ( name n -- )  {: a n :}  close-start  [char] < %c  a n %s  true build-mark! ;
: close-element ( name n -- )  {: a n :}  close-start  s" </" %s  a n %s  [char] > %c ;   \ its end tag

\ an attribute of the element just begun (before its content): name="value", the value escaped
: attr= ( value n name n -- )
    {: va vn na nn :}
    open? 0= abort" attr=: only right after an element is begun, before its content"
    32 %c  na nn %s  s\" =\"" %s  va vn %escaped  [char] " %c ;
\ a URL a link or a source may hold: one of the site's (a path, ?query, #part, relative) or http(s):, mailto:, tel: -
\ not javascript:, data:, vbscript: ... (what a user typed, put in a link, would run as the page). Its scheme read as a
\ browser reads it: spaces, tabs and line ends anywhere in it passed over (java\tscript: is javascript:)
: safe-url? ( a n -- flag )
    {: a n | b :}  <bytes> to b
    n 0 ?do  a i + c@ {: c :}  c 32 > if  c b b-push  then  loop   \ (what a browser drops, dropped)
    b b-data {: ca cn :}  -1 {: colon :}
    cn 0 ?do  ca i + c@ {: ch :}
        ch [char] : = if  i to colon leave  then
        ch [char] / =  ch [char] ? = or  ch [char] # = or if  leave  then
    loop
    colon 0< if  b dispose  true exit  then   \ (no scheme: the site's own)
    ca colon s-lower {: sa sn :}
    sa sn s" http" s=  sa sn s" https" s= or  sa sn s" mailto" s= or  sa sn s" tel" s= or
    sa dispose  b dispose ;
\ a URL attribute (href, src, action): its value escaped - one not safe-url? written as "#", a link to nowhere
: url-attr= ( value n name n -- )
    {: va vn na nn :}  va vn safe-url? 0= if  s" #" to vn to va  then  va vn na nn attr= ;
\ a flag attribute (checked, disabled ...): there if the flag is true
: flag-attr= ( flag name n -- )
    {: flag na nn :}
    open? 0= abort" flag-attr=: only right after an element is begun, before its content"
    flag if  32 %c  na nn %s  then ;

: text ( a n -- )  close-start  %escaped ;   \ text, escaped
\ a URL as text in an attribute a template writes: as url-attr= does
: url-text ( a n -- )  {: a n :}  a n safe-url? if  a n text  else  s" #" text  then ;
: raw ( a n -- )  close-start  %s ;          \ markup as it is: only what you made, never what a user wrote
: text-of ( s -- )  string text ;            \ a string object's, escaped
: number ( n -- )  close-start  %n ;         \ a number, in decimal
: doctype ( -- )  s" <!doctype html>" raw ;

\ (No <u: its u> would hide the core's unsigned comparison - `s" u" open-element ... s" u" close-element`.)
\ HTML built: the code's elements, the last start tag closed - the string. (build, for HTML.)
: html ( xt -- s )  {: xt :}  [: xt execute  close-start ;] build ;

<PRIVATE
\ a word for each element: <name and name> (a void one: <name only), and for the attributes
: elements ( names n -- )
    {: a n :}
    a n >string drop s"  " >string drop split [: string {: ea en :}
        ea en ea en s\" : <{s} ( -- ) s\" {s}\" open-element ;" format evaluate
        ea en ea en s\" : {s}> ( -- ) s\" {s}\" close-element ;" format evaluate ;] each ;
: voids ( names n -- )
    {: a n :}
    a n >string drop s"  " >string drop split [: string {: ea en :}
        ea en ea en s\" : <{s} ( -- ) s\" {s}\" open-element ;" format evaluate ;] each ;
: attributes ( names n -- )   \ name= ( value n -- )
    {: a n :}
    a n >string drop s"  " >string drop split [: string {: ea en :}
        ea en ea en s\" : {s}= ( a n -- ) s\" {s}\" attr= ;" format evaluate ;] each ;
: flag-attributes ( names n -- )   \ name= ( flag -- )
    {: a n :}
    a n >string drop s"  " >string drop split [: string {: ea en :}
        ea en ea en s\" : {s}= ( flag -- ) s\" {s}\" flag-attr= ;" format evaluate ;] each ;
PRIVATE>

s" html head body title script style noscript template main header footer nav section article aside div span p a"
elements
s" h1 h2 h3 h4 h5 h6 ul ol li dl dt dd table thead tbody tfoot tr th td caption colgroup form button select option"
elements
s" optgroup textarea label fieldset legend output datalist progress meter details summary dialog figure figcaption"
elements
s" blockquote pre code kbd samp var strong em small b i s q cite abbr mark time sub sup address picture video audio"
elements
s" canvas iframe svg menu search hgroup"
elements
s" meta link input img br hr col source track wbr area base embed" voids
s" id class type name value placeholder for method title alt rel style target rows cols min max"
attributes
: href= ( a n -- )  s" href" url-attr= ;   : src= ( a n -- )  s" src" url-attr= ;   \ (URLs: checked - safe-url?)
: action= ( a n -- )  s" action" url-attr= ;   : formaction= ( a n -- )  s" formaction" url-attr= ;
s" step pattern autocomplete colspan rowspan width height lang charset content enctype accept role tabindex"
attributes
s" checked selected disabled required readonly autofocus multiple hidden open novalidate" flag-attributes

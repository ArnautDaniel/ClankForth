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

\ the text with & < > " and ' as their entities (a new string): for an element's text and an attribute's quoted value
: html-escape ( a n -- s len )
    {: a n | b :}  <bytes> to b
    n 0 ?do
        a i + c@ {: c :}
        c case
            [char] & of  s" &amp;" b b-append  endof
            [char] < of  s" &lt;" b b-append  endof
            [char] > of  s" &gt;" b b-append  endof
            [char] " of  s" &quot;" b b-append  endof
            [char] ' of  s" &#39;" b b-append  endof
            c b b-push
        endcase
    loop
    b b>string  b dispose ;

<PRIVATE
: special? ( c -- flag )
    {: c :}  c [char] & =  c [char] < = or  c [char] > = or  c [char] " = or  c [char] ' = or ;
: entity ( c -- )   \ its entity onto what is being built
    case
        [char] & of  s" &amp;" %s  endof
        [char] < of  s" &lt;" %s  endof
        [char] > of  s" &gt;" %s  endof
        [char] " of  s" &quot;" %s  endof
        s" &#39;" %s
    endcase ;
PRIVATE>

\ the text onto what is being built, escaped (the runs with nothing to escape added whole)
: %escaped ( a n -- )
    {: a n | from :}
    n 0 ?do
        a i + c@ special? if  a from + i from - %s  a i + c@ entity  i 1+ to from  then
    loop
    a from + n from - %s ;

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
\ a flag attribute (checked, disabled ...): there if the flag is true
: flag-attr= ( flag name n -- )
    {: flag na nn :}
    open? 0= abort" flag-attr=: only right after an element is begun, before its content"
    flag if  32 %c  na nn %s  then ;

: text ( a n -- )  close-start  %escaped ;   \ text, escaped
: raw ( a n -- )  close-start  %s ;          \ markup as it is: only what you made, never what a user wrote
: text-of ( s -- )  string text ;            \ a string object's, escaped
: number ( n -- )  close-start  %n ;         \ a number, in decimal
: doctype ( -- )  s" <!doctype html>" raw ;

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
s" blockquote pre code kbd samp var strong em small b i u s q cite abbr mark time sub sup address picture video audio"
elements
s" canvas iframe svg menu search hgroup"
elements
s" meta link input img br hr col source track wbr area base embed" voids
s" id class href src type name value placeholder for action method title alt rel style target rows cols min max"
attributes
s" step pattern autocomplete colspan rowspan width height lang charset content enctype accept role tabindex"
attributes
s" checked selected disabled required readonly autofocus multiple hidden open novalidate" flag-attributes

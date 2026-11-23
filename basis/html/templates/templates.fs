\ templates.fs - HTML files compiled into Forth words as they load: HTML with holes, read at a glance by a person or a
\ model. Each a word ( v -- ) that adds its HTML to what is being built (html, build), its holes the fields of v.
\
\   <tr id="task-{id}">                       task-row.html: the word task-row ( v -- )
\     <td>{title}</td>
\     <td>{#if done}done{#else}open{/if}</td>
\   </tr>
\
\   templates views                           \ every .html in the folder views/ (beside this file), a word each
\   [: tasks task-table ;] html               ( -- s )
\
\ {name} a field of v (a JSON object: what json> gives), escaped - {a.b} a field of a field; {.} v itself;
\ {#each name}...{/each} each item of a list (the item inside), {#each} v's own; {#if name}...{#else}...{/if} the
\ field there and not false, null, 0, "" or empty; <task-row/> another template's word (or any word ( v -- )), given v
\ - a name with a hyphen, as a custom element's; {{ and }} braces. Nothing else: logic is Forth's, in words.
IN: html.templates
USING: html json system accessors ;

<PRIVATE
: path-part ( v a n -- x true | false )   \ one field of v
    {: v a n :}  v map? 0= if  false exit  then  a n v s-at ;
PRIVATE>

\ ---- what a template's word calls ----
\ the value at the path (a.b.c: a field of a field ...; "." v itself)
: (tpl-field) ( v path n -- x true | false )
    {: v a n | x at :}
    a n s" ." s= if  v true exit  then
    v to x
    begin
        a n s" ." s-find to at
        at 0< if  x a n path-part exit  then
        x a at path-part 0= if  false exit  then  to x
        a at 1+ +  n at - 1-  to n to a
    again ;
\ the value at the path as text, escaped: a string itself; a whole number in decimal; true as "true"; false, null and
\ none as nothing
: (tpl-url-hole) ( v path n -- )   \ a hole a URL attribute's value begins with: as url-attr= checks it
    (tpl-field) 0= if  exit  then  {: x :}
    x string? if  x string url-text exit  then
    x object? if  true s" template: a URL's hole is a list or an object" (abort")  then
    x number ;
: (tpl-hole) ( v path n -- )
    (tpl-field) 0= if  exit  then  {: x :}
    x string? if  x string text exit  then
    x json-true = if  s" true" text exit  then
    x json-false =  x json-null = or if  exit  then
    x json-float instance? if  x value>> f>string text exit  then
    x object? if  true s" template: a hole's value is a list or an object (give a field of it)" (abort")  then
    x number ;
\ the value at the path there, and not false, null, 0, "" or an empty list
: (tpl-truthy) ( v path n -- flag )
    (tpl-field) 0= if  false exit  then  {: x :}
    x json-false =  x json-null = or  x 0= or if  false exit  then
    x string?  x list? or if  x length 0<>  exit  then
    true ;
\ the list at the path (none, or not a list: an empty one)
: (tpl-items) ( v path n -- l )
    (tpl-field) 0= if  { } exit  then  dup list? 0= if  drop { }  then ;

<PRIVATE
\ ---- compiling a template: its text into a Forth definition's ----
variable tpl-name   variable tpl-file   variable tpl-text   \ (what is being compiled: for its errors)
: line-at ( at -- n )   \ the template's line the byte at `at` is on
    {: at | n :}  1 to n  tpl-text @ string drop {: a :}
    at 0 ?do  a i + c@ 10 = if  n 1+ to n  then  loop  n ;
: tpl-error ( at a n -- )
    {: at a n :}  true  tpl-file @ string  at line-at  a n  s" template {s}:{}: {s}" format  (abort") ;

\ literal text as Forth that adds it: s\" ..." raw, in pieces s\" can hold
2000 constant piece-max
variable url-next   \ the text before a hole ends href=" (src, action, formaction ...): the hole begins a URL
: url-attribute? ( a n -- flag )   \ the text ends where a URL attribute's value begins
    {: a n | l ln found :}  a n s-lower to ln to l
    s\" href=\" src=\" action=\" formaction=\" poster=\" href=' src=' action=' formaction=' poster='" s"  " s-split
    [: string {: pa pn :}  ln pn >= if  l ln + pn - pn pa pn s= if  true to found  then  then ;] each
    l dispose  found ;
: literal ( a n -- )
    {: a n | from :}
    a n url-attribute? url-next !
    begin  from n <  while
        s\" s\\\" " %s
        n from - piece-max min 0 ?do
            a from + i + c@ {: c :}
            c [char] " =  c [char] \ = or if  [char] \ %c  c %c
            else c 10 = if  s\" \\n" %s
            else c 13 = if  s\" \\r" %s
            else c 9 = if  s\" \\t" %s
            else c 32 < if  c s" \x{02x}" format {: e en :}  e en %s  e dispose
            else  c %c  then then then then then
        loop
        s\" \" html:raw " %s
        from piece-max + to from
    repeat ;

: path-char? ( c -- flag )
    {: c :}  c [char] a [char] z 1+ within  c [char] A [char] Z 1+ within or  c [char] 0 [char] 9 1+ within or
    c [char] _ = or  c [char] - = or  c [char] . = or ;
: ?path ( a n at -- )   \ a hole's path: letters, digits, _ - .
    {: a n at :}
    n 0= if  at s" an empty hole" tpl-error  then
    n 0 ?do  a i + c@ path-char? 0= if  at a n s" a hole's path is letters, digits, _ - and . (not {s})" format tpl-error  then  loop ;

variable nesting   \ each blocks within each other: the item's local v0, v1 ...
: v ( -- )  nesting @ s" v{} " format {: s n :}  s n %s  s dispose ;   \ the value in scope
: path-arg ( a n -- )  {: a n :}  s\" s\" " %s  a n %s  s\" \" " %s ;

\ the blocks open (a list of their kinds: each, if, else) and where each began
create blocks 64 cells allot   create block-at 64 cells allot   variable nblocks
1 constant each-block   2 constant if-block   3 constant else-block
: open-block ( kind at -- )
    {: kind at :}
    nblocks @ 64 = if  at s" blocks within each other too deep" tpl-error  then
    kind blocks nblocks @ cells + !  at block-at nblocks @ cells + !  1 nblocks +! ;
: top-block ( -- kind )  nblocks @ 0= if  0  else  blocks nblocks @ 1- cells + @  then ;
: close-block ( -- )  -1 nblocks +! ;

: tag ( a n at -- )   \ {...}'s inside: a hole, or a block's start, else or end
    {: a n at :}
    a n s-trim to n to a
    a n s" #each" s-starts? if
        a 5 + n 5 - s-trim {: pa pn :}  pn 0= if  s" ." to pn to pa  then
        pa pn at ?path
        v pa pn path-arg  s" html.templates:(tpl-items) forth:[: " %s
        1 nesting +!  s" forth:{: " %s v s" :} " %s
        each-block at open-block exit
    then
    a n s" #if" s-starts? if
        a 3 + n 3 - s-trim {: qa qn :}  qa qn at ?path
        v qa qn path-arg  s" html.templates:(tpl-truthy) forth:if " %s
        if-block at open-block exit
    then
    a n s" #else" s= if
        top-block if-block <> if  at s" {#else} with no {#if} open" tpl-error  then
        s" forth:else " %s  close-block  else-block at open-block exit
    then
    a n s" /each" s= if
        top-block each-block <> if  at s" {/each} with no {#each} open" tpl-error  then
        s" forth:;] forth:each " %s  -1 nesting +!  close-block exit
    then
    a n s" /if" s= if
        top-block dup if-block <>  swap else-block <> and if  at s" {/if} with no {#if} open" tpl-error  then
        s" forth:then " %s  close-block exit
    then
    a c@ [char] # =  a c@ [char] / = or if  at a n s" no such block: {{{s}}}" format tpl-error  then
    a n at ?path
    v a n path-arg  url-next @ if  s" html.templates:(tpl-url-hole) " %s  else  s" html.templates:(tpl-hole) " %s  then
    false url-next ! ;

\ a component: <name-with-hyphen/> at i - its length (0: none there)
: component-at ( a n i -- len )
    {: a n i | j hyphen :}
    i 1+ to j
    begin  j n <  if  a j + c@ {: c :}  c [char] a [char] z 1+ within  c [char] 0 [char] 9 1+ within or  c [char] - = or  else  false  then
    while  a j + c@ [char] - = if  true to hyphen  then  j 1+ to j  repeat
    hyphen 0=  j i 1+ = or if  0 exit  then
    begin  j n <  if  a j + c@ 32 =  else  false  then  while  j 1+ to j  repeat
    j 2 + n <=  if  a j + 2 s" />" s=  else  false  then  0= if  0 exit  then
    j 2 + i - ;
: component ( name n at -- )
    {: a n at :}
    a n find-word 0= if  at a n s" <{s}/>: no word of that name (a template, or a word ( v -- ), defined before)" format tpl-error  then
    v a n %s  32 %c ;

\ the template's text as the body of a Forth definition ( v0 -- ): what it adds, a piece at a time
: body ( a n -- )
    {: a n | i from :}
    0 nblocks !  0 nesting !
    begin  i n <  while
        a i + c@ {: c :}
        c [char] { =  if
            a from + i from - literal
            i 1+ n <  if  a i + 1+ c@ [char] { =  else  false  then  if
                s" {" literal  i 2 + to i  i to from
            else
                a i + n i - s" }" s-find {: j :}
                j 0< if  i s" a { not closed by }" tpl-error  then
                a i + 1+  j 1-  i tag
                i j + 1+ to i  i to from
            then
        else c [char] } =  if
            a from + i from - literal
            i 1+ n <  if  a i + 1+ c@ [char] } =  else  false  then  if  i 2 + to i  else  i 1+ to i  then
            s" }" literal  i to from
        else c [char] < =  if
            a n i component-at {: len :}
            len if
                a from + i from - literal
                a i + 1+  len 3 -  s-trim  i component
                i len + to i  i to from
            else  i 1+ to i  then
        else  i 1+ to i  then then then
    repeat
    a from + n from - literal
    nblocks @ if  block-at nblocks @ 1- cells + @  s" a block not closed (its {/...} missing)" tpl-error  then ;

\ the Forth text of the template's word: `: name ( v0 -- )  {: v0 :}  ... ;` (or `:noname ...` for is)
: definition ( text n name n noname? -- s )
    {: ta tn na nn noname? :}
    [:  noname? if  s" :noname ( v0 -- ) " %s  else  s" : " %s  na nn %s  s"  ( v0 -- ) " %s  then
        s" forth:{: v0 :} " %s  ta tn body  s" ; " %s ;] build ;
: compiling ( text n name n file n -- )   \ (what is being compiled, for errors)
    {: ta tn na nn fa fn :}
    na nn >string drop tpl-name !  fa fn >string drop tpl-file !  ta tn >string drop tpl-text ! ;
: name-of ( path n -- name n )   \ its file's name without .html
    {: a n | at :}  -1 to at
    n 0 ?do  a i + c@ [char] / =  if  i to at  then  loop
    a at 1+ +  n at - 1- {: fa fn :}
    fa fn s" .html" s-ends? if  fn 5 - to fn  then
    fa fn ;
: beside ( path n -- s len )   \ a path from the folder of the file being read (one from / as it is)
    {: a n :}  n if  a c@ [char] / = if  a n >string exit  then  then
    source-dir s" /" s+  a n s+ ;
PRIVATE>

\ a template's word, defined from its text: name ( v -- ) in the current vocabulary
: template-text ( text n name n -- )
    {: ta tn na nn :}  ta tn na nn na nn compiling
    ta tn na nn false definition string evaluate ;
\ the template in the file (a path from the folder of the file being read): a word of the file's name, without .html
: template-file ( path n -- )
    {: pa pn :}  pa pn beside {: fa fn :}
    fa fn read-file {: ta tn :}
    fa fn name-of {: na nn :}
    ta tn na nn fa fn compiling
    ta tn na nn false definition string evaluate ;
\ every .html in the folder: a word each - all declared first, so one may use another whatever their order
: templates-in ( dir n -- )
    {: da dn | names :}  da dn beside {: fa fn :}
    fa fn dir-list [: string s" .html" s-ends? ;] filter to names
    names [: string name-of s" defer {s} ( v -- )" format evaluate ;] each
    names [: string {: ea en :}
        fa fn s" /" s+  ea en s+ {: pa pn :}
        pa pn read-file {: ta tn :}
        pa pn name-of {: na nn :}
        ta tn na nn pa pn compiling
        ta tn na nn true definition string evaluate   \ (:noname ... ; - its xt the deferred word's)
        na nn find-word >body ! ;] each ;
: template ( "path" -- )  parse-name template-file ;     \ template views/task-row.html
: templates ( "dir" -- )  parse-name templates-in ;      \ templates views

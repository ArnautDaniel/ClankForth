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
    {: v a n | x at :}  a n s-bytes drop to a
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
    x json-float instance? if  x value>> f>exact text exit  then
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
    {: at | n :}  1 to n  tpl-text @ string s-bytes drop {: a :}
    at 0 ?do  a i + c@ 10 = if  n 1+ to n  then  loop  n ;
: tpl-error ( at a n -- )
    {: at a n :}  true  tpl-file @ string  at line-at  a n  s" template {s}:{}: {s}" format  (abort") ;

\ literal text as Forth that adds it: s\" ..." raw, in pieces s\" can hold
2000 constant piece-max
\ ---- where a hole is: the template's HTML read as a browser would, up to it ----
\ In text or a quoted attribute's value a hole is escaped; at the start of a URL attribute's value (href, src ...)
\ checked as a URL too; anywhere else - a tag's name, an unquoted value, an on* or style attribute, <script> or
\ <style> - escaping can't make it safe, and the template is refused as it compiles.
0 constant in-text      1 constant tag-opened   2 constant in-tag      3 constant attr-name   4 constant after-name
5 constant before-value 6 constant in-double    7 constant in-single   8 constant unquoted    9 constant in-comment
10 constant in-raw      11 constant end-tag     12 constant in-decl
variable html-state   variable raw-script   \ (in-raw: true inside <script>, false inside <style>)
create tag-name 16 allot   variable tag-n      \ (the tag's name, its first 16 bytes, lower case)
create attr 32 allot       variable attr-n     \ (the attribute's, its first 32)
variable value-start   variable prev1   variable prev2   \ (no byte of the value yet but spaces; the bytes before)
: space? ( c -- flag )  {: c :}  c 32 =  c 9 = or  c 10 = or  c 13 = or  c 12 = or ;
: letter? ( c -- flag )  32 or [char] a [char] z 1+ within ;
: lower ( c -- c' )  dup [char] A [char] Z 1+ within if  32 or  then ;
: add-to ( c buf n-var max -- )  {: c buf nv mx :}  nv @ mx < if  c lower  buf nv @ + c!  1 nv +!  then ;
: start-attr ( c -- )  0 attr-n !  attr attr-n 32 add-to  attr-name html-state ! ;
: tag-is? ( a n -- flag )  tag-name tag-n @ 2swap s= ;
: tag-ends ( -- )   \ a start tag's > : <script> and <style> hold raw text till their end tag
    s" script" tag-is? if  true raw-script !  in-raw html-state ! exit  then
    s" style" tag-is? if  false raw-script !  in-raw html-state ! exit  then
    in-text html-state ! ;
: ahead? ( a n i s sn -- flag )   \ s at a+i (case ignored)
    {: a n i sa sn :}  a n s-bytes drop to a  i sn + n > if  false exit  then
    a i + sn s-lower {: l ln :}  l ln sa sn s=  l dispose ;
: step ( a n i -- )   \ the byte at a+i read
    {: a n i :}  a n s-bytes drop to a  a i + c@ {: c :}
    html-state @ case
        in-text of  c [char] < = if  0 tag-n !  tag-opened html-state !  then  endof
        tag-opened of
            c letter? if  c tag-name tag-n 16 add-to  else
            tag-n @ 0= if
                c [char] / = if  end-tag html-state !  else  c [char] ! = if  in-decl html-state !  else
                in-text html-state !  then then
            else  c [char] > = if  tag-ends  else  in-tag html-state !  then  then then  endof
        in-tag of  c [char] > = if  tag-ends  else  c space?  c [char] / = or 0= if  c start-attr  then  then  endof
        attr-name of
            c [char] = = if  before-value html-state !  else  c [char] > = if  tag-ends  else
            c space? if  after-name html-state !  else  c [char] / = if  in-tag html-state !  else
            c attr attr-n 32 add-to  then then then then  endof
        after-name of
            c [char] = = if  before-value html-state !  else  c [char] > = if  tag-ends  else
            c space? 0= if  c start-attr  then then then  endof
        before-value of
            true value-start !
            c [char] " = if  in-double html-state !  else  c [char] ' = if  in-single html-state !  else
            c [char] > = if  tag-ends  else  c space? 0= if  unquoted html-state !  then then then then  endof
        in-double of  c [char] " = if  in-tag html-state !  else  c space? 0= if  false value-start !  then  then  endof
        in-single of  c [char] ' = if  in-tag html-state !  else  c space? 0= if  false value-start !  then  then  endof
        unquoted of  c space? if  in-tag html-state !  else  c [char] > = if  tag-ends  then  then  endof
        end-tag of  c [char] > = if  in-text html-state !  then  endof
        in-decl of
            c [char] - =  prev1 @ [char] - = and  prev2 @ [char] ! = and if  in-comment html-state !  else
            c [char] > = if  in-text html-state !  then  then  endof
        in-comment of  c [char] > =  prev1 @ [char] - = and  prev2 @ [char] - = and if  in-text html-state !  then  endof
        in-raw of  c [char] < = if
            a n i 1+  raw-script @ if  s" /script"  else  s" /style"  then  ahead? if  end-tag html-state !  then  then  endof
    endcase
    prev1 @ prev2 !  c prev1 ! ;
: scan ( a n -- )  {: a n :}  n 0 ?do  a n i step  loop ;
: attr-is? ( a n -- flag )  attr attr-n @ 2swap s= ;
: url-attr? ( -- flag )   \ an attribute whose value is a URL a browser follows or loads
    s" href src action formaction poster data cite background longdesc manifest codebase icon usemap xlink:href ping"
    s"  " s-split {: l :}  false  l [: string attr-is? or ;] each ;
\ how the hole here is written: 0 as text, 1 as a URL - else the template is refused, saying why
: hole-kind ( at -- 0 | 1 )
    {: at :}
    html-state @ {: st :}
    st in-text =  st in-comment = or  st in-decl = or if  0 exit  then
    st in-raw = if  at s" a hole inside <script> or <style> (give it data some other way: a data- attribute)" tpl-error  then
    st in-double =  st in-single = or 0= if
        at s\" a hole inside a tag but not in a quoted attribute value (write name=\"{x}\")" tpl-error  then
    attr-n @ 2 >=  if  attr c@ [char] o =  attr 1+ c@ [char] n = and  else  false  then
    s" style" attr-is? or  s" srcdoc" attr-is? or if
        at attr attr-n @ s" a hole in {s}=: script, style or HTML there isn't made safe by escaping" format tpl-error  then
    url-attr? value-start @ and if  1 exit  then   \ (every hole before the value's first literal byte checked as a URL:
    0 ;                                         \  {#else}'s, the one after an empty one - a hole may give nothing)
: literal ( a n -- )
    {: a n | from :}  a n s-bytes drop to a
    a n scan
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
    {: a n at :}  a n s-bytes drop to a
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
    {: a n at :}  a n s-bytes drop to a
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
    at hole-kind {: url? :}
    v a n path-arg  url? if  s" html.templates:(tpl-url-hole) " %s  else  s" html.templates:(tpl-hole) " %s  then ;

\ a component: <name-with-hyphen/> at i - its length (0: none there)
: component-at ( a n i -- len )
    {: a n i | j hyphen :}  a n s-bytes drop to a
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
    {: a n | i from :}  a n s-bytes drop to a
    0 nblocks !  0 nesting !  in-text html-state !  0 prev1 !  0 prev2 !
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
: (definition) ( text n name n noname? -- s )
    {: ta tn na nn noname? :}
    [:  noname? if  s" :noname ( v0 -- ) " %s  else  s" : " %s  na nn %s  s"  ( v0 -- ) " %s  then
        s" forth:{: v0 :} " %s  ta tn body  s" ; " %s ;] build ;
\ (atomically: the scan's state - where in the HTML, the blocks open - is the node's, not each handler's)
: compiling ( text n name n file n -- )   \ (what is being compiled, for errors)
    {: ta tn na nn fa fn :}
    na nn >string drop tpl-name !  fa fn >string drop tpl-file !  ta tn >string drop tpl-text ! ;
: definition ( text n name n file n noname? -- s )   \ (atomically: the names its errors give, and the scan, the node's)
    {: ta tn na nn fa fn noname? :}  [: ta tn na nn fa fn compiling  ta tn na nn noname? (definition) ;] atomically ;
: name-of ( path n -- name n )   \ its file's name without .html
    {: a n | at :}  a n s-bytes drop to a  -1 to at
    n 0 ?do  a i + c@ [char] / =  if  i to at  then  loop
    a at 1+ +  n at - 1- {: fa fn :}
    fa fn s" .html" s-ends? if  fn 5 - to fn  then
    fa fn ;
: beside ( path n -- s len )   \ a path from the folder of the file being read (one from / as it is)
    {: a n :}  a n s-bytes drop to a  n if  a c@ [char] / = if  a n >string exit  then  then
    source-dir s" /" s+  a n s+ ;
PRIVATE>

\ a template's word, defined from its text: name ( v -- ) in the current vocabulary
: template-text ( text n name n -- )
    {: ta tn na nn :}  ta tn na nn na nn false definition string evaluate ;
\ the template in the file (a path from the folder of the file being read): a word of the file's name, without .html
: template-file ( path n -- )
    {: pa pn :}  pa pn beside {: fa fn :}
    fa fn read-file {: ta tn :}
    fa fn name-of {: na nn :}
    ta tn na nn fa fn false definition string evaluate ;
\ every .html in the folder: a word each - all declared first, so one may use another whatever their order
: templates-in ( dir n -- )
    {: da dn | names :}  da dn beside {: fa fn :}
    fa fn dir-list [: string s" .html" s-ends? ;] filter to names
    names [: string name-of s" defer {s} ( v -- )" format evaluate ;] each
    names [: string {: ea en :}
        fa fn s" /" s+  ea en s+ {: pa pn :}
        pa pn read-file {: ta tn :}
        pa pn name-of {: na nn :}
        ta tn na nn pa pn true definition string evaluate   \ (:noname ... ; - its xt the deferred word's)
        na nn find-word >body ! ;] each ;
: template ( "path" -- )  parse-name template-file ;     \ template views/task-row.html
: templates ( "dir" -- )  parse-name templates-in ;      \ templates views

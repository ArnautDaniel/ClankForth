\ html.templates (basis/html/templates): HTML files with holes compiled into words - fields, paths, each, if/else,
\ components (in any order in a folder), escaping; a template said wrong, an error naming its file and line
IN: test-html-templates
USING: tester html html.templates json ;

: is ( s a n -- flag )  rot string s= ;
templates templates   \ (tests/templates: task-row.html, task-table.html - the table using the row)
s\" [{\"id\":1,\"title\":\"Fix <bug>\",\"done\":true,\"owner\":{\"name\":\"ann\"}},{\"id\":2,\"title\":\"Ship\",\"done\":false}]" json> constant tasks
: rendered ( -- a n )  [: tasks task-table ;] html string ;

testing a folder's templates: a word each; one uses another (whatever their order)
T{ rendered s\" <tr id=\"task-1\" class=\"done\">" s-find 0> -> true }T
T{ rendered s" <td>Fix &lt;bug&gt;</td><td>ann</td><td>yes</td>" s-find 0> -> true }T
T{ rendered s\" <tr id=\"task-2\" class=\"\">" s-find 0> -> true }T
T{ rendered s" <td>Ship</td><td></td><td>no</td>" s-find 0> -> true }T
testing holes: a field, a path, the value itself, a number, true; false and null and none as nothing
s" [{.}]" s" self" template-text
s" {n}|{t}|{f}|{z}|{missing}" s" kinds" template-text
T{ [: " x<y" self ;] html  s" [x&lt;y]" is -> true }T
T{ [: s\" {\"n\":42,\"t\":true,\"f\":false,\"z\":null}" json> kinds ;] html  s" 42|true|||" is -> true }T
testing each and if: a list's items; the field there and not false, null, 0, empty
s" {#each xs}<i>{.}</i>{/each}" s" items" template-text
s" {#if a}A{#else}-{/if}" s" when-a" template-text
T{ [: s\" {\"xs\":[\"p\",\"q\"]}" json> items ;] html  s" <i>p</i><i>q</i>" is -> true }T
T{ [: s\" {\"xs\":[]}" json> items ;] html  length -> 0 }T
T{ [: s\" {\"a\":1}" json> when-a  s\" {\"a\":0}" json> when-a  s\" {\"a\":\"\"}" json> when-a  s\" {}" json> when-a ;] html
   s" A---" is -> true }T
testing a hole a URL attribute begins with: javascript: (or data:, with a tab in its scheme) written as # - not the
testing  site's own path or https; a hole later in the URL as it was
s\" <a href=\"{u}\">x</a><img src='{u}'><a href=\"/users/{u}\">y</a>" s" linked" template-text
: link-of ( url n -- a n )  {: ua un | m :}  <map> to m  ua un >string drop s" u" m s-at!  [: m linked ;] html string ;
T{ s" javascript:alert(1)" link-of  s\" <a href=\"#\">x</a><img src='#'>" s-find 0>= -> true }T
T{ s\" java\tscript:alert(1)" link-of  s" javascript" s-find -> -1 }T
T{ s" https://example.org/a" link-of  s\" <a href=\"https://example.org/a\">" s-find 0>= -> true }T
T{ s" /docs?x=1" link-of  s\" <a href=\"/docs?x=1\">" s-find 0>= -> true }T
T{ s" javascript:x" link-of  s" /users/javascript:x" s-find 0> -> true }T   \ (later in it: the site's path still)
testing braces doubled: themselves
s" {{x}} }}" s" braces" template-text
T{ [: 0 braces ;] html  s" {x} }" is -> true }T
testing a template said wrong: an error, its line said
: compiles? ( text n -- flag )   \ (a template of that text made as the word broken: without an error?)
    {: a n :}  [: a n s" broken" template-text ;] catch 0= ;
T{ s" <p>{title</p>" compiles? -> false }T
T{ error-message s" a { not closed" s-find 0> -> true }T
T{ s\" ok\n{#if x}no end" compiles? -> false }T
T{ error-message s" broken:2: a block not closed" s-find 0>= -> true }T
T{ s" {/each}" compiles? -> false }T
T{ s" {#loop x}" compiles? -> false }T
T{ s" {a b}" compiles? -> false }T
T{ s" <no-such-word/>" compiles? -> false }T
T{ error-message s" no word of that name" s-find 0> -> true }T
testing a URL attribute however it is written - spaces round =, a space before the value, data= - its hole checked
s\" <a href = \"{u}\">1</a><a href=\" {u}\">2</a><object data=\"{u}\"></object><A HREF='{u}'>3</A>" s" spaced" template-text
: spaced-of ( url n -- a n )  {: ua un | m :}  <map> to m  ua un >string drop s" u" m s-at!  [: m spaced ;] html string ;
T{ s" javascript:alert(1)" spaced-of  s" javascript" s-find -> -1 }T
T{ s" /ok" spaced-of  s\" data=\"/ok\"" s-find 0>= -> true }T
testing every hole before a URL value's first literal byte checked: {#else}'s, one after a hole that gave nothing
s\" <a href=\"{#if url}{url}{#else}{fallback}{/if}\">x</a><a href=\"{base}{path}\">y</a>" s" leading" template-text
: leading-of ( -- a n )  <map> {: m :}  s" javascript:alert(1)" >string drop s" fallback" m s-at!
    s" javascript:alert(2)" >string drop s" path" m s-at!  [: m leading ;] html string ;
T{ leading-of s" javascript" s-find -> -1 }T
testing a hole escaping can't make safe: refused as the template compiles - unquoted, a tag's name, on*=, style=,
testing  srcdoc=, inside <script> or <style>
T{ s" <a href={u}>x</a>" compiles?  s" <p title={t}>x</p>" compiles?  s" <{t}>" compiles?  s" <p {t}>" compiles? -> false false false false }T
T{ error-message s" quoted attribute value" s-find 0>= -> true }T
T{ s\" <div onclick=\"go('{t}')\"></div>" compiles?  s\" <p ONMOUSEOVER='{t}'>" compiles? -> false false }T
T{ s\" <p style=\"color: {t}\">" compiles?  s\" <iframe srcdoc=\"{t}\">" compiles? -> false false }T
T{ s" <script>var t = '{t}';</script>" compiles?  s" <style>p {{ color: {t} }}</style>" compiles? -> false false }T
T{ error-message s" <script> or <style>" s-find 0>= -> true }T
testing what is safe still compiles: text, quoted values, a hole after a script's end, in a comment, after a < in text
T{ s\" <script src=\"/a.js\"></script><p title=\"{t}\" data-x='{t}'>{t}</p>" compiles? -> true }T
T{ s\" <!-- {t} --><p>a < b, {t}</p><SCRIPT>x()</SCRIPT>{t}<style>p {{}}</style>{t}" compiles? -> true }T
T{ s\" <!DOCTYPE html><a class=\"c\" href=\"/u/{t}\">{t}</a>" compiles? -> true }T

testing a template's code its own: words of the loading file's of the same names change nothing
: raw ( a n -- )  2drop ;   : each ( l xt -- )  2drop ;   : text ( a n -- )  2drop ;
s\" <b>{#each}<i>{.}</i>{/each}</b>" s" late-list" template-text
T{ [: s" [1,2]" json> late-list ;] html  s" <b><i>1</i><i>2</i></b>" is -> true }T

test-summary

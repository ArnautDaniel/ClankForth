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

test-summary

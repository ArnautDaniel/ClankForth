\ html (basis/html): text put into HTML as text; HTML written as Forth - elements, attributes, escaping
IN: test-html
USING: tester html json ;

: is ( s a n -- flag )  rot string s= ;

testing html-escape: & < > " ' as entities, the rest as it is (UTF-8 too)
T{ s" <b>ann</b> & co" html-escape s" &lt;b&gt;ann&lt;/b&gt; &amp; co" s= -> true }T
T{ s\" say \"hi\" it's" html-escape s" say &quot;hi&quot; it&#39;s" s= -> true }T
T{ s" plain text" html-escape s" plain text" s= -> true }T
T{ s" " html-escape nip -> 0 }T
T{ s\" caf\xc3\xa9 <script>" html-escape s\" caf\xc3\xa9 &lt;script&gt;" s= -> true }T

testing elements: <tag ... tag>, attributes after <tag, before its content; void elements; escaping everywhere
T{ [: <p s" hi" text p> ;] html  s" <p>hi</p>" is -> true }T
T{ [: <div s" card" class=  s" x" id=  <span s" a" text span>  div> ;] html  s\" <div class=\"card\" id=\"x\"><span>a</span></div>" is -> true }T
T{ [: <p s" <script>" text p> ;] html  s" <p>&lt;script&gt;</p>" is -> true }T
T{ [: <a s\" /x?a=1&b=\"2\"" href= s" go" text a> ;] html  s" <a href=" 2drop  s\" <a href=\"/x?a=1&amp;b=&quot;2&quot;\">go</a>" is -> true }T
T{ [: <input s" email" type=  true required=  false disabled= ;] html  s\" <input type=\"email\" required>" is -> true }T
T{ [: <br <br ;] html  s" <br><br>" is -> true }T
T{ [: <p s" <b>made</b>" raw p> ;] html  s" <p><b>made</b></p>" is -> true }T
T{ [: <p 42 number p> ;] html  s" <p>42</p>" is -> true }T
T{ [: doctype ;] html  s" <!doctype html>" is -> true }T
T{ [: <p  s" text" text  s" late" class= ;] ' html catch nip -> -1 }T   \ (an attribute after the content: an error)
testing a component: a word that writes HTML, called for each item
s\" [{\"t\":\"one\"},{\"t\":\"two & three\"}]" json> constant items
: item ( v -- )  {: v :}  <li  s" t" v json-string@ drop text  li> ;
T{ [: <ul  items [: item ;] each  ul> ;] html  s" <ul><li>one</li><li>two &amp; three</li></ul>" is -> true }T

test-summary

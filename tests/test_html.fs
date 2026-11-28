\ html (basis/html): text put into HTML as text; HTML written as Forth - elements, attributes, escaping
IN: test-html
USING: tester html json ;

: is ( s a n -- flag )  rot string s= ;

testing html-escape: & < > " ' as entities, the rest as it is (UTF-8 too)
T{ s" <b>ann</b> & co" html-escape s" &lt;b&gt;ann&lt;/b&gt; &amp; co" s= -> true }T
T{ s\" say \"hi\" it's" html-escape s" say &quot;hi&quot; it&#39;s" s= -> true }T
T{ s" plain text" html-escape s" plain text" s= -> true }T
T{ s" " html-escape nip -> 0 }T
T{ s" <" html-escape s" &lt;" s=  s" a&" html-escape s" a&amp;" s=  s" &&" html-escape s" &amp;&amp;" s= -> true true true }T
T{ s" '<>'" html-escape s" &#39;&lt;&gt;&#39;" s= -> true }T   \ (the first byte, the last, one after another)
: long-text ( -- s )  [: 200 0 do  s" a run of plain text, then <one> & " %s  loop ;] build ;
: escapes-all? ( -- flag )   \ a long one: every special byte an entity, nothing else changed (its length says)
    long-text {: t :}  t string html-escape {: a n :}
    a n s" <" s-find 0<  a n s" &lt;one&gt; &amp; " s-find 0>= and  n t string nip 200 10 * + = and
    a dispose  t dispose ;
T{ objects  escapes-all?  objects rot - -> true 0 }T
T{ s\" caf\xc3\xa9 <script>" html-escape s\" caf\xc3\xa9 &lt;script&gt;" s= -> true }T

testing elements: <tag ... tag>, attributes after <tag, before its content; void elements; escaping everywhere
T{ [: <p s" hi" text p> ;] html  s" <p>hi</p>" is -> true }T
testing href= src= action=: a URL that would run (javascript:, data:, vbscript:, a tab in its scheme) written as #;
testing  the site's own, http(s), mailto, tel as they are
T{ [: <a s" javascript:alert(1)" href= a> ;] html  s\" <a href=\"#\"></a>" is -> true }T
T{ [: <img s" data:text/html,<b>x" src= ;] html  s\" <img src=\"#\">" is -> true }T
T{ [: <form s\"  java\tScript:x" action= form> ;] html  s\" <form action=\"#\"></form>" is -> true }T
T{ [: <a s" https://a.example/x?y=1&z" href= a> ;] html  s\" <a href=\"https://a.example/x?y=1&amp;z\"></a>" is -> true }T
T{ [: <a s" /path:with-colon" href=  <a s" mailto:a@b.c" href= ;] html string s" #" s-find -> -1 }T
T{ s" ./x" safe-url?  s" tel:+1" safe-url?  s" vbscript:x" safe-url? -> true true false }T
T{ [: <div s" card" class=  s" x" id=  <span s" a" text span>  div> ;] html  s\" <div class=\"card\" id=\"x\"><span>a</span></div>" is -> true }T
T{ [: <p s" <script>" text p> ;] html  s" <p>&lt;script&gt;</p>" is -> true }T
T{ [: <a s\" /x?a=1&b=\"2\"" href= s" go" text a> ;] html  s" <a href=" 2drop  s\" <a href=\"/x?a=1&amp;b=&quot;2&quot;\">go</a>" is -> true }T
T{ [: <input s" email" type=  true required=  false disabled= ;] html  s\" <input type=\"email\" required>" is -> true }T
T{ [: <br <br ;] html  s" <br><br>" is -> true }T
T{ [: <p s" <b>made</b>" raw p> ;] html  s" <p><b>made</b></p>" is -> true }T
T{ [: <p 42 number p> ;] html  s" <p>42</p>" is -> true }T
T{ [: doctype ;] html  s" <!doctype html>" is -> true }T
T{ [: <p  s" text" text  s" late" class= ;] ' html catch nip -> -1 }T   \ (an attribute after the content: an error)
testing html's words hide none of the core's (no <u: u> is the unsigned comparison)
T{ 5 3 u> -> true }T
T{ -1 3 u> -> true }T

testing a component: a word that writes HTML, called for each item
s\" [{\"t\":\"one\"},{\"t\":\"two & three\"}]" json> constant items
: item ( v -- )  {: v :}  <li  s" t" v json-string@ drop text  li> ;
T{ [: <ul  items [: item ;] each  ul> ;] html  s" <ul><li>one</li><li>two &amp; three</li></ul>" is -> true }T

testing an attribute after the element's content: an error saying so; a long text escaped whole; nothing left
: late-attr ( -- )  <p  s" t" text  s" c" class=  p> ;
T{ [: ['] late-attr html drop ;] catch 0<>  error-message s" attr=: only right after an element is begun" s-find 0>= -> true true }T
: much-text ( -- s )  {: | b :}  <bytes> to b  5000 0 do  s" a<b&c " b b-append  loop  b b>string drop  b dispose ;
: long-escaped? ( -- flag )
    [: much-text {: t :}  [: <p  t string text  p> ;] html string {: a n :}
       n 5000 13 * 7 + =  a n s" <p>a&lt;b&amp;c a&lt;b" s-starts? and  a n s" &amp;c </p>" s-ends? and ;] with-scratch ;
T{ objects  long-escaped?  objects rot = -> true true }T

test-summary

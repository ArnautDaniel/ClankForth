\ html (basis/html): text put into HTML as text
IN: test-html
USING: tester html ;

testing html-escape: & < > " ' as entities, the rest as it is (UTF-8 too)
T{ s" <b>ann</b> & co" html-escape s" &lt;b&gt;ann&lt;/b&gt; &amp; co" s= -> true }T
T{ s\" say \"hi\" it's" html-escape s" say &quot;hi&quot; it&#39;s" s= -> true }T
T{ s" plain text" html-escape s" plain text" s= -> true }T
T{ s" " html-escape nip -> 0 }T
T{ s\" caf\xc3\xa9 <script>" html-escape s\" caf\xc3\xa9 &lt;script&gt;" s= -> true }T

test-summary

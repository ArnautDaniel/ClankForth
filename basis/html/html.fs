\ html.fs - text put into HTML safely: what a user wrote shown as text, never as markup (a script it holds doesn't run).
\   s" <b>ann</b> & co" html-escape ( -- s len )  "&lt;b&gt;ann&lt;/b&gt; &amp; co"
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

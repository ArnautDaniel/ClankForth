\ urls.encoding (basis/urls/encoding): %-escapes taken out and put in
IN: test-urls
USING: tester urls.encoding ;

: is ( s len a n -- flag )  {: s sn a n :}  s sn a n s=  s dispose ;
testing url-decode: %HH a byte, + a space; a % not followed by two hex digits kept
T{ s" a%41+b%zz%4" url-decode s" aA b%zz%4" is -> true }T
T{ s" %e2%9c%93" url-decode s\" \xe2\x9c\x93" is -> true }T
testing path-decode: + is itself
T{ s" a+b%20c" path-decode s" a+b c" is -> true }T
testing url-encode: letters, digits and - . _ ~ kept, the rest %HH
T{ s" a b&c=d/e~f.g-h_i" url-encode s" a%20b%26c%3Dd%2Fe~f.g-h_i" is -> true }T
T{ s\" \xe2\x9c\x93" url-encode s" %E2%9C%93" is -> true }T
T{ s" x y+z" url-encode url-decode s" x y+z" is -> true }T

test-summary

\ base64 (basis/base64): RFC 4648's test vectors, both ways; text that isn't base64 refused
IN: test-base64
USING: tester base64 ;

: encodes ( a n a2 n2 -- flag )  {: a n ea en :}  a n >base64 {: s sn :}  s sn ea en s=  s dispose ;
: decodes ( a n a2 n2 -- flag )   \ the first decoded is the second
    {: a n ea en :}  a n base64> if  {: s sn :}  s sn ea en s=  s dispose  else  false  then ;

testing >base64: RFC 4648's vectors
T{ s" " s" " encodes -> true }T
T{ s" f" s" Zg==" encodes -> true }T
T{ s" fo" s" Zm8=" encodes -> true }T
T{ s" foo" s" Zm9v" encodes -> true }T
T{ s" foob" s" Zm9vYg==" encodes -> true }T
T{ s" fooba" s" Zm9vYmE=" encodes -> true }T
T{ s" foobar" s" Zm9vYmFy" encodes -> true }T
T{ s\" \xff\xfe\x00" s" //4A" encodes -> true }T
testing base64>: the vectors back; unpadded and the URL form too
T{ s" Zm9vYmFy" s" foobar" decodes -> true }T
T{ s" Zm9vYg==" s" foob" decodes -> true }T
T{ s" Zm9vYg" s" foob" decodes -> true }T
T{ s" __4A" s\" \xff\xfe\x00" decodes -> true }T
testing what isn't base64: false, nothing kept
T{ objects  s" Zm9v!" base64>  swap objects swap - -> 0 0 }T
T{ s" Zg=a" base64> -> false }T

testing the URL form: - and _ for + and /, no padding; read back by base64>
T{ s\" \xfb\xff" >base64url s" -_8" s= -> true }T
T{ s" hi!" >base64url s" aGkh" s= -> true }T
T{ s" h" >base64url s" aA" s= -> true }T
T{ s\" \xfb\xff" >base64url base64> drop s\" \xfb\xff" s= -> true }T

test-summary

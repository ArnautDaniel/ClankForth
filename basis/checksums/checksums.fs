\ checksums.fs - what the digests share: bytes as hex. The digests are its parts: checksums.sha1.
IN: checksums

\ the bytes as lower-case hex digits, two each (a new string)
: >hex ( a n -- s len )
    {: a n | b :}  a n s-bytes drop to a  <bytes> to b
    n 0 ?do
        a i + c@ {: c :}
        s" 0123456789abcdef" drop  {: digits :}
        c 4 rshift digits + c@ b b-push  c 15 and digits + c@ b b-push
    loop
    b b>string  b dispose ;

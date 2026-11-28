\ checksums.fs - what the digests share: bytes as hex. The digests are its parts: checksums.sha1.
IN: checksums

\ the bytes as lower-case hex digits, two each (a new string): their bits by fours, each four its digit
" 0123456789abcdef" permanent constant hex-digits
: >hex ( a n -- s len )
    >string drop {: s :}  s 8 4 regroup {: fours :}  fours hex-digits translate  dup length  s dispose  fours dispose ;

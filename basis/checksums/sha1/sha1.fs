\ sha1.fs - SHA-1 (FIPS 180-4): what a WebSocket's handshake asks for, and checksums of files. Not for secrets: it is
\ broken for that.
\   s" abc" sha1 ( -- s 20 )   s" abc" sha1-hex ( -- s 40 )  "a9993e364706816aba3e25717850c26c9cd0d89d"
IN: checksums.sha1
USING: checksums ;

<PRIVATE
$ffffffff constant mask32
64 constant block-bytes
56 constant length-at        \ where in the last block the message's length in bits goes
80 constant rounds

create w  rounds cells allot   \ (the message schedule: the digest doesn't wait, so one is enough for a Forth)
variable h0  variable h1  variable h2  variable h3  variable h4

: w@ ( i -- x )  cells w + @ ;
: w! ( x i -- )  cells w + ! ;
: rol ( x n -- x' )  {: x n :}  x n lshift  x 32 n - rshift or  mask32 and ;
: be32@ ( a -- x )  {: a :}  a c@ 24 lshift  a 1+ c@ 16 lshift or  a 2 + c@ 8 lshift or  a 3 + c@ or ;
: be32-push ( x b -- )  {: x b :}  x 24 rshift 255 and b b-push  x 16 rshift 255 and b b-push
    x 8 rshift 255 and b b-push  x 255 and b b-push ;

: block ( a -- )   \ one 64-byte block into h0..h4
    {: blk | va vb vc vd ve :}
    16 0 do  blk i 4 * + be32@ i w!  loop
    rounds 16 do  i 3 - w@  i 8 - w@ xor  i 14 - w@ xor  i 16 - w@ xor  1 rol  i w!  loop
    h0 @ to va  h1 @ to vb  h2 @ to vc  h3 @ to vd  h4 @ to ve
    rounds 0 do
        i 20 < if  vb vc and  vb invert vd and or  $5A827999
        else i 40 < if  vb vc xor vd xor  $6ED9EBA1
        else i 60 < if  vb vc and  vb vd and or  vc vd and or  $8F1BBCDC
        else  vb vc xor vd xor  $CA62C1D6  then then then
        {: fv k :}
        va 5 rol fv + ve + k + i w@ + mask32 and {: t :}
        vd to ve  vc to vd  vb 30 rol to vc  va to vb  t to va
    loop
    h0 @ va + mask32 and h0 !  h1 @ vb + mask32 and h1 !  h2 @ vc + mask32 and h2 !
    h3 @ vd + mask32 and h3 !  h4 @ ve + mask32 and h4 ! ;
PRIVATE>

\ the 20-byte digest of the bytes (a new string)
: sha1 ( a n -- s len )
    {: a n | b :}
    $67452301 h0 !  $EFCDAB89 h1 !  $98BADCFE h2 !  $10325476 h3 !  $C3D2E1F0 h4 !
    <bytes> to b  a n b b-append  $80 b b-push
    begin  b b-length block-bytes mod length-at <>  while  0 b b-push  repeat
    n 8 * 32 rshift b be32-push  n 8 * mask32 and b be32-push
    b b-data {: at len :}
    len 0 ?do  at i + block  block-bytes +loop
    b b-clear  h0 @ b be32-push  h1 @ b be32-push  h2 @ b be32-push  h3 @ b be32-push  h4 @ b be32-push
    b b>string  b dispose ;

\ the digest as 40 hex digits (a new string)
: sha1-hex ( a n -- s len )  sha1 {: s n :}  s n >hex  s dispose ;

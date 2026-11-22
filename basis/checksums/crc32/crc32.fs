\ crc32.fs - CRC-32 (ISO-HDLC: zlib's, PNG's, Ethernet's): a check that bytes came back as they were written - a
\ torn or spoiled line of a log, a file copied. Not for secrets.
\   s" 123456789" crc32 ( -- $cbf43926 )      s" 123456789" crc32-hex ( -- s len ) "cbf43926"
IN: checksums.crc32

<PRIVATE
$edb88320 constant reflected-polynomial
$ffffffff constant all-ones
create crc-table 256 cells allot   \ each byte's remainder, worked out once
: make-table ( -- )
    256 0 do
        i  8 0 do  dup 1 and if  1 rshift reflected-polynomial xor  else  1 rshift  then  loop
        crc-table i cells + !
    loop ;
make-table
PRIVATE>

\ a CRC carried on over more bytes: start with 0, then each piece's crc32-more in turn
: crc32-more ( crc a n -- crc' )
    {: crc a n | c :}  crc all-ones xor to c
    n 0 ?do  c a i + c@ xor 255 and cells crc-table + @  c 8 rshift xor to c  loop
    c all-ones xor ;
: crc32 ( a n -- u )  {: a n :}  0 a n crc32-more ;
: crc32-hex ( a n -- s len )  crc32 s" {08x}" format ;   \ 8 lower-case hex digits (a new string)

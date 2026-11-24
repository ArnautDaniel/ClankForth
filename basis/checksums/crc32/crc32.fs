\ crc32.fs - CRC-32 (ISO-HDLC: zlib's, PNG's, Ethernet's): a check that bytes came back as they were written - a
\ torn or spoiled line of a log, a file copied. Not for secrets.
\   s" 123456789" crc32 ( -- $cbf43926 )      s" 123456789" crc32-hex ( -- s len ) "cbf43926"
\ The work is compression's (C: gzip's own CRC - a store's every line checked, at the speed of the disk).
IN: checksums.crc32

\ a CRC carried on over more bytes: start with 0, then each piece's crc32-more in turn
: crc32-more ( crc a n -- crc' )  compression:crc32-more ;
: crc32 ( a n -- u )  {: a n :}  0 a n crc32-more ;
: crc32-hex ( a n -- s len )  crc32 s" {08x}" format ;   \ 8 lower-case hex digits (a new string)

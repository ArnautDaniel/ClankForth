\ checksums.fs - what the digests share: bytes as hex. The digests are its parts: checksums.sha1.
IN: checksums

\ the bytes as lower-case hex digits, two each (a new string): their bits by fours, each four its digit
" 0123456789abcdef" permanent constant hex-digits
: >hex ( a n -- s len )
    >string drop {: s :}  s 8 4 regroup {: fours :}  fours hex-digits translate  dup length  s dispose  fours dispose ;

<PRIVATE
255 constant none   \ (a byte that is no hex digit: its value)
: digit-values ( -- s )   \ each byte's value as a hex digit, of either case - or none
    {: | b :}  <bytes> to b  256 0 do  none b push  loop
    16 0 do  i  i hex-digits nth  b nth!   i  i " 0123456789ABCDEF" nth  b nth!  loop
    b " " like  b dispose ;
: values-kept ( -- s )  {: | s :}  [: digit-values rescue-all to s ;] with-scratch  s ;
values-kept permanent constant values
PRIVATE>
\ hex digits as the bytes they write, a new string - either case, the spaces and line ends between them passed
\ over ("1F 8b 08"); false for text that isn't hex, or of an odd number of digits
: hex> ( a n -- s len true | false )
    {: a n | out :}
    [: a n >seq [: 32 > ;] filter values translate {: v :}
       none v index 0<  v length 1 and 0= and if  v 4 8 regroup rescue to out  then ;] with-scratch
    out if  out string true  else  false  then ;

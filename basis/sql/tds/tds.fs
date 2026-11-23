\ tds.fs - SQL Server's wire protocol (TDS 7.4, and 8.0's TLS-first order): its messages made and its replies read.
\ No sockets here - bytes in, bytes and values out - so each part is tested against the specification's own bytes
\ (MS-TDS, section 4). The `sql` vocabulary sends them on a connection.
\
\   s" SELECT 1 AS one" 0 sql-batch ( -- b )         a request: its payload, `packets` splits it for the socket
\   payload-a payload-n tds-reply> ( -- reply )      a reply's token stream: its rows, counts, errors, changes
\
\ A row is a map from its columns' names to their values: a whole number a cell; text a string (UTF-8, from the
\ server's UTF-16 or its code page); decimal, numeric and money a string ("12.50": exact, as the server has it);
\ float and real a json-float; bit json-true / json-false; dates and times ISO 8601 strings ("2026-11-22T10:30:00");
\ a uniqueidentifier its usual text; binary "0x" and hex; NULL json-null. A column with no name is "column1" ...
IN: sql.tds
USING: json accessors calendar ;

\ ---- the message types (a packet's first byte) ----
$01 constant sql-batch-type   $03 constant rpc-type   $04 constant reply-type   $06 constant attention-type
$10 constant login7-type      $12 constant prelogin-type

8 constant header-size        \ a packet's header: type, status, length (big-endian), SPID, packet id, window
4096 constant default-packet-size
$01 constant end-of-message   \ status: the message's last packet
$74000004 constant tds-7.4    \ the version LOGIN7 asks for (TDS 8.0 is the same, inside TLS from the first byte)

\ ---- prelogin's encryption option ----
0 constant encrypt-off   1 constant encrypt-on   2 constant encrypt-not-supported   3 constant encrypt-required

<PRIVATE
\ ---- bytes written: little-endian unless said ----
: le16 ( x b -- )  {: x b :}  x 255 and b b-push  x 8 rshift 255 and b b-push ;
: le32 ( x b -- )  {: x b :}  x b le16  x 16 rshift b le16 ;
: le64 ( x b -- )  {: x b :}  x b le32  x 32 rshift b le32 ;
: be16 ( x b -- )  {: x b :}  x 8 rshift 255 and b b-push  x 255 and b b-push ;
: be32 ( x b -- )  {: x b :}  x 16 rshift b be16  x b be16 ;
: le16! ( x i b -- )  {: x i b :}  x 255 and i b b!  x 8 rshift 255 and i 1+ b b! ;
: le32! ( x i b -- )  {: x i b :}  x i b le16!  x 16 rshift i 2 + b le16! ;

\ ---- read from memory ----
: le16@ ( a -- x )  {: a :}  a c@  a 1+ c@ 8 lshift or ;
: le32@ ( a -- x )  {: a :}  a le16@  a 2 + le16@ 16 lshift or ;
: le64@ ( a -- x )  {: a :}  a le32@  a 4 + le32@ 32 lshift or ;
: be16@ ( a -- x )  {: a :}  a c@ 8 lshift  a 1+ c@ or ;
: signed32 ( x -- n )  dup $80000000 and if  $100000000 -  then ;
: signed16 ( x -- n )  dup $8000 and if  $10000 -  then ;
PRIVATE>

\ ---- text: UTF-8 here, UTF-16LE on the wire ----
$d800 constant high-surrogate   $dc00 constant low-surrogate   $10000 constant past-bmp   $400 constant surrogate-span
\ UTF-8 text onto b as UTF-16LE (a character past the BMP as its surrogate pair)
: utf16, ( a n b -- )
    {: a n b :}
    begin  n 0>  while
        a n utf8-next to n to a {: c :}
        c past-bmp < if  c b le16
        else  c past-bmp - {: v :}
            v 10 rshift high-surrogate + b le16  v surrogate-span 1- and low-surrogate + b le16
        then
    repeat ;
\ UTF-16LE (n bytes) as UTF-8, a new string (an unpaired surrogate as U+FFFD)
: utf16> ( a n -- s len )
    {: a n | b i :}  <bytes> to b
    begin  i 1+ n <  while
        a i + le16@ {: c :}  i 2 + to i
        c high-surrogate low-surrogate within if
            i 1+ n <  if  a i + le16@  else  0  then {: d :}
            d low-surrogate low-surrogate surrogate-span + within if
                c high-surrogate - 10 lshift  d low-surrogate - or  past-bmp +  b b-push-char  i 2 + to i
            else  $fffd b b-push-char  then
        else  c low-surrogate low-surrogate surrogate-span + within if  $fffd  else  c  then  b b-push-char  then
    repeat
    b b>string  b dispose ;
\ the characters UTF-16 takes for UTF-8 text (what TDS's lengths count)
: utf16-length ( a n -- count )
    {: a n | count :}
    begin  n 0>  while  a n utf8-next to n to a  past-bmp < if  1  else  2  then  count + to count  repeat
    count ;

<PRIVATE
\ Windows-1252's 0x80-0x9F (the rest of its upper half is Latin-1's, U+00A0 on): the text of a column whose collation
\ isn't UTF-8 (SQL_Latin1_General_CP1, Latin1_General: the West's)
create cp1252-high
    $20ac , $0081 , $201a , $0192 , $201e , $2026 , $2020 , $2021 , $02c6 , $2030 , $0160 , $2039 , $0152 , $008d ,
    $017d , $008f , $0090 , $2018 , $2019 , $201c , $201d , $2022 , $2013 , $2014 , $02dc , $2122 , $0161 , $203a ,
    $0153 , $009d , $017e , $0178 ,
PRIVATE>
: cp1252> ( a n -- s len )
    {: a n | b :}  <bytes> to b
    n 0 ?do
        a i + c@ {: c :}
        c $80 < if  c b b-push
        else c $a0 < if  c $80 - cells cp1252-high + @ b b-push-char
        else  c b b-push-char  then then
    loop
    b b>string  b dispose ;

\ ---- packets: a message's payload cut into them, each its header ----
\ the payload as packets of `size` bytes at most (header and all), the last marked the message's end
: packets ( type a n size -- b )
    {: type a n size | b at id :}  <bytes> to b  1 to id
    size header-size - {: room :}
    begin
        n at - room min {: len :}
        type b b-push
        at len + n >= if  end-of-message  else  0  then  b b-push
        len header-size + b be16  0 b be16  id 255 and b b-push  0 b b-push
        a at + len b b-append
        at len + to at  id 1+ to id
        at n >=
    until
    b ;
\ the first message's end in the bytes come: the bytes its packets take - 0 if it isn't all there yet; -1 if what
\ came is no packet (a length under a header's)
: message-end ( a n -- used | 0 | -1 )
    {: a n | at :}
    begin
        at header-size + n > if  0 exit  then
        a at + 2 + be16@ {: len :}
        len header-size < if  -1 exit  then
        at len + n > if  0 exit  then
        a at + 1+ c@ end-of-message and  at len + to at
        if  at exit  then
    again ;
\ the first message's payload (its packets' headers off), onto b; its type
: message-payload ( a used b -- type )
    {: a used b | at :}
    begin  at used <  while
        a at + 2 + be16@ {: len :}
        a at + header-size +  len header-size -  b b-append
        at len + to at
    repeat
    a c@ ;

\ ---- PRELOGIN: what the client is and asks for ----
<PRIVATE
0 constant opt-version   1 constant opt-encryption   2 constant opt-instance   3 constant opt-thread   4 constant opt-mars
$ff constant opt-end
5 constant option-entry   \ each option's entry: its token, its data's offset and length (big-endian)
PRIVATE>
\ the payload: the client's version (32 bits, then a 16-bit build: 0), the encryption it asks for, its thread id and
\ MARS (0: off) - the specification's example is $09000000 1 $0db8 1
: prelogin-payload ( version encrypt thread mars -- b )
    {: version encrypt thread mars | b :}  <bytes> to b
    5 option-entry * 1+ {: at :}   \ (the data after the five entries and the end)
    opt-version b b-push  at b be16  6 b be16
    opt-encryption b b-push  at 6 + b be16  1 b be16
    opt-instance b b-push  at 7 + b be16  1 b be16
    opt-thread b b-push  at 8 + b be16  4 b be16
    opt-mars b b-push  at 12 + b be16  1 b be16
    opt-end b b-push
    version b be32  0 b be16  encrypt b b-push  0 b b-push  thread b le32  mars b b-push
    b ;
\ the server's answer: the encryption it chose (-1 if the answer says none)
: prelogin-encryption ( a n -- encrypt | -1 )
    {: a n | at :}
    begin
        at n >= if  -1 exit  then
        a at + c@ {: token :}
        token opt-end = if  -1 exit  then
        at option-entry + n > if  -1 exit  then
        token opt-encryption = if
            a at + 1+ be16@ {: off :}
            off n < if  a off + c@  else  -1  then  exit
        then
        at option-entry + to at
    again ;

\ ---- LOGIN7: who connects, to which database ----
\ what a login says: the TDS version, the packet size asked for, the client program's version and process id, the
\ names (host, user, password, application, server, the client library, language, database - UTF-8 strings or 0 for
\ none) and the client's id (48 bits, as a network card's)
TUPLE: login-info  version packet-size prog-version pid host user password app server library language database
    client-id ;
<PRIVATE
94 constant login-fixed    \ the fixed part: its numbers and the offsets and lengths of its strings
$e0 constant flags1        \ use the database (fUseDB), a database that can't be used fails the login (fDatabase), set the language
$03 constant flags2        \ a language that can't be set fails it; ODBC's settings (ANSI_NULLS, QUOTED_IDENTIFIER on ...)
$0409 constant lcid-en-us
: text-of ( s|0 -- a n )  ?dup if  string  else  s" "  then ;
\ a password as LOGIN7 hides it: each byte of its UTF-16 its halves swapped, then xor $A5
: hidden, ( a n b -- )
    {: a n b | t :}  <bytes> to t  a n t utf16,
    t b-length 0 ?do  i t b@ {: c :}  c 4 lshift  c 4 rshift or  255 and  $a5 xor  b b-push  loop
    t dispose ;
\ a string's offset and length (in characters) at entry i of the fixed part; its UTF-16 onto the data
: field, ( s|0 entry hide? b -- )
    {: s entry hide? b :}
    s text-of {: a n :}
    b b-length entry b le16!  a n utf16-length entry 2 + b le16!
    hide? if  a n b hidden,  else  a n b utf16,  then ;
PRIVATE>
: login7 ( info -- b )
    {: li | b :}  <bytes> to b
    login-fixed b b-resize   \ (the fixed part, filled in as the strings go after it)
    li version>> 4 b le32!  li packet-size>> 8 b le32!  li prog-version>> 12 b le32!  li pid>> 16 b le32!
    flags1 24 b b!  flags2 25 b b!  lcid-en-us 32 b le32!
    li host>> 36 false b field,   li user>> 40 false b field,   li password>> 44 true b field,
    li app>> 48 false b field,    li server>> 52 false b field,
    b b-length 56 b le16!         \ (no extension: its offset where the data is, length 0)
    li library>> 60 false b field,   li language>> 64 false b field,   li database>> 68 false b field,
    li client-id>> {: id :}  6 0 do  id 5 i - 8 * rshift 255 and  72 i + b b!  loop
    b b-length {: end :}  end 78 b le16!  end 82 b le16!  end 86 b le16!   \ (SSPI, a file to attach, a new password: none)
    b b-length 0 b le32!
    b ;

\ ---- requests: a batch of SQL, a procedure called ----
<PRIVATE
22 constant all-headers-size   18 constant txn-header-size   2 constant txn-header
10 constant sp-executesql      \ the procedure's number (ProcIDSwitch $FFFF, then it)
$ffff constant proc-by-id
8000 constant short-max        \ bytes: an nvarchar of up to this many sent with its length; longer in pieces (PLP)
$ffff constant max-length      \ a (max) type's length - its values in pieces
create param-collation  $09 c, $04 c, $d0 c, $00 c, $34 c,   \ a parameter's text: Latin1_General_CI_AS's (UTF-16 is the same in any)
PRIVATE>
\ the headers every request begins with: the transaction it is in (0 none) and the requests outstanding (1)
: all-headers, ( txn outstanding b -- )
    {: txn out b :}  all-headers-size b le32  txn-header-size b le32  txn-header b le16  txn b le64  out b le32 ;
\ SQL text to run, in the transaction (its descriptor: 0 for none)
: sql-batch ( a n txn -- b )
    {: a n txn | b :}  <bytes> to b  txn 1 b all-headers,  a n b utf16,  b ;

\ ---- a parameter's value: by what it is ----
<PRIVATE
$26 constant intn   $6d constant fltn   $68 constant bitn   $e7 constant nvarchar
8 constant int-bytes   8 constant float-bytes   1 constant bit-bytes
: plp, ( a n b -- )   \ UTF-16 in pieces: its whole length, one piece, the end
    {: a n b | t :}  <bytes> to t  a n t utf16,
    t b-length b le64  t b-length b le32  t b-data b b-append  0 b le32  t dispose ;
: text-param, ( a n b -- )
    {: a n b :}
    a n utf16-length 2* {: bytes :}
    nvarchar b b-push
    bytes short-max <= if  short-max b le16  else  max-length b le16  then
    param-collation 5 b b-append
    bytes short-max <= if  bytes b le16  a n b utf16,  else  a n b plp,  then ;
: null-param, ( b -- )
    {: b :}  nvarchar b b-push  short-max b le16  param-collation 5 b b-append  $ffff b le16 ;
PRIVATE>
\ a value as a parameter's type and data: a whole number bigint, a json-float float, json-true / json-false bit,
\ json-null (or 0 as an object: none) NULL, a string nvarchar
: param-value, ( v b -- )
    {: v b :}
    v json-null = if  b null-param, exit  then
    v json-true =  v json-false = or if  bitn b b-push  bit-bytes b b-push  bit-bytes b b-push  v json-true = 1 and b b-push  exit  then
    v json-float instance? if
        fltn b b-push  float-bytes b b-push  float-bytes b b-push
        b b-length {: at :}  float-bytes b b-resize  v value>> b b-data drop at + f!  exit
    then
    v string? if  v string b text-param, exit  then
    v object? if  true s" sql: a parameter's value is a number, a string, true, false, null or a float (not a list or a map)" (abort")  then
    intn b b-push  int-bytes b b-push  int-bytes b b-push  v b le64 ;
\ the type a value is declared as, for sp_executesql's list of them
: param-type ( v -- a n )
    {: v :}
    v json-null = if  s" nvarchar(4000)" exit  then
    v json-true =  v json-false = or if  s" bit" exit  then
    v json-float instance? if  s" float" exit  then
    v string? if  v string utf16-length 2* short-max <= if  s" nvarchar(4000)"  else  s" nvarchar(max)"  then  exit  then
    s" bigint" ;

<PRIVATE
: param-name, ( a n b -- )  {: a n b :}  a n utf16-length b b-push  a n b utf16, ;
: text-arg, ( a n b -- )  {: a n b :}  0 b b-push  0 b b-push  a n b text-param, ;   \ (no name, input)
PRIVATE>
\ sp_executesql: the SQL, its parameters' declarations ("@id bigint, @t nvarchar(4000)") and their values - a map of
\ names (without @) to values, in its order - in the transaction (0 none)
: rpc-executesql ( sql n decls n params txn -- b )
    {: sa sn da dn params txn | b :}  <bytes> to b
    txn 1 b all-headers,
    proc-by-id b le16  sp-executesql b le16  0 b le16
    sa sn b text-arg,  da dn b text-arg,
    params ?dup if
        [: {: k v :}  k string s" @" 2swap s+ {: na nn :}  na nn b param-name,  0 b b-push  v b param-value,
            na dispose ;] map-each
    then
    b ;
\ the declarations a map of parameters gives them by their values (`types`: a map of names to types said instead,
\ or 0)
: param-decls ( params types -- s len )
    {: params types | b :}  <bytes> to b
    params [: {: k v :}
        b b-length if  s" , " b b-append  then
        s" @" b b-append  k string b b-append  s"  " b b-append
        types if  k string types s-at  else  false  then  if  string  else  v param-type  then  b b-append
    ;] map-each
    b b>string  b dispose ;

\ ---- a reply: its token stream read ----
\ an error or a message the server sent: its number, state, class (severity: 11 and over an error), text, the
\ procedure it was in ("" none) and the line
TUPLE: sql-error  number state class message proc line ;
\ a result set: its columns' names (a list) and its rows (a list of maps)
TUPLE: tds-set  columns rows ;
\ what a reply said: its result sets (a list), the rows the last statement counted (-1 none), its errors and
\ messages (lists of sql-error), what changed in the session - the database (a string, or 0), the packet size (0
\ unchanged), the transaction's descriptor (-1 unchanged; 0 ended) - where it routes the client to (host string or
\ 0, port), a procedure's return status (or 0), output parameters (a map), and whether it logged the client in
TUPLE: tds-reply  sets affected errors messages database packet-size txn route-host route-port status outputs
    logged-in ;

<PRIVATE
\ ---- the reader: the token stream, at a position ----
variable rd-a   variable rd-n   variable rd-at
: fail-with ( a n -- )  true -rot (abort") ;
: cut-short ( -- )  s" sql: a reply cut short, or said wrong" fail-with ;
: need ( n -- )  rd-at @ + rd-n @ > if  cut-short  then ;
: here@ ( -- a )  rd-a @ rd-at @ + ;
: skip ( n -- )  dup need  rd-at +! ;
: u8> ( -- x )  1 need  here@ c@  1 rd-at +! ;
: u16> ( -- x )  2 need  here@ le16@  2 rd-at +! ;
: u32> ( -- x )  4 need  here@ le32@  4 rd-at +! ;
: u64> ( -- x )  8 need  here@ le64@  8 rd-at +! ;
: bytes> ( n -- a n )  {: n :}  n need  here@ n  n rd-at +! ;
: chars> ( count -- s )  2* bytes> utf16> drop ;   \ UTF-16 text of `count` characters, a string
: b-varchar> ( -- s )  u8> chars> ;               \ its length in characters a byte
: us-varchar> ( -- s )  u16> chars> ;             \ ... two bytes

\ ---- the types ----
$1f constant null-type
$30 constant int1   $32 constant bit-t   $34 constant int2   $38 constant int4   $7f constant int8
$3a constant datetim4   $3d constant datetime-t   $3b constant flt4   $3e constant flt8   $3c constant money-t   $7a constant money4
$24 constant guid-t   $26 constant intn-t   $68 constant bitn-t   $6d constant fltn-t   $6e constant moneyn   $6f constant datetimn
$37 constant decimal-t   $3f constant numeric-t   $6a constant decimaln   $6c constant numericn
$28 constant daten   $29 constant timen   $2a constant datetime2n   $2b constant datetimeoffsetn
$2f constant char-t   $27 constant varchar-t   $2d constant binary-t   $25 constant varbinary-t
$a5 constant bigvarbin   $a7 constant bigvarchr   $ad constant bigbinary   $af constant bigchar
$e7 constant nvarchar-t   $ef constant nchar   $f1 constant xml-t   $f0 constant udt
$23 constant text-t   $22 constant image-t   $63 constant ntext   $62 constant ssvariant
$40 constant utf8-flag-byte3   \ (the collation's fUTF8: bit-t 26 of its first four bytes, read little-endian - $04 of its fourth byte)

\ a column's type: its type byte, its length (0 fixed; -1 a (max) one, its values in pieces), precision, scale and
\ whether its text is UTF-8 (else Windows-1252) - read from its TYPE_INFO
TUPLE: col-type  type len precision scale utf8 ;
: fixed-len ( type -- n | 0 )
    case
        null-type of 0 endof  int1 of 1 endof  bit-t of 1 endof  int2 of 2 endof  int4 of 4 endof  int8 of 8 endof
        datetim4 of 4 endof  datetime-t of 8 endof  flt4 of 4 endof  flt8 of 8 endof  money-t of 8 endof
        money4 of 4 endof
        -1 swap
    endcase ;
: collation> ( -- utf8? )  5 bytes> drop 3 + c@ $04 and 0<> ;
: table-name> ( -- )  u8> 0 ?do  us-varchar> drop  loop ;   \ (text, ntext, image-t: the table they are in - passed over)
: type-info> ( -- ct )
    {: | t len prec scale utf8 :}
    u8> to t
    t fixed-len {: fl :}
    fl 0>= if  t fl 0 0 false <col-type> exit  then
    t case
        guid-t of  u8> to len  endof   intn-t of  u8> to len  endof   bitn-t of  u8> to len  endof
        fltn-t of  u8> to len  endof   moneyn of  u8> to len  endof   datetimn of  u8> to len  endof
        decimaln of  u8> to len  u8> to prec  u8> to scale  endof
        numericn of  u8> to len  u8> to prec  u8> to scale  endof
        decimal-t of  u8> to len  u8> to prec  u8> to scale  endof
        numeric-t of  u8> to len  u8> to prec  u8> to scale  endof
        daten of  3 to len  endof
        timen of  u8> to scale  endof   datetime2n of  u8> to scale  endof   datetimeoffsetn of  u8> to scale  endof
        char-t of  u8> to len  collation> to utf8  endof   varchar-t of  u8> to len  collation> to utf8  endof
        binary-t of  u8> to len  endof   varbinary-t of  u8> to len  endof
        bigvarbin of  u16> to len  endof   bigbinary of  u16> to len  endof
        bigvarchr of  u16> to len  collation> to utf8  endof   bigchar of  u16> to len  collation> to utf8  endof
        nvarchar-t of  u16> to len  collation> drop  endof   nchar of  u16> to len  collation> drop  endof
        xml-t of  u8> if  b-varchar> drop  b-varchar> drop  us-varchar> drop  then  -1 to len  endof
        text-t of  u32> to len  collation> to utf8  table-name>  endof
        ntext of  u32> to len  collation> drop  table-name>  endof
        image-t of  u32> to len  table-name>  endof
        dup s" sql: a column of a type not read (sql_variant, a CLR type ...): {x}" format fail-with
    endcase
    len $ffff =  t bigvarbin = t bigvarchr = or  t nvarchar-t = or  and if  -1 to len  then
    t len prec scale utf8 <col-type> ;

\ ---- a value read, by its type ----
: hex-string ( a n -- s )   \ "0x" and the bytes in hex
    {: a n | b :}  <bytes> to b  s" 0x" b b-append
    n 0 ?do  a i + c@ s" {02X}" format {: h hn :}  h hn b b-append  h dispose  loop
    b b>string drop  b dispose ;
: text-value ( a n ct -- s )   \ a non-Unicode column's bytes
    utf8>> if  >string drop  else  cp1252> drop  then ;

\ a decimal's magnitude (its little-endian bytes) and scale as text: "-12.50"
create magnitude 16 allot
: decimal-text ( positive? a n scale -- s )
    {: pos a n scale | b digits :}
    magnitude 16 erase  a magnitude n 16 min move
    <bytes> to b
    begin   \ (divided by 10 till nothing is left, the remainders the digits, last first)
        0 {: rem :}
        16 0 do  15 i - magnitude + {: at :}  rem 8 lshift at c@ or {: x :}  x 10 / at c!  x 10 mod to rem  loop
        rem [char] 0 + b b-push  digits 1+ to digits
        false  16 0 do  magnitude i + c@ if  drop true leave  then  loop  0=
        digits scale > and
    until
    <bytes> {: out :}
    pos 0= if  [char] - out b-push  then
    digits 0 ?do
        digits i - scale = if  [char] . out b-push  then
        digits 1- i - b b@ out b-push
    loop
    b dispose  out b>string drop  out dispose ;
: money-text ( n -- s )   \ ten-thousandths: "12.5000"
    {: n :}  n 0< if  n negate  else  n  then {: m :}
    m 10000 /mod {: frac whole :}
    whole frac  n 0< if  s" -{}.{04}"  else  s" {}.{04}"  then  format drop ;

\ dates and times: days counted from 0001-01-01; a time in units of 10^-scale s
719162 constant days-0001-to-1970   693595 constant days-0001-to-1900
create powers-of-ten  1 , 10 , 100 , 1000 , 10000 , 100000 , 1000000 , 10000000 ,
: ten^ ( n -- 10^n )  cells powers-of-ten + @ ;
: date-text, ( days b -- )   \ YYYY-MM-DD
    {: days b :}  days days-0001-to-1970 - days>date  s" {04}-{02}-{02}" format {: s n :}  s n b b-append  s dispose ;
: time-text, ( units scale b -- )   \ hh:mm:ss and the fraction, scale digits
    {: u scale b :}
    u scale ten^ /mod {: frac secs :}
    secs 3600 / 24 mod  secs 60 / 60 mod  secs 60 mod  s" {02}:{02}:{02}" format {: s n :}  s n b b-append  s dispose
    scale if  [char] . b b-push
        scale 0 do  frac  scale 1- i - ten^ /  10 mod  [char] 0 + b b-push  loop
    then ;
: text-of-bytes ( b -- s )  {: b :}  b b>string drop  b dispose ;
: date-value ( days -- s )  {: d :}  <bytes> {: b :}  d b date-text,  b text-of-bytes ;
: time-value ( units scale -- s )  {: u sc :}  <bytes> {: b :}  u sc b time-text,  b text-of-bytes ;
: datetime-value ( days units scale -- s )
    {: d u sc :}  <bytes> {: b :}  d b date-text,  [char] T b b-push  u sc b time-text,  b text-of-bytes ;
\ a time's bytes for its scale: 3 to scale 2, 4 to 4, 5 to 7
: time-bytes ( scale -- n )  dup 3 < if  drop 3 exit  then  5 < if  4  else  5  then ;
: le-n ( a n -- x )  {: a n | x :}  n 0 ?do  a i + c@  i 8 * lshift  x or to x  loop  x ;
: offset-value ( units days scale minutes -- s )   \ datetimeoffset: UTC, shown at its offset: ...+01:00
    {: u d sc mins :}
    sc ten^ 86400 * {: per-day :}
    u  mins 60 * sc ten^ * +  {: local :}
    local 0< if  d 1- to d  local per-day + to local  then
    local per-day >= if  d 1+ to d  local per-day - to local  then
    <bytes> {: b :}  d b date-text,  [char] T b b-push  local sc b time-text,
    mins abs {: m :}  m 60 /  m 60 mod  mins 0< if  s" -{02}:{02}"  else  s" +{02}:{02}"  then  format {: s n :}
    s n b b-append  s dispose
    b text-of-bytes ;

: guid-text ( a -- s )   \ its first three parts little-endian, as SQL Server shows it: 6F9619FF-8B86-D011-B42D-00C04FC964FF
    {: a :}
    a le32@  a 4 + le16@  a 6 + le16@  a 8 + c@  a 9 + c@
    s" {08X}-{04X}-{04X}-{02X}{02X}-" format {: s n :}
    a 10 + 6 hex-string {: h :}  s n  h string 2 - swap 2 + swap  s+ drop  s dispose  h dispose ;

: float-value ( a n -- v )
    {: a n :}  n 4 = if  a sf@  else  a f@  then  <json-float> ;
: datetime-old ( a n -- s )   \ datetime (days from 1900, 1/300 s) and smalldatetime (days, minutes)
    {: a n :}
    n 4 = if  a le16@ days-0001-to-1900 +  a 2 + le16@ 60 *  0 datetime-value exit  then
    a le32@ signed32 days-0001-to-1900 +  a 4 + le32@ 10 * 3 + 3 /  3 datetime-value ;
: money-value ( a n -- s )
    {: a n :}  n 4 = if  a le32@ signed32  else  a le32@ 32 lshift  a 4 + le32@ or  then  money-text ;
: int-value ( a n -- x )
    {: a n :}
    n case
        1 of  a c@  endof
        2 of  a le16@ signed16  endof
        4 of  a le32@ signed32  endof
        a le64@ swap
    endcase ;

\ the value of a type, from its n bytes
: value-of ( a n ct -- v )
    {: a n ct :}
    ct type>> {: t :}
    t int1 =  t int2 = or  t int4 = or  t int8 = or  t intn-t = or if  a n int-value exit  then
    t bit-t =  t bitn-t = or if  a c@ json-bool exit  then
    t flt4 =  t flt8 = or  t fltn-t = or if  a n float-value exit  then
    t money-t =  t money4 = or  t moneyn = or if  a n money-value exit  then
    t datetime-t =  t datetim4 = or  t datetimn = or if  a n datetime-old exit  then
    t decimaln =  t numericn = or  t decimal-t = or  t numeric-t = or if  a c@ 1 =  a 1+ n 1-  ct scale>> decimal-text exit  then
    t daten = if  a 3 le-n date-value exit  then
    t timen = if  a n le-n  ct scale>> time-value exit  then
    t datetime2n = if  ct scale>> time-bytes {: tb :}  a tb + 3 le-n  a tb le-n  ct scale>> datetime-value exit  then
    t datetimeoffsetn = if
        ct scale>> time-bytes {: ob :}  a ob le-n  a ob + 3 le-n  ct scale>>  a ob + 3 + le16@ signed16  offset-value exit
    then
    t guid-t = if  a guid-text exit  then
    t nvarchar-t =  t nchar = or  t ntext = or  t xml-t = or if  a n utf16> drop exit  then
    t bigvarchr =  t bigchar = or  t char-t = or  t varchar-t = or  t text-t = or if  a n ct text-value exit  then
    a n hex-string ;   \ (binary, varbinary, image-t)

\ ---- a column's value in a row ----
-1 constant plp-null   -2 constant plp-unknown
: plp> ( -- a n | 0 -1 )   \ a (max) value: its pieces joined (NULL: 0 -1)
    u64> {: total :}
    total plp-null = if  0 -1 exit  then
    <bytes> {: b :}
    begin  u32> dup  while  bytes> b b-append  repeat  drop
    b b>string  b dispose ;
: column-bytes ( ct -- a n | 0 -1 )   \ its bytes as the type sends them (NULL: 0 -1)
    {: ct :}
    ct type>> {: t :}
    ct len>> -1 = if  plp> exit  then
    t fixed-len 0>= if  t null-type = if  0 -1  else  t fixed-len bytes>  then  exit  then
    t text-t =  t ntext = or  t image-t = or if
        u8> ?dup 0= if  0 -1 exit  then  skip  8 skip   \ (its text pointer and timestamp)
        u32> bytes> exit
    then
    t bigvarbin =  t bigvarchr = or  t bigbinary = or  t bigchar = or  t nvarchar-t = or  t nchar = or if
        u16> dup $ffff = if  drop 0 -1  else  bytes>  then  exit
    then
    u8> ?dup 0= if  0 -1 exit  then  bytes> ;   \ (a byte its length; 0 NULL)
: column-value ( ct -- v )
    {: ct :}  ct column-bytes {: a n :}
    n -1 = if  json-null exit  then
    a n ct value-of ;

\ ---- the tokens ----
$81 constant colmetadata   $d1 constant row-token   $d2 constant nbcrow   $fd constant done-t   $fe constant doneproc
$ff constant doneinproc    $aa constant error-token   $ab constant info-token   $ad constant loginack
$e3 constant envchange     $a9 constant order-t   $79 constant returnstatus   $ac constant returnvalue
$ae constant featureextack   $a5 constant colinfo   $a4 constant tabname   $e4 constant sessionstate
$ed constant sspi   $ee constant fedauthinfo   $a3 constant dataclassification
$10 constant done-count    \ a DONE's rowcount means something
$02 constant done-error
11 constant error-class    \ a message's class from which it is an error

variable reply   variable cols   variable col-names   variable this-set
: colmetadata> ( -- )
    u16> {: count :}
    count $ffff = if  exit  then
    list cols !  list col-names !
    count 0 ?do
        4 skip  2 skip   \ (its user type and flags)
        type-info> cols @ push
        b-varchar> {: name :}
        name string nip 0= if  name dispose  i 1+ s" column{}" format drop to name  then
        name col-names @ push
    loop
    col-names @ list <tds-set> this-set !
    this-set @ reply @ sets>> push ;
: row-of ( nulls? -- )   \ a row: each column's value (nbcrow: a bitmap of those NULL first)
    {: nbc | bits m :}
    cols @ 0= if  cut-short  then
    cols @ length {: n :}
    nbc if  n 7 + 8 / bytes> drop to bits  then
    <map> to m
    n 0 ?do
        nbc if  bits i 8 / + c@  1 i 8 mod lshift and  else  0  then
        if  json-null  else  i cols @ nth column-value  then
        i col-names @ nth string m s-at!
    loop
    m this-set @ rows>> push ;
: done> ( -- )
    u16> {: status :}  2 skip  u64> {: count :}
    status done-count and if  reply @ count >>affected drop  then ;
: message> ( -- e )
    2 skip   \ (its length)
    u32>  u8>  u8>  us-varchar>  b-varchar> drop  b-varchar>  u32>  <sql-error> ;
: envchange> ( -- )
    u16> {: len :}  rd-at @ {: start :}
    u8> {: kind :}
    kind 1 = if  b-varchar> reply @ swap >>database drop  then   \ the database
    kind 4 = if  b-varchar> string string>number drop  reply @ swap >>packet-size drop  then
    kind 8 = if  u8> 8 = if  u64>  else  0  then  reply @ swap >>txn drop  then   \ a transaction begun: its descriptor
    kind 9 =  kind 10 = or  kind 17 = or if  reply @ 0 >>txn drop  then             \ committed, rolled back, ended
    kind 20 = if   \ routed: protocol (0, TCP), port, server
        2 skip  u8> drop  u16> {: port :}  us-varchar> {: host :}
        reply @ host >>route-host port >>route-port drop
    then
    start len + rd-at ! ;
: returnvalue> ( -- )
    2 skip  b-varchar> {: name :}  1 skip  4 skip  2 skip
    type-info> column-value {: v :}
    v name string reply @ outputs>> s-at! ;
: length16-skip ( -- )  u16> skip ;
: token> ( -- )
    u8> {: t :}
    t case
        colmetadata of  colmetadata>  endof
        row-token of  false row-of  endof
        nbcrow of  true row-of  endof
        done-t of  done>  endof   doneproc of  done>  endof   doneinproc of  done>  endof
        error-token of  message> reply @ errors>> push  endof
        info-token of  message> reply @ messages>> push  endof
        loginack of  length16-skip  reply @ true >>logged-in drop  endof
        envchange of  envchange>  endof
        returnstatus of  u32> signed32 reply @ swap >>status drop  endof
        returnvalue of  returnvalue>  endof
        order-t of  length16-skip  endof   colinfo of  length16-skip  endof   tabname of  length16-skip  endof
        sspi of  length16-skip  endof   fedauthinfo of  u32> skip  endof   sessionstate of  u32> skip  endof
        featureextack of  begin  u8> $ff <>  while  u32> skip  repeat  endof
        dup s" sql: a reply's token not read: {x}" format fail-with
    endcase ;
PRIVATE>

\ a reply's payload read: what it said (an error the server sent is in its errors, not thrown; a reply said wrong is
\ an error)
: tds-reply> ( a n -- reply )
    rd-n !  rd-a !  0 rd-at !
    list -1 list list 0 0 -1 0 0 0 <map> false <tds-reply> reply !
    0 cols !  0 col-names !  0 this-set !
    begin  rd-at @ rd-n @ <  while  token>  repeat
    reply @ ;
\ the first error of a reply (class 11 and over), or 0
: reply-error ( reply -- e | 0 )
    errors>> [: class>> error-class >= ;] find 0= if  0  then ;
\ an error as text: "Invalid object name 'x'. (208, line 1)"
: error-text ( e -- s len )
    {: e :}  e message>> string  e number>>  e line>>  s" {s} ({}, line {})" format ;

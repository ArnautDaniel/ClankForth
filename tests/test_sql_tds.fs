\ sql.tds (basis/sql/tds): SQL Server's messages made and its replies read - checked against the specification's own
\ bytes (MS-TDS section 4: its PRELOGIN, LOGIN7, SQL batch, login reply and result set examples)
IN: test-sql-tds
USING: tester json accessors sql.tds ;

\ ---- bytes written as hex, and compared ----
: hex-digit ( c -- n )  {: c :}  c [char] 9 <= if  c [char] 0 -  else  c 32 or [char] a - 10 +  then ;
: hex> ( a n -- b )   \ "12 01 00 2F ..." as bytes (spaces and line breaks between them passed over)
    {: a n | b hi :}  a n s-bytes drop to a  <bytes> to b  -1 to hi
    n 0 ?do
        a i + c@ {: c :}
        c 32 >  if
            hi 0< if  c hex-digit to hi  else  hi 4 lshift  c hex-digit or  b b-push  -1 to hi  then
        then
    loop
    b ;
: le16@ ( a -- x )  {: a :}  a c@  a 1+ c@ 8 lshift or ;
: same? ( b a n -- flag )  {: b a n :}  a n hex> {: e :}  b b-data  e b-data  s=  e dispose ;
: payload ( b type -- b' )   \ a payload as one packet, as the examples show it
    {: b type :}  type b b-data 4096 packets ;

testing PRELOGIN: the specification's example (4.1), byte for byte
T{ $09000000 encrypt-on $0db8 1 prelogin-payload prelogin-type payload
   s" 12 01 00 2F 00 00 01 00 00 00 1A 00 06 01 00 20 00 01 02 00 21 00 01 03 00 22 00 04 04 00 26 00 01 FF 09 00 00 00 00 00 01 00 B8 0D 00 00 01"
   same? -> true }T
T{ $09000000 encrypt-required 0 0 prelogin-payload b-data prelogin-encryption -> 3 }T
T{ s" " prelogin-encryption -> -1 }T

testing LOGIN7: the specification's example (4.2) - its names, a password hidden
: spec-login ( -- info )
    $72090002 4096 $07000000 $100
    " skostov1"  " sa"  0  " OSQL-32"  0  " ODBC"  0  0  $00508BE2B78F  0 0  <login-info> ;
: vector1 ( -- a n )  s" 10 01 00 90 00 00 01 00 88 00 00 00 02 00 09 72 00 10 00 00 00 00 00 07 00 01 00 00 00 00 00 00 E0 03 00 00 00 00 00 00 09 04 00 00 5E 00 08 00 6E 00 02 00 72 00 00 00 72 00 07 00 80 00 00 00 80 00 00 00 80 00 04 00 88 00 00 00 88 00 00 00 00 50 8B E2 B7 8F 88 00 00 00 88 00 00 00 88 00 00 00 00 00 00 00 73 00 6B 00 6F 00 73 00 74 00 6F 00 76 00 31 00 73 00 61 00 4F 00 53 00 51 00 4C 00 2D 00 33 00 32 00 4F 00 44 00 42 00 43 00" ;
T{ spec-login login7 login7-type payload
   vector1
   same? -> true }T
\ a password: each byte of its UTF-16 its halves swapped, xor A5 ("a" is 61 00: 16 xor A5 = B3, 00 xor A5 = A5)
: pw-login ( -- info )  tds-7.4 4096 0 0  0 0 " a" 0 0 0 0 0 0  0 0 <login-info> ;
T{ pw-login login7 b-data drop 94 + 2  s" B3 A5" hex> b-data s= -> true }T

testing LOGIN7 with an Entra token (FEDAUTH): the extension flag, the block where ibExtension's offset says - its
testing  id, length, the security token's library and the echo, the token as UTF-16 - no user nor password
: token-login ( -- b )  tds-7.4 4096 0 0  0 " sa" " pw" 0 0 0 0 0 0  " tok" true <login-info> login7 ;
: at32 ( b i -- x )  {: b i :}  i b b@  i 1+ b b@ 8 lshift or  i 2 + b b@ 16 lshift or  i 3 + b b@ 24 lshift or ;
: feature-at ( b -- i )  {: b :}  b  56 b b@ 57 b b@ 8 lshift or  at32 ;   \ (ibExtension -> the DWORD -> the block)
T{ token-login 27 swap b@ $10 and -> $10 }T
: feature-head ( -- id len options toklen )  token-login {: b :}  b feature-at {: f :}  f b b@  b f 1+ at32  f 5 + b b@  b f 6 + at32 ;
T{ feature-head -> 2 11 3 6 }T
: token-bytes ( -- a n )  token-login {: b :}  b feature-at 10 + b b-data drop + 6 ;
T{ token-bytes s\" t\x00o\x00k\x00" s= -> true }T
: feature-end ( -- c )  token-login {: b :}  b feature-at 16 + b b@ ;
T{ feature-end -> $ff }T   \ (the block's end)
T{ 0 encrypt-on 0 0 true prelogin-with prelogin-type payload b-data drop 8 + 25 + c@ -> 6 }T   \ (FEDAUTHREQUIRED's entry: the sixth)

testing a SQL batch: the specification's example (4.6) - its headers, its text as UTF-16
: spec-batch ( -- b )
    <bytes> {: b :}  $0100000000000000 0 b all-headers,  s\" \nselect 'foo' as 'bar'\n        " b utf16,  b ;
: vector2 ( -- a n )  s" 01 01 00 5C 00 00 01 00 16 00 00 00 12 00 00 00 02 00 00 00 00 00 00 00 00 01 00 00 00 00 0A 00 73 00 65 00 6C 00 65 00 63 00 74 00 20 00 27 00 66 00 6F 00 6F 00 27 00 20 00 61 00 73 00 20 00 27 00 62 00 61 00 72 00 27 00 0A 00 20 00 20 00 20 00 20 00 20 00 20 00 20 00 20 00" ;
T{ spec-batch sql-batch-type payload
   vector2
   same? -> true }T
T{ s" SELECT 1" 0 sql-batch b-length -> 22 16 + }T   \ (the headers, then 8 characters of UTF-16)

testing sp_executesql: each parameter whole after the one before (a float's 8 bytes added, not the request cut to them)
: float-rpc ( -- b )  s\" {\"f\":0.5,\"i\":7}" json> {: p :}  s" x" p 0 param-decls p 0 rpc-executesql ;
\ (headers 22, procedure 6, the SQL "x" 14, the declarations 12 + 38, @f and @i 17 each)
T{ float-rpc b-length -> 22 6 + 14 + 12 38 + + 17 + 17 + }T
T{ float-rpc b-data drop 22 6 + 14 + + 10 + le16@ -> 38 }T   \ (the declarations' length: "@f float, @i bigint", UTF-16)
testing bytes as a parameter: varbinary(8000) with their length, past 8000 varbinary(max) in pieces - as they are
: bytes-of ( n -- b )  {: n :}  <bytes> {: b :}  n 0 ?do  i 255 and b b-push  loop  b ;
: bin-params ( n -- m )  <map> {: m :}  bytes-of s" b" m s-at!  m ;
: decls-of ( n -- s len )  bin-params 0 param-decls ;
T{ 3 decls-of s" @b varbinary(8000)" s=  9000 decls-of s" @b varbinary(max)" s= -> true true }T
: bin-rpc ( n -- b )  bin-params {: p :}  s" x" p 0 param-decls p 0 rpc-executesql ;
: tail-of ( b k -- a n )  {: b k :}  b b-data k - + k ;   \ (its last k bytes)
T{ 3 bin-rpc 3 tail-of s\" \x00\x01\x02" s=  3 bin-rpc 8 tail-of drop c@ -> true $a5 }T   \ (the type, then the bytes)
T{ 9000 bin-rpc 4 tail-of s\" \x00\x00\x00\x00" s=  9000 bin-rpc b-length 9000 > -> true true }T   \ (PLP: its end piece)

testing packets: a message cut at the size; its end found, its payload put back together
create long-text 10000 allot  long-text 10000 char x fill
: cut ( -- b )  sql-batch-type long-text 10000 512 packets ;
T{ cut b-length -> 10000 10000 504 + 1- 504 / 8 * + }T   \ (each packet 504 bytes of payload and its 8 of header)
T{ cut b-data message-end -> cut b-length }T
T{ cut b-data 100 min message-end -> 0 }T                 \ (not all there yet)
T{ cut b-data 1000 - message-end -> 0 }T
<bytes> constant whole
T{ cut b-data message-end  cut b-data drop swap whole message-payload  whole b-length -> sql-batch-type 10000 }T
T{ s" 04 01 00 03 00 00 01 00" hex> b-data message-end -> -1 }T   \ (a length under a header's)

testing text: UTF-8 to UTF-16 and back - past the BMP as a surrogate pair; one unpaired as U+FFFD
: round ( a n -- s len )  <bytes> {: b :}  b utf16,  b b-data utf16> ;
T{ s" héllo €" round s" héllo €" s= -> true }T
T{ s" 😀" round s" 😀" s= -> true }T
: utf16-bytes ( a n -- n )  <bytes> {: b :}  b utf16,  b b-length ;
T{ s" 😀" utf16-bytes -> 4 }T
T{ s" 3D D8" hex> b-data utf16> s" �" s= -> true }T
T{ s" a😀" utf16-length -> 3 }T
: vector3 ( -- a n )  s" 04 01 01 61 00 00 01 00 E3 1B 00 01 06 6D 00 61 00 73 00 74 00 65 00 72 00 06 6D 00 61 00 73 00 74 00 65 00 72 00 AB 58 00 45 16 00 00 02 00 25 00 43 00 68 00 61 00 6E 00 67 00 65 00 64 00 20 00 64 00 61 00 74 00 61 00 62 00 61 00 73 00 65 00 20 00 63 00 6F 00 6E 00 74 00 65 00 78 00 74 00 20 00 74 00 6F 00 20 00 27 00 6D 00 61 00 73 00 74 00 65 00 72 00 27 00 2E 00 00 00 00 00 00 00 E3 08 00 07 05 09 04 D0 00 34 00 E3 17 00 02 0A 75 00 73 00 5F 00 65 00 6E 00 67 00 6C 00 69 00 73 00 68 00 00 E3 13 00 04 04 34 00 30 00 39 00 36 00 04 34 00 30 00 39 00 36 00 AB 5C 00 47 16 00 00 01 00 27 00 43 00 68 00 61 00 6E 00 67 00 65 00 64 00 20 00 6C 00 61 00 6E 00 67 00 75 00 61 00 67 00 65 00 20 00 73 00 65 00 74 00 74 00 69 00 6E 00 67 00 20 00 74 00 6F 00 20 00 75 00 73 00 5F 00 65 00 6E 00 67 00 6C 00 69 00 73 00 68 00 2E 00 00 00 00 00 00 00 AD 36 00 01 72 09 00 02 16 4D 00 69 00 63 00 72 00 6F 00 73 00 6F 00 66 00 74 00 20 00 53 00 51 00 4C 00 20 00 53 00 65 00 72 00 76 00 65 00 72 00 00 00 00 00 00 00 00 00 FD 00 00 00 00 00 00 00 00 00 00 00 00 00" ;
T{ s" 80 E9 41" hex> b-data cp1252> s" €éA" s= -> true }T

testing a reply: the specification's login reply (4.4) - logged in, the database, the packet size, its messages
: spec-reply ( a n -- reply )   \ (the hex of a whole packet: its payload read)
    hex> {: b :}  <bytes> {: p :}  b b-data drop  b b-data message-end  p message-payload drop  p b-data tds-reply> ;
vector3
spec-reply constant login-reply
T{ login-reply logged-in>> -> true }T
T{ login-reply database>> string s" master" s= -> true }T
T{ login-reply packet-size>> -> 4096 }T
T{ login-reply messages>> length  login-reply reply-error -> 2 0 }T
T{ 0 login-reply messages>> nth message>> string s" Changed database context to 'master'." s= -> true }T
T{ 0 login-reply messages>> nth number>> -> $1645 }T

testing a result set: the specification's (4.7) - a varchar column, its row, the count
s" 04 01 00 33 00 00 01 00 81 01 00 00 00 00 00 20 00 A7 03 00 09 04 D0 00 34 03 62 00 61 00 72 00 D1 03 00 66 6F 6F FD 10 00 C1 00 01 00 00 00 00 00 00 00"
spec-reply constant select-reply
T{ select-reply sets>> length  select-reply affected>> -> 1 1 }T
T{ 0 select-reply sets>> nth columns>> length -> 1 }T
T{ s" bar" 0 select-reply sets>> nth rows>> first s-at drop string s" foo" s= -> true }T

: types-hex ( -- a n )  s" 81 0D 00 00 00 00 00 01 00 26 04 01 69 00 00 00 00 00 01 00 E7 64 00 09 04 D0 00 34 01 73 00 00 00 00 00 01 00 6A 05 09 02 01 64 00 00 00 00 00 01 00 2A 07 02 64 00 74 00 00 00 00 00 01 00 68 01 01 62 00 00 00 00 00 01 00 6D 08 01 66 00 00 00 00 00 01 00 24 10 01 67 00 00 00 00 00 01 00 6E 08 01 6D 00 00 00 00 00 01 00 2B 00 01 6F 00 00 00 00 00 01 00 26 04 01 6E 00 00 00 00 00 01 00 E7 FF FF 09 04 D0 00 34 01 78 00 00 00 00 00 01 00 28 02 64 00 64 00 00 00 00 00 01 00 38 00 D1 04 2A 00 00 00 06 00 68 00 E9 00 21 00 05 01 E2 04 00 00 08 00 44 8E 02 58 65 4A 0B 01 01 08 00 00 00 00 00 00 F8 3F 10 FF 19 96 6F 86 8B 11 D0 B4 2D 00 C0 4F C9 64 FF 08 00 00 00 00 48 E8 01 00 08 98 85 00 65 4A 0B 3C 00 00 04 00 00 00 00 00 00 00 04 00 00 00 61 00 62 00 00 00 00 00 03 65 4A 0B 07 00 00 00 D2 FE 1F 04 FB FF FF FF FD 10 00 C1 00 02 00 00 00 00 00 00 00" ;
: error-hex ( -- a n )  s" AA 4A 00 D0 00 00 00 01 10 1B 00 49 00 6E 00 76 00 61 00 6C 00 69 00 64 00 20 00 6F 00 62 00 6A 00 65 00 63 00 74 00 20 00 6E 00 61 00 6D 00 65 00 20 00 27 00 6E 00 6F 00 70 00 65 00 27 00 2E 00 03 73 00 72 00 76 00 00 03 00 00 00 FD 10 00 C1 00 02 00 00 00 00 00 00 00" ;
: env-hex ( -- a n )  s" E3 0B 00 08 08 01 02 03 04 05 06 07 08 00 E3 2A 00 14 25 00 00 F9 2A 10 00 78 00 2E 00 77 00 6F 00 72 00 6B 00 65 00 72 00 2E 00 65 00 78 00 61 00 6D 00 70 00 6C 00 65 00 00 00 FD 10 00 C1 00 02 00 00 00 00 00 00 00" ;
: tokens ( a n -- reply )  hex> b-data tds-reply> ;
testing a varbinary column read: a bytes object (half the memory of hex text) - as JSON, "0x" and its hex
: binary-hex ( -- a n )  s" 810100000000000000a5401f016200d103000102fffd1000c1000100000000000000" ;
: bin-row ( -- m )  binary-hex tokens sets>> first rows>> first ;
T{ s" b" bin-row s-at drop  dup bytes?  swap b-data s\" \x01\x02\xff" s= -> true true }T
T{ bin-row >json s\" {\"b\":\"0x0102ff\"}" s= -> true }T

testing a row of each type: int, nvarchar, decimal, datetime2, bit, float, uniqueidentifier, money, datetimeoffset,
testing  NULL, nvarchar(max) in pieces, date, a column with no name; a row whose NULLs are a bitmap (NBCROW)
types-hex tokens constant typed
: row0 ( -- m )  0 typed sets>> nth rows>> first ;
: field ( name n -- v )  row0 s-at drop ;
: text= ( v a n -- flag )  rot string s= ;
T{ s" i" field -> 42 }T
T{ s" s" field s" hé!" text= -> true }T
T{ s" d" field s" 12.50" text= -> true }T
T{ s" dt" field s" 2026-11-22T10:30:00.0000000" text= -> true }T
T{ s" b" field -> json-true }T
T{ s" f" field value>> f>s -> 1 }T
T{ s" g" field s" 6F9619FF-8B86-D011-B42D-00C04FC964FF" text= -> true }T
T{ s" m" field s" 12.5000" text= -> true }T
T{ s" o" field s" 2026-11-22T10:30:00+01:00" text= -> true }T
T{ s" n" field -> json-null }T
T{ s" x" field s" ab" text= -> true }T
T{ s" dd" field s" 2026-11-22" text= -> true }T
T{ s" column13" field -> 7 }T
: row1 ( -- m )  1 0 typed sets>> nth rows>> nth ;
T{ s" i" row1 s-at drop  s" s" row1 s-at drop -> -5 json-null }T
T{ typed affected>> -> 2 }T

testing an error the server sent: in the reply (not thrown) - its number, class, text, line
error-hex tokens constant failed
T{ failed reply-error number>>  failed reply-error class>>  failed reply-error line>> -> 208 16 3 }T
T{ failed reply-error error-text s" Invalid object name 'nope'. (208, line 3)" s= -> true }T
testing changes to the session: a transaction begun (its descriptor), a route to another server
env-hex tokens constant changed
T{ changed txn>> -> $0807060504030201 }T
T{ changed route-host>> string s" x.worker.example" s=  changed route-port>> -> true 11001 }T
testing a reply cut short: an error
T{ s" FD 10 00" ' tokens catch 0<> nip nip -> true }T

testing a value shorter than its type (a guid of 1 byte, a hostile server's): refused, not read past the reply
: short-guid ( -- )  s" 81 01 00 00 00 00 00 00 00 24 10 01 41 00 D1 01 AB" hex> b-data tds-reply> drop ;
T{ ' short-guid catch 0<> -> true }T
testing a time of a scale past 7: refused (not read past the table of tens)
: deep-time ( -- )  s" 81 01 00 00 00 00 00 00 00 29 C8 01 41 00 D1 05 01 02 03 04 05" hex> b-data tds-reply> drop ;
T{ ' deep-time catch 0<> -> true }T
testing a guid of its 16 bytes still read
: good-guid ( -- s )  s" 81 01 00 00 00 00 00 00 00 24 10 01 41 00 D1 10 FF 19 96 6F 86 8B 11 D0 B4 2D 00 C0 4F C9 64 FF" hex> b-data
    tds-reply> sets>> 0 swap nth rows>> 0 swap nth  s" A" rot s-at drop string ;
T{ good-guid s" 6F9619FF-8B86-D011-B42D-00C04FC964FF" s= -> true }T

testing a parameter's name letters, digits and _ only (written into the declarations: a client's key adds no SQL)
: decls-of-json ( a n -- s len )  json> 0 param-decls ;
T{ s\" {\"ok_1\":1}" decls-of-json s" @ok_1 bigint" s= -> true }T
T{ [: s\" {\"a bigint; drop table t--\":1}" decls-of-json ;] catch 0<> -> true }T
T{ [: s\" {\"1a\":1}" decls-of-json ;] catch 0<> -> true }T
testing a row of no columns (metadata of none, then rows): refused - not a map for each byte of the reply
: no-columns ( -- )  s" 81 00 00 D1 D1 D1" hex> b-data tds-reply> drop ;
T{ ' no-columns catch 0<> -> true }T

test-summary

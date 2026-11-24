\ sha1.fs - SHA-1 (FIPS 180-4): what a WebSocket's handshake asks for, and checksums of files. Not for secrets: it is
\ broken for that.
\   s" abc" sha1 ( -- s 20 )   s" abc" sha1-hex ( -- s 40 )  "a9993e364706816aba3e25717850c26c9cd0d89d"
\ The digest is crypto's (C: every host has it - and no state of the node's to share between handlers).
IN: checksums.sha1
USING: checksums ;

\ the 20-byte digest of the bytes (a new string)
: sha1 ( a n -- s len )  crypto:sha1 ;
\ the digest as 40 hex digits (a new string)
: sha1-hex ( a n -- s len )  sha1 {: s n :}  s n >hex  s dispose ;

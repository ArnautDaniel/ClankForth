\ checksums (basis/checksums): >hex, and SHA-1 by FIPS 180's vectors
IN: test-checksums
USING: tester checksums checksums.sha1 ;

: hashes ( a n a2 n2 -- flag )  {: a n ea en :}  a n sha1-hex {: s sn :}  s sn ea en s=  s dispose ;

testing >hex
T{ s\" \x00\x0f\xa5\xff" >hex  over >r  s" 000fa5ff" s=  r> dispose -> true }T
testing sha1: FIPS 180's vectors, one block and two, and the empty message
T{ s" abc" s" a9993e364706816aba3e25717850c26c9cd0d89d" hashes -> true }T
T{ s" " s" da39a3ee5e6b4b0d3255bfef95601890afd80709" hashes -> true }T
T{ s" abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" s" 84983e441c3bd26ebaae4aa1f95129e5e54670f1" hashes -> true }T
T{ s" The quick brown fox jumps over the lazy dog" s" 2fd4e1c67a2d28fced849ee1bb76e7391b93eb12" hashes -> true }T
T{ s" abc" sha1 nip -> 20 }T
testing a WebSocket key's answer (RFC 6455's example)
USING: base64 ;
: accept-key ( -- flag )
    s" dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11" sha1 {: d dn :}  d dn >base64 {: s sn :}
    s sn s" s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" s=  s dispose  d dispose ;
T{ accept-key -> true }T

test-summary

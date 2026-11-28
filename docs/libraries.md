# The libraries

Built into the library (every host has them), laid out as Factor's `basis/`: a folder for each, its parts folders
within it - `basis/checksums/sha1/sha1.fs` is the vocabulary `checksums.sha1`. `USING:` loads one.

| vocabulary | what | doc |
|---|---|---|
| `tester` | `T{ ... -> ... }T` tests | docs/forth.md, Tests |
| `json` | JSON read and written; a value's fields of the kind wanted | below |
| `html`, `html.templates` | HTML written as Forth, or as templates compiled into words; escaping | docs/html.md |
| `base64` | base64 both ways | below |
| `checksums`, `checksums.sha1`, `checksums.crc32` | hex, SHA-1, CRC-32 | below |
| `compression` (built in, C; zlib's work where the build has it) | DEFLATE both ways, gzip - what HTTP's pages and WebSockets' messages go in | below |
| `crypto` (built in, C), `crypto.passwords`, `crypto.tokens`, `crypto.jwt` | SHA-256, HMAC, PBKDF2, secure random bytes; password hashes; tokens, UUIDs, signed values | below |
| `calendar` | dates from the clock's seconds, HTTP's and ISO 8601's forms of them | below |
| `logging` | a log file that is an actor: lines from anywhere, their times, rotated; JSON lines to stdout (needs `system`) | below |
| `supervisor` | an errors actor by default: a handler's failure logged, its actor on; one failing too often killed | docs/actors.md |
| `urls.encoding` | URLs' %-escapes | below |
| `c` (built in, C) | C libraries loaded and their functions called (where the host gives it: `forth_c`) | docs/c.md |
| `http` and its parts | a web server of actors, and a client; `http.ops`: health, readiness, metrics, request ids (needs `net`) | docs/http.md |
| `json.query` | a query written in Forth - `where$=`, `sorted-by`, `page` - for jsondb and `sql.model` | docs/jsondb.md |
| `jsondb` | a JSON database that is an actor: fsynced log, snapshot, indexes (needs `system`) | docs/jsondb.md |
| `azure.identity` | a managed identity's access tokens (SQL Database, Key Vault ...), kept till near their end (needs `net`) | docs/sql.md |
| `sql` and its parts | SQL Server from actors: a pool, parameters, models, watching; locked to its database (needs `net`) | docs/sql.md |

A new library gets its own folder in `basis/`, its parts vocabularies of their own, and a test
(`tests/test_<name>.fs`).

## json

```forth
USING: json ;
s\" {\"name\": \"ann\", \"tags\": [1, 2.5, true]}" json> {: v :}
s" name" v s-at drop string type        \ ann
v >json type                             \ {"name":"ann","tags":[1,2.5,true]}
v json-free
```

- `json> ( a n -- v )`: the text read (in C; a long text read in a handler gives way as it goes, as any long work
  does - a megabyte doesn't hold its node; it reads a copy, so the text may be another's to let go meanwhile). An object is a map (its keys strings, kept in their order), an array a list, a
  string a string, a whole number a cell; a number with a fraction or an exponent (or past a cell) a `json-float`
  (its float: `value>>`, USING: accessors); `true`, `false`, `null` the objects `json-true`, `json-false`, `json-null`
  (`json-bool ( flag -- v )`). Text that isn't JSON is an error saying where (`json: ']' expected at byte 5`):
  `catch` it. Arrays and objects deeper than `json-depth-max` are an error (32; set higher it is 40 at most - as
  deep as `json-copy` and `json-free` go on a task's return stack. Objects are kept and sent 64 deep, so what is
  read goes inside a program's own lists and maps). A number of more than 1024 characters, or too big for a float
  (`1e999`), is an error too. A whole number with a reference's mark (4.6 to 5.8 million million million) is read
  as a `json-float`: no number in a document names an object to `>json` or `json-free` (Objects, docs/forth.md).
- `>json ( v -- s len )`: the value as JSON text, a new string (written in C; a long value written in a handler
  gives way as it goes - and is asked for anew at each item, so one another actor lets go meanwhile is an error
  said, not a crash: copy what isn't the handler's own first, `json-copy`): maps (a number key written as a string), lists,
  strings (escaped), numbers, json-floats (an infinity or NaN as null), the three literals. Any other tuple is an
  error.
- `json-free ( v -- )`: all of what `json>` gave let go. Read inside a scratch region (a web handler's), it goes with
  the region instead.
- `json-copy ( v -- v' )`: a tree of its own, all the way down (strings, lists, maps, bytes, json-floats; numbers and
  the three constants as they are) - what to keep of a value someone else lets go (a page's state replaced, a
  message's), without `>json json>` between.

- A value's fields, of the kind wanted - what a client sent, checked as it is read: `json-string@ ( key n v -- a n
  true | false )`, `json-int@ ( key n v -- n true | false )`, `json-bool@ ( key n v -- flag true | false )`,
  `json-list@`, `json-map@ ( key n v -- x true | false )`. With a default, no flag: `json-text ( key n v -- a n )`
  (a string, a whole number in decimal, else ""), `json-int ( key n v -- n )` (else 0), `json-flag ( key n v --
  flag )` (else false) - what a page reads of a row or a form a hundred times. Each false if v isn't an object, it has no such field, or
  the field is of another kind (`"age": "41"` isn't `json-int@`'s).
- `json-string! ( a n key n m -- )`: a field written - the text as a new string of the map's (a number, a map, a
  list: `s-at!`).

## base64

`>base64 ( a n -- s len )` the bytes as base64, padded; `>base64url` the URL form (`- _`, no padding: a URL's, a
cookie's, a token's); `base64> ( a n -- s len true | false )` back - padded or not, `+ /` or the URL form's `- _` -
false for text that isn't base64.

## checksums, checksums.sha1, checksums.crc32

`>hex ( a n -- s len )` bytes as lower-case hex; `hex> ( a n -- s len true | false )` hex digits (either case,
spaces between them passed over) as their bytes, false for what isn't hex. `sha1 ( a n -- s len )` the 20-byte digest, `sha1-hex` as 40 hex
digits. (SHA-1 is broken for secrets: it is here for WebSockets' handshake and checksums of files.)

`crc32 ( a n -- u )` CRC-32 (zlib's, PNG's), `crc32-hex ( a n -- s len )` as 8 hex digits, `crc32-more ( crc a n --
crc' )` carried on over another piece (start at 0): that bytes came back as they were written - a log's lines
(jsondb's). Not for secrets.

## crypto, crypto.passwords, crypto.tokens

What a web server's secrets need. `crypto` is built in (`src/crypto.c`: a password's hash is a million SHA-256
blocks - in C); the other two are Forth on it.

- `sha256 ( a n -- s len )` the 32-byte digest (FIPS 180-4); `hmac-sha256 ( key n msg n -- s len )` (RFC 2104);
  `pbkdf2-sha256 ( password n salt n iterations length -- s len )` (RFC 8018) - each a new string of bytes (`>hex`,
  `>base64url` to show one).
- `sha1 ( a n -- s len )`, `hmac-sha1 ( key n msg n -- s len )`, `pbkdf2-sha1 ( password n salt n iterations length
  -- s len )`: the same with SHA-1 (20-byte digests) - for reading what another system made (ASP.NET Identity 2's
  password hashes), never for a new secret.
- `random-bytes ( n -- s len )`: from the system's secure random source (getrandom, arc4random, RtlGenRandom) - for
  anything a guesser mustn't guess. (`random` is a seeded generator, for games and tests: never for a secret.)
- `same-bytes? ( a n a2 n2 -- flag )`: looks at every byte however soon they differ - compare a MAC or a token with
  it, so its time tells a guesser nothing.
- `rsa-sha256-verify ( msg n sig n modulus n exponent n -- flag )`: an RS256 signature (RSA, PKCS#1 v1.5, SHA-256)
  is the key's - the key's n and e big-endian bytes (a JWK's, base64url-decoded); 512 to 4096 bits.
  `rsa-sha256-sign ( msg n modulus n d n -- sig len )` with a private key (its n and d).
- `crypto.jwt`: JSON Web Tokens, RS256 only. `jwt-verify ( token n keys -- claims )` - keys a JWKS (`json>` of what a
  provider's `jwks_uri` gives): the token's kid's key, its signature, `exp` and `nbf` (`jwt-skew-s`, 300 s, allowed) -
  its claims, or an error saying why not ("none" and HS256 refused whatever they say). `jwt-check ( claims iss n aud
  n -- )` its issuer and audience. `jwt-sign ( claims n kid n modulus n d n -- token n )`, `jwks-of ( kid n
  modulus n e n -- s len )` (a set of one key) - for tokens of one's own, and tests.

`crypto.passwords`: `hash-password ( password n -- s len )` -
`pbkdf2-sha256$600000$<salt>$<hash>` (base64; a random 16-byte salt each), the text to keep in the user's record;
`check-password ( password n stored n -- flag )` (by the iterations the hash was made with; false for a hash not of
this form); `rehash? ( stored n -- flag )` - made with fewer than `password-iterations` (600,000, OWASP's): hash the
password again as it signs in. A hash takes ~0.25 s, holding up its node meanwhile - which is the point, for a
guesser; a site with many sign-ins does them on a node of their own. `aspnet-v2-ok? ( password n hash n -- flag )`:
the password an ASP.NET Identity 2 hash (an AspNetUsers row's `PasswordHash`, its "V2" form: base64 of 0x00, a
16-byte salt, PBKDF2-HMAC-SHA1's 32 bytes of 1,000 iterations) was made of - false for a hash not of that form.
Signed in so, the user's password is hashed again by `hash-password` (`rehash?` is true of that form).

`crypto.tokens`: `random-token ( -- s len )` 256 random bits, 43 characters (base64url): a session's id, a CSRF
token, a reset link's; `random-hex ( n -- s len )`; `uuid4 ( -- s len )` (RFC 9562, version 4); `sign ( a n key n --
s len )` the text, a dot, its HMAC by the key - and `unsign ( s n key n -- a n true | false )` the text back if it is
the key's: a value given out (a cookie) that can't be changed unseen. Signed, not secret: it hides nothing.

## compression

`USING: compression ;` - built in (C, `src/compress.c`), as `crypto` is. The work is zlib's where the build found
zlib (`CLANKFORTH_ZLIB`, on by default: the build says which); without, the system's own code - the same words, the
same streams. `compressed-by ( -- s len )` says whose: `zlib 1.3.1`, or `clankforth`.

- `gzip ( a n -- s len )`: gzip's form (a `.gz` file's, `Content-Encoding: gzip`'s); `gunzip ( a n max -- s len true
  | false )` back - false for what isn't gzip, a CRC or a length that doesn't match, or more than max bytes. A file
  of several members (gzips put end to end) is the bytes of all of them, max the most of them together; bytes
  after the last member that are no member are refused (padding too).
- `deflate ( a n -- s len )`: raw DEFLATE (RFC 1951); `inflate ( a n max -- s len true | false )` back (its input may
  end at a block's end, unfinished: a WebSocket message's). `deflate-sync ( a n -- s len )`: ended as a sync flush,
  its last `00 00 FF FF` left off - permessage-deflate's form.
- A call is whole: bytes in, a new string out, nothing kept after it and nothing shared with another call - any
  actor's, on any node, at the same time (zlib is given a stream of its own each call; the system's own code has
  none to share).
- **In a handler the node's other actors don't wait for it.** Up to 8 KB (4 KB by the system's own code) is done
  where the word stands - under a slice's tenth of a millisecond - and counted against the handler's slice, so a
  handler compressing page after page gives way as any long handler does. More is done on a worker thread while
  the handler waits (docs/actors.md, Work that blocks): its node goes on. Inflating, what will be made isn't known
  first: up to that much is made in place, and only a stream that would make more is begun again on a worker.
  So these words may wait in a handler - what is true of any wait holds (another actor's handler may run
  meantime; a socket may have gone: `net-gone?`). At the prompt, through `execute`, inside `atomically`: where
  the word stands, as long as it takes. Measured, another actor's longest wait while one handler gzipped a
  megabyte 40 times: 611 ms before (15 at a call), 0.7 ms now.
- What a call works in is as big as its input calls for - zlib's window sized to it (with zlib's usual 32 KB one a
  300-byte message took 37 us; with one of 512 bytes, 7), and zlib works at level 3, not its default 6: a page is
  deflated each time it is asked for. Measured, gzip of a 2.5 KB page: 53 us by the system's own code, 18 by zlib;
  a 62 KB JSON text 600 and 87; what zlib makes 2 to 10% longer than the system's own.
- The system's own: LZ77 over a 32 KB window, each block in codes of its own or the fixed ones, whichever is
  shorter. Either way inflating checks every length and distance, and `max` bounds what a stream may make (a small
  stream that would inflate to gigabytes is refused, not followed).

## calendar

Dates from the clock's seconds (`system`'s `time-now`), UTC, the proleptic Gregorian calendar.

- `seconds>date ( s -- year month day hour minute second weekday )` (weekday 0 for Sunday), `date>seconds ( year
  month day hour minute second -- s )`; `days>date ( days -- year month day )` and `date>days` for days since
  1970-01-01. Before 1970 too.
- `>http-date ( s -- s len )`: `Sun, 06 Nov 1994 08:49:37 GMT` (RFC 9110's IMF-fixdate), a new string;
  `http-date> ( a n -- s true | false )` reads one.
- `>iso-date ( s -- s len )`: `2026-11-22T09:05:00Z` (ISO 8601 / RFC 3339, UTC: JSON's dates, a log's), a new
  string; `iso-date> ( a n -- s true | false )` reads one - a date alone, a time (`T` or a space), a fraction of a
  second (dropped), `Z` or an offset (`+01:00`, `-0500`; none: UTC).
- `seconds-per-day`, `seconds-per-hour`, `seconds-per-minute`.

## logging

A log file that is an actor: lines sent it from any actor, on any node, each written with the time it came; those
that come together written together (one write); the file rotated as it grows.

- `log-open ( path n -- logger )`: the file (made if it isn't there, written at its end) and its actor, here - from
  the interpreter. Register its name for the other nodes.
- `log ( a n logger -- )`: a line (`2026-11-22T09:05:00Z ` before it). A write that fails is said, and the site goes on.
- `log-max-bytes` (10 MiB): the file rotated then - `log` becomes `log.1`, `log.1` `log.2` ... up to `log-keep` (5).
- Killed, its last words write what came and close the file. `http.logging`'s `access-log`: a line each request.


JSON lines to stdout - what a container's collector reads (Azure's Log Analytics, Docker's): `log-json ( m -- )` the
map's fields as one line, a `"time"` first (ISO 8601, UTC); `log-event ( level n event n fields -- )` one with
`"level"` and `"event"` and the map's fields (or 0). A line is written whole.
## urls.encoding

`url-decode ( a n -- s len )` (a query's: `+` a space), `path-decode` (a path's: `+` itself) - each `%HH` its byte,
a `%` without two hex digits kept; `url-encode ( a n -- s len )` every byte but letters, digits and `- . _ ~` as
`%HH`. Each a new string.

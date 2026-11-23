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
| `compression` (built in, C) | DEFLATE both ways, gzip - what HTTP's pages and WebSockets' messages go in | below |
| `crypto` (built in, C), `crypto.passwords`, `crypto.tokens`, `crypto.jwt` | SHA-256, HMAC, PBKDF2, secure random bytes; password hashes; tokens, UUIDs, signed values | below |
| `calendar` | dates from the clock's seconds, HTTP's and ISO 8601's forms of them | below |
| `logging` | a log file that is an actor: lines from anywhere, their times, rotated; JSON lines to stdout (needs `system`) | below |
| `supervisor` | an errors actor by default: a handler's failure logged, its actor on; one failing too often killed | docs/actors.md |
| `urls.encoding` | URLs' %-escapes | below |
| `http` and its parts | a web server of actors, and a client; `http.ops`: health, readiness, metrics, request ids (needs `net`) | docs/http.md |
| `jsondb` | a JSON database that is an actor: fsynced log, snapshot, indexes (needs `system`) | docs/jsondb.md |
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

- `json> ( a n -- v )`: the text read. An object is a map (its keys strings, kept in their order), an array a list, a
  string a string, a whole number a cell; a number with a fraction or an exponent (or past a cell) a `json-float`
  (its float: `value>>`, USING: accessors); `true`, `false`, `null` the objects `json-true`, `json-false`, `json-null`
  (`json-bool ( flag -- v )`). Text that isn't JSON is an error saying where (`json: ']' expected at byte 5`):
  `catch` it. Arrays and objects deeper than `json-depth-max` (128) are an error.
- `>json ( v -- s len )`: the value as JSON text, a new string: maps (a number key written as a string), lists,
  strings (escaped), numbers, json-floats (an infinity or NaN as null), the three literals. Any other tuple is an
  error.
- `json-free ( v -- )`: all of what `json>` gave let go. Read inside a scratch region (a web handler's), it goes with
  the region instead.

- A value's fields, of the kind wanted - what a client sent, checked as it is read: `json-string@ ( key n v -- a n
  true | false )`, `json-int@ ( key n v -- n true | false )`, `json-bool@ ( key n v -- flag true | false )`,
  `json-list@`, `json-map@ ( key n v -- x true | false )`. Each false if v isn't an object, it has no such field, or
  the field is of another kind (`"age": "41"` isn't `json-int@`'s).

## base64

`>base64 ( a n -- s len )` the bytes as base64, padded; `>base64url` the URL form (`- _`, no padding: a URL's, a
cookie's, a token's); `base64> ( a n -- s len true | false )` back - padded or not, `+ /` or the URL form's `- _` -
false for text that isn't base64.

## checksums, checksums.sha1, checksums.crc32

`>hex ( a n -- s len )` bytes as lower-case hex. `sha1 ( a n -- s len )` the 20-byte digest, `sha1-hex` as 40 hex
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
guesser; a site with many sign-ins does them on a node of their own.

`crypto.tokens`: `random-token ( -- s len )` 256 random bits, 43 characters (base64url): a session's id, a CSRF
token, a reset link's; `random-hex ( n -- s len )`; `uuid4 ( -- s len )` (RFC 9562, version 4); `sign ( a n key n --
s len )` the text, a dot, its HMAC by the key - and `unsign ( s n key n -- a n true | false )` the text back if it is
the key's: a value given out (a cookie) that can't be changed unseen. Signed, not secret: it hides nothing.

## compression

`USING: compression ;` - built in (C, `src/compress.c`, no library), as `crypto` is.

- `gzip ( a n -- s len )`: gzip's form (a `.gz` file's, `Content-Encoding: gzip`'s); `gunzip ( a n max -- s len true
  | false )` back - false for what isn't gzip, a CRC or a length that doesn't match, or more than max bytes.
- `deflate ( a n -- s len )`: raw DEFLATE (RFC 1951); `inflate ( a n max -- s len true | false )` back (its input may
  end at a block's end, unfinished: a WebSocket message's). `deflate-sync ( a n -- s len )`: ended as a sync flush,
  its last `00 00 FF FF` left off - permessage-deflate's form.
- Its own LZ77 over a 32 KB window, each block in codes of its own or the fixed ones, whichever is shorter: within
  a few per cent of zlib's default on HTML. Nothing is kept between calls, and what a call works in is as big as its
  input calls for - a short message's few KB, a big page's a few hundred. Inflating checks every length and distance; `max` bounds what a stream
  may make (a small stream that would inflate to gigabytes is refused, not followed).

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

# The libraries

Built into the library (every host has them), laid out as Factor's `basis/`: a folder for each, its parts folders
within it - `basis/checksums/sha1/sha1.fs` is the vocabulary `checksums.sha1`. `USING:` loads one.

| vocabulary | what | doc |
|---|---|---|
| `tester` | `T{ ... -> ... }T` tests | docs/forth.md, Tests |
| `json` | JSON read and written | below |
| `base64` | base64 both ways | below |
| `checksums`, `checksums.sha1`, `checksums.crc32` | hex, SHA-1, CRC-32 | below |
| `crypto` (built in, C), `crypto.passwords`, `crypto.tokens` | SHA-256, HMAC, PBKDF2, secure random bytes; password hashes; tokens, UUIDs, signed values | below |
| `calendar` | dates from the clock's seconds, HTTP's form of them | below |
| `urls.encoding` | URLs' %-escapes | below |
| `http` and its parts | a web server of actors, and a client (needs `net`) | docs/http.md |
| `jsondb` | a JSON database that is an actor: fsynced log, snapshot, indexes (needs `system`) | docs/jsondb.md |

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

## calendar

Dates from the clock's seconds (`system`'s `time-now`), UTC, the proleptic Gregorian calendar.

- `seconds>date ( s -- year month day hour minute second weekday )` (weekday 0 for Sunday), `date>seconds ( year
  month day hour minute second -- s )`; `days>date ( days -- year month day )` and `date>days` for days since
  1970-01-01. Before 1970 too.
- `>http-date ( s -- s len )`: `Sun, 06 Nov 1994 08:49:37 GMT` (RFC 9110's IMF-fixdate), a new string;
  `http-date> ( a n -- s true | false )` reads one.
- `seconds-per-day`, `seconds-per-hour`, `seconds-per-minute`.

## urls.encoding

`url-decode ( a n -- s len )` (a query's: `+` a space), `path-decode` (a path's: `+` itself) - each `%HH` its byte,
a `%` without two hex digits kept; `url-encode ( a n -- s len )` every byte but letters, digits and `- . _ ~` as
`%HH`. Each a new string.

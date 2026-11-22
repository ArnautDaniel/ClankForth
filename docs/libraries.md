# The libraries

Built into the library (every host has them), laid out as Factor's `basis/`: a folder for each, its parts folders
within it - `basis/checksums/sha1/sha1.fs` is the vocabulary `checksums.sha1`. `USING:` loads one.

| vocabulary | what | doc |
|---|---|---|
| `tester` | `T{ ... -> ... }T` tests | docs/forth.md, Tests |
| `json` | JSON read and written | below |
| `base64` | base64 both ways | below |
| `checksums`, `checksums.sha1` | hex, SHA-1 | below |
| `calendar` | dates from the clock's seconds, HTTP's form of them | below |
| `urls.encoding` | URLs' %-escapes | below |
| `http` and its parts | a web server of actors, and a client (needs `net`) | docs/http.md |

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

`>base64 ( a n -- s len )` the bytes as base64, padded; `base64> ( a n -- s len true | false )` back - padded or not,
`+ /` or the URL form's `- _` - false for text that isn't base64.

## checksums, checksums.sha1

`>hex ( a n -- s len )` bytes as lower-case hex. `sha1 ( a n -- s len )` the 20-byte digest, `sha1-hex` as 40 hex
digits. (SHA-1 is broken for secrets: it is here for WebSockets' handshake and checksums of files.)

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

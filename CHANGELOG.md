# Changelog

The version is `forth.h`'s (`CLANKFORTH_VERSION`, `CLANKFORTH_VERSION_NUMBER` for `#if`; `clankforth --version`,
`clankforth-version` in Forth). Before 1.0 the minor number goes up when a host must change something to build or run
with it - its entry says so, and [docs/hosts.md](docs/hosts.md) says how - and the patch number for what needs nothing
of a host. A release is a commit that moves the version, tagged `vX.Y.Z`.

Changes not yet released are listed under **Unreleased**; a release renames that heading to its version.

## Unreleased

## 0.2.0 - 2026-11-22

- The version, in `forth.h` (`CLANKFORTH_VERSION_MAJOR` `_MINOR` `_PATCH`, `CLANKFORTH_VERSION`,
  `CLANKFORTH_VERSION_NUMBER`), `forth_version()`, the word `clankforth-version ( -- addr len )` and the tool's
  `--version`. CMake's project version is read from `forth.h`. Nothing for a host to change.

## 0.1.0 - 2026-11-21

Everything up to pull request #17 (commit `570b07a`), before there were versions: the interpreter and its native
backend (x86-64), vocabularies, stack effects and the checker, locals, quotations, lists, tuples and generic words,
strings, maps, tasks; the actor kernel, nodes; the `system` and `net` vocabularies; `basis/` (`http` with WebSockets,
server-sent events, static files, cookies and TLS; `json`, `base64`, `checksums.sha1`, `calendar`, `urls.encoding`).
What a host written before then changes is in docs/hosts.md.

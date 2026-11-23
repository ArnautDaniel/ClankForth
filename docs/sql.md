# SQL Server

`sql` and its parts talk to SQL Server - Azure SQL Database, Azure SQL Managed Instance, SQL Server 2022 - from
actors, in Forth over `net` (no ODBC, no driver to install). Each connection is an actor speaking TDS on its socket;
a pool hands them out; whoever asks awaits the answer, its other messages kept meanwhile, so nothing blocks the node.
A host gives `net` (the `clankforth` tool does).

| vocabulary | what |
|---|---|
| `sql` | connections, a pool, queries with parameters, transactions, the database lock, dry runs |
| `sql.model` | a table's words from its declaration: get, find (jsondb's query shape), count, insert, update, delete |
| `sql.describe` | the catalog read: a table as a model declaration, the databases a login may open |
| `sql.watch` | tables watched: `db-changed` to an actor when one changes - live views' `changed:` |
| `sql.tds` | the protocol itself: messages made, replies read (bytes in and out; the others use it) |

```forth
USING: actors json sql sql.model ;

s" myserver.database.windows.net" s" app_user" s" SQL_PASSWORD" getenv drop s" stagingCRSql1" sql-config
    s" prodCRSql1" rot sql-forbid
4 sql-pool constant db              \ up to 4 connections, opened as they are first wanted

model task dbo.Tasks
    key: TaskId int identity
    column: Title nvarchar(200)
    column: Status varchar(20)
    version: RowVer
end-model

\ ... in a handler (an actor's: a web request's, a live view's):
s" SELECT TOP 10 Title FROM dbo.Tasks WHERE Status = @s" s\" {\"s\":\"open\"}" json> db sql-query  ( -- rows )
7 db task-get  ( -- row|0 )
```

## Connecting

- `sql-config ( host n user n password n database n -- config )`: a server, a SQL login, the database. Port 1433
  (`sql-port ( port config -- config )`); the application's name the server's sessions show is `clankforth`
  (`sql-app ( name n config -- config )`).
- **Encrypted from the first byte**: TDS 8.0's strict mode - TLS before anything else (ALPN `tds/8.0`), the server's
  certificate checked against the host's name (`tls-ca!` for a certificate of your own; docs/net.md). Needs a build
  with TLS. `sql-plain ( config -- config )`: no TLS - a server on the same machine or a test's; one that requires
  encryption refuses it. (The older order - TLS inside TDS's own packets - isn't spoken: every server above speaks
  strict.)
- **Azure's gateway** may route a login to another server and port (Azure SQL Database's "redirect": ports
  11000-11999 must be open from the VM, not only 1433): the connection follows it, its certificate still checked
  against the name configured.
- `sql-pool ( config size -- db )`: up to `size` connections, opened as they are first wanted, lent one asker at a
  time. A connection that fails (closed, timed out, its database changed) is dropped, the next ask opens another.
  `sql-timeout-ms` (30 s): a request with no answer in that long fails and its connection is closed;
  `sql-connect-ms` (60 s) for a login; `sql-response-max` (64 MB) an answer's bytes.
- **A database resuming**: a serverless Azure database pauses when idle, and the first login after refuses for a
  while (40613 "not currently available"; 40197, 40501, 49918-49920 and 40540 are the same kind). The pool tries
  such a login again after `sql-retry-ms` (1 s), doubling up to `sql-retry-max-ms` (10 s), `sql-retries` (8) times -
  about a minute - before everyone waiting is told. Other errors (a wrong password, a database not there) are told
  at once. A request that fails isn't sent again: it may have written.
- **Idle connections are closed**: one left free for `sql-idle-ms` (60 s; 0 keeps them) is closed, the next ask
  opening another - a serverless database pauses only when no session is open.
- Logins are SQL logins (a user and a password). Microsoft Entra ID - managed identities, access tokens: TDS's
  FEDAUTH login - isn't spoken yet.

## The database, and only that one

A login may reach other databases on the same server; a program that must never touch one (prod, beside the
staging it is tested on) has these locks:

- **The login asks for the database configured and fails without it** (LOGIN7's fDatabase): a database it can't
  open fails the login - it never falls back to the login's default database.
- **The server must say it**: a connection whose login reply names another database is closed before it is used.
- **It can't change**: a request that changes the session's database (`USE other`, a procedure that does) fails, and
  its connection is closed before anything else runs on it.
- `sql-forbid ( name n config -- config )`: SQL whose text names that database (in any case) is refused before it is
  sent - `prodCRSql1.dbo.Orders` can't be reached by a three-part name. (Azure SQL Database refuses those itself;
  Managed Instance and SQL Server don't.)
- `sql-dry-run ( config -- config )`: every request runs in a transaction rolled back after it - what it would do,
  done and undone; a COMMIT in its text refused. (A transaction's own end is a rollback too.)

The strongest lock is the server's: a login with no user in the other database can't open it at all. The check
tool says which databases a login can open.

`tools/sql-check.fs` is a first look that writes nothing: it refuses a database whose name doesn't say "staging"
(or says "prod"), runs every request dry, says where it connected (the database, server, login, version), which
databases the login may open (forbidding, from then, any whose name says prod), and a table's model:

```sh
SQL_PASSWORD=... clankforth $PWD/tools/sql-check.fs -- myserver.database.windows.net app_user stagingCRSql1 dbo.Orders
```

## Asking

Inside an actor (a handler: the answer is a message):

- `sql-query ( sql n params db -- rows )`: the first result set's rows - a list of maps, each column's name to its
  value. `sql-one ( ... -- row | 0 )` its first row; `sql-exec ( ... -- n )` the rows its last statement changed (-1
  if none were counted); `sql-reply ( ... -- reply )` all of it (sql.tds's `tds-reply`: every result set, counts,
  the server's messages, output parameters).
- **Parameters**: `params` is a map of names (without `@`) to values, or 0 - sent with sp_executesql, never put into
  the SQL's text. A list is several: `WHERE Id IN (@ids)` with `ids` [3, 5, 8] is sent as `IN (@ids0, @ids1,
  @ids2)`, each item a parameter (declared as `types` says of `ids`); an empty list as `SELECT NULL WHERE 1 = 0` (IN
  finds no row, NOT IN every row). An `@ids` in quotes, brackets or a comment is left. A request has at most 2100
  parameters (SQL Server's limit; more is an error saying so): a list longer than that belongs in a table. A whole number goes as bigint, a json-float as float, json-true / json-false as bit, json-null as
  NULL, a string as nvarchar. `sql-reply-typed ( sql n params types db -- reply )` declares them as `types` says (a
  map of names to SQL types): `{"code": "varchar(20)"}` - a varchar column compared with an nvarchar parameter can't
  use its index under a SQL collation. (sql.model declares each as its column is.)
- **Values read**: a whole number a cell; text a string (UTF-8: from UTF-16, or a column's code page - Windows-1252,
  or UTF-8 under a UTF-8 collation); decimal, numeric and money a string (`"12.50"`: exact, as the server has it);
  float and real a json-float; bit json-true / json-false; dates and times ISO 8601 strings (`2026-11-22T10:30:00`,
  a datetimeoffset with its offset); uniqueidentifier its text; binary `0x` and hex; NULL json-null. A column with
  no name is `column1` ...
- **Errors**: a SQL error is thrown, its text the server's: `Invalid object name 'x'. (208, line 1)` - `catch` it.
  A connection that fails throws why (`sql: the connection closed`, `sql: login failed: ...`).
- `sql-transaction ( xt db -- )`: xt `( session -- )` runs on one connection in a transaction - committed if it
  ends, rolled back if it throws (the error thrown on). Its requests take the session as their db.
- **A connection given back clean**: one returned to the pool with a transaction still open (a rollback that failed,
  a BEGIN TRANSACTION of the caller's own) is rolled back before it is lent again - or closed if that fails. No
  request ever runs in someone else's transaction.
- **Parameters stay the caller's**: a copy of the map and its values goes with the request (a message takes what it
  carries); the caller's map, and the values in it, are as they were - one map may go with many requests.
- **Fewer round trips**: one round trip to Azure is ~50 ms, so a page's queries add up. Several SELECTs in one batch
  are one round trip - `sql-reply`'s `sets>>` has each one's rows, in order. Queries that can't share a batch go out
  at once with `sql-query-all ( xt db -- lists )`: xt adds them (`sql-also ( sql n params -- )`), they run on as many
  of the pool's connections as there are (up to its size), and their rows come back together, in the order they
  were added - as long as the slowest, not their sum. The first error is thrown once all have answered. (In a
  transaction - one connection - they go one after another.)
- No request inside a `map-each` (a walk of C's can't wait): walk `keys` with `each` instead.

## Models (sql.model)

A model maps a table that is there (no tables made or dropped): its key, its columns and their SQL types, and its
rowversion column if it has one. `sql-describe` writes one from the table.

```forth
model task dbo.Tasks
    key: TaskId int identity          \ identity: the server numbers it - an insert gives it back
    column: Title nvarchar(200)
    column: Status varchar(20)
    version: RowVer                   \ a rowversion: updates and deletes checked by it
end-model
```

Each model has words named after it (and `model-get` ... taking the model):

- `task-get ( key db -- row | 0 )`; `task-find ( query n db -- rows )`; `task-count ( query n db -- n )`. A query is
  jsondb's shape (docs/jsondb.md): `where` - a column's value, null, or `{"op": value}` with `= != < <= > >= in
  contains exists` - `sort` (a `-` first: descending), `offset`, `limit`. Every column named is checked against the
  model: a query can't reach one it doesn't name. The version comes back as a number.
- `task-insert ( row db -- key )`: the row's columns the model has (not an identity key, not the version).
- `task-update ( changes row db -- saved? )`: the row as it was read, changed as `changes` (a map) says: true if
  saved; **false if someone changed it since it was read** - by its version, or without one by the old values of
  the columns changed. `task-delete ( row db -- deleted? )` the same way.

## Watching (sql.watch)

SQL Server pushes nothing a login without schema rights can turn on, so a watcher looks:

- `sql-watcher ( db ms -- watcher )`: an actor looking every ms at the tables watched through it - a count and a
  checksum, or a model with a version its highest version (its index read, not the table). Writes through the db
  (sql.model's words) are looked at at once.
- `model-watch ( model watcher -- )`, `sql-watch ( table n watcher -- )`, `sql-unwatch`: inside an actor - a live
  view's mount. A change sends it `db-changed ( c id rev json -- )` (jsondb's: c the table's name, the rest 0), so a
  live view's `changed:` reloads what it shows.
- `model-watch-where ( model where n params watcher -- )`, `sql-watch-where ( table n where n params watcher -- )`:
  only the rows a WHERE finds - `CompanyId = @id`, its parameters a map. A page watching its own company's rows
  costs an index's seek (with an index on CompanyId), not a scan of the table; a big table without a rowversion
  should be watched this way.
- A watcher gone is forgotten, and a watch nobody watches any more is dropped: a database no page looks at isn't
  looked at, and a serverless one can pause.

Coarse by design: one notice per table per look. A write from elsewhere (another node, the other project) is seen at
the next look.

## Tests

`tests/test_sql_tds.fs` reads the specification's own example packets (MS-TDS section 4) and a row of each type;
`tests/test_sql.fs`, `test_sql_model.fs` and `test_sql_watch.fs` run against a stand-in server (`tests/sql/`) -
plain, and TLS first behind `tls-listen`; `tests/sql/live.fs` against a real one, or one of your own (its first
lines say how). CI runs it against SQL Server 2022 in a container, plain: the container refuses strict
connections whatever its certificate (error 17821, microsoft/mssql-docker#878), so TLS first there is tried and
said, not required. Azure SQL Database and Managed Instance accept strict.

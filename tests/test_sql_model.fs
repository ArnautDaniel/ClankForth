\ sql.model (basis/sql/model): a table's words from its declaration - the SQL each sends (its parameters declared as
\ the columns are), queries in jsondb's shape, updates checked by the version or by the old values, names checked
IN: test-sql-model
USING: tester actors net json json.query accessors sql.tds sql sql.model ;
source-dir s" /sql/stand-in.fs" s+ included

: test-config ( -- c )  s" 127.0.0.1" s" sa" s" pw" s" stagingdb" sql-config sql-plain  stand-in-port @ swap sql-port ;
test-config 1 sql-pool constant db

model task dbo.Tasks
    key: TaskId int identity
    column: Title nvarchar(200)
    column: Status varchar(20)
    column: Priority int
    version: RowVer
end-model
model note Notes
    key: NoteId varchar(10)
    column: Body nvarchar(max)
end-model

: sent ( -- a n )  asked last string ;
: sent? ( a n -- flag )  sent s= ;
: shown ( -- )  sent type cr ;

testing get: by its key, the version as a number
T{ [: 7 db task-get 0<> ;] run -> true }T
T{ s" SELECT [TaskId], [Title], [Status], [Priority], CAST([RowVer] AS bigint) AS [RowVer] FROM [dbo].[Tasks] WHERE [TaskId] = @p1|@p1 int" sent? -> true }T

testing find: where (a value, null, operators, in, contains), sort, paging - each parameter as its column's type
: q1 ( -- a n )  s\" {\"where\":{\"Status\":\"open\",\"Priority\":{\">=\":2,\"<\":5},\"Title\":null},\"sort\":[\"-Priority\",\"Title\"],\"limit\":50,\"offset\":10}" ;
T{ [: q1 db task-find drop 0 ;] run -> 0 }T
: expected1 ( -- a n )  s" SELECT [TaskId], [Title], [Status], [Priority], CAST([RowVer] AS bigint) AS [RowVer] FROM [dbo].[Tasks] WHERE [Status] = @p1 AND [Priority] >= @p2 AND [Priority] < @p3 AND [Title] IS NULL ORDER BY [Priority] DESC, [Title] OFFSET 10 ROWS FETCH NEXT 50 ROWS ONLY|@p1 varchar(20), @p2 int, @p3 int" ;
T{ expected1 sent? -> true }T
: q2 ( -- a n )  s\" {\"where\":{\"Status\":{\"in\":[\"open\",\"review\"]},\"Title\":{\"contains\":\"50%_off\"}},\"limit\":5}" ;
T{ [: q2 db task-find drop 0 ;] run -> 0 }T
: expected2 ( -- a n )  s" SELECT [TaskId], [Title], [Status], [Priority], CAST([RowVer] AS bigint) AS [RowVer] FROM [dbo].[Tasks] WHERE [Status] IN (@p1, @p2) AND [Title] LIKE @p3 ESCAPE '\' ORDER BY [TaskId] OFFSET 0 ROWS FETCH NEXT 5 ROWS ONLY|@p1 varchar(20), @p2 varchar(20), @p3 nvarchar(4000)" ;
T{ expected2 sent? -> true }T
testing ... the same query written in Forth (json.query), and counted
T{ [: [: s" open" s" Status" where$=  2 s" >=" s" Priority" where  5 s" <" s" Priority" where  s" Title" where-null
         s" -Priority" sorted-by  s" Title" sorted-by  10 50 page ;] query db task-select drop 0 ;] run -> 0 }T
T{ expected1 sent? -> true }T
T{ [: [: json-null s" !=" s" Status" where  json-true s" exists" s" Status" where ;] query db task-count-of ;] run -> 5 }T
T{ [: s\" {\"where\":{\"Status\":{\"!=\":null,\"exists\":true}}}" db task-count ;] run -> 5 }T
T{ s" SELECT COUNT_BIG(*) AS n FROM [dbo].[Tasks] WHERE [Status] IS NOT NULL AND [Status] IS NOT NULL" sent? -> true }T

testing names checked against the model: a column it hasn't, an operator there is none of - nothing sent
asked length constant before
T{ [: s\" {\"where\":{\"Secret\":1}}" db task-find ;] run  s" no column Secret in dbo.Tasks" failed-with? -> -1 true }T
T{ [: s\" {\"sort\":[\"-Pw\"]}" db task-find ;] run  s" no column Pw" failed-with? -> -1 true }T
T{ [: s\" {\"where\":{\"Title\":{\"like\":\"x\"}}}" db task-find ;] run  s" no operator like" failed-with? -> -1 true }T
T{ [: s\" {\"limit\":\"5; DROP TABLE x\"}" db task-find ;] run  s" limit is a whole number" failed-with? -> -1 true }T
T{ asked length -> before }T

testing insert: the columns given that may be set (not an identity key, not the version); its key back
T{ [: s\" {\"Title\":\"Fix it\",\"Priority\":3,\"TaskId\":1,\"RowVer\":5}" json> db task-insert ;] run -> 99 }T
T{ s" INSERT INTO [dbo].[Tasks] ([Title], [Priority]) VALUES (@p1, @p2); SELECT CAST(SCOPE_IDENTITY() AS bigint) AS id|@p1 nvarchar(200), @p2 int" sent? -> true }T
T{ [: s\" {\"NoteId\":\"n1\",\"Body\":\"hi\"}" json> db note-insert string s" n1" s= ;] run -> true }T
T{ s" INSERT INTO [Notes] ([NoteId], [Body]) VALUES (@p1, @p2)|@p1 varchar(10), @p2 nvarchar(max)" sent? -> true }T

testing update: checked by the version read; saved, or not (someone else's change since)
: read-task ( -- row )  s\" {\"TaskId\":7,\"Title\":\"Old\",\"RowVer\":1234}" json> ;
: changes ( -- m )  s\" {\"Title\":\"New\",\"Status\":\"done\"}" json> ;
T{ [: changes read-task db task-update ;] run -> true }T
T{ s" UPDATE [dbo].[Tasks] SET [Title] = @p1, [Status] = @p2 WHERE [TaskId] = @p3 AND CAST([RowVer] AS bigint) = @p4; SELECT @@ROWCOUNT AS n|@p1 nvarchar(200), @p2 varchar(20), @p3 int, @p4 bigint" sent? -> true }T
0 rowcount-answer !
T{ [: changes read-task db task-update ;] run -> false }T
1 rowcount-answer !
testing update without a version: checked by the old values of the columns it changes (NULL as NULL)
: read-note ( -- row )  s\" {\"NoteId\":\"n1\",\"Body\":null}" json> ;
T{ [: s\" {\"Body\":\"new\"}" json> read-note db note-update ;] run -> true }T
T{ s" UPDATE [Notes] SET [Body] = @p1 WHERE [NoteId] = @p2 AND [Body] IS NULL; SELECT @@ROWCOUNT AS n|@p1 nvarchar(max), @p2 varchar(10)" sent? -> true }T
T{ [: s\" {\"TaskId\":8}" json> read-task db task-update ;] run  s" isn't the key or the version" failed-with? -> -1 true }T
testing delete: checked as an update is
T{ [: read-task db task-delete ;] run -> true }T
T{ s" DELETE FROM [dbo].[Tasks] WHERE [TaskId] = @p1 AND CAST([RowVer] AS bigint) = @p2; SELECT @@ROWCOUNT AS n|@p1 int, @p2 bigint" sent? -> true }T

testing a form: typed by the columns' types; only what differs from the row read (12.5 is 12.50); what is wrong said
variable audited   variable inserted-key
: priority-rule ( changes row errors -- )   \ (the model's own check: 1 to 5)
    {: ch row e :}  s" Priority" ch s-at if  dup 1 6 within 0= if  drop s" 1 to 5" s" Priority" e field-error  else  drop  then  then ;
: audit-update ( changes row db -- )  2drop keys length audited ! ;
: audit-insert ( row key db -- )  drop inserted-key !  drop ;
: audit-delete ( row db -- )  2drop  -1 audited ! ;
model job dbo.Jobs
    key: JobId int identity
    column: Title nvarchar(20)  required
    column: Priority int
    column: Price decimal(10,2)
    column: Done bit
    column: Ratio float
    validate: priority-rule
    on-update: audit-update
    on-insert: audit-insert
    on-delete: audit-delete
end-model
: read-job ( -- row )  s\" {\"JobId\":7,\"Title\":\"Fix it\",\"Priority\":2,\"Price\":\"12.50\",\"Done\":false,\"Ratio\":0.5}" json> ;
: form-of ( text n -- m )  json> ;
: changes-of ( form n -- changes errors )  form-of read-job job model-form ;
: one-form ( -- changes errors )  s\" {\"Title\":\"Fix it\",\"Priority\":\"3\",\"Price\":\"12.5\",\"Done\":\"on\",\"Ratio\":\"0.5\",\"JobId\":\"99\"}" changes-of ;
T{ one-form keys length  swap keys length -> 0 2 }T   \ (Priority and Done changed; Title, Price, Ratio the same; the key not taken)
: one-changes ( -- p d )  one-form drop {: c :}  s" Priority" c s-at drop  s" Done" c s-at drop ;
T{ one-changes -> 3 json-true }T
: bad-form ( -- changes errors )  s\" {\"Title\":\"  \",\"Priority\":\"x\",\"Price\":\"1.234\",\"Ratio\":\"abc\"}" changes-of ;
: error-of ( errors field n -- a n )  rot s-at if  string  else  s" "  then ;
: bad-1 ( -- f f )  bad-form nip {: e :}  e s" Title" error-of s" required" s=  e s" Priority" error-of s" a whole number" s= ;
: bad-2 ( -- f f )  bad-form nip {: e :}  e s" Price" error-of s" at most 2 places after the point" s=  e s" Ratio" error-of s" a number" s= ;
T{ bad-1 -> true true }T
T{ bad-2 -> true true }T
T{ s\" {\"Title\":\"a title much too long for it\"}" changes-of nip s" Title" error-of s" at most 20 characters" s= -> true }T
T{ s\" {\"Price\":\"123456789\"}" changes-of nip s" Price" error-of s" at most 8 digits before the point" s= -> true }T
T{ s\" {\"Priority\":\"9\"}" changes-of nip s" Priority" error-of s" 1 to 5" s= -> true }T   \ (validate:'s)
T{ s\" {\"Priority\":\"\"}" changes-of drop s" Priority" rot s-at drop -> json-null }T   \ (empty: NULL)
testing a form over NULLs and dates: an empty field over NULL, "off" over a NULL bit, the same date written
testing  otherwise - nothing changed; text trimmed; a NOT NULL text column's empty field is ""
model branch dbo.Branches
    key: BranchId int identity
    column: Name nvarchar(50)  not-null
    column: Note nvarchar(200)
    column: Remind date
    column: Since datetime
    column: Exclude bit
end-model
: read-branch ( -- row )
    s\" {\"BranchId\":3,\"Name\":\"North\",\"Note\":null,\"Remind\":\"2026-11-23\",\"Since\":\"2026-01-05T09:30:00.000\",\"Exclude\":null}" json> ;
: branch-form ( form n -- changes errors )  json> read-branch branch model-form ;
: unchanged ( -- n n )
    s\" {\"Name\":\" North \",\"Note\":\"\",\"Remind\":\"2026-11-23\",\"Since\":\"2026-01-05T09:30\",\"Exclude\":\"off\"}"
    branch-form  keys length  swap keys length ;
T{ unchanged -> 0 0 }T
: changed ( -- note name remind exclude )
    s\" {\"Name\":\"\",\"Note\":\"  hi  \",\"Remind\":\"\",\"Exclude\":\"on\"}" branch-form drop {: c :}
    s" Note" c s-at drop string s" hi" s=  s" Name" c s-at drop string nip  s" Remind" c s-at drop  s" Exclude" c s-at drop ;
T{ changed -> true 0 json-null json-true }T   \ (trimmed; "" for the NOT NULL one; an empty date NULL)
T{ s\" {\"Remind\":\"23/11/2026\"}" branch-form nip s" Remind" error-of s" a date (2026-11-23)" s= -> true }T
T{ s\" {\"Remind\":\"2026-11-24\"}" branch-form drop s" Remind" rot s-at drop string s" 2026-11-24" s= -> true }T

testing the hooks: after a save, an insert, a delete - with what was written
T{ 0 audited !  [: one-form drop read-job db job-update ;] run  audited @ -> true 2 }T
T{ [: s\" {\"Title\":\"New\"}" json> db job-insert ;] run  inserted-key @ -> 99 99 }T
T{ [: read-job db job-delete ;] run  audited @ -> true -1 }T
0 rowcount-answer !
T{ 0 audited !  [: one-form drop read-job db job-update ;] run  audited @ -> false 0 }T   \ (refused: no hook)
1 rowcount-answer !

testing values a column's type would change: compared by their own type (2.5 not 2 for an int, a text longer than
testing  the column not cut to match); written: refused, not cut and "saved"
T{ [: s\" {\"where\":{\"Priority\":{\"<\":2.5}}}" db task-count drop 0 ;] run -> 0 }T
T{ sent s" @p1 float" s-find 0>= -> true }T
: long-status ( -- a n )  s\" {\"where\":{\"Status\":\"a status much longer than twenty\"}}" ;
T{ [: long-status db task-count drop 0 ;] run  sent s" @p1 nvarchar(max)" s-find 0>= -> 0 true }T
T{ [: s\" {\"Title\":\"x\",\"Status\":\"a status much longer than twenty\"}" json> db task-insert ;] run
   s" Status longer than its varchar(20)" failed-with? -> -1 true }T
testing null only with = and !=; a limit of 0 sends nothing; a key spelt in another case found
T{ [: s\" {\"where\":{\"Priority\":{\">\":null}}}" db task-find ;] run  s" only = and != take null" failed-with? -> -1 true }T
asked length constant before-limit
T{ [: s\" {\"limit\":0}" db task-find length ;] run  asked length before-limit - -> 0 0 }T
T{ [: s\" {\"noteid\":\"n9\",\"Body\":\"hi\"}" json> db note-insert string s" n9" s= ;] run -> true }T
testing a delete without a version: checked by the old values, as an update is (a row changed since: not deleted)
T{ [: s\" {\"NoteId\":\"n1\",\"Body\":\"as read\"}" json> db note-delete ;] run drop
   s" DELETE FROM [Notes] WHERE [NoteId] = @p1 AND [Body] = @p2; SELECT @@ROWCOUNT AS n|@p1 varchar(10), @p2 nvarchar(max)" sent? -> true }T
testing a form's whole numbers: a sign and digits, in the type's range ($10, too large, 300 for a tinyint: said)
model small Smalls
    key: SmallId int identity
    column: Level tinyint
    column: Count bigint
    column: Name nvarchar(2)
    column: Ratio float
end-model
: small-error ( form n field n -- a n )   \ the form's error for that field ("" none)
    {: fa fn ka kn :}  fa fn json> 0 small model-form nip {: e :}  ka kn e s-at if  string  else  s" "  then ;
T{ s\" {\"Level\":\"300\"}" s" Level" small-error s" from 0 to 255" s= -> true }T
T{ s\" {\"Level\":\"$10\"}" s" Level" small-error s" a whole number" s= -> true }T
T{ s\" {\"Count\":\"99999999999999999999\"}" s" Count" small-error s" too large" s= -> true }T
T{ s\" {\"Level\":\"-7\"}" s" Level" small-error s" from 0 to 255" s=  s\" {\"Level\":\"+7\"}" s" Level" small-error nip -> true 0 }T
T{ s\" {\"Name\":\"\\ud83d\\ude00\"}" s" Name" small-error nip -> 0 }T   \ (one emoji: two UTF-16 units, fits nvarchar(2))
T{ s\" {\"Name\":\"\\ud83d\\ude00\\ud83d\\ude00\"}" s" Name" small-error s" at most 2 characters" s= -> true }T
T{ s\" {\"Ratio\":\"1e400\"}" s" Ratio" small-error s" a number" s= -> true }T

test-summary

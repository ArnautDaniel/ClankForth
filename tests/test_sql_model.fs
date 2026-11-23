\ sql.model (basis/sql/model): a table's words from its declaration - the SQL each sends (its parameters declared as
\ the columns are), queries in jsondb's shape, updates checked by the version or by the old values, names checked
IN: test-sql-model
USING: tester actors net json accessors sql.tds sql sql.model ;
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

test-summary

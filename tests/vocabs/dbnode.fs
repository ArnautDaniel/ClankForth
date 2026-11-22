\ (test_jsondb.fs) run in a node of its own by start-node: a database there, registered as "db"
IN: vocabs.dbnode
USING: actors jsondb ;
s" jsondb-node-data" db-open  s" db" rot register-name

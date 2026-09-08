# Getting Started with Database Branching

Database branching creates isolated, writable views of the versioned tables in
one PostgreSQL database. Creating a branch updates catalog metadata and does not
copy existing tables or indexes. After selecting a branch, a session uses
ordinary SQL for reads and writes.

This guide covers setup and routine use. [CHRONOS_VERSIONING.md](CHRONOS_VERSIONING.md)
describes the interval algorithm and PostgreSQL changes.

## 1. Start PostgreSQL

This feature changes PostgreSQL system catalogs, so initialize a new cluster
with the binaries built from this source tree. A data directory created by
standard PostgreSQL cannot be reused.

Set `CHRONOS_PG_BIN` to the installed binary directory. Choose an empty data
directory and an unused port.

~~~bash
export CHRONOS_PG_BIN=/path/to/chronos-postgresql/bin
export CHRONOS_PG_DATA=/tmp/chronos-pg-data
export CHRONOS_PG_PORT=55490

"$CHRONOS_PG_BIN/initdb" -D "$CHRONOS_PG_DATA"
"$CHRONOS_PG_BIN/pg_ctl" -D "$CHRONOS_PG_DATA" -l "$CHRONOS_PG_DATA/server.log" -o "-p $CHRONOS_PG_PORT -k /tmp" start
"$CHRONOS_PG_BIN/createdb" -h /tmp -p "$CHRONOS_PG_PORT" branch_demo
"$CHRONOS_PG_BIN/psql" -h /tmp -p "$CHRONOS_PG_PORT" -d branch_demo
~~~

Each PostgreSQL database manages its own branches. Creating a branch in
`branch_demo` has no effect on another database in the same cluster.

## 2. Create data on `main`

Branching is enabled in every new database. Permanent and unlogged user tables
receive the required record metadata and internal indexes when they are
created. Temporary tables do not participate in branching.

`pg_branch_enable()` remains available for databases initialized by an older
development build. It returns `false` in a new database because no conversion
is necessary.

Create a table and initial data on the default `main` branch.

~~~sql
SHOW BRANCH;

CREATE TABLE accounts (
    id bigint PRIMARY KEY,
    owner text NOT NULL,
    balance numeric NOT NULL
);

INSERT INTO accounts VALUES
    (1, 'Ada', 100),
    (2, 'Linus', 80);
~~~

## 3. Create and select a branch

Create `experiment` from `main`.

~~~sql
CREATE BRANCH experiment FROM main;
~~~

The branch initially sees the same records as `main`. Select it before
starting a transaction.

~~~sql
SET BRANCH experiment;
SHOW BRANCH;

BEGIN;
UPDATE accounts SET balance = balance + 25 WHERE id = 1;
INSERT INTO accounts VALUES (3, 'Grace', 60);
COMMIT;

SELECT * FROM accounts ORDER BY id;
~~~

The selected session now returns the updated balance and the new record. Switch
back to `main` to verify isolation.

~~~sql
SET BRANCH main;

SELECT * FROM accounts ORDER BY id;
~~~

`main` still returns the original two records. Branch selection belongs to
the session, so two sessions can use different branches concurrently while
addressing the same table names.

PostgreSQL rejects `SET BRANCH` inside an explicit transaction or
subtransaction. Use this order.

~~~sql
SET BRANCH experiment;
BEGIN;
-- application transaction
COMMIT;
~~~

Connection pools should set the intended branch whenever they assign an idle
connection. Set `main` before returning a connection when the next user
should receive the default branch.

## 4. Create nested branches

When `FROM` is omitted, PostgreSQL uses the current branch as the source.

~~~sql
SET BRANCH experiment;
CREATE BRANCH fix_balance;

SET BRANCH fix_balance;
UPDATE accounts SET balance = 0 WHERE id = 2;
~~~

The update affects `fix_balance`. The source branch `experiment` retains
its previous value, and `main` remains unchanged.

For workloads with a known topology, `branch_interval_fanout` can give the
allocator an expected number of children before creation.

~~~sql
SET branch_interval_fanout = 128;
CREATE BRANCH child_001 FROM main;
~~~

The default value `-1` adapts to unknown fanout. This setting influences
coordinate allocation only and does not cap the number of children.

## 5. Change a table schema

`ALTER TABLE` and `CREATE INDEX` on an ordinary, nonpartitioned table are
branch-local. The first such command on a shared table copies the records
visible to the selected branch into a private physical table and then applies
the requested change.

~~~sql
SET BRANCH experiment;

ALTER TABLE accounts
    ADD COLUMN note text NOT NULL DEFAULT '';

UPDATE accounts SET note = 'candidate result' WHERE id = 3;

CREATE INDEX CONCURRENTLY accounts_note_idx ON accounts (note);
~~~

The new column exists on `experiment` and descendants created from it
afterward. It does not appear on `main`. Renaming a column follows the same
branch-local schema-copy path.

~~~sql
SET BRANCH main;

SELECT column_name, data_type
FROM information_schema.columns
WHERE table_schema = 'public'
  AND table_name = 'accounts'
ORDER BY ordinal_position;
~~~

The initial schema change copies the visible heap and builds required indexes,
so its cost grows with table size. Later metadata-only ALTER operations and
index builds on the private table use PostgreSQL's regular paths. Ordinary
secondary indexes from the source are built by a background worker after
commit. A concurrent index build on an existing private table does not make
the index visible to sibling branches. `CREATE BRANCH` reports a retryable
error if it overlaps that build.

`DROP INDEX` is accepted when the selected branch owns a private physical
table. It is rejected on a shared table, where dropping the physical index
would affect another branch; apply a branch-local `ALTER TABLE` first if the
index must be removed. `DROP TABLE` is accepted only for a base physical table
when the selected branch is the database's sole active branch. Other table
drops are rejected instead of deleting another branch's table or exposing an
older physical schema. Renaming a table is likewise accepted only for a base
physical table in the sole active branch, as is moving a table with `SET
SCHEMA`. Relation creation and table identity changes do not yet provide
complete branch-local schema semantics.

## 6. Inspect branches

`SHOW BRANCH` reports the session selection. The following query lists active
branches with their source branch, read point, and writable interval.

~~~sql
SELECT b.oid AS branch_oid,
       b.brname AS branch,
       parent.brname AS parent,
       b.brchildcount AS children,
       s.brseglow AS low,
       s.brsegpoint AS read_point,
       s.brseghigh AS high,
       s.brsegdepth AS depth
FROM pg_catalog.pg_branch AS b
JOIN pg_catalog.pg_branch_segment AS s
  ON s.oid = b.brhead
LEFT JOIN pg_catalog.pg_branch AS parent
  ON parent.oid = b.brparent
WHERE b.brstate = 'a'
ORDER BY b.oid;
~~~

Branch-local schema copies and secondary-index progress appear in
`pg_branch_relversion`.

~~~sql
SELECT rv.brvlogical::regclass AS logical_relation,
       rv.brvphysical::regclass AS physical_relation,
       b.brname AS branch,
       rv.brvlow AS low,
       rv.brvhigh AS high,
       CASE rv.brvindexstate
           WHEN 'p' THEN 'pending'
           WHEN 'r' THEN 'ready'
           WHEN 'f' THEN 'failed'
       END AS secondary_indexes
FROM pg_catalog.pg_branch_relversion AS rv
JOIN pg_catalog.pg_branch AS b
  ON b.oid = rv.brvbranch
ORDER BY rv.oid;
~~~

The catalog may briefly contain a branch whose state is `d`. That state
records deletion work awaiting or undergoing background reclamation.

## 7. Delete branches

A session cannot delete its current branch. Switch to another branch first.
`DROP BRANCH` uses RESTRICT semantics by default and fails when the target has
children. Use `CASCADE` to delete the complete subtree.

~~~sql
SET BRANCH main;
DROP BRANCH experiment RESTRICT;
-- ERROR: branch "experiment" has child branches

DROP BRANCH experiment CASCADE;
~~~

To keep a descendant, delete individual leaf branches first and then delete
their parents without `CASCADE`.

The foreground command removes the branch from use and schedules physical
cleanup. A background worker later deletes versions written by the branch and
drops its private schema copies. The branch name can be reused after the
foreground transaction commits. Normal VACUUM reclaims dead tuple space.

`main` cannot be deleted.

## 8. Permissions and operational behavior

Activating branching and creating or deleting branches requires `CREATE`
privilege on the database. Selecting a branch requires `CONNECT` privilege.
The current implementation has no separate per-branch ACL, so database
privileges define access to every branch.

Branch creation waits for transactions that are writing its source branch and
then commits a metadata change. It does not scan application tables. Reads can
continue during branch creation.

The schema-copy and deletion workers consume dynamic background worker slots.
Configure `max_worker_processes` with room for these workers. A copied table
remains queryable if a secondary-index worker cannot start, although queries
may use sequential scans until those indexes are built.

PostgreSQL publications are not supported because logical decoding would
expose physical interval records. `CREATE PUBLICATION` is rejected.

Sequences are shared across branches. Switching or deleting a branch does not
restore sequence values. This includes serial and identity sequences.

## 9. Command reference

| Command | Behavior |
| --- | --- |
| `SELECT pg_branch_enable()` | Converts a catalog created by an older development build and returns whether conversion occurred; returns `false` for a new database |
| `CREATE BRANCH child FROM source` | Creates a branch from an explicit source |
| `CREATE BRANCH child` | Creates a branch from the session's current branch |
| `SET BRANCH name` | Selects a branch for the session |
| `SHOW BRANCH` | Reports the selected branch |
| `DROP BRANCH name` | Deletes a childless branch |
| `DROP BRANCH name CASCADE` | Deletes a branch and its descendants |

`IF NOT EXISTS` is available for creation, and `IF EXISTS` is available for
deletion.

## 10. Common errors

| Error | Resolution |
| --- | --- |
| `branch cannot be changed inside a transaction block` | Finish the transaction, select the branch, and begin a new transaction |
| `cannot drop the current branch` | Select `main` or another branch before deletion |
| `branch has child branches` | Delete the children first or use `CASCADE` |
| Publications are rejected | Logical replication is not supported in a branching database |
| `could not start background index builder` | Increase `max_worker_processes`; the table remains available without the pending secondary indexes |
| `branch interval is exhausted` | Create a new branch from an ancestor with available interval space and review the fanout hint |

## 11. Stop the example server

~~~bash
"$CHRONOS_PG_BIN/pg_ctl" -D "$CHRONOS_PG_DATA" stop
~~~

For tuple layouts, interval splitting, locking, query rewrite, constraint
enforcement, and schema-copy internals, continue with
[CHRONOS_VERSIONING.md](CHRONOS_VERSIONING.md).

# Chronos Versioning in PostgreSQL 19

This patch implements the Chronos interval-based versioning approach in
PostgreSQL 19. Branch metadata and record updates participate in PostgreSQL
transactions, while the lock manager serializes branch creation with writes
that use the source interval. A branch spans one
`pg_database` and therefore covers every versioned table in that database.
Sessions select a branch and continue to issue ordinary SQL against the original
table names. PostgreSQL resolves the correct physical schema version and applies
record visibility internally.

## 1. Interval versioning

### 1.1 Branch and record representation

Each branch has a writable interval `[l,h)` and a read point `p` within that
interval. The implementation stores interval endpoints in
`pg_branch_coord`, a fixed-width unsigned 128-bit type represented by two
`uint64` words. The type provides decimal and binary I/O, comparison
operators, and a btree operator class. The initial branch covers
`[0, 2^128 - 1)`.

Every logical record has a stable 64-bit identifier. A physical version contains
the application attributes and five engine attributes.

| Attribute | Type | Purpose |
| --- | --- | --- |
| `__pg_branch_rowid` | `int8` | Stable logical record identifier |
| `__pg_branch_low` | `pg_branch_coord` | Inclusive visibility bound |
| `__pg_branch_high` | `pg_branch_coord` | Exclusive visibility bound |
| `__pg_branch_writer` | `oid` | Branch segment that created the version |
| `__pg_branch_deleted` | `bool` | Tombstone flag |

A version is visible when its interval contains the branch read point and it is
not a tombstone.

~~~sql
__pg_branch_low <= p
AND p < __pg_branch_high
AND NOT __pg_branch_deleted
~~~

The visibility intervals of two physical versions of the same logical record
never overlap. This invariant lets a read point select at most one version
without traversing branch ancestry. PostgreSQL first applies its normal MVCC
snapshot to physical tuples, after which the interval predicate selects the
version for the current branch. A transaction therefore observes both a
consistent PostgreSQL snapshot and a consistent branch view.

Activation initially appends the engine attributes to each table. Later schema
changes can add application attributes after them, so executor code discovers
the attributes through `attishidden` and their names rather than fixed
attribute numbers. Schema copying also maps tuple descriptors by name.

### 1.2 Branch creation

Assume a source branch owns `[l,h)`. Creating a child chooses a split point
`m`, assigns `[l,m)` to the child, and advances the source branch to
`[m,h)`. Both branches use the midpoint of their new writable interval as the
read point.

~~~text
old source interval [l,h)
          |
          +---- child [l,m)
          |
          +---- source continuation [m,h)
~~~

The old segment becomes immutable metadata. Existing records remain unchanged
because any version that covered the old read point also covers the new child
and source points. A fork updates `pg_branch` and
`pg_branch_segment` without scanning application tables or rebuilding
indexes.

Writers hold a `RowExclusiveLock` on the selected branch. A fork takes
`ShareRowExclusiveLock` on its source branch, waits for active writers, and
then reads the current head again before splitting it. Readers do not hold a
branch lock. A reader that started before the fork can finish with its existing
MVCC snapshot and read point, while catalog invalidation refreshes later
statements.

The `branch_interval_fanout` GUC supplies an expected number of children.
The default value, `-1`, uses the adaptive allocator from Chronos. A value of
one reserves approximately 1/256 of the remaining interval for continuation
and gives the rest to the child, which supports deep branch spines. A larger
known fanout derives a stable child width from the branch's original interval
and reserves eight additional slots. The unknown-fanout policy retains a
`2^56` continuation range on the first split and then uses a harmonic
allocation for later children. If the planned allocation no longer fits, the
allocator splits the remaining space in half. Exhaustion rejects the fork
without relabeling stored intervals.

### 1.3 Record modifications

An INSERT obtains a new stable identifier from
`pg_catalog.pg_branch_rowid_seq` and tags the tuple with the current writable
interval and segment OID. The executor restores these values after a
`BEFORE` trigger so user code cannot replace engine metadata.

An UPDATE or DELETE may target a version inherited from an ancestor. Let the
visible version cover `[a,b)` and let the current branch write within
`[l,h)`. The executor preserves the parts outside the current branch.

~~~text
[a,l)  old value, when a < l
[l,h)  updated value or tombstone
[h,b)  old value, when h < b
~~~

For UPDATE, PostgreSQL's heap update creates the new version over `[l,h)`.
The executor inserts the remaining old-value fragments through the table access
method. DELETE performs the heap delete, inserts the remaining fragments, and
adds a tombstone over `[l,h)`. The tombstone causes future descendants to
inherit the deletion. `TRUNCATE` applies this delete procedure to records
visible in the current branch instead of replacing the relation file.

History fragments keep the stable record identifier and original writer.
`EIIT_BRANCH_HISTORY` suppresses repeated unique and exclusion checks for
these fragments because their application values were already valid. User
triggers and transition tables observe one logical UPDATE or DELETE rather than
the internal fragments.

### 1.4 Concurrent writes

Physical interval fragments have independent CTIDs and may not belong to one
HOT chain. Two sibling branches can therefore reach the same inherited version
through different physical update paths. The implementation serializes them
with a lock derived from the logical relation OID and stable record identifier.

`ExecutorStart` installs a relation intent lock before a modifying plan runs.
Point operations use compatible `RowExclusiveLock` intent locks and acquire
an exclusive logical-record lock for each modified record. A plan estimated to
modify at least 1,024 records takes one `ShareRowExclusiveLock` on the
logical relation and skips individual record locks.

EvalPlanQual, row locking, triggers, MERGE, and
`INSERT ... ON CONFLICT` can encounter a stale CTID after a concurrent split.
These paths call `BranchFindVisibleTuple()` or the corresponding tuple
resolution helper to relocate the tuple by stable record identifier. The normal
PostgreSQL isolation level determines whether execution retries or reports a
serialization failure.

### 1.5 Private physical versions

A schema copy belongs to one branch until that branch creates a child. The
executor can use ordinary PostgreSQL UPDATE and DELETE for such a table when
the `pg_branch_relversion` owner and bounds exactly match the current branch.
It holds the branch write lock while checking this condition, so a concurrent
fork cannot begin until the transaction ends.

The result is cached by physical relation and local transaction ID.
`fireRIRrules()` omits interval predicates on private UPDATE, DELETE, and
MERGE targets. Creating a child changes the parent's bounds and invalidates the
cache. Both parent and child then return to interval splitting on the shared
physical version. The original logical heap always uses the conservative shared
path.

## 2. PostgreSQL integration

### 2.1 Database initialization and catalogs

`initdb` enables branching in every new database. Permanent and unlogged user
tables receive five hidden attributes and two internal btree indexes during
`CREATE TABLE`.

~~~text
(__pg_branch_rowid, __pg_branch_low)     tuple relocation
(__pg_branch_writer, __pg_branch_rowid)  branch deletion
~~~

`pg_branch_enable()` remains as an idempotent conversion function for catalogs
created by older development builds. Conversion orders existing tables by OID,
takes `AccessExclusiveLock` on them, and adds the same attributes and indexes.
The locks remain until commit, so another session observes either the original
database or the fully converted database. Temporary tables remain
unversioned.

The patch stores branch state in three database-local, WAL-logged catalogs.

| Catalog | Stored state |
| --- | --- |
| `pg_branch` | Name, parent, current segment, owner, child count, fanout metadata, and deletion state |
| `pg_branch_segment` | Segment ancestry, interval, read point, depth, child count, and mutable or retired state |
| `pg_branch_relversion` | Logical and physical relation OIDs, source relation, owning branch, interval, and secondary-index state |

`initdb` creates `main` with OID 9100, a retired root segment with OID 9101,
and a mutable full-range segment with OID 9102. The compatibility conversion
replaces an older catalog's mutable root with a segment over the same interval,
which records that the database has been converted without consuming interval
space.

### 2.2 Relation lookup and query rewrite

PostgreSQL keeps the original table OID as the logical relation identifier. A
schema copy receives another physical OID and a
`pg_branch_relversion` binding over the branch interval. Bindings can overlap
after nested schema changes. `BranchResolveRelationOid()` chooses the newest
binding whose interval contains the current read point.

`RangeVarGetRelidExtended()` calls this resolver after ordinary namespace
lookup. The parser and utility commands can therefore continue to use the
logical table name while operating on the selected physical relation.

During rule rewriting, `fireRIRrules()` adds the interval and tombstone
predicates to every versioned relation RTE. It combines them with row-level
security and applies them independently to each scan in joins, subqueries,
views, inheritance, and partition plans. The read point appears as a constant,
so the normal optimizer can choose sequential, index, bitmap, and parallel
plans.

A backend caches the branch OID, head segment, read point, and logical-to-
physical relation mappings. Stable reads do not query the branch catalogs.
Branch creation, deletion, schema copying, and branch selection invalidate the
relevant caches and reset cached plans. `ExecutorStart` refreshes the branch
before using a prepared modifying plan.

### 2.3 Branch write guard

The first modifying statement in a transaction acquires the branch
`RowExclusiveLock` and rereads the current head. The backend records the lock
under its local transaction ID, so later DML in the same transaction does not
repeat the branch metadata lookup or lock acquisition. Forks conflict with
this transaction-level guard and cannot assign part of its writable interval to
a child before the transaction completes.

`SET BRANCH` changes the session GUC and resets the plan cache. PostgreSQL
rejects a branch change inside an explicit transaction or subtransaction,
which prevents one transaction from using multiple read points.

### 2.4 Indexes and constraints

One physical user index can contain versions from several disjoint branch
intervals. The btree insertion path uses a branch-aware table tuple probe, so an
equal key conflicts only when the corresponding heap tuple is visible at the
current read point. Unique index construction first builds the physical index
with duplicate keys permitted, then scans it once per active branch point to
validate SQL uniqueness. Reindex performs the same validation.

Exclusion checks filter the candidate and conflicting tuple at the same branch
point. Index construction validates exclusion constraints once per active
point. Foreign-key checks use PostgreSQL's SPI path in branching databases
because the direct RI index probe bypasses query rewrite.

### 2.5 Branch-local relation DDL

The implementation uses eager physical copy on the first `ALTER TABLE` or
`CREATE INDEX` against a shared relation. The DDL preparation path takes
`ShareRowExclusiveLock` on the branch and `AccessShareLock` on the source
table. Readers continue to use the source while the copy runs.

The copy starts with an index-free heap created by `CREATE TABLE ... LIKE`.
It includes column defaults, generated and identity definitions, constraints,
storage settings, and compression, and sets `fillfactor=50`. A table scan
selects records visible at the branch read point and inserts them into the new
heap in batches of 1,000 with `TABLE_INSERT_SKIP_FSM`. Attribute mapping by
name handles schema versions with different physical column orders. The copy
preserves `__pg_branch_rowid` and assigns the current branch bounds and
writer to each copied tuple.

Before installing the relation binding, PostgreSQL builds the two internal
indexes and all primary, unique, and exclusion indexes. The binding then makes
the new table the target of the original DDL. PostgreSQL takes the requested
table or index-build lock on this private target rather than the shared source.

On a newly copied, unpublished target, `CREATE INDEX CONCURRENTLY` uses a
regular bulk build: no other session can reach the target, and adding internal
transactions would provide no concurrency. On an existing private target it
retains PostgreSQL's multi-transaction execution and holds a session-level
database DDL gate until validation completes. A concurrent `CREATE BRANCH`
fails with a retryable error instead of sharing a half-built index or forming a
lock cycle with PostgreSQL's older-snapshot wait.

After commit, a dynamic background worker copies ordinary secondary indexes
with a regular PostgreSQL index build. A later ALTER waits for pending indexes
on its source and completes them synchronously before starting another copy.
Subsequent metadata-only ALTER operations on an exact private version retain
PostgreSQL's normal fast DDL path. The first ALTER on a shared table includes
the heap copy and required index builds.

### 2.6 Branch deletion

`DROP BRANCH` changes only metadata in the foreground. It marks the branch as
dropping, renames it to `__pg_branch_gc_<oid>` so the original name can be
reused, and updates its parent. `CASCADE` applies the same transition to
descendants.

A background worker collects the branch's segment OIDs and deletes versions
through the `(__pg_branch_writer, __pg_branch_rowid)` index. It falls back to
a heap scan when the index is unavailable, drops physical schema versions owned
by the branch, and removes the catalog records. A dropping
`pg_branch` tuple serves as the durable work item. A later database session
resubmits unfinished deletion work after a crash or worker registration
failure. VACUUM reclaims the resulting dead tuples.

## 3. Source changes

The implementation uses existing PostgreSQL interfaces at relation lookup,
rewrite, executor, table access, and index enforcement boundaries.

| Component | Files and changes |
| --- | --- |
| Core branch code | [branchcmds.c](src/backend/commands/branchcmds.c) and [branchcmds.h](src/include/commands/branchcmds.h) implement catalog conversion, allocation, locking, interval writes, schema copies, and deletion |
| Coordinate type | [branchcoord.c](src/backend/utils/adt/branchcoord.c), [branchcoord.h](src/include/utils/branchcoord.h), and the type, procedure, operator, and opclass catalog data add `pg_branch_coord` |
| Catalogs | [pg_branch.h](src/include/catalog/pg_branch.h), [pg_branch_segment.h](src/include/catalog/pg_branch_segment.h), [pg_branch_relversion.h](src/include/catalog/pg_branch_relversion.h), their data files, `Catalog.pm`, and bootstrap code add branch metadata |
| SQL interface | `gram.y`, `parsenodes.h`, `kwlist.h`, `cmdtaglist.h`, and `utility.c` add CREATE and DROP BRANCH and prepare ALTER TABLE and CREATE INDEX |
| Session state | `guc_parameters.dat` and `guc_tables.c` define `branch` and `branch_interval_fanout` |
| Relation reads | `namespace.c` resolves physical schema versions, while `rewriteHandler.c` adds visibility predicates |
| Record writes | `execMain.c`, `nodeModifyTable.c`, `nodeLockRows.c`, and `trigger.c` add branch locks, interval splitting, private DML, and tuple relocation |
| Table operations | `tableam.c`, `copy*.c`, `createas.c`, `tablecmds.c`, and `parse_utilcmd.c` cover branch-aware probes, COPY, CTAS, TRUNCATE, and new tables |
| Constraints | `nbtinsert.c`, `nbtsort.c`, `index.c`, `execIndexing.c`, and `ri_triggers.c` enforce unique, exclusion, and foreign-key semantics |
| Hidden attributes | `pg_attribute.h`, `htup_details.h`, parser and tuple descriptor code, row and JSON conversion, `information_schema.sql`, and `system_views.sql` hide engine metadata, including its planner statistics |
| Client tools | `psql/describe.c` and `pg_dump.c` omit hidden attributes and internal support indexes from normal output |
| Workers | `bgworker.c` exposes the in-core entry points used for secondary index construction and branch deletion |

The patch retains PostgreSQL heap pages, WAL records, buffer management, TOAST,
VACUUM, and the standard table access method. It raises the physical heap
attribute limit from 1,600 to 1,605 while keeping the SQL-visible limit at
1,600.

## 4. Tutorial

### 4.1 Start a server

The catalog changes require a cluster initialized by this build. Set
`CHRONOS_PG_BIN` to the binary directory of the patched PostgreSQL installation.

~~~bash
export CHRONOS_PG_BIN=/path/to/chronos-postgresql/bin
export CHRONOS_PG_DATA=/tmp/chronos-pg-demo-data
export CHRONOS_PG_PORT=55490

"$CHRONOS_PG_BIN/initdb" -D "$CHRONOS_PG_DATA"
"$CHRONOS_PG_BIN/pg_ctl" -D "$CHRONOS_PG_DATA" -l "$CHRONOS_PG_DATA/server.log" -o "-p $CHRONOS_PG_PORT -k /tmp" start
"$CHRONOS_PG_BIN/createdb" -h /tmp -p "$CHRONOS_PG_PORT" chronos_demo
"$CHRONOS_PG_BIN/psql" -h /tmp -p "$CHRONOS_PG_PORT" -d chronos_demo
~~~

### 4.2 Create and use a branch

Create the table on `main`.

~~~sql
CREATE TABLE accounts (
    id bigint PRIMARY KEY,
    balance numeric NOT NULL,
    status text NOT NULL DEFAULT 'open'
);

INSERT INTO accounts VALUES (1, 100, 'open');

SHOW BRANCH;
~~~

Create a branch and modify it with ordinary DML. In a new database,
`pg_branch_enable()` returns `false` because branching is already enabled.

~~~sql
CREATE BRANCH dev FROM main;
SET BRANCH dev;

UPDATE accounts SET balance = 125 WHERE id = 1;
INSERT INTO accounts VALUES (2, 50, 'open');

SELECT * FROM accounts ORDER BY id;
-- 1 | 125 | open
-- 2 |  50 | open

SET BRANCH main;
SELECT * FROM accounts ORDER BY id;
-- 1 | 100 | open
~~~

A nested fork inherits the selected branch.

~~~sql
SET BRANCH dev;
CREATE BRANCH feature;
SET BRANCH feature;

DELETE FROM accounts WHERE id = 1;
UPDATE accounts SET status = 'review' WHERE id = 2;

SET BRANCH dev;
SELECT * FROM accounts ORDER BY id;
-- dev retains both records and the original status
~~~

### 4.3 Apply branch-local DDL

The first ALTER copies the records visible to `dev` and runs the ALTER on the
new table.

~~~sql
SET BRANCH dev;

ALTER TABLE accounts
    ADD COLUMN note text NOT NULL DEFAULT '';

UPDATE accounts SET note = 'investigate' WHERE id = 2;

SELECT column_name, data_type
FROM information_schema.columns
WHERE table_schema = 'public'
  AND table_name = 'accounts'
ORDER BY ordinal_position;
~~~

The query includes `note` on `dev`. After `SET BRANCH main`, the same
`information_schema` query returns the original three columns.

### 4.4 Inspect branch metadata

This query reports each branch's current interval and read point.

~~~sql
SELECT b.oid AS branch_oid,
       b.brname AS branch,
       parent.brname AS parent,
       CASE b.brstate
           WHEN 'a' THEN 'active'
           WHEN 'd' THEN 'dropping'
       END AS state,
       b.brchildcount AS children,
       s.oid AS head_segment_oid,
       s.brseglow AS low,
       s.brsegpoint AS read_point,
       s.brseghigh AS high,
       s.brsegdepth AS depth
FROM pg_catalog.pg_branch AS b
JOIN pg_catalog.pg_branch_segment AS s
  ON s.oid = b.brhead
LEFT JOIN pg_catalog.pg_branch AS parent
  ON parent.oid = b.brparent
ORDER BY b.oid;
~~~

Schema copies appear in `pg_branch_relversion`.

~~~sql
SELECT rv.oid AS version_oid,
       rv.brvlogical::regclass AS logical_relation,
       rv.brvphysical::regclass AS physical_relation,
       rv.brvsource::regclass AS source_relation,
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

The complete allocation history remains in `pg_branch_segment`. Mutable
segments use `brsegkind='m'` and retired segments use `'r'`. Engine
attributes can be inspected through `pg_attribute`.

~~~sql
SELECT attnum, attname, atttypid::regtype
FROM pg_catalog.pg_attribute
WHERE attrelid = 'public.accounts'::regclass
  AND attishidden
  AND NOT attisdropped
ORDER BY attnum;
~~~

`pg_branch_logical_relation(regclass)` maps a physical schema version back to
the stable logical OID. `pg_branch_relation_is_current(regclass)` reports
whether a physical relation serves the current branch.

### 4.5 Delete a branch

A session cannot delete its current branch or `main`. Switch to `main`
before deleting the example.

~~~sql
SET BRANCH main;
DROP BRANCH dev RESTRICT;  -- fails while feature exists
DROP BRANCH dev CASCADE;
~~~

Activation, branch creation, and branch deletion require `CREATE` privilege
on the database. Branch selection requires `CONNECT`. Select the branch
before `BEGIN` because `SET BRANCH` is rejected inside an explicit
transaction. Connection pools should restore `branch` before reusing a
session.

Stop the example server with the following command.

~~~bash
"$CHRONOS_PG_BIN/pg_ctl" -D "$CHRONOS_PG_DATA" stop
~~~

## 5. Current limitations

### 5.1 Schema and database records

Branch-local schema copying currently covers `ALTER TABLE`, column rename, and
`CREATE INDEX` on ordinary, nonpartitioned heap tables. `DROP INDEX` works on a
private physical version; it rejects shared or sibling physical indexes
instead of changing another branch. `DROP TABLE` and table rename are limited
to a base physical table in the sole active branch; unsafe operations fail
before PostgreSQL changes the shared catalog. Relation creation and table
identity still use shared PostgreSQL catalogs. Partition topology, schemas,
views, rules,
triggers, foreign-key dependency closure, ownership, ACLs, comments, and
extension metadata do not yet receive complete per-branch copies. `CREATE
TABLE ... LIKE` also omits several dependent records, so a DDL operation that
creates a physical table does not reproduce every trigger, rule, ACL, or
dependency of the source.

Sequences remain global, including serial and identity sequences. Temporary
tables, large objects, materialized views, and foreign tables are outside the
versioning path. The implementation does not provide merge, historical
`AS OF` queries, or branch rename.

### 5.2 Planning, indexing, and storage

Rewrite-time visibility predicates appear in `EXPLAIN`, and ANALYZE collects
statistics over physical versions rather than individual branch views. The
optimizer has no branch-specific selectivity model. User indexes also retain
entries for versions from several branches, which increases index size as
written intervals accumulate.

A copied table uses `fillfactor=50`. The setting leaves room for branch-local
updates but consumes more pages for read-mostly copies. Secondary indexes use a
regular background index build that can block writers on the copied table.
Worker registration depends on `max_worker_processes`, and secondary index
recovery has no durable retry queue.

The unique and exclusion changes cover PostgreSQL's core implementations.
Concurrent btree index creation is covered by branch regression and concurrency
tests. Deferred-constraint corner cases, extension index access methods, and
table access methods that bypass the modified tuple probes need additional
validation.

### 5.3 Deletion, backup, and replication

Branch deletion records unfinished work in `pg_branch`, but PostgreSQL has no
progress view or error history for the worker. Deleted tuples require VACUUM,
and the worker does not coalesce adjacent surviving intervals. Repeated
fork-write-delete workloads can therefore accumulate physical fragmentation.

`pg_dump` hides engine attributes and support indexes from logical table
definitions, but it does not serialize and restore the branch graph as a
branch-aware backup. The catalog changes also require a fresh `initdb`;
`pg_upgrade` from standard PostgreSQL is not implemented.

The patch rejects publications because logical decoding would expose physical
interval operations. Physical WAL recovery uses the normal PostgreSQL path,
although streaming replication, failover, and point-in-time recovery still
need dedicated qualification.

### 5.4 Allocation and access control

The 128-bit allocator never relabels existing records. A sufficiently long or
poorly estimated topology can exhaust an interval and cause the next fork to
fail. `branch_interval_fanout` is a session hint rather than stored topology
policy. The catalogs contain a terminal-branch kind, but SQL cannot set it.

`pg_branch.browner` records the creator without defining a separate branch
ACL. Any role with database `CONNECT` can select a branch, and database
`CREATE` controls branch creation, deletion, and legacy catalog conversion.
Hidden attributes
protect the SQL interface but do not form a security boundary against
superusers or direct catalog inspection.

## 6. Tests

[branching.sql](src/test/regress/sql/branching.sql) covers interval reads and
writes, nested and high-fanout creation, default initialization, schema
copying, private DML, COPY, CTAS, TRUNCATE, MERGE, constraints, and hidden attributes. The
isolation suite exercises concurrent creation, deletion, private DML, and
logical-record locks in [src/test/isolation/specs](src/test/isolation/specs).

Run the core regression and isolation suites from this checkout with the
following command.

~~~bash
meson test -C ../postgres-build \
  --suite postgresql:regress \
  --suite postgresql:isolation \
  --print-errorlogs
~~~

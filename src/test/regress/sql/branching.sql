-- Native database branching must activate and remain isolated from the
-- ordinary regression database.  Use a dedicated database because the first
-- branch upgrades every permanent user table in its database atomically.
CREATE DATABASE regression_branching;
\connect regression_branching

CREATE TABLE accounts
(
    id integer PRIMARY KEY,
    email text UNIQUE,
    balance integer
);
INSERT INTO accounts VALUES
    (1, 'one@example.test', 10),
    (2, 'two@example.test', 20);
SELECT (ROW(3, 'three@example.test', 30)::accounts).email
       AS record_cast_hides_metadata;
CREATE FUNCTION make_account(integer, text, integer) RETURNS accounts
LANGUAGE SQL IMMUTABLE
AS 'SELECT $1, $2, $3';
SELECT (make_account(4, 'four@example.test', 40)).email
       AS function_result_hides_metadata;
DROP FUNCTION make_account(integer, text, integer);
CREATE FUNCTION make_accounts_plpgsql() RETURNS SETOF accounts
LANGUAGE plpgsql AS $$
BEGIN
    RETURN QUERY SELECT 5, 'five@example.test'::text, 50;
END
$$;
SELECT email AS plpgsql_result_hides_metadata
FROM make_accounts_plpgsql();
DROP FUNCTION make_accounts_plpgsql();
CREATE FUNCTION assign_account_plpgsql() RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    account accounts;
BEGIN
    SELECT 6, 'six@example.test'::text, 60 INTO account;
    RETURN account.email;
END
$$;
SELECT assign_account_plpgsql() AS plpgsql_assignment_hides_metadata;
DROP FUNCTION assign_account_plpgsql();

-- Internal indexes must not make ordinary cross-schema table moves collide.
CREATE SCHEMA branch_move_a;
CREATE SCHEMA branch_move_b;
CREATE TABLE branch_move_a.same_name (id integer);
CREATE TABLE branch_move_b.same_name (id integer);
ALTER TABLE branch_move_a.same_name RENAME TO moved_table;
ALTER TABLE branch_move_a.moved_table SET SCHEMA branch_move_b;
SELECT count(*) AS moved_table_count
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'branch_move_b'
  AND c.relname IN ('same_name', 'moved_table')
  AND c.relkind = 'r';
DROP SCHEMA branch_move_a CASCADE;
DROP SCHEMA branch_move_b CASCADE;

CREATE TABLE branch_rule_source (id integer, value text);
CREATE TABLE branch_rule_target (id integer, value text);
CREATE RULE branch_rule_insert AS ON INSERT TO branch_rule_source
DO INSTEAD
    INSERT INTO branch_rule_target VALUES (NEW.id, NEW.value)
    RETURNING id, value;
INSERT INTO branch_rule_source VALUES (1, 'through-rule') RETURNING value;
SELECT * FROM branch_rule_target;
DROP TABLE branch_rule_source, branch_rule_target;

CREATE TABLE branch_like_source (id integer, value text);
CREATE INDEX branch_like_value_idx ON branch_like_source (value);
CREATE TABLE branch_like_copy (LIKE branch_like_source INCLUDING ALL);
SELECT count(*) FILTER (WHERE NOT a1.attishidden) AS copied_user_indexes,
       count(*) FILTER (WHERE a1.attishidden) AS storage_indexes
FROM pg_index i
JOIN pg_attribute a1 ON a1.attrelid = i.indrelid
                     AND a1.attnum = i.indkey[0]
WHERE i.indrelid = 'branch_like_copy'::regclass;
DROP TABLE branch_like_source, branch_like_copy;

CREATE TABLE branch_error_detail (id integer CHECK (id > 0));
INSERT INTO branch_error_detail VALUES (-1);
DROP TABLE branch_error_detail;

CREATE TABLE branch_private_truncate (id integer);
INSERT INTO branch_private_truncate VALUES (1);
TRUNCATE branch_private_truncate;
ALTER TABLE branch_private_truncate
    ADD CONSTRAINT branch_private_truncate_negative CHECK (id < 0);
INSERT INTO branch_private_truncate VALUES (-1);
SELECT * FROM branch_private_truncate;
DROP TABLE branch_private_truncate;

CREATE TABLE branch_graph_vertex (id integer PRIMARY KEY, label text);
CREATE PROPERTY GRAPH branch_property_graph
    VERTEX TABLES
    (branch_graph_vertex KEY (id) PROPERTIES ALL COLUMNS);
DROP PROPERTY GRAPH branch_property_graph;
DROP TABLE branch_graph_vertex;

CREATE TABLE branch_parallel_source AS
SELECT g AS id FROM generate_series(1, 100) AS g;
SET debug_parallel_query = on;
CREATE TABLE branch_parallel_copy AS
SELECT * FROM branch_parallel_source;
RESET debug_parallel_query;
SELECT count(*) AS parallel_copy_rows FROM branch_parallel_copy;
DROP TABLE branch_parallel_source, branch_parallel_copy;

CREATE ROLE branch_column_reader;
CREATE TABLE branch_acl_row (id integer, value text);
INSERT INTO branch_acl_row VALUES (1, 'visible');
GRANT SELECT (id, value) ON branch_acl_row TO branch_column_reader;
SET ROLE branch_column_reader;
SELECT branch_acl_row FROM branch_acl_row;
RESET ROLE;
DROP TABLE branch_acl_row;
DROP ROLE branch_column_reader;

CREATE TYPE branch_typed_row AS (id integer, value text);
CREATE TABLE branch_typed_table (id integer, value text);
ALTER TABLE branch_typed_table OF branch_typed_row;
ALTER TABLE branch_typed_table NOT OF;
DROP TABLE branch_typed_table;
DROP TYPE branch_typed_row;

ALTER TABLE IF EXISTS branch_missing_table ADD COLUMN value integer;

CREATE TABLE branch_partitioned (id integer) PARTITION BY RANGE (id);
SET default_tablespace TO pg_global;
CREATE TABLE branch_partitioned_p0 PARTITION OF branch_partitioned
  FOR VALUES FROM (0) TO (10) TABLESPACE pg_default;
RESET default_tablespace;
SELECT count(*) AS partitioned_root_storage_indexes
FROM pg_index WHERE indrelid = 'branch_partitioned'::regclass;
SELECT count(*) AS partition_leaf_storage_indexes
FROM pg_index WHERE indrelid = 'branch_partitioned_p0'::regclass;
DROP TABLE branch_partitioned;

BEGIN;
SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;
CREATE TABLE branch_serial_cursor (value integer);
INSERT INTO branch_serial_cursor VALUES (1);
DECLARE branch_cursor NO SCROLL CURSOR FOR
  SELECT * FROM branch_serial_cursor FOR UPDATE;
UPDATE branch_serial_cursor SET value = 2;
FETCH ALL FROM branch_cursor;
COMMIT;
DROP TABLE branch_serial_cursor;

SET datestyle TO ISO, YMD;
CREATE TABLE branch_temporal_split
(
  id integer,
  valid_at daterange,
  value text
);
INSERT INTO branch_temporal_split
VALUES (1, '[2020-01-01,2030-01-01)', 'before');
UPDATE branch_temporal_split
  FOR PORTION OF valid_at FROM '2022-01-01' TO '2024-01-01'
  SET value = 'during';
SELECT valid_at, value FROM branch_temporal_split
ORDER BY valid_at FOR UPDATE;
DROP TABLE branch_temporal_split;
RESET datestyle;

CREATE TABLE parent_fk (id integer PRIMARY KEY);
CREATE TABLE child_fk
(
    id integer PRIMARY KEY,
    parent_id integer REFERENCES parent_fk
);
INSERT INTO parent_fk VALUES (1);
INSERT INTO child_fk VALUES (1, 1);

CREATE TABLE cascade_parent (id integer PRIMARY KEY);
CREATE TABLE cascade_child
(
    id integer PRIMARY KEY,
    parent_id integer REFERENCES cascade_parent ON DELETE CASCADE
);
INSERT INTO cascade_parent VALUES (1);
INSERT INTO cascade_child VALUES (1, 1);

CREATE TABLE conflict_target (id integer PRIMARY KEY, value text);
INSERT INTO conflict_target VALUES (1, 'main');
CREATE TABLE late_unique (id integer, value text);
INSERT INTO late_unique VALUES (1, 'shared');
CREATE TABLE invalid_unique (id integer, value text);
INSERT INTO invalid_unique VALUES (1, 'duplicate'), (1, 'duplicate');
CREATE TABLE exclusion_target (period int4range);
CREATE TABLE invalid_exclusion (period int4range);
CREATE TABLE schema_copy_target
(
    id integer PRIMARY KEY,
    value text
);
CREATE INDEX schema_copy_target_value_idx ON schema_copy_target (value);
INSERT INTO schema_copy_target VALUES (1, 'one'), (2, 'two');
CREATE VIEW account_view AS SELECT id, email, balance FROM accounts;
CREATE VIEW account_join_view AS
    SELECT a.id, a.email
    FROM accounts a JOIN parent_fk p USING (id);
SELECT pg_get_viewdef('account_join_view'::regclass, true)
       !~ '__pg_branch_' AS view_hides_metadata;
CREATE TABLE generated_target
(
    id integer PRIMARY KEY,
    base integer,
    doubled integer GENERATED ALWAYS AS (base * 2) STORED
);
INSERT INTO generated_target (id, base) VALUES (1, 5);
CREATE TABLE deferred_target
(
    id integer UNIQUE DEFERRABLE INITIALLY DEFERRED,
    value text
);
CREATE TABLE merge_target (id integer PRIMARY KEY, value text);
INSERT INTO merge_target VALUES (1, 'shared');
CREATE TABLE rule_target (id integer PRIMARY KEY, value text);
CREATE TABLE rule_log (id integer, value text);
INSERT INTO rule_target VALUES (1, 'shared');
CREATE RULE rule_target_update AS ON UPDATE TO rule_target
    DO ALSO INSERT INTO rule_log VALUES (NEW.id, NEW.value);

CREATE TABLE trigger_target (id integer PRIMARY KEY, value integer);
CREATE TABLE trigger_log
(
    phase text,
    operation text,
    old_value integer,
    new_value integer,
    row_count bigint
);
INSERT INTO trigger_target VALUES (1, 10), (2, 20);
CREATE TABLE truncate_target (id integer PRIMARY KEY);
INSERT INTO truncate_target VALUES (1), (2);
CREATE TABLE copy_target (id integer PRIMARY KEY, value text);
INSERT INTO copy_target VALUES (1, 'main');
CREATE UNLOGGED TABLE unlogged_target (id integer PRIMARY KEY, value text);
INSERT INTO unlogged_target VALUES (1, 'main');
CREATE TABLE bulk_update_target (id integer PRIMARY KEY, value integer);
INSERT INTO bulk_update_target
SELECT i, 0 FROM generate_series(1, 10000) AS i;
CREATE FUNCTION forge_branch_insert_metadata() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    NEW.__pg_branch_rowid := 0;
    NEW.__pg_branch_deleted := true;
    RETURN NEW;
END
$$;
CREATE TRIGGER copy_target_forge_metadata
BEFORE INSERT ON copy_target
FOR EACH ROW EXECUTE FUNCTION forge_branch_insert_metadata();

CREATE FUNCTION branch_row_trigger() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' AND TG_WHEN = 'BEFORE' THEN
        NEW.value := NEW.value + 1;
    END IF;
    INSERT INTO trigger_log
    VALUES (TG_WHEN, TG_OP,
            CASE WHEN TG_OP <> 'INSERT' THEN OLD.value END,
            CASE WHEN TG_OP <> 'DELETE' THEN NEW.value END,
            NULL);
    IF TG_OP = 'DELETE' THEN
        RETURN OLD;
    END IF;
    RETURN NEW;
END
$$;

CREATE FUNCTION branch_update_statement_trigger() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO trigger_log
    SELECT TG_WHEN, TG_OP, NULL, NULL, count(*) FROM new_rows;
    RETURN NULL;
END
$$;

CREATE FUNCTION branch_delete_statement_trigger() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO trigger_log
    SELECT TG_WHEN, TG_OP, NULL, NULL, count(*) FROM old_rows;
    RETURN NULL;
END
$$;

CREATE TRIGGER trigger_target_bu
BEFORE UPDATE ON trigger_target
FOR EACH ROW EXECUTE FUNCTION branch_row_trigger();
CREATE TRIGGER trigger_target_au
AFTER UPDATE ON trigger_target
FOR EACH ROW EXECUTE FUNCTION branch_row_trigger();
CREATE TRIGGER trigger_target_bd
BEFORE DELETE ON trigger_target
FOR EACH ROW EXECUTE FUNCTION branch_row_trigger();
CREATE TRIGGER trigger_target_ad
AFTER DELETE ON trigger_target
FOR EACH ROW EXECUTE FUNCTION branch_row_trigger();
CREATE TRIGGER trigger_target_su
AFTER UPDATE ON trigger_target
REFERENCING NEW TABLE AS new_rows
FOR EACH STATEMENT EXECUTE FUNCTION branch_update_statement_trigger();
CREATE TRIGGER trigger_target_sd
AFTER DELETE ON trigger_target
REFERENCING OLD TABLE AS old_rows
FOR EACH STATEMENT EXECUTE FUNCTION branch_delete_statement_trigger();

CREATE TABLE inherited_parent (id integer, value text);
CREATE TABLE inherited_child (extra integer) INHERITS (inherited_parent);
INSERT INTO inherited_parent VALUES (1, 'parent');
INSERT INTO inherited_child VALUES (2, 'child', 9);

CREATE TABLE measurements (id integer, value text) PARTITION BY RANGE (id);
CREATE TABLE measurements_low PARTITION OF measurements
    FOR VALUES FROM (0) TO (10);
CREATE TABLE measurements_high PARTITION OF measurements
    FOR VALUES FROM (10) TO (20);
INSERT INTO measurements VALUES (1, 'low'), (11, 'high');

DO $$
DECLARE
    columns text;
BEGIN
    SELECT string_agg(format('c%s integer', i), ',')
      INTO columns
      FROM generate_series(1, 1600) i;
    EXECUTE 'CREATE TABLE wide_table (' || columns || ')';
END
$$;

CREATE TYPE branch_alter_composite AS (first_value integer);
ALTER TYPE branch_alter_composite ADD ATTRIBUTE second_value integer;
SELECT count(*) = 2 AS composite_alter_uses_core_path
FROM pg_attribute
WHERE attrelid = 'branch_alter_composite'::regclass AND attnum > 0;
DROP TYPE branch_alter_composite;

CREATE INDEX accounts_balance_idx ON accounts ((balance + 1));
ALTER INDEX accounts_balance_idx ALTER COLUMN 1 SET STATISTICS 100;
SELECT attstattarget = 100 AS index_alter_uses_core_path
FROM pg_attribute
WHERE attrelid = 'accounts_balance_idx'::regclass AND attnum = 1;
DROP INDEX accounts_balance_idx;

SELECT count(*) AS hidden_before
FROM pg_attribute
WHERE attrelid IN ('accounts'::regclass,
                   'inherited_parent'::regclass,
                   'inherited_child'::regclass,
                   'measurements'::regclass,
                   'measurements_low'::regclass,
                   'measurements_high'::regclass)
  AND attishidden;

SELECT count(*) = 0 AS pristine_ddl_stayed_in_place
FROM pg_branch_relversion;

BEGIN;
UPDATE accounts SET balance = balance WHERE id = -1;
SELECT count(*) = 0 AS pristine_heap_uses_private_dml
FROM pg_locks
WHERE pid = pg_backend_pid()
  AND locktype = 'object'
  AND classid = 'pg_branch_segment'::regclass
  AND objsubid = 1;
ROLLBACK;

CREATE BRANCH dev;
BEGIN;
UPDATE accounts SET balance = balance WHERE id = -1;
SELECT count(*) > 0 AS forked_heap_uses_logical_locks
FROM pg_locks
WHERE pid = pg_backend_pid()
  AND locktype = 'object'
  AND classid = 'pg_branch_segment'::regclass
  AND objsubid = 1;
ROLLBACK;
CREATE PUBLICATION branch_pub FOR TABLE accounts;
INSERT INTO accounts VALUES (6, 'main-after-fork@example.test', 60);
INSERT INTO parent_fk VALUES (2);
INSERT INTO conflict_target VALUES (3, 'main-only');
INSERT INTO late_unique VALUES (2, 'main-only');
INSERT INTO exclusion_target VALUES ('[10,15)');
INSERT INTO invalid_exclusion VALUES ('[20,25)'), ('[22,28)');
INSERT INTO deferred_target VALUES (2, 'main-only');
INSERT INTO merge_target VALUES (3, 'main-only');

SELECT count(*) FILTER (WHERE attishidden) AS hidden,
       count(*) FILTER (WHERE attnum > 0 AND NOT attishidden) AS visible
FROM pg_attribute
WHERE attrelid = 'wide_table'::regclass;

SELECT bool_and(hidden_count = 5) AS all_tables_versioned
FROM (
    SELECT c.oid, count(*) FILTER (WHERE a.attishidden) AS hidden_count
    FROM pg_class c
    JOIN pg_attribute a ON a.attrelid = c.oid
    WHERE c.relname IN ('accounts', 'inherited_parent', 'inherited_child',
                        'measurements', 'measurements_low',
                        'measurements_high')
    GROUP BY c.oid
) s;

SELECT __pg_branch_low FROM accounts;
INSERT INTO accounts (__pg_branch_deleted) VALUES (true);
UPDATE accounts SET __pg_branch_deleted = true WHERE id = 1;
ALTER TABLE accounts RENAME COLUMN __pg_branch_low TO exposed;
ALTER TABLE accounts ADD COLUMN __pg_branch_fake integer;

SHOW BRANCH;
SET BRANCH dev;

COPY copy_target FROM STDIN WITH (FORMAT csv);
2,dev
\.
INSERT INTO copy_target VALUES (3, 'executor');
COPY copy_target TO STDOUT WITH (FORMAT csv);

INSERT INTO child_fk VALUES (2, 1);
INSERT INTO child_fk VALUES (3, 2);

DELETE FROM parent_fk WHERE id = 1;
DELETE FROM child_fk WHERE parent_id = 1;
DELETE FROM parent_fk WHERE id = 1;
SELECT * FROM parent_fk ORDER BY id;
SELECT * FROM child_fk ORDER BY id;

DELETE FROM cascade_parent WHERE id = 1;
SELECT * FROM cascade_parent ORDER BY id;
SELECT * FROM cascade_child ORDER BY id;

INSERT INTO conflict_target VALUES (1, 'dev')
ON CONFLICT (id) DO UPDATE SET value = excluded.value
RETURNING *;
INSERT INTO conflict_target VALUES (2, 'dev-only')
ON CONFLICT DO NOTHING;
INSERT INTO conflict_target VALUES (2, 'ignored')
ON CONFLICT DO NOTHING;
INSERT INTO conflict_target VALUES (3, 'dev-version')
ON CONFLICT DO NOTHING;
SELECT * FROM conflict_target ORDER BY id;

INSERT INTO late_unique VALUES (2, 'dev-only');
CREATE UNIQUE INDEX late_unique_id_key ON late_unique (id);
INSERT INTO late_unique VALUES (2, 'duplicate-in-dev');
UPDATE late_unique SET id = 1 WHERE id = 2;
SELECT * FROM late_unique ORDER BY id;
CREATE UNIQUE INDEX invalid_unique_id_key ON invalid_unique (id);

INSERT INTO exclusion_target VALUES ('[10,15)');
ALTER TABLE exclusion_target ADD CONSTRAINT exclusion_target_no_overlap
    EXCLUDE USING gist (period WITH &&);
INSERT INTO exclusion_target VALUES ('[14,18)');
SELECT * FROM exclusion_target ORDER BY period;
ALTER TABLE invalid_exclusion ADD CONSTRAINT invalid_exclusion_no_overlap
    EXCLUDE USING gist (period WITH &&);

UPDATE account_view SET balance = 12 WHERE id = 1 RETURNING *;
UPDATE generated_target SET base = 7 WHERE id = 1 RETURNING *;
UPDATE rule_target SET value = 'dev' WHERE id = 1;
SELECT * FROM rule_target ORDER BY id;
SELECT * FROM rule_log ORDER BY id;
INSERT INTO deferred_target VALUES (2, 'dev-only');
BEGIN;
INSERT INTO deferred_target VALUES (2, 'duplicate-in-dev');
COMMIT;
SELECT * FROM deferred_target ORDER BY id, value;
MERGE INTO merge_target AS target
USING (VALUES (1, 'updated'), (2, 'inserted'), (3, 'dev-version'))
    AS source(id, value)
ON target.id = source.id
WHEN MATCHED THEN UPDATE SET value = source.value
WHEN NOT MATCHED THEN INSERT VALUES (source.id, source.value);
SELECT * FROM merge_target ORDER BY id;

UPDATE unlogged_target SET value = 'dev' WHERE id = 1;
INSERT INTO unlogged_target VALUES (2, 'dev-only');
SELECT * FROM unlogged_target ORDER BY id;

-- The first DDL on a shared physical schema copies visible rows.  Secondary
-- indexes finish asynchronously; another DDL on the resulting private schema
-- takes the native PostgreSQL fast path without creating another version.
SELECT last_value AS rowid_before_schema_copy
FROM pg_catalog.pg_branch_rowid_seq \gset
ALTER TABLE schema_copy_target ADD COLUMN dev_only integer DEFAULT 9;
SELECT last_value = :rowid_before_schema_copy AS copied_rowids_preserved
FROM pg_catalog.pg_branch_rowid_seq;
SELECT * FROM schema_copy_target ORDER BY id;
SELECT string_agg(column_name::text, ',' ORDER BY ordinal_position)
FROM information_schema.columns
WHERE table_name = 'schema_copy_target';
DO $$
BEGIN
    FOR i IN 1..500 LOOP
        EXIT WHEN NOT EXISTS
        (
            SELECT 1
            FROM pg_branch_relversion
            WHERE brvlogical = pg_branch_logical_relation('schema_copy_target'::regclass)
              AND brvindexstate = 'p'
        );
        PERFORM pg_sleep(0.01);
    END LOOP;
END
$$;
SELECT bool_and(brvindexstate = 'r') AS secondary_indexes_ready
FROM pg_branch_relversion
WHERE brvlogical = pg_branch_logical_relation('schema_copy_target'::regclass);
SELECT count(*) = 2 AS internal_storage_indexes_ready
FROM pg_index i
WHERE i.indrelid = 'schema_copy_target'::regclass
  AND i.indisvalid AND i.indisready
  AND EXISTS
      (SELECT 1 FROM unnest(i.indkey::smallint[]) AS key(attnum)
       JOIN pg_attribute a
         ON a.attrelid = i.indrelid AND a.attnum = key.attnum
       WHERE a.attishidden);
SELECT reloptions @> ARRAY['fillfactor=50'] AS schema_copy_fillfactor
FROM pg_class WHERE oid = 'schema_copy_target'::regclass;
SELECT count(*) AS schema_versions_before_private_ddl
FROM pg_branch_relversion
WHERE brvlogical = pg_branch_logical_relation('schema_copy_target'::regclass);
ALTER TABLE schema_copy_target ADD COLUMN private_fast text DEFAULT 'fast';
SELECT count(*) AS schema_versions_after_private_ddl
FROM pg_branch_relversion
WHERE brvlogical = pg_branch_logical_relation('schema_copy_target'::regclass);
SELECT private_fast FROM schema_copy_target ORDER BY id;
DO $$
DECLARE
    plan_line text;
    plan_text text := '';
BEGIN
    FOR plan_line IN EXECUTE
        'EXPLAIN (COSTS OFF) UPDATE schema_copy_target SET value = value WHERE id = -1'
    LOOP
        plan_text := plan_text || plan_line;
    END LOOP;
    IF plan_text LIKE '%__pg_branch_low%' THEN
        RAISE EXCEPTION 'private DML retained interval visibility quals';
    END IF;
    RAISE NOTICE 'private DML visibility quals suppressed';
END
$$;

-- The first write uses the private physical fast path.  Creating a child in
-- the same transaction invalidates that decision, so the following delete
-- must preserve the child's fork-time row through interval splitting.
BEGIN;
UPDATE schema_copy_target SET value = 'private-update' WHERE id = 2;
CREATE BRANCH schema_copy_child FROM dev;
DO $$
DECLARE
    plan_line text;
    plan_text text := '';
BEGIN
    FOR plan_line IN EXECUTE
        'EXPLAIN (COSTS OFF) UPDATE schema_copy_target SET value = value WHERE id = -1'
    LOOP
        plan_text := plan_text || plan_line;
    END LOOP;
    IF plan_text NOT LIKE '%__pg_branch_low%' THEN
        RAISE EXCEPTION 'shared DML lost interval visibility quals';
    END IF;
    RAISE NOTICE 'shared DML visibility quals retained';
END
$$;
DELETE FROM schema_copy_target WHERE id = 1;
COMMIT;
SELECT id, value FROM schema_copy_target ORDER BY id;
SET BRANCH schema_copy_child;
SELECT id, value FROM schema_copy_target ORDER BY id;
-- A second physical generation has user columns after the predecessor's
-- hidden columns.  Heap copying must map attributes by name, not position.
ALTER TABLE schema_copy_target ADD COLUMN generation_two text DEFAULT 'mapped';
SELECT id, value, dev_only, private_fast, generation_two
FROM schema_copy_target ORDER BY id;
SET BRANCH dev;

UPDATE trigger_target SET value = 30 WHERE id = 1;
DELETE FROM trigger_target WHERE id = 2;
SELECT * FROM trigger_target ORDER BY id;
SELECT * FROM trigger_log ORDER BY ctid;
TRUNCATE truncate_target;
SELECT * FROM truncate_target ORDER BY id;

UPDATE accounts SET email = 'dev-one@example.test', balance = 11 WHERE id = 1;
UPDATE bulk_update_target SET value = 1;
SELECT count(*) AS bulk_dev_rows
FROM bulk_update_target WHERE value = 1;
DELETE FROM accounts WHERE id = 2;
INSERT INTO accounts VALUES (3, 'one@example.test', 30);
INSERT INTO inherited_child VALUES (3, 'dev-child', 10);
UPDATE measurements SET value = 'dev-high' WHERE id = 11;
UPDATE measurements SET id = 12, value = 'moved' WHERE id = 1;

SELECT * FROM accounts ORDER BY id;
SELECT accounts::text, row_to_json(accounts), to_jsonb(accounts)
FROM accounts ORDER BY id;
SELECT '(4,four@example.test,40)'::accounts;
SELECT json_populate_record(NULL::accounts,
                            '{"id":5,"email":"five@example.test","balance":50,"__pg_branch_deleted":true}');
SELECT * FROM inherited_parent ORDER BY id;
SELECT * FROM measurements ORDER BY id;

CREATE TABLE dev_snapshot AS SELECT * FROM accounts;
SELECT * FROM dev_snapshot ORDER BY id;

CREATE TABLE created_after_fork (id integer PRIMARY KEY, value text);
INSERT INTO created_after_fork VALUES (1, 'dev');
SELECT count(*) AS hidden_after_fork
FROM pg_attribute
WHERE attrelid = 'created_after_fork'::regclass AND attishidden;

CREATE BRANCH feature FROM dev;
INSERT INTO accounts VALUES (7, 'dev-after-fork@example.test', 70);
SET BRANCH feature;
UPDATE accounts SET balance = 31 WHERE id = 3;
SELECT * FROM accounts ORDER BY id;

SET BRANCH dev;
SELECT * FROM accounts ORDER BY id;

SET BRANCH main;
SELECT * FROM accounts ORDER BY id;
SELECT count(*) AS bulk_main_rows
FROM bulk_update_target WHERE value = 1;
SELECT string_agg(column_name::text, ',' ORDER BY ordinal_position)
FROM information_schema.columns
WHERE table_name = 'schema_copy_target';
SELECT * FROM inherited_parent ORDER BY id;
SELECT * FROM measurements ORDER BY id;
SELECT * FROM created_after_fork ORDER BY id;
SELECT * FROM parent_fk ORDER BY id;
SELECT * FROM child_fk ORDER BY id;
SELECT * FROM cascade_parent ORDER BY id;
SELECT * FROM cascade_child ORDER BY id;
SELECT * FROM conflict_target ORDER BY id;
SELECT * FROM late_unique ORDER BY id;
SELECT * FROM exclusion_target ORDER BY period;
SELECT * FROM invalid_exclusion ORDER BY period;
SELECT * FROM account_view ORDER BY id;
SELECT * FROM generated_target ORDER BY id;
SELECT * FROM deferred_target ORDER BY id, value;
SELECT * FROM merge_target ORDER BY id;
SELECT * FROM rule_target ORDER BY id;
SELECT * FROM rule_log ORDER BY id;
SELECT * FROM dev_snapshot ORDER BY id;
SELECT * FROM trigger_target ORDER BY id;
SELECT * FROM trigger_log ORDER BY ctid;
SELECT * FROM truncate_target ORDER BY id;
SELECT * FROM copy_target ORDER BY id;
SELECT * FROM unlogged_target ORDER BY id;
DO $$
BEGIN
    FOR i IN 1..200 LOOP
        EXECUTE format('CREATE BRANCH fanout_%s FROM main', i);
    END LOOP;
END
$$;
SELECT count(*) AS main_children FROM pg_branch WHERE brparent = 9100;
DROP BRANCH dev;
DROP BRANCH dev CASCADE;
SELECT count(*) AS active_physical_schema_versions
FROM pg_branch_relversion rv
JOIN pg_branch b ON b.oid = rv.brvbranch
WHERE b.brstate = 'a';

SELECT * FROM accounts ORDER BY id;

-- Let short-lived reclamation workers disconnect before DROP DATABASE.
SELECT pg_sleep(1);

\connect regression
DROP DATABASE regression_branching;

-- New databases start with branching enabled.  The compatibility function is
-- therefore a no-op, and tables created before its call are already versioned.
CREATE DATABASE regression_branch_default;
\connect regression_branch_default
CREATE TABLE activation_target (id integer PRIMARY KEY, value text);
INSERT INTO activation_target VALUES (1, 'main');
SELECT count(*) AS hidden_by_default
FROM pg_attribute
WHERE attrelid = 'activation_target'::regclass AND attishidden;
SELECT pg_branch_enable() AS activation_needed;
SELECT count(*) AS branches_after_activation FROM pg_branch WHERE brstate = 'a';
CREATE BRANCH activation_child FROM main;
SET BRANCH activation_child;
SELECT * FROM activation_target;
SET BRANCH main;
CREATE TABLE branch_cached_read (id integer PRIMARY KEY, value text);
INSERT INTO branch_cached_read VALUES (1, 'main');
PREPARE branch_cached_plan AS
  SELECT value FROM branch_cached_read WHERE id = 1;
EXECUTE branch_cached_plan;
CREATE BRANCH branch_cached_child FROM main;
SET BRANCH branch_cached_child;
UPDATE branch_cached_read SET value = 'child' WHERE id = 1;
SET BRANCH main;
EXECUTE branch_cached_plan;
SET BRANCH branch_cached_child;
EXECUTE branch_cached_plan;
DEALLOCATE branch_cached_plan;
SET BRANCH main;
DROP BRANCH branch_cached_child;
DROP TABLE branch_cached_read;
DROP BRANCH activation_child;
CREATE TABLE branch_postdrop_private (id integer PRIMARY KEY);
SELECT count(*) = 1 AS postdrop_creation_is_tracked
FROM pg_branch_relversion;
ALTER TABLE branch_postdrop_private ADD COLUMN value text;
SELECT count(*) = 1 AS postdrop_alter_stayed_in_place
FROM pg_branch_relversion;
DROP TABLE branch_postdrop_private;
SELECT count(*) = 0 AS postdrop_metadata_was_removed
FROM pg_branch_relversion;
\connect regression
DROP DATABASE regression_branch_default;

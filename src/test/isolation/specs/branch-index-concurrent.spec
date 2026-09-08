# A branch-local CREATE INDEX CONCURRENTLY spans internal transactions.  Its
# session-level branch lock must keep a child fork behind the completed index.

setup
{
  CREATE TABLE branch_cic_target (id integer, value integer);
  INSERT INTO branch_cic_target VALUES (1, 10), (2, 20);
  CREATE FUNCTION branch_cic_wait(integer) RETURNS boolean
    IMMUTABLE LANGUAGE plpgsql AS $$
    BEGIN
      PERFORM pg_advisory_lock_shared(982451653);
      RETURN true;
    END
    $$;
  CREATE BRANCH branch_cic_parent FROM main;
}

setup
{
  SET BRANCH branch_cic_parent;
}

setup
{
  ALTER TABLE branch_cic_target ADD COLUMN private_marker integer;
}

setup
{
  SET BRANCH main;
}

teardown
{
  SET BRANCH main;
  DROP BRANCH branch_cic_parent CASCADE;
  DROP FUNCTION branch_cic_wait(integer) CASCADE;
  DROP TABLE branch_cic_target;
}

session builder
step set_parent { SET BRANCH branch_cic_parent; }
step build_index
{
  CREATE INDEX CONCURRENTLY branch_cic_value_idx
    ON branch_cic_target (value)
    WHERE branch_cic_wait(value);
}

session blocker
step hold_build { SELECT pg_advisory_lock(982451653); }
step release_build { SELECT pg_advisory_unlock(982451653); }

session forker
step fork_child { CREATE BRANCH branch_cic_child FROM branch_cic_parent; }
step fork_retry { CREATE BRANCH branch_cic_child FROM branch_cic_parent; }
step set_child { SET BRANCH branch_cic_child; }
step child_index
{
  SELECT count(*)
  FROM pg_index
  WHERE indrelid = 'branch_cic_target'::regclass
    AND indexrelid = 'branch_cic_value_idx'::regclass;
}
step set_main { SET BRANCH main; }
step main_index
{
  SELECT count(*)
  FROM pg_index
  WHERE indrelid = 'branch_cic_target'::regclass
    AND indexrelid = 'branch_cic_value_idx'::regclass;
}

permutation hold_build set_parent build_index fork_child release_build(build_index) fork_retry set_child child_index set_main main_index

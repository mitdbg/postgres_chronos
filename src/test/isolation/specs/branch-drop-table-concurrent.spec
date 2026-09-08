# DROP TABLE holds the selected branch against a concurrent fork while it
# waits to upgrade the table lock.  The child must be created only after the
# globally safe physical drop completes.

setup
{
  CREATE TABLE branch_drop_table_target (id integer);
}

teardown
{
  SET BRANCH main;
  DROP BRANCH branch_drop_table_child CASCADE;
}

session locker
step lock_begin { BEGIN; }
step lock_table
{
  LOCK TABLE branch_drop_table_target IN ACCESS SHARE MODE;
}
step lock_commit { COMMIT; }

session dropper
step drop_table { DROP TABLE branch_drop_table_target; }

session forker
step fork_child { CREATE BRANCH branch_drop_table_child FROM main; }
step set_child { SET BRANCH branch_drop_table_child; }
step table_absent
{
  SELECT to_regclass('branch_drop_table_target') IS NULL AS table_absent;
}

permutation lock_begin lock_table drop_table fork_child lock_commit(drop_table) set_child table_absent

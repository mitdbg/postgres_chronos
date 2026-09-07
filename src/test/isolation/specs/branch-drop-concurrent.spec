# DROP is metadata-only.  Even while a reader keeps the physical relation
# locked, concurrent drops return and background reclaimers wait until it is
# safe to remove tuples and schema versions.

setup
{
  CREATE TABLE branch_drop_target (id integer PRIMARY KEY);
  INSERT INTO branch_drop_target VALUES (1);
  CREATE BRANCH drop_parent_left FROM main;
  CREATE BRANCH drop_parent_right FROM main;
  CREATE BRANCH drop_leaf_left FROM drop_parent_left;
  CREATE BRANCH drop_leaf_right FROM drop_parent_right;
}

teardown
{
  SET BRANCH main;
  DROP BRANCH drop_parent_left;
  DROP BRANCH drop_parent_right;
  DROP TABLE branch_drop_target;
}

session blocker
step blocker_begin { BEGIN; }
step blocker_read { SELECT count(*) FROM branch_drop_target; }
step blocker_commit { COMMIT; }

session left_drop
step left_set { SET BRANCH drop_leaf_left; }
step left_alter { ALTER TABLE branch_drop_target ADD COLUMN left_value integer; }
step left_main { SET BRANCH main; }
step drop_left { DROP BRANCH drop_leaf_left; }

session right_drop
step right_set { SET BRANCH drop_leaf_right; }
step right_alter { ALTER TABLE branch_drop_target ADD COLUMN right_value integer; }
step right_main { SET BRANCH main; }
step drop_right { DROP BRANCH drop_leaf_right; }

permutation left_set left_alter left_main right_set right_alter right_main blocker_begin blocker_read drop_left drop_right blocker_commit

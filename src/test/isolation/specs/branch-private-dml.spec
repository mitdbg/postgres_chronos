# A writer may use ordinary PostgreSQL UPDATE on a private schema copy only
# while its branch lock prevents a concurrent child from observing a partially
# modified fork point.

setup
{
  CREATE BRANCH branch_private_activation;
  DROP BRANCH branch_private_activation;
  CREATE TABLE branch_private_target (id integer PRIMARY KEY, value text);
  CREATE TABLE branch_private_ddl_target (id integer PRIMARY KEY);
  INSERT INTO branch_private_target VALUES (1, 'before');
}

teardown
{
  SET BRANCH main;
  DROP BRANCH branch_private_parent CASCADE;
  DROP TABLE branch_private_target, branch_private_ddl_target;
}

session writer
step writer_main_begin { BEGIN; }
step writer_main_update { UPDATE branch_private_target SET value = value WHERE id = 1; }
step writer_main_commit { COMMIT; }
step writer_set { SET BRANCH branch_private_parent; }
step writer_alter { ALTER TABLE branch_private_target ADD COLUMN local_column integer DEFAULT 1; }
step writer_begin { BEGIN; }
step writer_update { UPDATE branch_private_target SET value = 'after' WHERE id = 1; }
step writer_commit { COMMIT; }

session creator
step create_parent { CREATE BRANCH branch_private_parent FROM main; }
step alter_unrelated { ALTER TABLE branch_private_ddl_target ADD COLUMN value integer; }
step create_child { CREATE BRANCH branch_private_child FROM branch_private_parent; }
step child_set { SET BRANCH branch_private_child; }
step child_read { SELECT id, value, local_column FROM branch_private_target; }

permutation writer_main_begin writer_main_update alter_unrelated writer_main_commit create_parent writer_set writer_alter writer_begin writer_update create_child writer_commit(create_child) child_set child_read

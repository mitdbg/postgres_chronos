# Concurrent creators must serialize without upgrading their session's
# AccessShareLock and deadlocking.  Pre-activate branching so this test covers
# the steady-state CREATE BRANCH path.

setup
{
  CREATE TABLE branch_create_target (id integer PRIMARY KEY);
  INSERT INTO branch_create_target VALUES (1);
  CREATE BRANCH activation;
  DROP BRANCH activation;
}

teardown
{
  SET BRANCH main;
  DROP BRANCH branch_left;
  DROP BRANCH branch_right;
  DROP TABLE branch_create_target;
}

session s1
step s1b { BEGIN; }
step s1read { SELECT count(*) FROM branch_create_target; }
step s1create { CREATE BRANCH branch_left FROM main; }
step s1c { COMMIT; }

session s2
step s2b { BEGIN; }
step s2read { SELECT count(*) FROM branch_create_target; }
step s2create { CREATE BRANCH branch_right FROM main; }
step s2c { COMMIT; }

permutation s1b s1read s2b s2read s1create s2create s1c(s2create) s2c

# A serializable cursor must distinguish a replacement written by this
# transaction from a logical-row version relocated by another transaction.

setup
{
  CREATE TABLE branch_serial_target (id integer PRIMARY KEY, value text);
  INSERT INTO branch_serial_target VALUES (1, 'before');
  CREATE BRANCH branch_serial_child;
}

teardown
{
  DROP BRANCH branch_serial_child;
  DROP TABLE branch_serial_target;
}

session s1
step s1begin { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1snapshot { SELECT count(*) FROM branch_serial_target; }
step s1declare { DECLARE branch_serial_cursor NO SCROLL CURSOR FOR
                   SELECT * FROM branch_serial_target FOR UPDATE; }
step s1fetch { FETCH ALL FROM branch_serial_cursor; }
step s1rollback { ROLLBACK; }

session s2
step s2begin { BEGIN; }
step s2update { UPDATE branch_serial_target SET value = 'after' WHERE id = 1; }
step s2commit { COMMIT; }

permutation s1begin s1snapshot s1declare s2begin s2update s2commit s1fetch s1rollback

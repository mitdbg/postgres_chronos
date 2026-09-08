# Concurrent changes in different database branches share a logical row lock.
# BEFORE triggers must resolve the current branch's physical interval fragment
# after waiting, rather than following a heap CTID chain into a sibling branch.

setup
{
  CREATE TABLE branch_lock_target (id integer PRIMARY KEY, value text);
  CREATE TABLE branch_lock_audit (event text, old_value text);
  INSERT INTO branch_lock_target VALUES
    (1, 'base1'), (2, 'base2'), (3, 'base3'), (4, 'base4'), (5, 'base5');

  CREATE FUNCTION branch_lock_trigger() RETURNS trigger
  LANGUAGE plpgsql AS $$
  BEGIN
    IF TG_OP = 'UPDATE' THEN
      NEW.value := OLD.value || '>' || NEW.value;
      RETURN NEW;
    END IF;
    INSERT INTO branch_lock_audit VALUES (TG_OP, OLD.value);
    RETURN OLD;
  END
  $$;
  CREATE TRIGGER branch_lock_before_update
    BEFORE UPDATE ON branch_lock_target
    FOR EACH ROW EXECUTE FUNCTION branch_lock_trigger();
  CREATE TRIGGER branch_lock_before_delete
    BEFORE DELETE ON branch_lock_target
    FOR EACH ROW EXECUTE FUNCTION branch_lock_trigger();
}

teardown
{
  SET BRANCH main;
  DROP BRANCH dev;
  DROP TABLE branch_lock_target, branch_lock_audit, activation_late;
  DROP FUNCTION branch_lock_trigger();
}

session s1
step s1createb { BEGIN; }
step s1create { CREATE TABLE activation_late (id integer); }
step s1createc { COMMIT; }
step s1dev { SET BRANCH dev; }
step s1b { BEGIN; }
step s1u1 { UPDATE branch_lock_target SET value = 'dev1' WHERE id = 1; }
step s1c1 { COMMIT; }
step s1b2 { BEGIN; }
step s1u2 { UPDATE branch_lock_target SET value = 'dev2' WHERE id = 2; }
step s1c2 { COMMIT; }
step s1b3 { BEGIN; }
step s1u3 { UPDATE branch_lock_target SET value = 'dev3' WHERE id = 3; }
step s1c3 { COMMIT; }
step s1b4 { BEGIN; }
step s1u4 { UPDATE branch_lock_target SET value = 'dev4' WHERE id = 4; }
step s1c4 { COMMIT; }
step s1b5 { BEGIN; }
step s1u5 { UPDATE branch_lock_target SET value = 'dev5' WHERE id = 5; }
step s1c5 { COMMIT; }
step s1rows { SELECT * FROM branch_lock_target ORDER BY id; }

session s2
step s2branch { CREATE BRANCH dev; }
step s2late { SELECT count(*) AS hidden FROM pg_attribute WHERE attrelid = 'activation_late'::regclass AND attishidden; }
step s2b { BEGIN; }
step s2u1 { UPDATE branch_lock_target SET value = 'main1' WHERE id = 1; }
step s2c1 { COMMIT; }
step s2b2 { BEGIN; }
step s2d2 { DELETE FROM branch_lock_target WHERE id = 2; }
step s2c2 { COMMIT; }
step s2b3 { BEGIN; }
step s2skip { SELECT value FROM branch_lock_target WHERE id = 3 FOR UPDATE SKIP LOCKED; }
step s2share { SELECT value FROM branch_lock_target WHERE id = 3 FOR KEY SHARE; }
step s2c3 { COMMIT; }
step s2notrig { DROP TRIGGER branch_lock_before_update ON branch_lock_target; }
step s2b4 { BEGIN; }
step s2m4 { MERGE INTO branch_lock_target t
            USING (VALUES (4, 'main4')) s(id, value) ON t.id = s.id
            WHEN MATCHED THEN UPDATE SET value = s.value; }
step s2c4 { COMMIT; }
step s2b5 { BEGIN; }
step s2oc5 { INSERT INTO branch_lock_target VALUES (5, 'main5')
             ON CONFLICT (id) DO UPDATE SET value = excluded.value; }
step s2c5 { COMMIT; }
step s2rows { SELECT * FROM branch_lock_target ORDER BY id; }
step s2audit { SELECT * FROM branch_lock_audit ORDER BY event, old_value; }

permutation s1createb s1create s1createc s2branch s2late s1dev s1b s1u1 s2b s2u1 s1c1(s2u1) s2c1 s1b2 s1u2 s2b2 s2d2 s1c2(s2d2) s2c2 s1b3 s1u3 s2b3 s2skip s2share s1c3(s2share) s2c3 s2notrig s1b4 s1u4 s2b4 s2m4 s1c4(s2m4) s2c4 s1b5 s1u5 s2b5 s2oc5 s1c5(s2oc5) s2c5 s2rows s2audit s1rows

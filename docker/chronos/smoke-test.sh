#!/usr/bin/env bash
set -Eeuo pipefail

if (( $# != 1 )); then
	printf 'usage: %s IMAGE\n' "$0" >&2
	exit 2
fi

image=$1
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
container="chronos-postgres-smoke-$$"
volume="chronos-postgres-smoke-$$"
password='chronos-container-smoke-test'

cleanup() {
	docker container rm --force "$container" >/dev/null 2>&1 || true
	docker volume rm "$volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT

wait_until_ready() {
	local attempt
	for attempt in $(seq 1 90); do
		if docker exec "$container" pg_isready --username postgres --dbname chronos_test >/dev/null 2>&1; then
			return
		fi
		sleep 1
	done
	docker logs "$container" >&2
	return 1
}

docker volume create "$volume" >/dev/null
docker run --detach \
	--name "$container" \
	--env POSTGRES_DB=chronos_test \
	--env POSTGRES_PASSWORD="$password" \
	--mount "type=volume,src=$volume,dst=/var/lib/postgresql" \
	--mount "type=bind,src=$script_dir/smoke-init.sql,dst=/docker-entrypoint-initdb.d/10-chronos-smoke.sql,readonly" \
	"$image" >/dev/null

wait_until_ready

docker exec --interactive \
	--env PGPASSWORD="$password" \
	"$container" \
	psql --username postgres --dbname chronos_test --no-psqlrc --set ON_ERROR_STOP=1 <<'SQL'
DO $$
BEGIN
    IF current_setting('server_version') NOT LIKE '19beta3-chronos%' THEN
        RAISE EXCEPTION 'unexpected server version: %', current_setting('server_version');
    END IF;
    IF (SELECT count(*) FROM docker_entrypoint_marker) <> 1 THEN
        RAISE EXCEPTION 'docker-entrypoint-initdb.d script did not run';
    END IF;
END
$$;

CREATE TABLE accounts
(
    id integer PRIMARY KEY,
    owner text UNIQUE,
    balance integer NOT NULL
);
INSERT INTO accounts VALUES (1, 'main', 100);

SELECT pg_branch_enable();
CREATE BRANCH dev FROM main;
SET BRANCH dev;
UPDATE accounts SET owner = 'dev', balance = 75 WHERE id = 1;
ALTER TABLE accounts ADD COLUMN note text DEFAULT 'branch-local';
INSERT INTO accounts (id, owner, balance) VALUES (2, 'dev-only', 25);

DO $$
BEGIN
    IF (SELECT array_agg(ROW(id, owner, balance, note)::text ORDER BY id) FROM accounts)
       <> ARRAY['(1,dev,75,branch-local)', '(2,dev-only,25,branch-local)'] THEN
        RAISE EXCEPTION 'development branch contents are incorrect';
    END IF;
    IF EXISTS (
        SELECT 1
        FROM information_schema.columns
        WHERE table_schema = 'public'
          AND table_name = 'accounts'
          AND column_name LIKE '__pg_branch_%'
    ) THEN
        RAISE EXCEPTION 'hidden branch metadata leaked through information_schema';
    END IF;
    IF EXISTS (
        SELECT 1
        FROM accounts AS account
        WHERE row_to_json(account)::text LIKE '%__pg_branch_%'
    ) THEN
        RAISE EXCEPTION 'hidden branch metadata leaked through row JSON';
    END IF;
END
$$;

SET BRANCH main;
DO $$
BEGIN
    IF (SELECT count(*) FROM accounts) <> 1
       OR (SELECT owner FROM accounts WHERE id = 1) <> 'main'
       OR (SELECT balance FROM accounts WHERE id = 1) <> 100 THEN
        RAISE EXCEPTION 'main branch was modified by development-branch DML';
    END IF;
    IF EXISTS (
        SELECT 1
        FROM information_schema.columns
        WHERE table_schema = 'public'
          AND table_name = 'accounts'
          AND column_name = 'note'
    ) THEN
        RAISE EXCEPTION 'development-branch DDL leaked into main';
    END IF;
END
$$;
SQL

schema_dump=$(docker exec \
	--env PGPASSWORD="$password" \
	"$container" \
	pg_dump --username postgres --dbname chronos_test --schema-only)
if grep -q '__pg_branch_' <<<"$schema_dump"; then
	printf 'hidden branch metadata leaked through pg_dump\n' >&2
	exit 1
fi

docker restart "$container" >/dev/null
wait_until_ready

docker exec --interactive \
	--env PGPASSWORD="$password" \
	"$container" \
	psql --username postgres --dbname chronos_test --no-psqlrc --set ON_ERROR_STOP=1 <<'SQL'
SET BRANCH dev;
DO $$
BEGIN
    IF (SELECT count(*) FROM accounts) <> 2
       OR (SELECT owner FROM accounts WHERE id = 1) <> 'dev'
       OR (SELECT note FROM accounts WHERE id = 2) <> 'branch-local' THEN
        RAISE EXCEPTION 'branch state did not survive a container restart';
    END IF;
END
$$;
SQL

printf 'container smoke test passed: %s\n' "$image"

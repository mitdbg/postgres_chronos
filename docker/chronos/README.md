# Chronos PostgreSQL container image

This image keeps the operating contract of the Docker Official Image for
PostgreSQL while replacing its PostgreSQL binaries with a release build from
this repository. The official entrypoint continues to provide `POSTGRES_USER`,
`POSTGRES_PASSWORD`, `POSTGRES_DB`, `POSTGRES_INITDB_ARGS`, Docker secrets,
`/docker-entrypoint-initdb.d`, and the PostgreSQL 19 data-volume layout.

## Run the image

```sh
docker run --name chronos-postgres \
  -e POSTGRES_PASSWORD=postgres \
  -p 5432:5432 \
  -v chronos-postgres-data:/var/lib/postgresql \
  zxjcarrot/chronos-postgres:19
```

Connect with `psql`, enable branching, and create a branch:

```sql
CREATE TABLE accounts (id bigint PRIMARY KEY, balance bigint NOT NULL);
INSERT INTO accounts VALUES (1, 100);

SELECT pg_branch_enable();
CREATE BRANCH dev FROM main;
SET BRANCH dev;
UPDATE accounts SET balance = 75 WHERE id = 1;
```

See [`BRANCHING.md`](../../BRANCHING.md) for the user guide and
[`CHRONOS_VERSIONING.md`](../../CHRONOS_VERSIONING.md) for implementation
details.

## Build and validate locally

Run the same build and smoke test used before publication:

```sh
docker build \
  -f docker/chronos/Dockerfile \
  -t chronos-postgres:test \
  .
docker/chronos/smoke-test.sh chronos-postgres:test
```

The smoke test verifies the official initialization interface, branch DML
isolation, branch-local DDL, metadata hiding in SQL and `pg_dump`, and state
persistence across a container restart.

The published image currently targets `linux/amd64`. It does not publish a
`latest` tag because the PostgreSQL 19 base image is a beta release. Publication
uses the currently authenticated Docker Hub account and creates a public
`chronos-postgres` repository if it does not exist. Set
`DOCKERHUB_PRIVATE=true` to create a private repository instead.

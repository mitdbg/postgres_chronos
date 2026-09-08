CREATE TABLE docker_entrypoint_marker
(
    initialized_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

INSERT INTO docker_entrypoint_marker DEFAULT VALUES;

# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
my $port = $node->port;

$node->init;
$node->start;

$node->safe_psql(
	'postgres',
	q{
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
ALTER TABLE accounts ADD COLUMN note text DEFAULT 'branch-local';
INSERT INTO accounts VALUES (2, 'dev', 25, 'dev-only');
});

is($node->safe_psql(
		'postgres',
		q{SELECT count(*)
          FROM pg_branch_relversion
         WHERE brvlogical = 'public.accounts'::regclass
           AND brvphysical <> brvlogical}),
	'1',
	'branch-local DDL created one physical schema copy');

my $dump_file = $node->basedir . '/schema.sql';
command_ok(
	[
		'pg_dump', '--port' => $port, '--schema-only',
		'--file' => $dump_file, 'postgres'
	],
	'schema dump succeeds');
my $stdout = slurp_file($dump_file);

like($stdout, qr/CREATE TABLE public\.accounts \(/,
	'logical table is present in schema dump');
unlike($stdout, qr/__pg_branch_v_/,
	'physical schema-copy table is absent from schema dump');
unlike($stdout, qr/__pg_branch_(?:low|high|rowid|deleted|writer)/,
	'hidden tuple metadata is absent from schema dump');

done_testing();

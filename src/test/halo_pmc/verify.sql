-- Run in the same database as 初始化PMC.sql.
\set ON_ERROR_STOP on
\if :{?pmc_event}
\else
  \set pmc_event cycles,instructions
\endif

SHOW server_version;
SHOW halo_pmc_list;
SHOW halo_pmc_segment;

SET client_min_messages = notice;
SET max_parallel_workers_per_gather = 0;
SET enable_nestloop = off;
SET enable_mergejoin = off;

CREATE TEMP TABLE pmc_r(id int NOT NULL);
CREATE TEMP TABLE pmc_s(id int NOT NULL, val int NOT NULL);
INSERT INTO pmc_r SELECT generate_series(1, 10000);
INSERT INTO pmc_s SELECT n, n FROM generate_series(1, 10000) AS t(n);
ANALYZE pmc_r;
ANALYZE pmc_s;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.val > 100;

-- Diagnostic for unknown event. SQL must still return 9900.
SET halo_pmc_list = missing_pmc;
SELECT count(*) AS expected_9900 FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.val > 100;

-- Baseline with the chosen event group.
SELECT pg_catalog.set_config('halo_pmc_list', :'pmc_event', false);
SELECT count(*) AS expected_9900 FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.val > 100;

-- Pair snapshots around the hash/insert body of each iteration.
SET halo_pmc_segment = on;
SELECT count(*) AS expected_9900 FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.val > 100;

RESET halo_pmc_segment;
RESET halo_pmc_list;

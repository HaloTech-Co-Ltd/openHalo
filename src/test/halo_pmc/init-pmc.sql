-- Run as the PostgreSQL superuser in the dedicated course database.
\set ON_ERROR_STOP on
CREATE TABLE IF NOT EXISTS public.pmc_name (
    pcm_name varchar(40) PRIMARY KEY CHECK (pcm_name = lower(pcm_name)),
    pmutype integer NOT NULL CHECK (pmutype >= 0),
    pe_config bigint NOT NULL,
    config1 bigint NOT NULL DEFAULT 0,
    config2 bigint NOT NULL DEFAULT 0
);

INSERT INTO public.pmc_name (pcm_name, pmutype, pe_config) VALUES
    ('cycles', 0, 0),
    ('instructions', 0, 1),
    ('perf_count_hw_cache_l1d_rd', 3, 0),
    ('perf_count_hw_cache_l1d_miss_rd', 3, 65536),
    -- Intel raw event from the course example. CPU model dependent.
    ('mem_inst_retired_all_stores', 4, 33488),
    -- Software-only diagnostic events for Linux guests without a PMU.
    ('task_clock', 1, 1),
    ('cpu_clock', 1, 0)
ON CONFLICT (pcm_name) DO NOTHING;

# Private hash build performance counters

In PostgreSQL compatibility mode, an openHalo superuser can opt in to Linux
CPU performance counters for private hash table builds. Each observation
window attaches to the current backend thread and covers user-mode execution.

## Build and run

Build openHalo as described in [CONTRIBUTING.md](../../../CONTRIBUTING.md).
Use an isolated test instance and a regular OS account with permission to
access hardware events through perf_event_open. Run both scripts in the same
database from the repository root:

    psql -X -d halo0root -f src/test/halo_pmc/init-pmc.sql -f src/test/halo_pmc/verify.sql

The scripts create the event metadata table and run a private Hash Join.
Each query returns 9900. Look for cycle and instruction counts, nonzero
scheduling percentages, and a positive RDPMC sample count with segment mode
enabled. The explicit unknown-event case emits a diagnostic and still returns
the correct result.

On Linux without a hardware PMU, pass -v pmc_event=task_clock,cpu_clock for a
software-event smoke test. These events cannot validate RDPMC.

## Scope

The default empty halo_pmc_list disables measurement. Only superusers can set
halo_pmc_list and halo_pmc_segment. Initialize the event table in a trusted
database and keep write access limited to trusted roles. Event definitions are
read from public.pmc_name at the start of each private hash build. Hardware
event availability and raw event encodings depend on the CPU and kernel.

The stage counters cover retrieval, hashing, insertion and final sizing.
Segment mode is supported on Linux x86 and brackets each tuple's hash/insert
body with paired RDPMC reads. It does not issue perf read or enable/disable
system calls inside the loop. Rounds with unavailable or inconsistent
snapshots are skipped. Shared parallel hash builds are outside the scope of
this change. The paired reads incur overhead of their own.

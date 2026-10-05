# Reproducible benchmark harness

`pageforge_bench` measures three end-to-end paths against a newly created
database: durable typed-row insertion, a catalog-bound SQL filter, and a full
table scan after closing and reopening the heap file. It validates row counts
and ID checksums before emitting one JSON object, so corrupt or incomplete work
cannot be reported as a successful timing.

```bash
make benchmark
./build/pageforge_bench benchmark.db 1000
rm benchmark.db
```

The database path must not already exist and the row count must be from 1 to
10,000,000. The tool deliberately leaves the database behind so its size and
contents can be inspected after the run.

The JSON fields are:

| Field | Meaning |
| --- | --- |
| `rows` | Rows inserted and scanned after reopen |
| `matched_rows` | Even-ID rows returned by the SQL predicate |
| `*_microseconds` | Wall-clock duration for the named phase |
| `*_rows_per_second` | Derived throughput using input rows for that phase |
| `database_bytes` | Final heap-file size |
| `selected_id_checksum` | Sum of IDs returned by the SQL filter |

Insertion timing includes the final buffer-pool flush but excludes database and
catalog creation. SQL selection runs before close and may benefit from the
buffer pool. The reopen scan constructs a new heap, buffer pool, catalog, and
table store, although the operating system page cache may still be warm.

Results are diagnostic, not a published performance claim. Compiler, hardware,
filesystem, power state, row count, and cache state all affect them; compare
changes only under the same controlled environment. `make check` runs a tiny
200-row smoke case to validate behavior without enforcing unstable speed
thresholds. Start with 1,000 rows when validating a checkout, then increase the
workload deliberately: table and index metadata are not cached yet, so bulk
insert time is not expected to scale linearly at larger row counts.

## Development measurement: insertion-page hint

The insertion-page hint was evaluated on 2026-10-04 using Apple clang 21.0.0,
an arm64 macOS host, the repository's optimized build flags, a 64-frame buffer
pool, and newly created 1,000-row databases. The previous page-zero scan took
784,030 microseconds for insertion in one baseline run. Three runs after the
change took 69,866, 80,771, and 70,139 microseconds; the median was 70,139
microseconds, or about 14,257 inserted rows per second.

This is an engineering comparison, not a cross-machine performance promise. It
isolates the benefit of starting near the last successful page while preserving
wraparound reuse. It does not remove the current repeated catalog scans or the
linear worst case when every existing page is unsuitable.

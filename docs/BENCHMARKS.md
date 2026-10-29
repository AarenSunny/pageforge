# Reproducible benchmark harness

`pageforge_bench` measures five end-to-end paths against a newly created
database: durable typed-row insertion, a catalog-bound SQL filter, integer
index construction, a full table scan after closing and reopening the heap
file, and nine indexed equality queries from a second reopen. The lookup keys
are deterministic and evenly spaced across the inserted ID range. The harness
verifies that the planner selects the persisted index for every sample and
validates row contents, counts, and ID checksums before emitting one JSON
object, so corrupt or incomplete work cannot be reported as a successful
timing.

```bash
make benchmark
mkdir -p benchmark-artifacts
make benchmark-environment > benchmark-artifacts/environment.json
./build/pageforge_bench benchmark-artifacts/benchmark.db 1000 \
  | tee benchmark-artifacts/benchmark.json
```

The database path must not already exist and the row count must be from 1 to
10,000,000. The tool deliberately leaves the database behind so its size and
contents can be inspected after the run.

The environment sidecar records the operating system, architecture,
best-effort CPU model, logical CPU count, compiler, build flags, Git revision,
and whether the working tree is clean. Review it before sharing because build
flags can contain local paths. CPU discovery can fall back to the processor or
architecture name; the report is context for a local measurement, not proof
that results transfer to comparable hardware. Generated databases and reports
live in the ignored `benchmark-artifacts/` directory so creating the redirected
sidecar does not change its own cleanliness result.

The JSON fields are:

| Field | Meaning |
| --- | --- |
| `rows` | Rows inserted and scanned after reopen |
| `matched_rows` | Even-ID rows returned by the SQL predicate |
| `indexed_rows` | Rows returned across the reopened equality samples |
| `*_microseconds` | Wall-clock duration for the named phase |
| `*_rows_per_second` | Derived throughput using input rows for that phase |
| `index_lookup_iterations` | Number of reopened equality-query samples |
| `reopen_index_lookup_samples_microseconds` | Raw end-to-end lookup durations |
| `reopen_index_lookup_microseconds` | Median end-to-end lookup duration |
| `reopen_index_lookups_per_second` | Lookup rate derived from the median |
| `database_bytes` | Final heap-file size |
| `selected_id_checksum` | Sum of IDs returned by the SQL filter |
| `indexed_id_checksum` | Sum of IDs returned by the reopened index lookups |

Insertion and index-build timings include their final buffer-pool flushes but
exclude database and catalog creation. SQL selection runs before close and may
benefit from the buffer pool. The reopen scan and lookup sample set each
construct a new heap, buffer pool, catalog, and table store, although the
operating system page cache may still be warm. Raw lookup samples and their
median reduce sensitivity to one timing outlier, but nine sequential queries
are not a latency distribution or a concurrent-service benchmark.

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

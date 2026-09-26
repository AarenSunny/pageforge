# Reproducible benchmark harness

`pageforge_bench` measures three end-to-end paths against a newly created
database: durable typed-row insertion, a catalog-bound SQL filter, and a full
table scan after closing and reopening the heap file. It validates row counts
and ID checksums before emitting one JSON object, so corrupt or incomplete work
cannot be reported as a successful timing.

```bash
make benchmark
./build/pageforge_bench benchmark.db 100000
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
thresholds.

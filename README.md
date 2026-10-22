# PageForge

A small relational database I'm writing from scratch in C++20 to learn how databases actually store and find data. I became interested in backend development, especially data storage and analysis, and this project is my way of going deeper into both.

## What works right now

- Data lives in 4 KiB slotted pages with checksums, so corrupted pages get caught instead of silently read
- A buffer pool caches pages in memory and evicts them with the clock algorithm
- B+ tree indexes on integer columns, kept in sync on insert and delete
- A small SQL subset: `SELECT` with `WHERE`, `ORDER BY`, and `LIMIT`, plus `EXPLAIN` to show index choice, direction, and safe limit pushdown
- A CLI and interactive shell

## Try it

Use a new database path; the demo generator refuses to overwrite an existing
file.

```sh
make check
./tools/create_demo_database.sh demo.db
./build/pageforge demo.db query \
  "EXPLAIN SELECT name, id FROM people WHERE id >= 2 ORDER BY id DESC LIMIT 2;"
./build/pageforge demo.db query \
  "SELECT name, id FROM people WHERE id >= 2 ORDER BY id DESC LIMIT 2;"
./build/pageforge demo.db shell
```

The generator creates five deterministic rows and an integer index. The full
three-minute walkthrough and benchmark method are in
[`docs/DEMO.md`](docs/DEMO.md) and [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).

## The bug I learned the most from

Inserts were slow because every insert scanned from page 0 looking for free space. I added a hint that starts the search at the last page that had room. On my laptop, a 1,000-row insert went from 784 ms to about 70 ms (one baseline run versus the median of three, so treat it as rough).

## Not done yet

- No write-ahead log, so a crash mid-write can still lose data
- B+ trees only grow two levels deep (internal-node splits are next)
- No joins, aggregation, or transactions

I'm working through these roughly in that order. Design notes for each layer are in [`docs/`](docs/).

# PageForge

PageForge is a relational database engine built from scratch in modern C++.
Its goal is to make storage, indexing, query execution, and recovery mechanisms
visible and testable rather than hiding them behind an existing database.

The current milestone implements the physical storage foundation: portable
checksummed pages, variable-length records, stable record identifiers, page
compaction, a durable heap file, a buffer manager, and typed catalog metadata.

## Current capabilities

- 4 KiB versioned pages with explicit little-endian encoding
- Slotted-page storage for variable-length records
- Stable slot IDs across deletion, reuse, and compaction
- Full-page and heap-header corruption checksums
- Durable page allocation, reads, writes, flushing, and reopen
- Clock-sweep buffer pool with RAII pin guards and dirty-page eviction
- Multi-page record store with `(page_id, slot_id)` IDs, deletion, and scans
- Portable typed tuples with integers, text, booleans, nulls, and strict decoding
- Persistent table catalog with named, versioned schemas and duplicate detection
- Typed table rows with schema-bound insert, read, scan, and delete operations
- Streaming record and typed-table cursors without materializing row sets
- Composable filter, stable sort, projection, and limit query operators
- Position-aware SQL lexer with literals, comparisons, and comments
- Strict `SELECT` parser with conjunctive predicates and explicit null tests
- Catalog-bound SQL execution with typed filters and `ORDER BY`
- CLI for typed table creation, validated inserts, and streaming SQL queries
- Bounds, overlap, truncation, format, and page-position validation
- Warning-clean C++20 build on macOS and Linux CI

## Build and test

Requires a C++20 compiler and Make.

```bash
make check
```

## Try the storage engine

```bash
make
./build/pageforge demo.db init
./build/pageforge demo.db create-table people id:int name:text active:bool note:text?
./build/pageforge demo.db insert people 1 Ada true NULL
./build/pageforge demo.db insert people 2 "Grace Hopper" true compiler
./build/pageforge demo.db insert people 3 Bob false analyst
./build/pageforge demo.db query \
  "SELECT name, id FROM people WHERE active = TRUE ORDER BY id DESC LIMIT 10;"
```

Each command starts a new process and reopens the same database. `init` refuses
an existing path. `create-table` accepts `int`, `text`, and `bool` columns, with
`?` marking nullable columns. `insert` parses values from the stored schema and
uses uppercase `NULL` for null. `query` prints an escaped, tab-separated result.
Low-level `put`, `get`, `erase`, and `list` commands remain available for raw
record inspection. See [docs/CLI.md](docs/CLI.md) for the complete command and
output contract. Use a disposable path if you want to start over.

The test suite writes only temporary heap files and removes them afterward. See
[docs/STORAGE_FORMAT.md](docs/STORAGE_FORMAT.md) for byte layouts and validation
rules, [docs/BUFFER_POOL.md](docs/BUFFER_POOL.md) for caching and writeback, and
[docs/RECORD_STORE.md](docs/RECORD_STORE.md) for record-level behavior. The
[tuple-format notes](docs/TUPLES.md) document typed row encoding and its schema
contract, while [docs/CATALOG.md](docs/CATALOG.md) describes persisted table
definitions. [docs/TABLE_STORE.md](docs/TABLE_STORE.md) shows the typed-table
API and row envelope. [docs/QUERY_EXECUTION.md](docs/QUERY_EXECUTION.md)
demonstrates the composable streaming query pipeline; [docs/SQL_LEXER.md](docs/SQL_LEXER.md)
describes tokenization, and [docs/SQL_PARSER.md](docs/SQL_PARSER.md) defines the
currently accepted `SELECT` grammar and logical plan. [docs/SQL_EXECUTION.md](docs/SQL_EXECUTION.md)
documents binding and end-to-end execution.

## Architecture roadmap

- [x] Checksummed slotted pages and heap files
- [x] Clock-sweep buffer pool with dirty-page eviction
- [x] Heap record IDs, multi-page insertion, deletion, and scans
- [ ] B+ tree indexes
- [x] Typed tuple encoding and corruption validation
- [x] Catalog metadata and schema persistence
- [x] Typed table rows with logical table isolation
- [x] Streaming record and typed-table scans
- [x] Filter, stable in-memory sort, projection, and limit operators
- [x] Position-aware SQL lexical analysis
- [x] `SELECT` plans for projection, filters, ordering, and limit
- [x] Catalog binding and streaming execution for supported `SELECT` plans
- [ ] Joins and aggregation
- [ ] Write-ahead logging and crash recovery
- [ ] Transactions, locking, and isolation
- [x] Typed table and SQL CLI with end-to-end smoke test
- [ ] Interactive shell, benchmarks, Docker image, and demo database

Unchecked items are planned milestones rather than current claims.

## Why slotted pages?

Variable-length rows cannot be addressed safely by raw byte offset: compaction
would invalidate every external pointer. PageForge stores a small fixed slot ID
and lets the slot contain the changing offset. Higher layers can therefore use a
stable `(page_id, slot_id)` record identifier while the storage layer reclaims
fragmented space.

## License

MIT

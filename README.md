# PageForge

PageForge is a relational database engine built from scratch in modern C++.
Its goal is to make storage, indexing, query execution, and recovery mechanisms
visible and testable rather than hiding them behind an existing database.

The first milestone implements the physical storage foundation: portable
checksummed pages, variable-length records, stable record identifiers, page
compaction, and a durable heap file.

## Current capabilities

- 4 KiB versioned pages with explicit little-endian encoding
- Slotted-page storage for variable-length records
- Stable slot IDs across deletion, reuse, and compaction
- Full-page and heap-header corruption checksums
- Durable page allocation, reads, writes, flushing, and reopen
- Clock-sweep buffer pool with RAII pin guards and dirty-page eviction
- Bounds, overlap, truncation, format, and page-position validation
- Warning-clean C++20 build on macOS and Linux CI

## Build and test

Requires a C++20 compiler and Make.

```bash
make check
```

The test suite writes only temporary heap files and removes them afterward. See
[docs/STORAGE_FORMAT.md](docs/STORAGE_FORMAT.md) for byte layouts and validation
rules, and [docs/BUFFER_POOL.md](docs/BUFFER_POOL.md) for caching, pinning,
eviction, writeback, and durability boundaries.

## Architecture roadmap

- [x] Checksummed slotted pages and heap files
- [x] Clock-sweep buffer pool with dirty-page eviction
- [ ] B+ tree indexes and heap record IDs
- [ ] Typed tuples and catalog metadata
- [ ] SQL lexer, parser, and logical plans
- [ ] Iterator-based scans, filters, joins, and aggregation
- [ ] Write-ahead logging and crash recovery
- [ ] Transactions, locking, and isolation
- [ ] CLI shell, benchmarks, Docker image, and demo database

Unchecked items are planned milestones rather than current claims.

## Why slotted pages?

Variable-length rows cannot be addressed safely by raw byte offset: compaction
would invalidate every external pointer. PageForge stores a small fixed slot ID
and lets the slot contain the changing offset. Higher layers can therefore use a
stable `(page_id, slot_id)` record identifier while the storage layer reclaims
fragmented space.

## License

MIT

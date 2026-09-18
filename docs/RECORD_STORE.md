# Record store

`RecordStore` is the record-level layer over the heap file and buffer pool. It
returns a stable `RecordId { page_id, slot_id }` for each variable-length byte
record, and supports `insert`, `read`, `erase`, and an ordered full `scan`.

```cpp
auto heap = pageforge::HeapFile::create("example.db");
pageforge::BufferPool pool(heap, 64);
pageforge::RecordStore records(pool);
auto id = records.insert(payload);
auto bytes = records.read(id);
pool.flush_all();
```

Insertion searches existing pages in page-ID order, so a deletion can release
space for future records. A page compacts itself if needed, keeping live slot
IDs stable. If no page fits, the store allocates a new one. The current search
is linear in the number of pages; a free-space map is a planned optimization.

`scan()` returns live records in `(page_id, slot_id)` order and omits tombstones.
It materializes the result in memory, so the future query executor will use a
streaming cursor instead. A record ID remains valid until its record is
deleted. Because slots can be reused, a stale ID may subsequently name a new
record; generation numbers or a stable logical ID layer are needed before
external references can survive deletion.

The store inherits the buffer pool's single-threaded and explicit-flush
contract. It provides no transaction or crash-atomicity guarantee yet.

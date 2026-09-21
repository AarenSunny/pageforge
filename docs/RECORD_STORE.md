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
auto cursor = records.cursor();
while (auto next = cursor.next()) {
    // Process next->id and next->bytes one record at a time.
}
pool.flush_all();
```

Insertion searches existing pages in page-ID order, so a deletion can release
space for future records. A page compacts itself if needed, keeping live slot
IDs stable. If no page fits, the store allocates a new one. The current search
is linear in the number of pages; a free-space map is a planned optimization.

`cursor().next()` returns one live record at a time in `(page_id, slot_id)` order
and omits tombstones. It holds no page pin between calls, so even a one-frame
buffer pool can support a scan while other operations fetch pages. `scan()` is
a convenience wrapper that materializes all cursor results in memory. The
buffer pool must outlive its cursors.

The cursor captures the number of heap pages at creation; newly allocated
pages are not visited. Changes to existing pages may still become visible as
the cursor advances. This is not a snapshot or a transactionally consistent
scan. A record ID remains valid until its record is deleted. Because slots can
be reused, a stale ID may subsequently name a new record; generation numbers
or a stable logical ID layer are needed before external references can survive
deletion.

The store inherits the buffer pool's single-threaded and explicit-flush
contract. It provides no transaction or crash-atomicity guarantee yet.

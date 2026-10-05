# Record store

`RecordStore` is the record-level layer over the heap file and buffer pool. It
returns a stable `RecordId { page_id, slot_id }` for each variable-length byte
record, and supports `insert`, `read`, stable-ID `replace`, `erase`, and an
ordered full `scan`.

```cpp
auto heap = pageforge::HeapFile::create("example.db");
pageforge::BufferPool pool(heap, 64);
pageforge::RecordStore records(pool);
auto id = records.insert(payload);
auto bytes = records.read(id);
records.replace(id, replacement_payload);
auto cursor = records.cursor();
while (auto next = cursor.next()) {
    // Process next->id and next->bytes one record at a time.
}
pool.flush_all();
```

Insertion begins at an in-memory hint for the last page that accepted a record,
then wraps once across the existing pages. A reopened store starts at the final
heap page, which keeps append-heavy workloads from rescanning full pages from
page zero while still allowing a deletion on an earlier page to release space
for future records. A page compacts itself if needed, keeping live slot IDs
stable. If no page fits, the store allocates a new one and advances the hint.

The hint is deliberately a small optimization rather than persistent free-space
metadata. It makes sequential insertion close to the expected append path, but
the worst case remains a linear scan of the heap when no page can fit a record.
A persisted free-space map is still required for predictable large-database
insertion latency.

Replacement rebuilds the page payload region while preserving the target slot
and every neighboring slot. It checks total capacity before changing bytes, so
an oversized replacement throws `PageFull` and leaves the original record
intact. The B+ tree uses fixed-size records to make this stable-ID rewrite
predictable for leaf and internal nodes.

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

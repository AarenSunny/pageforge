# B+ tree leaf layer

`BPlusTreeIndex` is the durable leaf layer of PageForge's B+ tree. It stores
signed 64-bit keys mapped to `RecordId` values, permits duplicate keys, splits
full leaves, links leaves in global entry order, and supports exact and
inclusive range lookup across a database reopen.

```cpp
auto index = pageforge::BPlusTreeIndex::create(records);
auto header_id = index.header_id();

index.insert(42, row_id);
auto matches = index.find(42);
auto window = index.range(10, 50);

pool.flush_all();
// Persist header_id in the owning metadata layer.
```

`open(records, header_id)` validates the header and complete leaf chain before
returning. Exact `(key, RecordId)` insertion is idempotent, while one key may
map to multiple record IDs. Entries are ordered by signed key and then by page
and slot, making duplicate-key results deterministic. Bounds passed to `range`
are inclusive; either may be absent.

## On-disk records

The index currently uses two portable little-endian record types inside the
ordinary record store:

- a 16-byte `PFIH` header containing format version 1 and the first leaf's
  stable record ID;
- fixed 1,808-byte `PFIL` leaves containing a next-leaf record ID, entry count,
  and space for 128 fourteen-byte `(key, page, slot)` entries.

Leaves reserve their maximum size from creation. Updating a leaf therefore
uses the stable-slot replacement primitive without depending on adjacent free
space. An overflowing leaf is split at its midpoint: the new right leaf points
to the old successor, then the left leaf is rewritten to point to the right.

Decoding rejects unknown versions, malformed next pointers, oversized counts,
nonzero padding, unsorted or duplicate entries, missing records, link cycles,
and globally misordered leaf boundaries. The underlying page checksum still
protects every index record from accidental on-disk bit flips.

## Current boundary

This milestone intentionally stops before internal nodes. Lookup currently
validates and walks the leaf chain, so its routing cost is linear in the number
of leaves rather than logarithmic. Internal separator nodes, catalog ownership,
SQL planner selection, deletion/rebalancing, and uniqueness constraints remain
next steps. The caller must retain the header record ID until catalog metadata
owns it.

Splits are not crash-atomic without the planned write-ahead log. A crash after
allocating a right leaf but before linking it may leave an unreachable record;
the reachable chain remains in its earlier valid form. This layer is
single-threaded under the same contract as `RecordStore`.

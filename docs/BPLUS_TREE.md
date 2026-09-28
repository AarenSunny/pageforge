# B+ tree index

`BPlusTreeIndex` is PageForge's durable height-two B+ tree. It stores signed
64-bit keys mapped to `RecordId` values, permits duplicate keys, splits full
leaves, promotes an internal root, and supports routed exact and inclusive
range lookup plus exact-entry deletion across a database reopen.

```cpp
auto index = pageforge::BPlusTreeIndex::create(records);
auto header_id = index.header_id();

index.insert(42, row_id);
auto matches = index.find(42);
auto window = index.range(10, 50);
index.erase(42, row_id);

pool.flush_all();
// Persist header_id in the owning metadata layer.
```

`open(records, header_id)` validates the header and complete leaf chain before
returning. It also verifies that every internal child pointer and high-key
separator matches the linked leaves. Exact `(key, RecordId)` insertion is
idempotent, while one key may map to multiple record IDs. `erase` removes only
the exact `(key, RecordId)` pair and returns false when it is absent. Entries
are ordered by signed key and then by page and slot, making duplicate-key
results deterministic. Bounds passed to `range` are inclusive; either may be
absent.

## On-disk records

The index uses three portable little-endian record types inside the
ordinary record store:

- a 24-byte `PFIH` header containing format version 2, tree height, root record
  ID, and first-leaf record ID;
- a fixed 2,576-byte `PFIN` root containing up to 128 high-key separators and
  child record IDs;
- fixed 1,808-byte `PFIL` leaves containing a next-leaf record ID, entry count,
  and space for 128 fourteen-byte `(key, page, slot)` entries.

Leaves reserve their maximum size from creation. Updating a leaf therefore
uses the stable-slot replacement primitive without depending on adjacent free
space. An overflowing root leaf is split at its midpoint and promoted beneath
a new internal root. Later leaf splits insert a new separator into that root.
Point lookup and bounded range lookup binary-search root separators to choose a
starting leaf; range output then follows leaf links in key order.

Deletion rewrites a nonempty leaf and repairs its high-key separator. When a
leaf becomes empty, the tree unlinks and retires it, updates the first-leaf
pointer when needed, and removes its root child. A two-child internal root
collapses back to the remaining leaf, so deleting all entries returns the tree
to its original height-one empty state.

Decoding rejects unknown versions, malformed next pointers, oversized counts,
nonzero padding, unsorted or duplicate entries, missing records, link cycles,
globally misordered leaf boundaries, and separators that disagree with their
children. The underlying page checksum still protects every index record from
accidental on-disk bit flips.

## Current boundary

This milestone supports heights one and two. The fixed internal root can route
up to 128 leaves; an insertion that would require a height-three tree fails
before changing the full leaf. Recursive internal splits, minimum-occupancy
merging/redistribution, catalog ownership, SQL planner selection, and uniqueness
constraints remain next steps. The caller must retain the header record ID
until catalog metadata owns it.

Splits, deletion, and root promotion/collapse are not crash-atomic without the
planned write-ahead log. A crash between their record rewrites may leave an
unreachable node or a separator mismatch that reopen validation rejects. This
layer is single-threaded under the same contract as `RecordStore`.

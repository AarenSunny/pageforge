# Storage format

PageForge uses a deliberately explicit binary format. It never writes native
C++ structs to disk, so compiler padding and host endianness cannot silently
change a database file.

## Heap file

The first 4 KiB block is the heap header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic and format version: `PFDB0001` |
| 8 | 4 | Page size, little endian |
| 12 | 4 | Allocated page count |
| 16 | 4 | FNV-1a checksum |
| 20 | 4076 | Reserved, zero-filled |

Data page `n` starts at byte `4096 × (n + 1)`. The header checksum covers the
entire block with the checksum field treated as zero. Opening a truncated file,
unknown format, incompatible page size, or damaged header fails immediately.

## Slotted data page

Every 4 KiB page contains a 24-byte header, a forward-growing slot directory,
one contiguous free region, and record bytes growing backward from the end:

```text
0                 free_start          free_end                 4096
| header | slots → |       free        | ← variable records     |
```

The header stores magic, version, page ID, slot count, free-space bounds, and a
checksum over the full page. Each six-byte slot contains a record offset, record
length, and flags. All integers use little-endian encoding.

Deletes tombstone a slot without moving other records. Insertion reuses a
tombstone and compacts payload bytes only when contiguous space is insufficient.
Slot numbers therefore remain stable across deletion and compaction—a property
the future B-tree can rely on when it stores record identifiers as `(page, slot)`.

## Validation guarantees

Decoding verifies:

- page magic, version, and full-page checksum;
- exact slot-directory and free-space boundaries;
- live records stay inside the payload region;
- record payloads do not overlap;
- a page's encoded ID matches its physical heap-file position.

Checksums detect accidental corruption, not malicious tampering. A later
write-ahead log will provide crash atomicity; the current heap layer documents
that allocation is not yet transactionally recoverable.

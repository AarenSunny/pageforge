# Persistent catalog

The catalog persists named table definitions inside the same record store used
by the rest of PageForge. Each definition includes a positive schema version
and an ordered list of named, typed, nullable columns.

```cpp
pageforge::Catalog catalog(records);
catalog.create_table({
    "people",
    {{"id", pageforge::DataType::Integer, false},
     {"name", pageforge::DataType::Text, false},
     {"note", pageforge::DataType::Text, true}},
    1,
});
pool.flush_all();
```

`find_table` and `list_tables` scan persisted records and reconstruct their
schemas after the database is reopened. Creation rejects empty identifiers,
empty schemas, duplicate column names, duplicate table names, unknown types,
and schema version zero. Catalog reads reject truncated metadata, unknown type
tags or flags, trailing bytes, and duplicate persisted names.

## Record layout

Catalog entries reserve the four-byte `PFC1` prefix. It is followed by:

| Size | Field |
| ---: | --- |
| 2 | Catalog format version, little endian |
| 4 | Table schema version, little endian |
| 2 + n | Table-name byte length and bytes |
| 2 | Column count |
| repeated | Column-name length and bytes, one-byte type, one-byte flags |

Type tags are explicit (`1` integer, `2` text, `3` boolean); nullable is flag
bit zero. Identifier strings are stored as bytes without Unicode normalization.

## Current boundaries

Records without the reserved prefix are ignored, which lets catalog metadata
coexist with ordinary records during this stage. Application data beginning
with `PFC1` is therefore reserved and may be interpreted as metadata. The
planned table-storage layer will separate system and table records so this
temporary shared namespace disappears.

Catalog creation becomes durable at the buffer pool's explicit `flush_all()`
boundary. Schema alteration, table deletion, transactional DDL, and concurrent
catalog access are not implemented yet; `schema_version` is persisted now so
those operations can identify the row layout they are changing later.

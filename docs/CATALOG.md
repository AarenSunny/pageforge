# Persistent catalog

The catalog persists named table definitions and B+ tree ownership inside the
same record store used by the rest of PageForge. Each table definition includes
a positive schema version and an ordered list of named, typed, nullable columns.

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

An existing B+ tree can be assigned a durable name and bound to one integer
column:

```cpp
auto index = pageforge::BPlusTreeIndex::create(records);
catalog.register_index({"people_id_idx", "people", "id", index.header_id()});
pool.flush_all();
```

`find_table` and `list_tables` scan persisted records and reconstruct their
schemas after the database is reopened. Creation rejects empty identifiers,
empty schemas, duplicate column names, duplicate table names, unknown types,
and schema version zero. `register_index` additionally requires an existing
table and integer column, a unique index name, and a header that opens as a
valid B+ tree. `find_index` and `list_indexes` retain that header record ID so
the tree can be reopened. Catalog reads reject truncated metadata, unknown type
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

Index entries reserve the four-byte `PFX1` prefix. It is followed by:

| Size | Field |
| ---: | --- |
| 2 | Index catalog format version, little endian |
| 2 | Flags (currently zero) |
| 4 | B+ tree header page ID, little endian |
| 2 | B+ tree header slot ID, little endian |
| 2 | Reserved padding (zero) |
| 2 + n | Index-name byte length and bytes |
| 2 + n | Owning table-name byte length and bytes |
| 2 + n | Indexed column-name byte length and bytes |

## Current boundaries

Records without a reserved catalog prefix are ignored, which lets metadata
coexist with typed rows, B+ tree nodes, and ordinary records. Application data
beginning with `PFC1` or `PFX1` is reserved and may be interpreted as metadata.
Typed rows use their own `PFR1` prefix and are logically separated by table
name, but system and user records still share physical heap pages. Dedicated
table storage will remove this temporary shared namespace.

Catalog creation becomes durable at the buffer pool's explicit `flush_all()`
boundary. Index registration records ownership but does not backfill existing
rows; callers must populate the tree before registration. `TableStore` maintains
registered indexes for subsequent inserts and deletes, and the SQL binder uses
them for exact integer equality predicates. Range planning is not implemented
yet. Schema alteration, table or index deletion, transactional DDL, and
concurrent catalog access are also not implemented. `schema_version` is
persisted now so future schema operations can identify the row layout they are
changing.

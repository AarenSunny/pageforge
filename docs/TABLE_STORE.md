# Typed table storage

`TableStore` binds the catalog and tuple codec to the record store. A table row
can be inserted, read by record ID, scanned, or deleted using its named table.
The table's persisted schema validates every inserted tuple and decodes every
returned one.

```cpp
auto heap = pageforge::HeapFile::create("people.db");
pageforge::BufferPool pool(heap, 64);
pageforge::RecordStore records(pool);
pageforge::Catalog catalog(records);
pageforge::TableStore tables(records, catalog);

(void)catalog.create_table({
    "people",
    {{"id", pageforge::DataType::Integer, false},
     {"name", pageforge::DataType::Text, false}},
    1,
});
auto id = tables.insert("people", {std::int64_t{7}, std::string("Ada")});
auto row = tables.read("people", id);
pool.flush_all();
```

## Row envelope

Each typed row is an ordinary heap record with this prefix, followed by the
[tuple encoding](TUPLES.md):

| Size | Field |
| ---: | --- |
| 4 | Magic: `PFR1` |
| 2 | Row-envelope version, little endian |
| 4 | Table schema version, little endian |
| 2 + n | Table-name byte length and bytes |
| remaining | Encoded tuple |

Reading verifies ownership and the schema version before decoding values. A
row ID from another table is rejected rather than being interpreted through the
wrong schema. Scans ignore catalog entries, raw records, and other tables;
malformed reserved row records fail validation instead of being silently
skipped. Deletion checks ownership before tombstoning the underlying slot.

## Current boundaries

Isolation is logical, not physical: all tables and the catalog still share the
same heap pages. The `PFR1` and `PFC1` prefixes are reserved, so raw application
records beginning with either prefix can collide with system formats. Table
scans currently materialize their results in memory, and insertion linearly
searches heap pages for space. There is no schema migration, transaction, or
concurrent writer support yet. A deleted record ID can be reused by a later
insert, so callers must not treat old IDs as permanent external keys.

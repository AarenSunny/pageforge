# Typed tuple format

`TupleCodec` converts schema-checked values into portable record bytes. The
current type system supports signed 64-bit integers, text, booleans, and nulls.
It deliberately stores fields without native C++ structs, padding, or host
endianness.

```cpp
pageforge::Schema schema{
    {"id", pageforge::DataType::Integer, false},
    {"name", pageforge::DataType::Text, false},
    {"note", pageforge::DataType::Text, true},
};
pageforge::Tuple row{std::int64_t{7}, std::string("Ada"), std::monostate{}};
auto bytes = pageforge::TupleCodec::encode(schema, row);
auto decoded = pageforge::TupleCodec::decode(schema, bytes);
```

## Binary layout

Every tuple begins with this fixed eight-byte header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic and format version: `PFT1` |
| 4 | 2 | Codec version, little endian |
| 6 | 2 | Column count, little endian |

The header is followed by `ceil(column_count / 8)` null-bitmap bytes, least
significant bit first. Null fields have no payload. Non-null fields follow in
schema order:

- integer: eight little-endian bytes preserving the signed 64-bit bit pattern;
- text: a four-byte little-endian byte length followed by exactly those bytes;
- boolean: one byte, strictly `0` or `1`.

Text is stored as bytes; applications may use UTF-8, but the codec does not
silently normalize or reject another encoding.

## Validation and schema contract

Encoding rejects arity, type, nullability, and unknown-type violations.
Decoding rejects bad magic or versions, column-count mismatch, truncation,
invalid booleans, illegal nulls, unknown bitmap bits, and trailing bytes. Text
length is checked against remaining input before allocating its string.

Types are not repeated in each row. A decoder therefore needs the same schema
used by the encoder. The catalog now persists the schema, and the table-store
envelope binds a row to its table name and schema version. Schema evolution is
not implemented: a row whose version differs from the catalog is rejected.
Tuples larger than a slotted page can be encoded, but the record store will
reject them because overflow records are not implemented yet.

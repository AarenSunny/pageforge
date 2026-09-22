# Streaming query operators

`Query` composes a typed table scan with lazy filter, projection, and limit
operators. Each `next()` call pulls at most enough upstream rows to produce one
result; the query does not materialize a table-wide result unless the caller
does so explicitly.

```cpp
auto query = pageforge::Query::from(tables, "people");
query.filter([](const pageforge::TableRow& row) {
         return std::get<bool>(row.values[2]);
     })
    .project({1, 0})
    .limit(10);

while (auto row = query.next()) {
    // row->id identifies the source record; values contain selected columns.
}
```

Operators are applied in the order called. A filter after `project()` sees the
projected row shape. Projection indices are checked against the current width
before any rows are read; columns may be reordered or repeated. A limit of
zero never pulls from its input, and an exhausted limit does not fetch another
row. Filters receive a user-defined callback, so null handling and comparison
semantics are explicit in that callback rather than implicitly claiming SQL
three-valued logic.

This is an iterator execution layer, not a SQL parser. It inherits the table
cursor's single-threaded, non-snapshot scan behavior and the buffer pool's
explicit durability boundary. The next layer will translate parsed query
expressions into these operators; joins and aggregation remain planned work.

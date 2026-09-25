# Query operators

`Query` composes a typed table scan with filter, sort, projection, and limit
operators. Filter, projection, and limit are lazy. Sort is intentionally a
blocking operator: its first `next()` call consumes its input, performs a
stable in-memory sort, and then emits owned rows in comparator order.

```cpp
auto query = pageforge::Query::from(tables, "people");
query.filter([](const pageforge::TableRow& row) {
         return std::get<bool>(row.values[2]);
     })
    .sort([](const pageforge::TableRow& left, const pageforge::TableRow& right) {
         return std::get<std::string>(left.values[1]) <
                std::get<std::string>(right.values[1]);
     })
    .project({1, 0})
    .limit(10);

while (auto row = query.next()) {
    // row->id identifies the source record; values contain selected columns.
}
```

Operators are applied in the order called. A filter or sort after `project()`
sees the projected row shape. Projection indices are checked against the
current width before any rows are read; columns may be reordered or repeated.
A limit of zero never pulls from its input, and an exhausted limit does not
fetch another row. Filter and sort receive user-defined callbacks, so null and
comparison semantics remain explicit at this layer. `stable_sort` preserves
the input order of equivalent rows.

This is an iterator execution layer, not a SQL parser. It inherits the table
cursor's single-threaded, non-snapshot scan behavior and the buffer pool's
explicit durability boundary. The SQL binder translates parsed predicates and
ordering into these operators; joins and aggregation remain planned work.

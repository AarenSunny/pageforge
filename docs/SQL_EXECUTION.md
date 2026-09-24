# SQL binding and execution

`execute_select_sql` connects the SQL front end to typed table storage and the
streaming query pipeline. It parses one supported `SELECT`, resolves names and
types against the persisted catalog, and returns a move-only `BoundSelect`.

```cpp
auto result = pageforge::execute_select_sql(
    tables, catalog,
    "SELECT name, id FROM people "
    "WHERE active = TRUE AND note IS NOT NULL LIMIT 10");

for (const auto& column : result.output_schema()) {
    // Column names and types in result order.
}
while (auto row = result.next()) {
    // Consume one projected row at a time.
}
```

Unquoted table and column names are resolved case-insensitively while the
catalog's original spelling is retained in output metadata. A case-insensitive
collision is reported as ambiguous. Unknown names, mismatched literal types,
malformed hand-built plans, and ordered boolean comparisons fail during
binding—before a table row is read. Projection may reorder or repeat columns.

Integer comparisons are signed numeric comparisons; text comparisons use
`std::string` byte ordering; booleans support only equality and inequality.
Conjunctive predicates are applied as successive lazy filters and short-circuit
as each row flows through the pipeline. If either comparison operand is null,
the result is unknown and the row is removed by `WHERE`; consequently
`column = NULL` matches no rows. `IS NULL` and `IS NOT NULL` provide explicit
null tests for columns of any type. The output exposes its projected `Schema`,
including a wildcard query that returns the entire table schema even when
`LIMIT 0` yields no rows.

Execution remains single-threaded and inherits the underlying cursor's
non-snapshot behavior. The CLI exposes this path through its `query` command and
formats the result as escaped, tab-separated text.

# SQL binding and execution

`execute_select_sql` connects the SQL front end to typed table storage and the
streaming query pipeline. It parses one supported `SELECT`, resolves names and
types against the persisted catalog, and returns a move-only `BoundSelect`.

```cpp
auto result = pageforge::execute_select_sql(
    tables, catalog,
    "SELECT name, id FROM people "
    "WHERE active = TRUE AND note IS NOT NULL ORDER BY id DESC LIMIT 10");

for (const auto& column : result.output_schema()) {
    // Column names and types in result order.
}
while (auto row = result.next()) {
    // Consume one projected row at a time.
}
```

For an integer equality predicate, the binder looks for a registered B+ tree
on the resolved table and column. When one exists, only matching record IDs are
loaded before the ordinary filter pipeline runs. All predicates—including the
one used for lookup—are still evaluated against decoded rows, so residual
filters and SQL null semantics remain unchanged. The selected path is visible
on a bound query, and `EXPLAIN SELECT` exposes it without constructing a table
cursor or loading index hits:

```cpp
if (result.access_path() == pageforge::SelectAccessPath::IndexLookup) {
    // result.index_name() identifies the chosen persisted index.
}

auto explanation = pageforge::explain_select(catalog, plan);
// explanation contains TABLE_SCAN or INDEX_LOOKUP, the resolved table,
// optional index and key, predicate count, sort requirement, and limit.
```

Index selection follows predicate order and catalog creation order. Range,
inequality, null-test, text, and boolean predicates currently retain a table
scan. An equality lookup validates that every index hit still exists, belongs
to the indexed table, and contains the indexed key; stale logical entries fail
as table corruption rather than returning the wrong row.

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

`ORDER BY` resolves against the source schema, so a query may sort on a column
that it does not project. The sort happens after filtering and before projection
and limit. It compares integers numerically, text by `std::string` byte order,
and booleans with `false` before `true`; nulls sort last in both directions.
Sorting is stable but blocking and currently keeps every filtered row in memory.

`execute_sql` dispatches ordinary `SELECT` statements to `BoundSelect` and
`EXPLAIN SELECT` statements to `SelectExplanation`. Explanation still performs
full parsing and binding, so invalid tables, columns, literal types, and sort
keys fail exactly as they do for execution; it stops before creating a table
cursor or performing the planned index lookup.

Execution remains single-threaded. Table scans inherit the underlying cursor's
non-snapshot behavior; index lookups materialize matching rows before the
operator pipeline begins. The CLI exposes both paths through its `query`
command. Query rows use escaped, tab-separated text, while explanations emit
one stable line such as
`INDEX_LOOKUP table=people index=people_id_idx key=7 predicates=1 sort=false limit=none`.

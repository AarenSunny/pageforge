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

For an integer equality or ordering predicate, the binder looks for a
registered B+ tree on the resolved table and column. Compatible ordering
predicates on that index merge into one lower and upper candidate range. Only
exact or range candidate record IDs are loaded before the ordinary filter
pipeline runs. All predicates—including those used for index access—are
still evaluated against decoded rows, so strict range boundaries, residual
filters, and SQL null semantics remain unchanged. The selected path is visible
on a bound query, and `EXPLAIN SELECT` exposes it without constructing a table
cursor or loading index hits:

```cpp
if (result.access_path() == pageforge::SelectAccessPath::IndexLookup) {
    // result.index_name() identifies the chosen persisted index.
}

auto explanation = pageforge::explain_select(catalog, plan);
// explanation contains TABLE_SCAN, INDEX_LOOKUP, or INDEX_RANGE; the resolved
// table; optional index, key, or inclusive candidate bounds; total and residual
// predicate counts; the sort requirement; and the limit.
```

Exact equality is preferred over range access even when its predicate appears
later. Otherwise, the first usable range predicate selects an index, and later
compatible bounds for that same index are merged. Crossing bounds produce a
known-empty range: execution constructs an empty row source without opening the
tree, while `EXPLAIN` retains both bounds and reports `empty=true`. This folding
is limited to ordering predicates on the selected integer index; other logical
contradictions still run through ordinary filters. Non-contiguous inequality,
null-test, text, and boolean predicates retain a table scan. Every indexed
candidate is validated to still exist, belong to the indexed table, and contain
its recorded key; stale logical entries fail as table corruption rather than
returning the wrong row.

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
cursor or performing the planned index read.

Execution remains single-threaded. Table scans inherit the underlying cursor's
non-snapshot behavior; index accesses materialize candidate rows before the
operator pipeline begins. The CLI exposes both paths through its `query`
command. Query rows use escaped, tab-separated text, while explanations emit
one stable line such as
`INDEX_LOOKUP table=people index=people_id_idx key=7 predicates=2 residual_predicates=1 sort=false limit=none`.
Range explanations use inclusive candidate bounds, for example
`INDEX_RANGE table=people index=people_id_idx lower=7 upper=none predicates=1 residual_predicates=0 sort=false limit=none`.
For an index access, `residual_predicates` excludes each compatible inclusive
bound that the access path fully covers; all filters still run during execution
as a safety check. Strict `<` and `>` ranges conservatively include the boundary
key and remain residual so their filter can remove it, which avoids overflow at
signed-integer extremes. Predicates that prove a selected-index range empty are
fully covered by that short-circuit. A table scan marks every predicate as
residual because its access path does not narrow candidate rows.

# SQL `SELECT` parser

`parse_select(source)` lexes one statement and returns a `SelectPlan`. The plan
records its table, wildcard or ordered projection columns, predicates,
optional ordering, and optional row limit.

```cpp
auto plan = pageforge::parse_select(
    "SELECT name, age FROM people WHERE age >= -18 LIMIT 10;");
```

The accepted grammar is intentionally narrow:

```text
select     := SELECT ("*" | identifier ("," identifier)*)
              FROM identifier
              (WHERE predicate (AND predicate)*)?
              (ORDER BY identifier (ASC | DESC)?)?
              (LIMIT unsigned_integer)? ";"? EOF
predicate  := identifier comparison literal
            | identifier IS NULL
            | identifier IS NOT NULL
comparison := "=" | "!=" | "<>" | "<" | "<=" | ">" | ">="
literal    := ("+" | "-")? integer | string | TRUE | FALSE | NULL
```

Keywords are matched case-insensitively; identifier spelling is retained for
catalog binding. String content is already decoded by the lexer. Integer
literals are checked against the signed 64-bit range, including the asymmetric
minimum value, and `LIMIT` is checked against the platform's `size_t` range.
Syntax failures throw `SqlParseError` with a one-based line and column. Lexical
failures remain `SqlLexError` so callers can distinguish the phase.

The SQL executor resolves this logical description against the catalog,
translates every `AND` term into a streaming filter, and binds `ORDER BY` to a
typed sort. `OR`, parentheses, aliases, multiple sort keys, joins, aggregation,
expressions, and other statement types are rejected rather than partially
interpreted. The CLI exposes supported statements through its `query` command.

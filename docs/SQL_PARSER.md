# SQL `SELECT` parser

`parse_select(source)` lexes one statement and returns a `SelectPlan`. The plan
records its table, wildcard or ordered projection columns, optional comparison
predicate, and optional row limit.

```cpp
auto plan = pageforge::parse_select(
    "SELECT name, age FROM people WHERE age >= -18 LIMIT 10;");
```

The accepted grammar is intentionally narrow:

```text
select     := SELECT ("*" | identifier ("," identifier)*)
              FROM identifier
              (WHERE identifier comparison literal)?
              (LIMIT unsigned_integer)? ";"? EOF
comparison := "=" | "!=" | "<>" | "<" | "<=" | ">" | ">="
literal    := ("+" | "-")? integer | string | TRUE | FALSE | NULL
```

Keywords are matched case-insensitively; identifier spelling is retained for
catalog binding. String content is already decoded by the lexer. Integer
literals are checked against the signed 64-bit range, including the asymmetric
minimum value, and `LIMIT` is checked against the platform's `size_t` range.
Syntax failures throw `SqlParseError` with a one-based line and column. Lexical
failures remain `SqlLexError` so callers can distinguish the phase.

The SQL executor now resolves this logical description against the catalog and
translates it into streaming query operators. Multiple predicates, `AND`/`OR`,
aliases, ordering, joins, aggregation, expressions, and other statement types
are rejected rather than partially interpreted. SQL remains unavailable from
the CLI until a shell command and output formatter are added.

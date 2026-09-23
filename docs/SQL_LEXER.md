# SQL lexical analysis

`lex_sql(source)` turns SQL source text into a vector of tokens ending in an
explicit `End` token. Every token records its zero-based byte offset and
one-based line and column, including the end token. Lexical failures throw
`SqlLexError` with the offending line and column.

```cpp
auto tokens = pageforge::lex_sql(
    "SELECT name FROM people WHERE id >= -18 AND name <> 'O''Neil';");
```

The lexer recognizes ASCII identifiers (`[A-Za-z_][A-Za-z0-9_]*`), decimal
integer digit sequences, single-quoted strings, commas, periods, parentheses,
`*`, semicolons, `+`, `-`, and the comparison operators `=`, `!=`, `<>`, `<`,
`<=`, `>`, and `>=`. `--` line comments and `/* ... */` block comments are
ignored. A doubled apostrophe inside a string decodes to one apostrophe. CRLF
is counted as one line break, as are standalone CR and LF.

The `text` field preserves source spelling for names, numbers, and symbols;
for string literals it contains the decoded content without surrounding
quotes. Keywords are intentionally returned as identifiers so the parser can
handle them case-insensitively in the appropriate grammar position. A leading
sign is a separate token; the parser will attach it to a numeric expression.

The `SELECT` parser now consumes this token stream, but SQL is not yet accepted
by the CLI.
Quoted identifiers, floating-point literals, nested block comments, and
backslash string escapes are not supported. Non-ASCII text is allowed inside
quoted strings but not in identifiers. Unterminated strings or comments, NUL
bytes, and unsupported characters fail rather than being silently skipped.

# Command-line demo

The `pageforge` executable exposes typed relational operations, an interactive
read-only SQL shell, and the raw record layer. One-shot commands reopen the
database for every operation, making persistence visible across processes.

```text
pageforge <database> init
pageforge <database> create-table <table> <name:type>...
pageforge <database> insert <table> <value>...
pageforge <database> query <select-sql>
pageforge <database> shell
pageforge <database> put <text>
pageforge <database> get <page:slot>
pageforge <database> erase <page:slot>
pageforge <database> list
```

## Typed workflow

Column types are `int`/`integer`, `text`, and `bool`/`boolean`, matched
case-insensitively. A trailing `?` makes a column nullable. Table and column
names must be unquoted SQL identifiers and are unique case-insensitively.

```bash
pageforge contacts.db init
pageforge contacts.db create-table people id:int name:text active:bool note:text?
pageforge contacts.db insert people 1 "Ada Lovelace" true NULL
pageforge contacts.db insert people 2 "Grace Hopper" true compiler
pageforge contacts.db query \
  "SELECT name FROM people WHERE active = TRUE ORDER BY name ASC;"
```

`insert` requires exactly one shell argument per column. Integers use signed
decimal syntax, booleans are `true` or `false`, and uppercase `NULL` represents
a null value only for a nullable column. This makes the exact text `NULL`
unrepresentable in a nullable text column through this demo interface; the C++
API has no such restriction.

Query output begins with a column-name header. Fields are tab-separated, nulls
print as `NULL`, booleans as `true`/`false`, and text escapes backslash, tab,
line feed, and carriage return as `\\`, `\t`, `\n`, and `\r`. This is a
human-readable demo format, not a lossless interchange protocol: a text value
equal to `NULL` is visually indistinguishable from a null. Shell-quote the SQL
statement so it reaches PageForge as one argument.

The supported `WHERE` syntax accepts one or more comparisons joined by `AND`,
plus `IS NULL` and `IS NOT NULL`. Predicates remain streaming; `OR`, grouping,
and arbitrary expressions are intentionally rejected for now. A single
`ORDER BY` column may use `ASC` (the default) or `DESC`; nulls appear last.

## Interactive shell

`shell` keeps the database open and executes one supported `SELECT` statement
per input line. It prints prompts only when standard input is a terminal, so
scripts can pipe commands and receive the same tab-separated result format as
`query`. A statement error is reported to standard error without ending the
session.

```text
$ pageforge contacts.db shell
PageForge interactive shell. Type .help for help.
pageforge> .tables
people
pageforge> .schema people
people(id INTEGER NOT NULL, name TEXT NOT NULL, active BOOLEAN NOT NULL, note TEXT NULL) [schema version 1]
pageforge> SELECT name FROM people ORDER BY name;
name
Ada Lovelace
Grace Hopper
pageforge> .quit
```

Shell commands are `.tables`, `.schema TABLE`, `.help`, and `.quit` (with
`.exit` as an alias). Table lists are sorted case-insensitively. The shell is
read-only at the SQL level because PageForge currently parses only `SELECT`;
use the one-shot `create-table` and `insert` commands for writes.

## Raw workflow

`put`, `get`, `erase`, and `list` access the underlying record store directly.
Record IDs use `page:slot`. Raw records can contain arbitrary bytes through the
library, while `put` accepts text from one shell argument. The `PFC1` catalog
and `PFR1` typed-row prefixes are reserved; inserting raw data with either
prefix can make typed catalog or table scans treat it as malformed metadata.

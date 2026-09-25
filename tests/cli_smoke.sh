#!/bin/sh
set -eu

temporary_dir=$(mktemp -d)
database="$temporary_dir/demo.db"
symlink="$temporary_dir/link.db"
trap 'rm -f "$database" "$symlink"; rmdir "$temporary_dir"' EXIT

cli=$1
"$cli" "$database" init >/dev/null
first=$("$cli" "$database" put "hello world")
second=$("$cli" "$database" put "second row")
[ "$first" = "0:0" ]
[ "$second" = "0:1" ]
[ "$("$cli" "$database" get "$first")" = "hello world" ]
[ "$("$cli" "$database" list)" = "$(printf '0:0\thello world\n0:1\tsecond row')" ]

"$cli" "$database" erase "$first" >/dev/null
[ "$("$cli" "$database" list)" = "$(printf '0:1\tsecond row')" ]
if "$cli" "$database" get "$first" >/dev/null 2>&1; then
  echo "deleted record remained readable" >&2
  exit 1
fi
replacement=$("$cli" "$database" put "replacement")
[ "$replacement" = "$first" ]
[ "$("$cli" "$database" get "$replacement")" = "replacement" ]

if "$cli" "$database" init >/dev/null 2>&1; then
  echo "init overwrote an existing database" >&2
  exit 1
fi
ln -s "$temporary_dir/missing.db" "$symlink"
if "$cli" "$symlink" init >/dev/null 2>&1; then
  echo "init followed a dangling symlink" >&2
  exit 1
fi
if "$cli" "$database" get "not-an-id" >/dev/null 2>&1; then
  echo "invalid record ID was accepted" >&2
  exit 1
fi
if "$cli" "$database" get "0:65536" >/dev/null 2>&1; then
  echo "out-of-range slot ID was accepted" >&2
  exit 1
fi
if "$cli" "$database" put "" >/dev/null 2>&1; then
  echo "empty record was accepted" >&2
  exit 1
fi
[ "$("$cli" "$database" get "$second")" = "second row" ]

"$cli" "$database" create-table People id:int name:text active:bool note:text? >/dev/null
ada=$("$cli" "$database" insert people 1 Ada true NULL)
bob=$("$cli" "$database" insert PEOPLE 2 "Bob Smith" false analyst)
grace=$("$cli" "$database" insert People 3 Grace true pioneer)
[ -n "$ada" ]
[ -n "$bob" ]
[ -n "$grace" ]
[ "$("$cli" "$database" query "SELECT name, id FROM people WHERE active = TRUE LIMIT 2;")" = \
  "$(printf 'name\tid\nAda\t1\nGrace\t3')" ]
[ "$("$cli" "$database" query "SELECT note FROM People WHERE id >= 1 LIMIT 2")" = \
  "$(printf 'note\nNULL\nanalyst')" ]
[ "$("$cli" "$database" query "SELECT name FROM People WHERE active = TRUE AND note IS NULL")" = \
  "$(printf 'name\nAda')" ]
[ "$("$cli" "$database" query "SELECT name FROM People WHERE note IS NOT NULL AND id >= 3")" = \
  "$(printf 'name\nGrace')" ]
[ "$("$cli" "$database" query "SELECT name FROM People ORDER BY id DESC LIMIT 2")" = \
  "$(printf 'name\nGrace\nBob Smith')" ]
[ "$("$cli" "$database" query "SELECT * FROM people LIMIT 0")" = \
  "$(printf 'id\tname\tactive\tnote')" ]
special=$(printf 'Tab\tName\\Path')
"$cli" "$database" insert people 4 "$special" false NULL >/dev/null
[ "$("$cli" "$database" query "SELECT name FROM people WHERE id = 4")" = \
  "$(printf 'name\nTab\\tName\\\\Path')" ]
"$cli" "$database" insert people +5 Plus false NULL >/dev/null
[ "$("$cli" "$database" query "SELECT id FROM people WHERE name = 'Plus'")" = \
  "$(printf 'id\n5')" ]

if "$cli" "$database" create-table people other:int >/dev/null 2>&1; then
  echo "case-insensitive duplicate table was accepted" >&2
  exit 1
fi
if "$cli" "$database" create-table broken invalid >/dev/null 2>&1; then
  echo "invalid column specification was accepted" >&2
  exit 1
fi
if "$cli" "$database" insert people 4 only-two >/dev/null 2>&1; then
  echo "row with wrong arity was accepted" >&2
  exit 1
fi
if "$cli" "$database" insert people nope Name true NULL >/dev/null 2>&1; then
  echo "invalid typed value was accepted" >&2
  exit 1
fi
if "$cli" "$database" query "SELECT missing FROM people" >/dev/null 2>&1; then
  echo "query with unknown column was accepted" >&2
  exit 1
fi
echo "PASS  CLI raw records, typed tables, filtering, ordering, and validation"

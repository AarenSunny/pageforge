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
echo "PASS  CLI persistence, deletion, reuse, and validation"

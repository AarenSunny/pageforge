#!/bin/sh
set -eu

benchmark=${1:?usage: benchmark_smoke.sh BENCHMARK}
temporary_dir=$(mktemp -d)
database="$temporary_dir/benchmark.db"
invalid_database="$temporary_dir/zero.db"
trap 'rm -f "$database" "$invalid_database"; rmdir "$temporary_dir"' EXIT

output=$("$benchmark" "$database" 200)
[ -f "$database" ]
printf '%s\n' "$output" | grep -q '^{"rows":200,"matched_rows":100,'
printf '%s\n' "$output" | grep -q '"database_bytes":[1-9][0-9]*'
printf '%s\n' "$output" | grep -q '"selected_id_checksum":9900}$'

if "$benchmark" "$database" 10 >/dev/null 2>&1; then
  echo "benchmark overwrote an existing database" >&2
  exit 1
fi
if "$benchmark" "$invalid_database" 0 >/dev/null 2>&1; then
  echo "benchmark accepted a zero row count" >&2
  exit 1
fi

echo "PASS  benchmark metrics, durability verification, and input validation"

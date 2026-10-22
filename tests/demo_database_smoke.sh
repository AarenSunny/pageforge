#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <pageforge-cli> <demo-generator>" >&2
    exit 2
fi

cli=$1
generator=$2
tempdir=$(mktemp -d "${TMPDIR:-/tmp}/pageforge-demo.XXXXXX")
database="$tempdir/demo.db"

cleanup() {
    rm -f -- "$database"
    rmdir "$tempdir"
}
trap cleanup EXIT HUP INT TERM

PAGEFORGE_CLI="$cli" "$generator" "$database" >/dev/null

schema=$(printf '.schema people\n.quit\n' | "$cli" "$database" shell)
expected_schema='people(id INTEGER NOT NULL, name TEXT NOT NULL, active BOOLEAN NOT NULL, note TEXT NULL) [schema version 1]'
[ "$schema" = "$expected_schema" ]

plan=$("$cli" "$database" query \
    "EXPLAIN SELECT name FROM people WHERE id = 3;")
expected_plan='INDEX_LOOKUP table=people index=people_id_idx key=3 predicates=1 residual_predicates=0 sort=false limit=none'
[ "$plan" = "$expected_plan" ]

quickstart_plan=$("$cli" "$database" query \
    "EXPLAIN SELECT name, id FROM people WHERE id >= 2 ORDER BY id DESC LIMIT 2;")
expected_quickstart_plan='INDEX_RANGE table=people index=people_id_idx lower=2 upper=none direction=reverse predicates=1 residual_predicates=0 sort=false limit=2 candidate_limit=2'
[ "$quickstart_plan" = "$expected_quickstart_plan" ]

quickstart_rows=$("$cli" "$database" query \
    "SELECT name, id FROM people WHERE id >= 2 ORDER BY id DESC LIMIT 2;")
expected_quickstart_rows='name	id
Katherine Johnson	5
Linus Torvalds	4'
[ "$quickstart_rows" = "$expected_quickstart_rows" ]

names=$("$cli" "$database" query \
    "SELECT name FROM people WHERE active = TRUE ORDER BY id;")
expected_names='name
Ada Lovelace
Grace Hopper
Margaret Hamilton
Katherine Johnson'
[ "$names" = "$expected_names" ]

if PAGEFORGE_CLI="$cli" "$generator" "$database" >/dev/null 2>&1; then
    echo "demo generator unexpectedly overwrote an existing database" >&2
    exit 1
fi

row=$("$cli" "$database" query "SELECT name FROM people WHERE id = 3;")
[ "$row" = "$(printf 'name\nMargaret Hamilton')" ]

echo "demo database smoke test: PASS"

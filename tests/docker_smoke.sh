#!/bin/sh
set -eu

image=${1:?usage: docker_smoke.sh IMAGE}
volume="pageforge-smoke-$$"
trap 'docker volume rm "$volume" >/dev/null 2>&1 || true' EXIT

docker volume create "$volume" >/dev/null

run_pageforge() {
  docker run --rm --mount "source=$volume,target=/data" "$image" "$@"
}

run_pageforge demo.db init >/dev/null
run_pageforge demo.db create-table people id:int name:text active:bool >/dev/null
run_pageforge demo.db insert people 1 Ada true >/dev/null
run_pageforge demo.db insert people 2 Grace true >/dev/null
run_pageforge demo.db insert people 3 Bob false >/dev/null

result=$(run_pageforge demo.db query \
  "SELECT name, id FROM people WHERE active = TRUE ORDER BY id DESC;")
[ "$result" = "$(printf 'name\tid\nGrace\t2\nAda\t1')" ]

shell_result=$(printf '%s\n' '.tables' '.schema people' '.quit' | \
  docker run --rm -i --mount "source=$volume,target=/data" "$image" demo.db shell)
[ "$shell_result" = "$(printf '%s\n' \
  'people' \
  'people(id INTEGER NOT NULL, name TEXT NOT NULL, active BOOLEAN NOT NULL) [schema version 1]')" ]

echo "PASS  Docker image persistence, SQL query, and non-interactive shell"

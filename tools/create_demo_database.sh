#!/bin/sh
set -eu

usage() {
    echo "usage: PAGEFORGE_CLI=./build/pageforge $0 <database>" >&2
}

if [ "$#" -ne 1 ]; then
    usage
    exit 2
fi

database=$1
cli=${PAGEFORGE_CLI:-./build/pageforge}

if [ ! -x "$cli" ]; then
    echo "PageForge CLI is not executable: $cli" >&2
    exit 1
fi

if [ -e "$database" ] || [ -L "$database" ]; then
    echo "Refusing to overwrite existing path: $database" >&2
    exit 1
fi

created=0
cleanup() {
    status=$?
    if [ "$status" -ne 0 ] && [ "$created" -eq 1 ]; then
        rm -f -- "$database"
    fi
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

"$cli" "$database" init >/dev/null
created=1
"$cli" "$database" create-table people id:int name:text active:bool "note:text?" >/dev/null
"$cli" "$database" insert people 1 "Ada Lovelace" true NULL >/dev/null
"$cli" "$database" insert people 2 "Grace Hopper" true compiler >/dev/null
"$cli" "$database" insert people 3 "Margaret Hamilton" true guidance >/dev/null
"$cli" "$database" insert people 4 "Linus Torvalds" false kernel >/dev/null
"$cli" "$database" insert people 5 "Katherine Johnson" true orbital >/dev/null
"$cli" "$database" create-index people_id_idx people id >/dev/null

trap - EXIT HUP INT TERM
printf 'Created PageForge demo database: %s\n' "$database"

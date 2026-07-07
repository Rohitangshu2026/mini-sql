#!/usr/bin/env bash
#
# Black-box tests: pipe commands into the REPL, assert on its output.
# Mirrors cstack's rspec suite, adapted to this project's output.
#
# Usage:
#   MINI_SQL_BIN=./build/mini_sql tests/run_tests.sh [test_name]
#   (no test_name runs them all; CTest invokes one name per registered test)
#
set -u

DB="${MINI_SQL_BIN:-./build/mini_sql}"
TESTDB="${TMPDIR:-/tmp}/mini_sql_test.db"

# Strip our timing suffix " (12.345 ms)" and trailing whitespace so the
# output is deterministic and prompt spacing doesn't matter.
normalize() {
    sed -E -e 's/ \([0-9]+\.[0-9]+ ms\)//' -e 's/[[:space:]]+$//'
}

# run INPUT -> normalized output on stdout.
# The binary now needs a database file; start each case from an empty one.
run() {
    rm -f "$TESTDB"
    printf '%s' "$1" | "$DB" "$TESTDB" | normalize
}

# want OUTPUT NEEDLE -> 0 if NEEDLE appears as a literal substring
want() {
    grep -qF -- "$2" <<<"$1"
}

t_inserts_and_retrieves() {
    local out
    out=$(run $'insert 1 user1 person1@example.com\nselect\n.exit\n')
    want "$out" "db > (1, user1, person1@example.com)"
}

t_table_full() {
    # row_size = 4 + 32 + 255 = 291; 4096/291 = 14 rows/page * 100 pages = 1400 max.
    # Inserting the 1401st must report the table is full.
    local script="" i
    for i in $(seq 1 1401); do
        script+="insert $i user$i person$i@example.com"$'\n'
    done
    script+=".exit"$'\n'
    local out
    out=$(run "$script")
    want "$out" "Error: Table full."
}

t_max_length_strings() {
    local u e out
    u=$(printf 'a%.0s' $(seq 1 32))    # 32-char username (the max)
    e=$(printf 'a%.0s' $(seq 1 255))   # 255-char email   (the max)
    out=$(run "insert 1 $u $e"$'\n'"select"$'\n'".exit"$'\n')
    want "$out" "(1, $u, $e)"
}

t_string_too_long() {
    local u e out
    u=$(printf 'a%.0s' $(seq 1 33))    # one over the limit
    e=$(printf 'a%.0s' $(seq 1 256))
    out=$(run "insert 1 $u $e"$'\n'"select"$'\n'".exit"$'\n')
    want "$out" "String is too long."
}

t_negative_id() {
    local out
    out=$(run $'insert -1 cstack foo@bar.com\nselect\n.exit\n')
    want "$out" "ID must be positive."
}

t_persistence() {
    # Insert then exit (flushes to disk), reopen the SAME file, and read it back.
    rm -f "$TESTDB"
    printf 'insert 1 user1 person1@example.com\n.exit\n' | "$DB" "$TESTDB" >/dev/null
    local out
    out=$(printf 'select\n.exit\n' | "$DB" "$TESTDB" | normalize)
    want "$out" "(1, user1, person1@example.com)"
}

ALL=(inserts_and_retrieves table_full max_length_strings string_too_long negative_id persistence)

run_one() {
    if "t_$1"; then
        echo "ok   - $1"
        return 0
    else
        echo "FAIL - $1"
        return 1
    fi
}

rc=0
if [[ "${1:-all}" == "all" ]]; then
    for t in "${ALL[@]}"; do run_one "$t" || rc=1; done
else
    run_one "$1" || rc=1
fi
exit "$rc"

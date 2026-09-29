#!/usr/bin/env bash
#
# Black-box tests: pipe commands into the REPL, assert on its output.
# Mirrors cstack's rspec suite, adapted to this project's output.
#
# Usage:
#   MINI_SQL_BIN=./build/mini_sql \
#   MINI_SQL_SMALL_FANOUT_BIN=./build/mini_sql_small_fanout \
#   tests/run_tests.sh [test_name]
#   (no test_name runs them all; CTest invokes one name per registered test)
#
# Most tests drive the real binary. A few need internal nodes that fill up
# after a handful of rows, and drive the small-fan-out test build instead by
# declaring `local DB="$SMALL_FANOUT_DB"`, which the helpers below then use.
#
set -u

DB="${MINI_SQL_BIN:-./build/mini_sql}"
SMALL_FANOUT_DB="${MINI_SQL_SMALL_FANOUT_BIN:-./build/mini_sql_small_fanout}"
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

# btree_block OUTPUT -> just the tree printed by .btree: every line after the
# "Tree:" header up to the next prompt. Lets tests compare a tree's shape
# exactly — substring checks can't, since "- 1" also matches "- 10".
btree_block() {
    awk '/Tree:$/{f=1; next} /^db >/{f=0} f' <<<"$1"
}

# inserts ID... -> a script inserting one row per id, in the order given
inserts() {
    local id
    for id in "$@"; do
        printf 'insert %s user%s person%s@example.com\n' "$id" "$id" "$id"
    done
}

# select_rows OUTPUT -> just the rows a select printed, one per line, with the
# "db > " prompt that precedes the first one stripped off.
select_rows() {
    sed -nE 's/^(db > )?(\([0-9]+, .*\))$/\2/p' <<<"$1"
}

# expected_rows ID... -> the rows select should print for these ids, in order
expected_rows() {
    local id
    for id in "$@"; do
        printf '(%s, user%s, person%s@example.com)\n' "$id" "$id" "$id"
    done
}

# tree_over_leaves RANGE... -> what .btree prints for an internal root over one
# leaf per RANGE ("first-last", every id in between), left to right. Each leaf
# but the last is followed by its separator: the leaf's own largest key.
tree_over_leaves() {
    local range first last id remaining=$#
    printf -- '- internal (size %s)\n' $((remaining - 1))
    for range in "$@"; do
        first=${range%-*}
        last=${range#*-}
        printf -- '  - leaf (size %s)\n' $((last - first + 1))
        for id in $(seq "$first" "$last"); do
            printf -- '    - %s\n' "$id"
        done
        remaining=$((remaining - 1))
        if (( remaining > 0 )); then
            printf -- '  - key %s\n' "$last"
        fi
    done
}

# The 30-row insert order from cstack's Part 13 test, whose resulting tree the
# article prints (a 4-leaf tree with a mis-read first separator; see below).
CSTACK_4_LEAF_ORDER="18 7 10 29 23 4 14 30 15 26 22 19 2 1 21 11 6 20 5 8 9 3 12 27 17 16 13 24 25 28"

# The shape any 14-row insert order must produce: the full root leaf split
# 7/7 under a new internal root whose one key is the left leaf's maximum.
SPLIT_TREE=$(cat <<'EOF'
- internal (size 1)
  - leaf (size 7)
    - 1
    - 2
    - 3
    - 4
    - 5
    - 6
    - 7
  - key 7
  - leaf (size 7)
    - 8
    - 9
    - 10
    - 11
    - 12
    - 13
    - 14
EOF
)

t_inserts_and_retrieves() {
    local out
    out=$(run $'insert 1 user1 person1@example.com\nselect\n.exit\n')
    want "$out" "db > (1, user1, person1@example.com)"
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

t_constants() {
    # Our numbers differ from cstack's: row_size is 291 (no +1 null bytes),
    # and the node format keeps uint32 fields 4-byte aligned (8-byte common
    # header, cell padded 295 -> 296) so pointer accessors aren't UB. The leaf
    # header is 16 bytes: common header, cell count, next-leaf pointer.
    local out
    out=$(run $'.constants\n.exit\n')
    want "$out" "ROW_SIZE: 291" &&
    want "$out" "COMMON_NODE_HEADER_SIZE: 8" &&
    want "$out" "LEAF_NODE_HEADER_SIZE: 16" &&
    want "$out" "LEAF_NODE_CELL_SIZE: 296" &&
    want "$out" "LEAF_NODE_SPACE_FOR_CELLS: 4080" &&
    want "$out" "LEAF_NODE_MAX_CELLS: 13" &&
    want "$out" "INTERNAL_NODE_MAX_KEYS: 510"
}

t_btree_one_node() {
    # Inserted 3, 1, 2 but stored sorted: insert seeks the key's position
    # instead of appending.
    local out expected
    out=$(run "$(inserts 3 1 2)"$'\n.btree\n.exit\n')
    expected=$'- leaf (size 3)\n  - 1\n  - 2\n  - 3'
    [[ "$(btree_block "$out")" == "$expected" ]]
}

t_btree_split() {
    # The 14th row overflows the root leaf. Each order below drives the split
    # loop down a different branch, and every one must yield the same tree:
    #   ascending        -> new key lands at the tail, in the right node
    #   descending       -> new key lands at the head, in the left node
    #   1-6, 8-14, 7     -> new key is the left node's last cell
    #   1-7, 9-14, 8     -> new key is the right node's first cell (the
    #                       exact left/right boundary, the likeliest off-by-one)
    local order out
    for order in "$(seq 1 14)" "$(seq 14 -1 1)" "$(seq 1 6) $(seq 8 14) 7" "$(seq 1 7) $(seq 9 14) 8"; do
        out=$(run "$(inserts $order)"$'\n.btree\n.exit\n')
        [[ "$(btree_block "$out")" == "$SPLIT_TREE" ]] || return 1
    done
}

t_btree_split_persists() {
    # A split leaves three pages; all must reach disk, and the root must be
    # read back as an internal node when the file is reopened.
    rm -f "$TESTDB"
    { inserts $(seq 1 14); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    local out
    out=$(printf '.btree\n.exit\n' | "$DB" "$TESTDB" | normalize)
    [[ "$(btree_block "$out")" == "$SPLIT_TREE" ]]
}

t_btree_insert_after_split() {
    # Even ids 2-28 split into 2-14 | 16-28 under separator 14. Later inserts
    # must descend through the internal root to the right leaf:
    #   3  -> left              15 -> right (separator + 1)
    #   29 -> the right child, past every separator
    #   1  -> left, below every key
    #   14 -> equals the separator, so it must route LEFT, where that row
    #         already lives, and be rejected as a duplicate
    #   28 -> a duplicate found in the right leaf
    local out expected
    out=$(run "$(inserts $(seq 2 2 28) 3 15 29 1 14 28)"$'\n.btree\n.exit\n')
    [[ $(grep -cF 'Error: Duplicate key.' <<<"$out") -eq 2 ]] || return 1
    expected=$(cat <<'EOF'
- internal (size 1)
  - leaf (size 9)
    - 1
    - 2
    - 3
    - 4
    - 6
    - 8
    - 10
    - 12
    - 14
  - key 14
  - leaf (size 9)
    - 15
    - 16
    - 18
    - 20
    - 22
    - 24
    - 26
    - 28
    - 29
EOF
)
    [[ "$(btree_block "$out")" == "$expected" ]]
}

t_select_multilevel() {
    # A select must walk the leaf chain and return every row in key order.
    # The four orders from t_btree_split each place the row inserted *during*
    # the split somewhere different, so a split that misplaced that row's key
    # or value would show up here as a row whose id and name disagree.
    local order out
    for order in "$(seq 1 14)" "$(seq 14 -1 1)" "$(seq 1 6) $(seq 8 14) 7" "$(seq 1 7) $(seq 9 14) 8"; do
        out=$(run "$(inserts $order 15)"$'\nselect\n.exit\n')
        [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 15))" ]] || return 1
    done

    # 20 rows fill the right leaf completely; the chain must survive a reopen.
    rm -f "$TESTDB"
    { inserts $(seq 1 20); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    out=$(printf 'select\n.exit\n' | "$DB" "$TESTDB" | normalize)
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 20))" ]]
}

t_select_empty() {
    # A scan of an empty table starts by searching the empty root leaf for
    # key 0; that must mean "nothing to scan", not a phantom row.
    local out
    out=$(run $'select\n.exit\n')
    [[ -z "$(select_rows "$out")" ]] && want "$out" "Executed."
}

t_btree_nonroot_split() {
    # Thirty rows split leaves that aren't the root, so the parent has to take
    # a lowered separator for the old leaf and a new child for its upper half.
    # Each order drives that fix-up down a different path:
    #   ascending      -> the rightmost leaf keeps splitting, so each new leaf
    #                     takes over the root's right-child slot and the old
    #                     right child moves down into a cell
    #   descending     -> the leftmost leaf keeps splitting, so its separator
    #                     is lowered and later cells shift right for the new one
    #   cstack's order -> must build the tree the article prints, except for its
    #                     first separator: the article shows "key 1" there, from
    #                     the pointer-arithmetic bug in its internal_node_key
    #                     that this project never had, and the right key is 7
    # A full select must return all 30 rows in order across the four leaves.
    local orders=("$(seq 1 30)" "$(seq 30 -1 1)" "$CSTACK_4_LEAF_ORDER")
    local leaves=("1-7 8-14 15-21 22-30" "1-9 10-16 17-23 24-30" "1-7 8-15 16-22 23-30")
    local i out
    for i in 0 1 2; do
        out=$(run "$(inserts ${orders[i]})"$'\n.btree\nselect\n.exit\n')
        [[ "$(btree_block "$out")" == "$(tree_over_leaves ${leaves[i]})" ]] || return 1
        [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 30))" ]] || return 1
    done

    # With separators 7 | 14 | 21, re-inserting 14 and 21 (equal to a separator,
    # so routed left to the leaf holding them) and 22 (just past the last one,
    # so routed to the right child) must all be rejected as duplicates. The
    # tree and every row must then survive a reopen, all five pages intact.
    out=$(run "$(inserts $(seq 1 30) 14 21 22)"$'\n.exit\n')
    [[ $(grep -cF 'Error: Duplicate key.' <<<"$out") -eq 3 ]] || return 1
    out=$(printf '.btree\nselect\n.exit\n' | "$DB" "$TESTDB" | normalize)
    [[ "$(btree_block "$out")" == "$(tree_over_leaves 1-7 8-14 15-21 22-30)" ]] &&
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 30))" ]]
}

t_internal_split_unimplemented() {
    # Pins the boundary still standing: a full internal node can't take another
    # child until internal nodes can split. The real binary runs out of pages
    # long before a 510-key root fills, so this drives the test build capped at
    # 3 keys, after first checking it really is that build. Ascending inserts
    # give the root its third key at row 28 and fill the rightmost leaf at row
    # 34, so row 35 must stop there, right after row 34 succeeds.
    local DB="$SMALL_FANOUT_DB" out
    out=$(run $'.constants\n.exit\n')
    want "$out" "INTERNAL_NODE_MAX_KEYS: 3" || return 1
    out=$(run "$(inserts $(seq 1 35))"$'\n.exit\n')
    [[ "$(tail -n 2 <<<"$out")" == $'db > Executed.\ndb > Need to implement splitting internal node' ]]
}

t_duplicate_key() {
    # The second insert of id 1 must be rejected, leaving exactly one row.
    local out
    out=$(run $'insert 1 user1 person1@example.com\ninsert 1 user1 person1@example.com\nselect\n.exit\n')
    want "$out" "Error: Duplicate key." &&
    [[ $(grep -cF '(1, user1, person1@example.com)' <<<"$out") -eq 1 ]]
}

ALL=(inserts_and_retrieves max_length_strings string_too_long negative_id persistence constants btree_one_node duplicate_key btree_split btree_split_persists btree_insert_after_split select_multilevel select_empty btree_nonroot_split internal_split_unimplemented)

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

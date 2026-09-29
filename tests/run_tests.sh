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

# The table most tests work with. Its 291-byte rows fit 13 to a leaf, the
# layout every tree-shape expectation below is built around.
USERS_DDL="CREATE TABLE users (id INT PRIMARY KEY, username TEXT(32), email TEXT(255));"

# fresh_db -> replaces $TESTDB with a new database holding an empty users
# table. The table is created by a separate run of the binary, so that run's
# output never mixes with the output a test inspects.
fresh_db() {
    rm -f "$TESTDB"
    printf '%s\n.exit\n' "$USERS_DDL" | "$DB" "$TESTDB" >/dev/null
}

# run INPUT -> normalized output on stdout, starting from a fresh database
# that already holds the users table.
run() {
    fresh_db
    printf '%s' "$1" | "$DB" "$TESTDB" | normalize
}

# run_empty INPUT -> like run, but starting from a database with no tables.
run_empty() {
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
        printf "INSERT INTO users VALUES (%s, 'user%s', 'person%s@example.com');\n" "$id" "$id" "$id"
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

# error_lines OUTPUT -> every error message printed, one per line, with the
# "db > " prompt stripped off
error_lines() {
    sed -nE 's/^(db > )+((Syntax error|Type error|Error): .*)$/\2/p' <<<"$1"
}

# check_error_cases INPUT MESSAGE [INPUT MESSAGE ...] -> runs every INPUT in
# one session, followed by one valid insert and a select, and succeeds if each
# INPUT printed exactly its MESSAGE, in order, and none of them changed the
# table: the select must return the valid row and nothing else.
check_error_cases() {
    local inputs=() messages=() out
    while (( $# >= 2 )); do
        inputs+=("$1")
        messages+=("$2")
        shift 2
    done
    out=$(run "$(printf '%s\n' "${inputs[@]}")"$'\n'"$(inserts 1)"$'\nSELECT * FROM users;\n.exit\n')
    [[ "$(error_lines "$out")" == "$(printf '%s\n' "${messages[@]}")" ]] &&
    [[ "$(select_rows "$out")" == "$(expected_rows 1)" ]]
}

# The users table's leaf capacity (pinned by t_constants).
LEAF_MAX_CELLS=13

# btree_check OUTPUT MAX_KEYS [MAX_CELLS] -> checks the tree printed by .btree
# against the B+ tree invariants and prints its height (the number of levels,
# leaves included). MAX_CELLS is the table's leaf capacity, the users table's
# by default. Fails, naming the first broken rule, if:
#   - leaf keys don't strictly increase from the first leaf to the last
#   - a separator isn't the last leaf key printed before it, i.e. the largest
#     key in the subtree on its left
#   - a node's printed size disagrees with what's printed under it, or an
#     internal node doesn't alternate child, key, child ... child
#   - a node is empty or holds more than it may (MAX_CELLS / MAX_KEYS)
#   - leaves sit at different depths
# Lets a test cover a tree of any size and insertion order without spelling
# the whole tree out.
btree_check() {
    btree_block "$1" | awk -v max_keys="$2" -v max_cells="${3:-$LEAF_MAX_CELLS}" '
        function fail(msg) {
            printf "btree_check: %s (line %d: %s)\n", msg, NR, $0 > "/dev/stderr"
            failed = 1
            exit 1
        }
        # closes every open node at depth >= d, checking its counts
        function close_to(d) {
            while (top > 0 && depth[top] >= d) {
                if (entries[top] != size[top])
                    fail(kind[top] " prints " entries[top] " entries but claims size " size[top])
                if (kind[top] == "internal" && children[top] != size[top] + 1)
                    fail("internal node has " children[top] " children for " size[top] " keys")
                top--
            }
        }
        BEGIN { top = 0; last = -1; leaf_depth = -1 }
        {
            match($0, /^ */)
            if (RLENGTH % 2) fail("odd indentation")
            d = RLENGTH / 2
            line = substr($0, RLENGTH + 1)
        }
        line ~ /^- (internal|leaf) \(size [0-9]+\)$/ {
            close_to(d)
            if (top == 0 && NR > 1) fail("more than one root")
            if (top > 0) {
                if (kind[top] != "internal" || depth[top] != d - 1) fail("node outside an internal node")
                if (children[top] != entries[top]) fail("two children with no separator between them")
                children[top]++
            }
            n = line
            sub(/.*size /, "", n)
            sub(/\)$/, "", n)
            top++
            kind[top] = (line ~ /internal/) ? "internal" : "leaf"
            depth[top] = d
            size[top] = n + 0
            entries[top] = 0
            children[top] = 0
            if (size[top] < 1) fail("empty node")
            if (kind[top] == "leaf" && size[top] > max_cells) fail("leaf over capacity")
            if (kind[top] == "internal" && size[top] > max_keys) fail("internal node over capacity")
            if (kind[top] == "leaf") {
                if (leaf_depth < 0) leaf_depth = d
                else if (d != leaf_depth) fail("leaves at different depths")
            }
            next
        }
        line ~ /^- key [0-9]+$/ {
            close_to(d)
            if (top == 0 || kind[top] != "internal" || depth[top] != d - 1) fail("separator outside an internal node")
            if (children[top] != entries[top] + 1) fail("separator not preceded by a child")
            k = substr(line, 7) + 0
            if (k != last) fail("separator " k " is not the largest key on its left (" last ")")
            entries[top]++
            next
        }
        line ~ /^- [0-9]+$/ {
            if (top == 0 || kind[top] != "leaf" || depth[top] != d - 1) fail("key outside a leaf")
            k = substr(line, 3) + 0
            if (k <= last) fail("key " k " does not follow " last)
            last = k
            entries[top]++
            next
        }
        { fail("unrecognized line") }
        END {
            if (failed) exit 1
            if (NR == 0) fail("no tree printed")
            close_to(0)
            print leaf_depth + 1
        }
    '
}

# scrambled P -> 1 .. P-1 in a fixed scrambled order: i * 53 mod P for a prime
# P above 53, which visits every value exactly once. The same on every
# platform, unlike awk's rand(). The multiplier is chosen so that P = 211
# reaches the split the tutorial gets wrong (see t_btree_internal_split); many
# others never do.
scrambled() {
    local i
    for (( i = 1; i < $1; i++ )); do
        echo $(( i * 53 % $1 ))
    done
}

# require_small_fanout -> 0 if $DB is the test build whose internal nodes hold
# at most 3 keys. Tests that only mean something on that build check it first,
# so a misregistered binary fails loudly instead of passing vacuously.
require_small_fanout() {
    want "$(run $'.constants\n.exit\n')" "INTERNAL_NODE_MAX_KEYS: 3"
}

# The 30-row insert order from cstack's Part 13 test, whose resulting tree the
# article prints (a 4-leaf tree with a mis-read first separator; see below).
CSTACK_4_LEAF_ORDER="18 7 10 29 23 4 14 30 15 26 22 19 2 1 21 11 6 20 5 8 9 3 12 27 17 16 13 24 25 28"

# The 64-row insert order from cstack's Part 14 test, and the 3-level, 7-leaf
# tree the article prints for it with internal nodes capped at 3 keys.
CSTACK_7_LEAF_ORDER="58 56 8 54 77 7 25 71 13 22 53 51 59 32 36 79 10 33 20 4 35 76 49 24 70 48 39 15 47 30 86 31 68 37 66 63 40 78 19 46 14 81 72 6 50 85 67 2 55 69 5 65 52 1 29 9 43 75 21 82 12 18 60 44"
CSTACK_7_LEAF_TREE=$(cat <<'EOF'
- internal (size 1)
  - internal (size 2)
    - leaf (size 7)
      - 1
      - 2
      - 4
      - 5
      - 6
      - 7
      - 8
    - key 8
    - leaf (size 11)
      - 9
      - 10
      - 12
      - 13
      - 14
      - 15
      - 18
      - 19
      - 20
      - 21
      - 22
    - key 22
    - leaf (size 8)
      - 24
      - 25
      - 29
      - 30
      - 31
      - 32
      - 33
      - 35
  - key 35
  - internal (size 3)
    - leaf (size 12)
      - 36
      - 37
      - 39
      - 40
      - 43
      - 44
      - 46
      - 47
      - 48
      - 49
      - 50
      - 51
    - key 51
    - leaf (size 11)
      - 52
      - 53
      - 54
      - 55
      - 56
      - 58
      - 59
      - 60
      - 63
      - 65
      - 66
    - key 66
    - leaf (size 7)
      - 67
      - 68
      - 69
      - 70
      - 71
      - 72
      - 75
    - key 75
    - leaf (size 8)
      - 76
      - 77
      - 78
      - 79
      - 81
      - 82
      - 85
      - 86
EOF
)

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
    out=$(run $'INSERT INTO users VALUES (1, \'user1\', \'person1@example.com\');\nSELECT * FROM users;\n.exit\n')
    want "$out" "db > (1, user1, person1@example.com)"
}

t_max_length_strings() {
    local u e out
    u=$(printf 'a%.0s' $(seq 1 32))    # 32-char username (the max)
    e=$(printf 'a%.0s' $(seq 1 255))   # 255-char email   (the max)
    out=$(run "INSERT INTO users VALUES (1, '$u', '$e');"$'\nSELECT * FROM users;\n.exit\n')
    want "$out" "(1, $u, $e)"
}

t_string_too_long() {
    # One byte over either column's width is a type error naming the column,
    # and nothing is inserted.
    local u e out
    u=$(printf 'a%.0s' $(seq 1 33))    # one over the username limit
    e=$(printf 'a%.0s' $(seq 1 256))   # one over the email limit
    out=$(run "INSERT INTO users VALUES (1, '$u', 'x');"$'\n'"INSERT INTO users VALUES (1, 'x', '$e');"$'\nSELECT * FROM users;\n.exit\n')
    want "$out" "Type error: column 'username' is TEXT(32), but the value is 33 bytes." &&
    want "$out" "Type error: column 'email' is TEXT(255), but the value is 256 bytes." &&
    [[ -z "$(select_rows "$out")" ]]
}

t_negative_id() {
    local out
    out=$(run $'INSERT INTO users VALUES (-1, \'cstack\', \'foo@bar.com\');\nSELECT * FROM users;\n.exit\n')
    want "$out" "Error: column 'id' must not be negative." &&
    [[ -z "$(select_rows "$out")" ]]
}

t_persistence() {
    # Insert then exit (flushes to disk), reopen the SAME file, and read it back.
    fresh_db
    { inserts 1; printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    local out
    out=$(printf 'SELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    want "$out" "(1, user1, person1@example.com)"
}

t_constants() {
    # Our numbers differ from cstack's: row_size is 291 (no +1 null bytes),
    # and the node format keeps uint32 fields 4-byte aligned (8-byte common
    # header, cell padded 295 -> 296) so pointer accessors aren't UB. The leaf
    # header is 16 bytes: common header, cell count, next-leaf pointer.
    local out
    out=$(run $'.constants users\n.exit\n')
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
    out=$(run "$(inserts 3 1 2)"$'\n.btree users\n.exit\n')
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
        out=$(run "$(inserts $order)"$'\n.btree users\n.exit\n')
        [[ "$(btree_block "$out")" == "$SPLIT_TREE" ]] || return 1
    done
}

t_btree_split_persists() {
    # A split leaves three pages; all must reach disk, and the root must be
    # read back as an internal node when the file is reopened.
    fresh_db
    { inserts $(seq 1 14); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    local out
    out=$(printf '.btree users\n.exit\n' | "$DB" "$TESTDB" | normalize)
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
    out=$(run "$(inserts $(seq 2 2 28) 3 15 29 1 14 28)"$'\n.btree users\n.exit\n')
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
        out=$(run "$(inserts $order 15)"$'\nSELECT * FROM users;\n.exit\n')
        [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 15))" ]] || return 1
    done

    # 20 rows fill the right leaf completely; the chain must survive a reopen.
    fresh_db
    { inserts $(seq 1 20); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    out=$(printf 'SELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 20))" ]]
}

t_select_empty() {
    # A scan of an empty table starts by searching the empty root leaf for
    # key 0; that must mean "nothing to scan", not a phantom row.
    local out
    out=$(run $'SELECT * FROM users;\n.exit\n')
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
        out=$(run "$(inserts ${orders[i]})"$'\n.btree users\nSELECT * FROM users;\n.exit\n')
        [[ "$(btree_block "$out")" == "$(tree_over_leaves ${leaves[i]})" ]] || return 1
        [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 30))" ]] || return 1
    done

    # With separators 7 | 14 | 21, re-inserting 14 and 21 (equal to a separator,
    # so routed left to the leaf holding them) and 22 (just past the last one,
    # so routed to the right child) must all be rejected as duplicates. The
    # tree and every row must then survive a reopen, all five pages intact.
    out=$(run "$(inserts $(seq 1 30) 14 21 22)"$'\n.exit\n')
    [[ $(grep -cF 'Error: Duplicate key.' <<<"$out") -eq 3 ]] || return 1
    out=$(printf '.btree users\nSELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    [[ "$(btree_block "$out")" == "$(tree_over_leaves 1-7 8-14 15-21 22-30)" ]] &&
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 30))" ]]
}

t_btree_internal_split() {
    # A full internal node splits like a leaf: half its children move to a new
    # sibling, the pending child joins whichever half owns its keys, and the
    # parent gets a lowered separator plus the sibling — splitting in turn if
    # it's full too, up to a new root. The real binary's 510-key nodes never
    # fill within the page cache, so this runs on the 3-key test build.
    #
    # cstack's 64-row order must build the tree the article prints, compared
    # line for line (the article's own test compares the lines as an unordered
    # set), and a select must return every row in key order.
    local DB="$SMALL_FANOUT_DB" order out height
    require_small_fanout || return 1
    out=$(run "$(inserts $CSTACK_7_LEAF_ORDER)"$'\n.btree users\nSELECT * FROM users;\n.exit\n')
    [[ "$(btree_block "$out")" == "$CSTACK_7_LEAF_TREE" ]] || return 1
    [[ "$(select_rows "$out")" == "$(expected_rows $(printf '%s\n' $CSTACK_7_LEAF_ORDER | sort -n))" ]] || return 1

    # 210 rows ascending (every split on the rightmost path), descending (every
    # split on the leftmost path, so children shift right and the pending child
    # stays in the lower half) and scrambled must each leave a valid tree at
    # least four levels tall — the root split while its children were already
    # internal — with every row reachable.
    #
    # The scrambled order splits a node that is the last child its parent keeps
    # when the parent splits too, which sends the node's new sibling to the
    # parent's new half. The tutorial then points the sibling back at the old
    # half, and the sibling's own next split files a child in the wrong
    # subtree. A scan still finds every row through the leaf chain; only the
    # separator check in btree_check sees the damage.
    for order in "$(seq 1 210)" "$(seq 210 -1 1)" "$(scrambled 211)"; do
        out=$(run "$(inserts $order)"$'\n.btree users\nSELECT * FROM users;\n.exit\n')
        height=$(btree_check "$out" 3) && (( height >= 4 )) || return 1
        [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 210))" ]] || return 1
    done
}

t_btree_internal_split_persists() {
    # Splits find the node to update through parent pointers, and after a
    # reopen those come from disk. Even ids 2-200 build a four-level tree; odd
    # ids 199-1, inserted after a reopen, land in every leaf and set off splits
    # all over it. The tree must stay valid with every row reachable, and still
    # be so after one more reopen.
    local DB="$SMALL_FANOUT_DB" out height
    require_small_fanout || return 1
    fresh_db
    { inserts $(seq 2 2 200); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    out=$(printf '.btree users\n.exit\n' | "$DB" "$TESTDB" | normalize)
    height=$(btree_check "$out" 3) && (( height >= 4 )) || return 1

    out=$({ inserts $(seq 199 -2 1); printf '.btree users\nSELECT * FROM users;\n.exit\n'; } | "$DB" "$TESTDB" | normalize)
    btree_check "$out" 3 >/dev/null || return 1
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 200))" ]] || return 1

    out=$(printf '.btree users\nSELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    btree_check "$out" 3 >/dev/null &&
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 200))" ]]
}

t_large_table() {
    # The page cache grows with the file instead of stopping at 100 pages, which
    # takes the real binary to its first internal split. Ascending rows leave 7
    # per leaf, so 3,583 of them fill 511 leaves under a root holding exactly
    # 510 keys — full. Row 3,584 splits a leaf into that full root, so the root
    # splits too and the tree grows a third level. Every row must be reachable,
    # and still be after a reopen.
    local out height
    out=$(run "$(inserts $(seq 1 3583))"$'\n.btree users\n.exit\n')
    height=$(btree_check "$out" 510) && (( height == 2 )) || return 1
    want "$out" "- internal (size 510)" || return 1

    out=$(run "$(inserts $(seq 1 3584))"$'\n.btree users\nSELECT * FROM users;\n.exit\n')
    height=$(btree_check "$out" 510) && (( height == 3 )) || return 1
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 3584))" ]] || return 1

    out=$(printf '.btree users\nSELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    height=$(btree_check "$out" 510) && (( height == 3 )) &&
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 3584))" ]]
}

t_file_header() {
    # Page 0 of a new database is its header: the magic string, then format 2,
    # 4096-byte pages and the catalog's root on page 1 (the fields are read in
    # the host's byte order, like the file writes them). The first table
    # created gets page 2 for its root. The header doesn't change as the table
    # grows or across a reopen; the catalog stays a one-row leaf, while the
    # users root becomes internal after the first split.
    local copy="$TESTDB.copy" script
    fresh_db
    { inserts $(seq 1 30); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    [[ "$(head -c 16 "$TESTDB" | tr '\0' '@')" == "mini-sql format@" ]] || return 1
    [[ "$(od -An -tu4 -j16 -N12 "$TESTDB" | xargs)" == "2 4096 1" ]] || return 1
    [[ "$(od -An -tu1 -j4096 -N2 "$TESTDB" | xargs)" == "1 1" ]] || return 1   # catalog: leaf, root
    [[ "$(od -An -tu1 -j8192 -N2 "$TESTDB" | xargs)" == "0 1" ]] || return 1   # users: internal, root

    # The rest of page 0 is reserved for fields later formats add, which will
    # read an old file's zeros as "not set", so it must be zero.
    [[ "$(head -c 4096 "$TESTDB" | tail -c 4068 | tr -d '\0' | wc -c | xargs)" == 0 ]] || return 1

    head -c 4096 "$TESTDB" >"$copy"
    { inserts $(seq 31 60); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    head -c 4096 "$TESTDB" | cmp -s - "$copy" || return 1

    # The same statements always produce the same file, byte for byte — even
    # after a stream of rejected statements has churned the heap. New pages
    # start zeroed, so bytes nothing writes (cell padding, the tails of fresh
    # pages) can't carry leftover memory. On macOS a fresh 4 KiB allocation
    # happens to be zeroed anyway, so this check alone can't tell calloc from
    # malloc there; it pins the determinism the zeroing guarantees everywhere.
    script="$(printf 'SELECT * FORM users\nINSERT INTO users VALUES (1, 2, 3)\n%.0s' $(seq 1 50))"$'\n'"$(inserts $(scrambled 211))"$'\n.exit\n'
    fresh_db
    printf '%s' "$script" | "$DB" "$TESTDB" >/dev/null
    cp "$TESTDB" "$copy"
    fresh_db
    printf '%s' "$script" | "$DB" "$TESTDB" >/dev/null
    cmp -s "$TESTDB" "$copy"
    local same=$?
    rm -f "$copy"
    return "$same"
}

# expect_refusal MESSAGE -> opens $TESTDB, which the caller has prepared, and
# succeeds if the binary refused it: printed exactly MESSAGE, exited with a
# failing status, and left the file byte for byte as it was
expect_refusal() {
    local original="$TESTDB.original" out status
    cp "$TESTDB" "$original"
    out=$(printf '.exit\n' | "$DB" "$TESTDB")
    status=$?
    cmp -s "$TESTDB" "$original"
    local unchanged=$?
    rm -f "$original"
    [[ "$out" == "$1" ]] && (( status != 0 )) && (( unchanged == 0 ))
}

# patch_bytes OFFSET BYTES -> overwrites bytes of $TESTDB in place, BYTES being
# printf escapes such as '\002\000\000\000' (little-endian for a 4-byte field)
patch_bytes() {
    printf "$2" | dd of="$TESTDB" bs=1 seek="$1" conv=notrunc 2>/dev/null
}

t_file_rejects_foreign() {
    # A file this build can't read is refused before the REPL starts, with a
    # message saying why, and is never written to. Files from before the header
    # existed hold a node where the magic belongs, so they're refused as "not
    # a mini-sql database"; the other cases patch one field of a good header.
    local not_ours="Error: $TESTDB is not a mini-sql database, or was written by an older build."

    head -c 4096 /dev/zero >"$TESTDB"
    expect_refusal "$not_ours" || return 1

    { printf '\001\001'; head -c 4094 /dev/zero; } >"$TESTDB"   # an old root leaf on page 0
    expect_refusal "$not_ours" || return 1

    printf 'x%.0s' $(seq 1 4096) >"$TESTDB"
    expect_refusal "$not_ours" || return 1

    make_good_file() {
        fresh_db
        { inserts 1; printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    }

    # A format-1 file (from before the catalog) and one from a future format.
    make_good_file; patch_bytes 16 '\001\000\000\000'
    expect_refusal "Error: $TESTDB uses file format 1; this build reads format 2." || return 1

    make_good_file; patch_bytes 16 '\003\000\000\000'
    expect_refusal "Error: $TESTDB uses file format 3; this build reads format 2." || return 1

    make_good_file; patch_bytes 20 '\000\040\000\000'
    expect_refusal "Error: $TESTDB uses 8192-byte pages; this build uses 4096." || return 1

    make_good_file; patch_bytes 24 '\000\000\000\000'
    expect_refusal "Error: $TESTDB is corrupt: its catalog root page 0 is not a node page of the file." || return 1

    make_good_file; patch_bytes 24 '\143\000\000\000'
    expect_refusal "Error: $TESTDB is corrupt: its catalog root page 99 is not a node page of the file." || return 1

    # A length that isn't a whole number of pages is caught before the header
    # is even read.
    head -c 4097 /dev/zero >"$TESTDB"
    expect_refusal "Db file is not a whole number of pages. Corrupt file."
}

t_sql_syntax_errors() {
    # Each malformed line gets exactly one syntax error, saying what the parser
    # expected and where, or what's wrong with a malformed token — and changes
    # nothing. The cases include the tutorial's old syntax, inputs the old
    # line-splitting parser accepted (insertfoo, a trailing WHERE), clauses
    # that aren't supported yet, a reserved word used as a name, and a long
    # token that the message cuts short.
    local long
    long=$(printf 'x%.0s' $(seq 1 40))
    check_error_cases \
        "SELECT * FORM users" \
        "Syntax error: expected FROM near 'FORM' at column 10." \
        "SELECT * FROM" \
        "Syntax error: expected a table name at end of statement." \
        "INSERT INTO users VALUES (1, 'bob)" \
        "Syntax error: unterminated string starting at column 30." \
        "INSERT INTO users VALUES (7x, 'a', 'b')" \
        "Syntax error: malformed number '7x' at column 27." \
        "SELECT * FROM users @" \
        "Syntax error: unrecognized character '@' at column 21." \
        "UPDATE users SET id = 1" \
        "Syntax error: expected INSERT, SELECT or CREATE near 'UPDATE' at column 1." \
        "insert abc bob bob@x.com" \
        "Syntax error: expected INTO near 'abc' at column 8." \
        "insertfoo 3 dave d@x.com" \
        "Syntax error: expected INSERT, SELECT or CREATE near 'insertfoo' at column 1." \
        "SELECT * FROM users junk" \
        "Syntax error: expected end of statement near 'junk' at column 21." \
        "select * from nowhere where junk" \
        "Syntax error: expected end of statement near 'where' at column 23." \
        "SELECT id FROM users" \
        "Syntax error: expected '*' near 'id' at column 8." \
        "INSERT INTO users VALUES (1, 'a', 'b'" \
        "Syntax error: expected ',' or ')' at end of statement." \
        "INSERT INTO users VALUES ()" \
        "Syntax error: expected a value near ')' at column 27." \
        "INSERT INTO users (id, username, email VALUES (1, 'a', 'b')" \
        "Syntax error: expected ',' or ')' near 'VALUES' at column 40." \
        "INSERT INTO users VALUES (-'a', 'b', 'c')" \
        "Syntax error: expected an integer near ''a'' at column 28." \
        "INSERT INTO table VALUES (1, 'a', 'b')" \
        "Syntax error: expected a table name near 'table' at column 13." \
        "INSERT INTO users VALUES (1, 'a', 'b');;" \
        "Syntax error: expected end of statement near ';' at column 40." \
        "SELECT * FROM users $long" \
        "Syntax error: expected end of statement near '$(printf 'x%.0s' $(seq 1 32))...' at column 21."
}

t_sql_binding_errors() {
    # A statement that parses but doesn't fit the table gets exactly one error:
    # "Error:" for a name or shape that's wrong, "Type error:" for a value the
    # column can't hold. Checks run from the statement's shape down to single
    # values, and nothing reaches the table.
    local long
    long=$(printf 'x%.0s' $(seq 1 40))
    check_error_cases \
        "INSERT INTO orders VALUES (1, 'a', 'b')" \
        "Error: no such table: orders." \
        "SELECT * FROM orders" \
        "Error: no such table: orders." \
        "INSERT INTO users (id, nick, email) VALUES (1, 'a', 'b')" \
        "Error: no such column: nick." \
        "INSERT INTO users (id, ID, email) VALUES (1, 2, 'b')" \
        "Error: column 'id' is listed twice." \
        "INSERT INTO users (id, email) VALUES (1, 'a@b')" \
        "Error: column 'username' has no value." \
        "INSERT INTO users VALUES (1, 'bob')" \
        "Error: 2 values for 3 columns." \
        "INSERT INTO users (id, email) VALUES (1, 'a', 'b')" \
        "Error: 3 values for 2 columns." \
        "INSERT INTO users VALUES ('abc', 'b', 'c')" \
        "Type error: column 'id' is INT, but 'abc' is text." \
        "INSERT INTO users VALUES ('$long', 'b', 'c')" \
        "Type error: column 'id' is INT, but '$(printf 'x%.0s' $(seq 1 32))...' is text." \
        "INSERT INTO users VALUES (1, 42, 'c')" \
        "Type error: column 'username' is TEXT, but 42 is an integer." \
        "INSERT INTO users VALUES (2147483648, 'b', 'c')" \
        "Type error: 2147483648 is out of range for INT column 'id'." \
        "INSERT INTO users VALUES (-2147483649, 'b', 'c')" \
        "Type error: -2147483649 is out of range for INT column 'id'." \
        "INSERT INTO users VALUES (99999999999999999999999, 'b', 'c')" \
        "Type error: 99999999999999999999999 is out of range for INT column 'id'." \
        "INSERT INTO users VALUES (-5, 'b', 'c')" \
        "Error: column 'id' must not be negative." || return 1

    # The edges of INT that are allowed: the largest int32, and -0, which is 0.
    local out
    out=$(run $'INSERT INTO users VALUES (2147483647, \'max\', \'m@x.com\');\nINSERT INTO users VALUES (-0, \'zero\', \'z@x.com\');\nSELECT * FROM users;\n.exit\n')
    [[ -z "$(error_lines "$out")" ]] &&
    [[ "$(select_rows "$out")" == $'(0, zero, z@x.com)\n(2147483647, max, m@x.com)' ]]
}

t_sql_lexical() {
    # What the tokenizer and parser must accept: keywords and names in any
    # case, a column list in any order, doubled quotes inside strings, text
    # holding spaces, commas, semicolons and a comment marker, tabs and extra
    # whitespace, an optional semicolon, an empty string, and comments. Blank
    # lines, comment-only lines and a lone ';' must print nothing at all.
    # read -d '' rather than $(cat <<EOF): bash 3.2, the macOS default,
    # misparses quotes inside a heredoc that sits within $(...). IFS= keeps the
    # script's leading blank line.
    local script expected out
    IFS= read -r -d '' script <<'EOF'

-- a line that is only a comment
;
insert into USERS values (1, 'lower', 'l@x.com')
InSeRt InTo UsErS (EMAIL, Id, USERNAME) VaLuEs ('o''brien@x.com', 2, 'Mary Ann')
INSERT INTO users VALUES (3, 'a, b; c -- not a comment', 'x@x.com');
INSERT	INTO	users	VALUES	(4,'tabs','t@x.com')
   INSERT INTO users VALUES ( 5 , 'spaced' , 's@x.com' ) ;   -- trailing comment
INSERT INTO users VALUES (6, '', 'empty@x.com');
sElEcT * fRoM users
.exit
EOF
    read -r -d '' expected <<'EOF'
(1, lower, l@x.com)
(2, Mary Ann, o'brien@x.com)
(3, a, b; c -- not a comment, x@x.com)
(4, tabs, t@x.com)
(5, spaced, s@x.com)
(6, , empty@x.com)
EOF
    out=$(run "$script")
    [[ -z "$(error_lines "$out")" ]] || return 1
    # the blank line, the comment line and the lone ';' each got a prompt and
    # nothing else, so the first insert's result shares their line
    [[ "$(head -n 1 <<<"$out")" == "db > db > db > db > Executed." ]] || return 1
    [[ $(grep -c 'Executed\.' <<<"$out") -eq 7 ]] || return 1
    [[ "$(select_rows "$out")" == "$expected" ]] || return 1

    # The last line of input may lack a newline; it must be read whole, not
    # lose its final character (which would ask for a table named "user").
    out=$(run "$(inserts 1)"$'\nSELECT * FROM users')
    [[ "$(select_rows "$out")" == "$(expected_rows 1)" ]]
}

t_create_table() {
    # Tables of any shape can be created and used side by side: the primary
    # key orders the rows and catches duplicates wherever it sits among the
    # columns, INT columns other than the key take negative values, and names
    # keep their case while matching without it. .tables lists the tables in
    # creation order and .schema prints each canonical definition — comments
    # and spacing gone, keywords in upper case. All of it survives a reopen,
    # which rebuilds every table by re-parsing its stored definition.
    local script expected out
    IFS= read -r -d '' script <<'EOF'
CREATE TABLE orders (total INT, id INT PRIMARY KEY, note TEXT(40));
create   table Notes (body text(1), n int primary key) -- a comment
INSERT INTO orders VALUES (-5, 7, 'first');
INSERT INTO orders (note, id, total) VALUES ('second', 3, -2147483648);
INSERT INTO orders VALUES (1, 7, 'same key');
INSERT INTO notes VALUES ('x', 1);
.tables
.schema
.schema NOTES
.exit
EOF
    read -r -d '' expected <<'EOF'
db > Executed.
db > Executed.
db > Executed.
db > Executed.
db > Error: Duplicate key.
db > Executed.
db > orders
Notes
db > CREATE TABLE orders (total INT, id INT PRIMARY KEY, note TEXT(40));
CREATE TABLE Notes (body TEXT(1), n INT PRIMARY KEY);
db > CREATE TABLE Notes (body TEXT(1), n INT PRIMARY KEY);
db >
EOF
    out=$(run_empty "$script")
    [[ "$out" == "$expected" ]] || return 1

    read -r -d '' expected <<'EOF'
db > (-2147483648, 3, second)
(-5, 7, first)
Executed.
db > (x, 1)
Executed.
db > orders
Notes
db >
EOF
    out=$(printf 'SELECT * FROM orders;\nSELECT * FROM notes;\n.tables\n.exit\n' | "$DB" "$TESTDB" | normalize)
    [[ "$out" == "$expected" ]]
}

t_create_table_errors() {
    # Every way a definition can be refused, each with exactly its message and
    # none of them creating a table: a name that's taken (ignoring case) or too
    # long, a repeated column, a zero width, a missing, doubled or TEXT primary
    # key, a row too wide to fit three to a leaf (one byte over, and far over),
    # a definition too long for the catalog, and malformed syntax.
    local long cols
    long=$(printf 'n%.0s' $(seq 1 65))
    cols=$(for i in $(seq 1 30); do printf ', c%02d_%s INT' "$i" "$(printf 'x%.0s' $(seq 1 40))"; done)
    check_error_cases \
        "CREATE TABLE users (id INT PRIMARY KEY)" \
        "Error: table users already exists." \
        "CREATE TABLE USERS (id INT PRIMARY KEY)" \
        "Error: table USERS already exists." \
        "CREATE TABLE $long (id INT PRIMARY KEY)" \
        "Error: table name '$(printf 'n%.0s' $(seq 1 32))...' is longer than 64 bytes." \
        "CREATE TABLE t (a INT PRIMARY KEY, A INT)" \
        "Error: column 'A' is defined twice." \
        "CREATE TABLE t (id INT PRIMARY KEY, s TEXT(0))" \
        "Error: column 's' must be TEXT(1) or wider." \
        "CREATE TABLE t (a INT, b TEXT(5))" \
        "Error: table t needs an INT PRIMARY KEY column." \
        "CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)" \
        "Error: table t has more than one PRIMARY KEY." \
        "CREATE TABLE t (name TEXT(10) PRIMARY KEY)" \
        "Error: PRIMARY KEY column 'name' must be INT." \
        "CREATE TABLE t (id INT PRIMARY KEY, s TEXT(1353))" \
        "Error: a row of table t would take 1357 bytes; at most 1356 fit." \
        "CREATE TABLE t (id INT PRIMARY KEY, s TEXT(99999999999999999999))" \
        "Error: a row of table t would be wider than 1356 bytes, the most that fit." \
        "CREATE TABLE t (id INT PRIMARY KEY$cols)" \
        "Error: the definition of table t is $(( ${#cols} + 35 )) bytes; at most 1024 fit." \
        "CREATE TABLE t (id INT(4) PRIMARY KEY)" \
        "Syntax error: expected ',' or ')' near '(' at column 23." \
        "CREATE TABLE t (key INT PRIMARY KEY)" \
        "Syntax error: expected a column name near 'key' at column 17." \
        "CREATE TABLE t (id INT PRIMARY)" \
        "Syntax error: expected KEY near ')' at column 31." \
        "CREATE TABLE t (id BLOB)" \
        "Syntax error: expected INT or TEXT near 'BLOB' at column 20." \
        "CREATE TABLE t (s TEXT)" \
        "Syntax error: expected '(' near ')' at column 23." \
        "CREATE t (id INT PRIMARY KEY)" \
        "Syntax error: expected TABLE near 't' at column 8." || return 1

    # One byte narrower is accepted, and leaves exactly three rows to a leaf.
    # The meta-commands report a missing or unknown table and extra words,
    # and only the tables actually created are listed.
    local expected out
    read -r -d '' expected <<'EOF'
db > Executed.
db > LEAF_NODE_MAX_CELLS: 3
db > Usage: .btree TABLE
db > Error: no such table: nope.
db > Error: no such table: nope.
db > Usage: .constants [TABLE]
db > Usage: .schema [TABLE]
db > users
wide
db >
EOF
    out=$(run $'CREATE TABLE wide (id INT PRIMARY KEY, s TEXT(1352));\n.constants wide\n.btree\n.btree nope\n.schema nope\n.constants a b\n.schema a b\n.tables\n.exit\n' \
          | awk '/^db > Constants:$/ { printf "db > "; next } /^[A-Z_]+: [0-9]+$/ && !/^LEAF_NODE_MAX_CELLS/ { next } { print }')
    [[ "$out" == "$expected" ]]
}

# rows_text_first PREFIX ID... -> the select output for rows (PREFIX<id>, id)
# of a table whose text column comes first
rows_text_first() {
    local prefix=$1 id
    shift
    for id in "$@"; do
        printf '(%s%s, %s)\n' "$prefix" "$id" "$id"
    done
}

t_many_tables() {
    # Tables share one file and one page cache: three tables of different row
    # widths, filled with interleaved inserts, split in pages that interleave
    # through the file. On the 3-key build each tree grows internal levels,
    # and each must stay a valid B+ tree holding exactly its own rows, across
    # a reopen. Then thirty more tables fill the catalog past a leaf's three
    # rows, so the catalog itself splits and grows internal nodes, and a reopen
    # must find every table, in creation order, with its row.
    local DB="$SMALL_FANOUT_DB" script out id i order height
    require_small_fanout || return 1
    order=$(scrambled 151)
    script="CREATE TABLE notes (body TEXT(200), k INT PRIMARY KEY);"$'\n'
    script+="CREATE TABLE blobs (k INT PRIMARY KEY, blob TEXT(600));"$'\n'
    for id in $order; do
        script+="INSERT INTO users VALUES ($id, 'user$id', 'person$id@example.com');"$'\n'
        script+="INSERT INTO notes VALUES ('n$id', $id);"$'\n'
        script+="INSERT INTO blobs VALUES ($id, 'b$id');"$'\n'
    done
    out=$(run "$script.exit"$'\n')
    [[ -z "$(error_lines "$out")" ]] || return 1

    # Leaves hold 13 users rows, 19 notes rows and 6 blobs rows.
    out=$(printf '.btree users\nSELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    height=$(btree_check "$out" 3) && (( height >= 3 )) || return 1
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 150))" ]] || return 1
    out=$(printf '.btree notes\nSELECT * FROM notes;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    height=$(btree_check "$out" 3 19) && (( height >= 3 )) || return 1
    [[ "$(sed -nE 's/^(db > )?(\(n[0-9]+, [0-9]+\))$/\2/p' <<<"$out")" == "$(rows_text_first n $(seq 1 150))" ]] || return 1
    out=$(printf '.btree blobs\nSELECT * FROM blobs;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    height=$(btree_check "$out" 3 6) && (( height >= 3 )) || return 1
    [[ "$(sed -nE 's/^(db > )?(\([0-9]+, b[0-9]+\))$/\2/p' <<<"$out")" == "$(for id in $(seq 1 150); do printf '(%s, b%s)\n' "$id" "$id"; done)" ]] || return 1

    # Three tables fit one catalog leaf; thirty more must split it, turning
    # the catalog's root on page 1 into an internal node (type byte 0).
    [[ "$(od -An -tu1 -j4096 -N1 "$TESTDB" | xargs)" == 1 ]] || return 1
    script=""
    for i in $(seq 1 30); do
        script+="CREATE TABLE t$i (id INT PRIMARY KEY);"$'\n'"INSERT INTO t$i VALUES ($i);"$'\n'
    done
    out=$(printf '%s.exit\n' "$script" | "$DB" "$TESTDB" | normalize)
    [[ -z "$(error_lines "$out")" ]] || return 1
    [[ "$(od -An -tu1 -j4096 -N1 "$TESTDB" | xargs)" == 0 ]] || return 1

    script=".tables"$'\n'
    for i in $(seq 1 30); do
        script+="SELECT * FROM t$i;"$'\n'
    done
    out=$(printf '%s.exit\n' "$script" | "$DB" "$TESTDB" | normalize)
    [[ "$(sed -n '1,/^db > (/p' <<<"$out" | sed '$d' | sed 's/^db > //')" == "$(printf '%s\n' users notes blobs $(for i in $(seq 1 30); do echo "t$i"; done))" ]] || return 1
    [[ "$(sed -nE 's/^(db > )?\(([0-9]+)\)$/\2/p' <<<"$out")" == "$(seq 1 30)" ]]
}

t_catalog_corrupt() {
    # The catalog is checked as it's loaded, and a file whose catalog is
    # damaged is refused, untouched. In a new database the catalog's first row
    # sits at the start of page 1: key, then the row's id (byte 4116), name
    # (4120), root page (4184) and definition text (4188).
    fresh_db
    { inserts 1; printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    local good="$TESTDB.good"
    cp "$TESTDB" "$good"

    patch_bytes 4189 'X'                       # CREATE -> CXEATE
    expect_refusal "Error: $TESTDB is corrupt: the stored definition of table users doesn't parse." || { rm -f "$good"; return 1; }

    cp "$good" "$TESTDB"; patch_bytes 4242 '00'   # username TEXT(32) -> TEXT(00)
    expect_refusal "Error: $TESTDB is corrupt: the stored definition of table users is invalid." || { rm -f "$good"; return 1; }

    cp "$good" "$TESTDB"; patch_bytes 4120 'x'    # name users -> xsers
    expect_refusal "Error: $TESTDB is corrupt: the catalog lists table xsers, but its definition is for users." || { rm -f "$good"; return 1; }

    cp "$good" "$TESTDB"; patch_bytes 4184 '\143\000\000\000'   # root page 99
    expect_refusal "Error: $TESTDB is corrupt: table users has root page 99, which is not a node page of the file."
    local refused=$?
    rm -f "$good"
    return "$refused"
}

t_duplicate_key() {
    # The second insert of id 1 must be rejected, leaving exactly one row.
    local out
    out=$(run "$(inserts 1 1)"$'\nSELECT * FROM users;\n.exit\n')
    want "$out" "Error: Duplicate key." &&
    [[ $(grep -cF '(1, user1, person1@example.com)' <<<"$out") -eq 1 ]]
}

ALL=(inserts_and_retrieves max_length_strings string_too_long negative_id persistence constants btree_one_node duplicate_key btree_split btree_split_persists btree_insert_after_split select_multilevel select_empty btree_nonroot_split btree_internal_split btree_internal_split_persists large_table file_header file_rejects_foreign sql_syntax_errors sql_binding_errors sql_lexical create_table create_table_errors many_tables catalog_corrupt)

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

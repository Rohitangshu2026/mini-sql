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

# btree_check OUTPUT MAX_KEYS -> checks the tree printed by .btree against the
# B+ tree invariants and prints its height (the number of levels, leaves
# included). Fails, naming the first broken rule, if:
#   - leaf keys don't strictly increase from the first leaf to the last
#   - a separator isn't the last leaf key printed before it, i.e. the largest
#     key in the subtree on its left
#   - a node's printed size disagrees with what's printed under it, or an
#     internal node doesn't alternate child, key, child ... child
#   - a node is empty or holds more than it may (LEAF_MAX_CELLS / MAX_KEYS)
#   - leaves sit at different depths
# Lets a test cover a tree of any size and insertion order without spelling
# the whole tree out.
btree_check() {
    btree_block "$1" | awk -v max_keys="$2" -v max_cells="$LEAF_MAX_CELLS" '
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
    rm -f "$TESTDB"
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
        out=$(run "$(inserts $order 15)"$'\nSELECT * FROM users;\n.exit\n')
        [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 15))" ]] || return 1
    done

    # 20 rows fill the right leaf completely; the chain must survive a reopen.
    rm -f "$TESTDB"
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
        out=$(run "$(inserts ${orders[i]})"$'\n.btree\nSELECT * FROM users;\n.exit\n')
        [[ "$(btree_block "$out")" == "$(tree_over_leaves ${leaves[i]})" ]] || return 1
        [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 30))" ]] || return 1
    done

    # With separators 7 | 14 | 21, re-inserting 14 and 21 (equal to a separator,
    # so routed left to the leaf holding them) and 22 (just past the last one,
    # so routed to the right child) must all be rejected as duplicates. The
    # tree and every row must then survive a reopen, all five pages intact.
    out=$(run "$(inserts $(seq 1 30) 14 21 22)"$'\n.exit\n')
    [[ $(grep -cF 'Error: Duplicate key.' <<<"$out") -eq 3 ]] || return 1
    out=$(printf '.btree\nSELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
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
    out=$(run "$(inserts $CSTACK_7_LEAF_ORDER)"$'\n.btree\nSELECT * FROM users;\n.exit\n')
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
        out=$(run "$(inserts $order)"$'\n.btree\nSELECT * FROM users;\n.exit\n')
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
    rm -f "$TESTDB"
    { inserts $(seq 2 2 200); printf '.exit\n'; } | "$DB" "$TESTDB" >/dev/null
    out=$(printf '.btree\n.exit\n' | "$DB" "$TESTDB" | normalize)
    height=$(btree_check "$out" 3) && (( height >= 4 )) || return 1

    out=$({ inserts $(seq 199 -2 1); printf '.btree\nSELECT * FROM users;\n.exit\n'; } | "$DB" "$TESTDB" | normalize)
    btree_check "$out" 3 >/dev/null || return 1
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 200))" ]] || return 1

    out=$(printf '.btree\nSELECT * FROM users;\n.exit\n' | "$DB" "$TESTDB" | normalize)
    btree_check "$out" 3 >/dev/null &&
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 200))" ]]
}

t_page_cache_full() {
    # Pins the boundary still standing: the pager caches at most 100 pages. On
    # the real binary, 699 ascending rows fill 99 leaves under a root holding 98
    # separators — a valid tree with every row reachable — and row 700 needs a
    # 101st page, so it must stop right after row 699 succeeds.
    local out height
    out=$(run "$(inserts $(seq 1 699))"$'\n.btree\nSELECT * FROM users;\n.exit\n')
    height=$(btree_check "$out" 510) && (( height == 2 )) || return 1
    want "$out" "- internal (size 98)" || return 1
    [[ "$(select_rows "$out")" == "$(expected_rows $(seq 1 699))" ]] || return 1

    out=$(run "$(inserts $(seq 1 700))"$'\n.exit\n')
    [[ "$(tail -n 2 <<<"$out")" == $'db > Executed.\ndb > Tried to fetch page number out of bounds. 100 >= 100' ]]
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
        "Syntax error: expected INSERT or SELECT near 'UPDATE' at column 1." \
        "insert abc bob bob@x.com" \
        "Syntax error: expected INTO near 'abc' at column 8." \
        "insertfoo 3 dave d@x.com" \
        "Syntax error: expected INSERT or SELECT near 'insertfoo' at column 1." \
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

t_duplicate_key() {
    # The second insert of id 1 must be rejected, leaving exactly one row.
    local out
    out=$(run "$(inserts 1 1)"$'\nSELECT * FROM users;\n.exit\n')
    want "$out" "Error: Duplicate key." &&
    [[ $(grep -cF '(1, user1, person1@example.com)' <<<"$out") -eq 1 ]]
}

ALL=(inserts_and_retrieves max_length_strings string_too_long negative_id persistence constants btree_one_node duplicate_key btree_split btree_split_persists btree_insert_after_split select_multilevel select_empty btree_nonroot_split btree_internal_split btree_internal_split_persists page_cache_full sql_syntax_errors sql_binding_errors sql_lexical)

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

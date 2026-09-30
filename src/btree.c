#include "btree.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/*
 * Common node header layout (defined with #define rather than const variables
 * because C11 needs constant expressions at file scope, and offsets derived
 * from other consts don't qualify).
 *
 *   byte 0    node type    (uint8_t)
 *   byte 1    is_root      (uint8_t)
 *   bytes 2-3 padding
 *   bytes 4-7 parent page  (uint32_t)
 *
 * This diverges from cstack's packed 6-byte header on purpose: the two padding
 * bytes keep the parent-page field (and every uint32_t field after the header)
 * 4-byte aligned. The accessors dereference uint32_t* pointers into the page,
 * so a misaligned field would be undefined behavior even though x86/ARM happen
 * to tolerate it — UBSan flags it on the packed layout.
 */
#define NODE_TYPE_OFFSET        0u
#define IS_ROOT_OFFSET          1u
#define PARENT_POINTER_OFFSET   4u
#define COMMON_NODE_HEADER_SIZE 8u

/*
 * Leaf node header: the common header, a 4-byte cell count, then the page
 * number of the next leaf to the right. Following next_leaf from the leftmost
 * leaf visits every row in key order, which is how a scan crosses leaves. Zero
 * means "no right sibling": page 0 is the file header and never a node, so it
 * is free to serve as the sentinel.
 */
#define LEAF_NODE_NUM_CELLS_OFFSET COMMON_NODE_HEADER_SIZE
#define LEAF_NODE_NEXT_LEAF_OFFSET (COMMON_NODE_HEADER_SIZE + 4u)
#define LEAF_NODE_HEADER_SIZE      (COMMON_NODE_HEADER_SIZE + 8u)

/*
 * Leaf node body: an array of cells, each a 4-byte key followed by a serialized
 * row. The key size is fixed; the value size is the schema's row width, so the
 * cell size and per-node capacity are computed at runtime rather than being
 * constants.
 */
#define LEAF_NODE_KEY_SIZE 4u

/*
 * Internal node header: the common header, a 4-byte key count, then the page
 * number of the rightmost child. Both fields land on 4-byte boundaries because
 * the common header is padded to 8 bytes.
 */
#define INTERNAL_NODE_NUM_KEYS_OFFSET    COMMON_NODE_HEADER_SIZE
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET (COMMON_NODE_HEADER_SIZE + 4u)
#define INTERNAL_NODE_HEADER_SIZE        (COMMON_NODE_HEADER_SIZE + 8u)

/*
 * Internal node body: an array of cells, each a child page number followed by
 * a key. At 8 bytes a cell, one page holds 510 keys and 511 children — the huge
 * fan-out that keeps a b-tree only a few levels deep however much it stores.
 */
#define INTERNAL_NODE_CHILD_SIZE 4u
#define INTERNAL_NODE_KEY_SIZE   4u
#define INTERNAL_NODE_CELL_SIZE  (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)
#define INTERNAL_NODE_LAYOUT_MAX_KEYS ((PAGE_SIZE - INTERNAL_NODE_HEADER_SIZE) / INTERNAL_NODE_CELL_SIZE)

/*
 * How many keys an internal node is allowed to hold. By default that's every
 * cell the page has room for. A build can lower it with
 * -DMINI_SQL_INTERNAL_NODE_MAX_KEYS=N, in the spirit of SQLite's SQLITE_*
 * compile-time options: the test suite builds a second binary capped at 3 keys
 * (the tutorial's testing value), so internal-node boundaries are reachable with
 * a few dozen rows instead of thousands. It limits how full a node may get, not
 * how a page is laid out, so both builds read and write the same file format.
 */
#ifndef MINI_SQL_INTERNAL_NODE_MAX_KEYS
#define MINI_SQL_INTERNAL_NODE_MAX_KEYS INTERNAL_NODE_LAYOUT_MAX_KEYS
#elif MINI_SQL_INTERNAL_NODE_MAX_KEYS < 3
/*
 * Splitting a full internal node leaves half its keys on each side and sends
 * the middle one up, so with fewer than 3 keys one half would be left with no
 * keys and a single child — or, at 0, the split would underflow.
 */
#error "MINI_SQL_INTERNAL_NODE_MAX_KEYS must be at least 3"
#endif

/*
 * The page number stored in an internal node's right-child slot until a real
 * child is put there. Page 0 can't serve as "unset": it's the file header, and
 * following it would read the header's bytes as a node. This value lies past
 * the end of any file, so following it by mistake fails loudly in
 * pager_get_page's bounds check instead of quietly reading the wrong page.
 */
#define INVALID_PAGE_NUM UINT32_MAX

/*
 * Reads the node-kind byte. Stored as a uint8_t rather than the enum so the
 * on-disk format doesn't depend on how wide the compiler makes NodeType.
 */
NodeType get_node_type(void* node){
    uint8_t value = *((uint8_t*)((char*)node + NODE_TYPE_OFFSET));
    return (NodeType)value;
}

/* Writes the node-kind byte. Mirror of get_node_type. */
void set_node_type(void* node, NodeType type){
    uint8_t value = (uint8_t)type;
    *((uint8_t*)((char*)node + NODE_TYPE_OFFSET)) = value;
}

/*
 * Reads the is_root byte. Exactly one node per tree carries it — the page the
 * table treats as its root — and a split consults it to decide whether the two
 * halves need a brand-new parent (the root split) or an existing one updated.
 */
bool is_node_root(void* node){
    uint8_t value = *((uint8_t*)((char*)node + IS_ROOT_OFFSET));
    return value != 0;
}

/* Writes the is_root byte, normalized to 0 or 1. */
void set_node_root(void* node, bool is_root){
    uint8_t value = is_root ? 1u : 0u;
    *((uint8_t*)((char*)node + IS_ROOT_OFFSET)) = value;
}

/*
 * Pointer to the page number of the node's parent, for reading or writing. A
 * split follows it upward to find the internal node that has to learn about
 * the new sibling. It means nothing on the root, which has no parent, and it
 * lands 4-byte aligned because the header pads the two flag bytes out to 4.
 */
static uint32_t* node_parent(void* node){
    return (uint32_t*)((char*)node + PARENT_POINTER_OFFSET);
}

/*
 * Bytes occupied by one cell, rounded up to a multiple of 4 so that the key at
 * the start of every cell stays 4-byte aligned (see the header comment).
 */
static uint32_t leaf_node_cell_size(const Schema* schema){
    uint32_t raw = LEAF_NODE_KEY_SIZE + schema->row_size;
    return (raw + 3u) & ~3u;
}

/*
 * How many whole cells fit in a leaf after its header. Leftover bytes too small
 * for another cell are left unused so a cell never straddles two pages.
 */
uint32_t leaf_node_max_cells(const Schema* schema){
    return (PAGE_SIZE - LEAF_NODE_HEADER_SIZE) / leaf_node_cell_size(schema);
}

/*
 * The widest row for which a leaf still holds `min_cells` cells: the space for
 * cells split `min_cells` ways, rounded down to the 4-byte cell alignment, less
 * the key. Row-size limits are derived from this rather than restated, so they
 * can't drift from the actual node layout.
 */
uint32_t leaf_node_max_row_size(uint32_t min_cells){
    uint32_t cell_size = ((PAGE_SIZE - LEAF_NODE_HEADER_SIZE) / min_cells) & ~3u;
    return cell_size - LEAF_NODE_KEY_SIZE;
}

/* Pointer to the leaf's cell-count field, for reading or writing. */
uint32_t* leaf_node_num_cells(void* node){
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

/* Pointer to the page number of the leaf's right sibling (0 if it has none). */
uint32_t* leaf_node_next_leaf(void* node){
    return (uint32_t*)((char*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}

/* Address of the start of cell `cell_num` (i.e. its key). */
static void* leaf_node_cell(void* node, uint32_t cell_num, const Schema* schema){
    return (char*)node + LEAF_NODE_HEADER_SIZE + cell_num * leaf_node_cell_size(schema);
}

/* Pointer to cell `cell_num`'s key (the first field of the cell). */
uint32_t* leaf_node_key(void* node, uint32_t cell_num, const Schema* schema){
    return (uint32_t*)leaf_node_cell(node, cell_num, schema);
}

/* Pointer to cell `cell_num`'s value, which sits right after the key. */
void* leaf_node_value(void* node, uint32_t cell_num, const Schema* schema){
    return (char*)leaf_node_cell(node, cell_num, schema) + LEAF_NODE_KEY_SIZE;
}

/*
 * Locates `key` among the cells, which insert keeps in ascending key order.
 *
 * Standard binary search over the half-open range [min_index,
 * one_past_max_index). On an exact hit the cell's own index comes back; on a
 * miss the loop converges on the first index whose key is greater than `key`,
 * which is precisely where the key would have to be inserted to keep the node
 * sorted. That index equals the cell count when the key sorts after every
 * existing cell, so callers must bounds-check before dereferencing it.
 */
uint32_t leaf_node_find_cell(void* node, uint32_t key, const Schema* schema){
    uint32_t min_index = 0;
    uint32_t one_past_max_index = *leaf_node_num_cells(node);

    while(one_past_max_index != min_index){
        uint32_t index = (min_index + one_past_max_index) / 2;
        uint32_t key_at_index = *leaf_node_key(node, index, schema);

        if(key == key_at_index)
            return index;
        if(key < key_at_index)
            one_past_max_index = index;
        else
            min_index = index + 1;
    }

    return min_index;
}

/*
 * Prepares a fresh page as an empty leaf. It starts life as a non-root with no
 * right sibling: the one caller that installs a leaf as a tree's root (db_open,
 * for a new file) sets the root flag explicitly afterwards, and a split links
 * a new leaf into the chain and under its parent itself. The kind is stamped so
 * the page describes itself — cursors dispatch on it, and a leftover header
 * byte would send them down the wrong branch. The parent field is zeroed for
 * the same reason, so no page ever reaches disk carrying leftover memory there.
 */
void initialize_leaf_node(void* node){
    set_node_type(node, NODE_LEAF);
    set_node_root(node, false);
    *node_parent(node) = 0;
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;
}

/* Pointer to the internal node's key-count field. */
uint32_t* internal_node_num_keys(void* node){
    return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

/* Pointer to the rightmost child's page number, which lives in the header. */
uint32_t* internal_node_right_child(void* node){
    return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

/* Address of the start of cell `cell_num` (i.e. its child page number). */
static uint32_t* internal_node_cell(void* node, uint32_t cell_num){
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE);
}

/*
 * Returns child `child_num`, hiding the format's asymmetry from callers: the
 * first num_keys children sit in the cells, the last one in the header. An
 * index beyond num_keys can only come from a bug, so it's fatal.
 */
uint32_t* internal_node_child(void* node, uint32_t child_num){
    uint32_t num_keys = *internal_node_num_keys(node);
    if(child_num > num_keys){
        printf("Tried to access child_num %u > num_keys %u\n", child_num, num_keys);
        exit(EXIT_FAILURE);
    }
    if(child_num == num_keys)
        return internal_node_right_child(node);
    return internal_node_cell(node, child_num);
}

/*
 * Pointer to key `key_num`, the second half of its cell. The offset is added in
 * bytes on purpose: adding INTERNAL_NODE_CHILD_SIZE to the uint32_t* cell
 * pointer, as the tutorial does, would advance 4 elements (16 bytes) instead of
 * 4 bytes — wrong in a way that stays invisible until cells overlap.
 */
uint32_t* internal_node_key(void* node, uint32_t key_num){
    return (uint32_t*)((char*)internal_node_cell(node, key_num) + INTERNAL_NODE_CHILD_SIZE);
}

/*
 * Chooses which child of an internal node to descend into for `key`.
 *
 * Each separator key is the largest key stored under the child to its left,
 * so `key` belongs to the first child whose separator is >= key, and to the
 * rightmost child if it is greater than every separator. That is a lower-bound
 * binary search over [min_index, max_index), with max_index starting at
 * num_keys rather than num_keys - 1 because there is one more child than there
 * are keys: landing on num_keys means "the right child". A key equal to a
 * separator goes left, since that separator is the left child's maximum —
 * sending it right would miss the row that holds it.
 *
 * The probe index is always strictly below max_index, and so below num_keys,
 * which means only keys that actually exist are ever read.
 */
uint32_t internal_node_find_child(void* node, uint32_t key){
    uint32_t min_index = 0;
    uint32_t max_index = *internal_node_num_keys(node);

    while(min_index != max_index){
        uint32_t index = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);

        if(key_to_right >= key)
            max_index = index;
        else
            min_index = index + 1;
    }

    return min_index;
}

/*
 * Prepares a page as an empty, non-root internal node with no parent set. The
 * right child is marked invalid rather than left as whatever the page held:
 * a leftover 0 there would make the new node look like the root's parent.
 * Every caller installs the real right child before the node is read.
 */
static void initialize_internal_node(void* node){
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *node_parent(node) = 0;
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

/*
 * The number of keys an internal node may hold: the configured cap, but never
 * more than the page has cells for, so a misconfigured build can't write past
 * the end of a page.
 */
static uint32_t internal_node_max_keys(void){
    uint32_t configured = MINI_SQL_INTERNAL_NODE_MAX_KEYS;
    uint32_t layout_max = INTERNAL_NODE_LAYOUT_MAX_KEYS;
    return configured < layout_max ? configured : layout_max;
}

/*
 * The largest key stored anywhere under a node, used as the separator key a
 * parent keeps for it.
 *
 * For a leaf that's simply its last cell. For an internal node it's the
 * maximum of its rightmost subtree, so the walk follows right children down to
 * a leaf and takes that leaf's last key. An internal node's own last separator
 * would be the wrong answer: it only bounds the child to its left, and misses
 * every key under the right child. The walk is a loop, like the descent in
 * table_find, and costs one page fetch per level.
 *
 * The caller must not pass an empty node. An unrecognized type byte means the
 * page is corrupt, so it aborts rather than hand back a made-up key.
 */
static uint32_t get_node_max_key(Pager* pager, void* node, const Schema* schema){
    while(get_node_type(node) == NODE_INTERNAL)
        node = pager_get_page(pager, *internal_node_right_child(node));

    if(get_node_type(node) != NODE_LEAF){
        printf("Corrupt node: unknown node type\n");
        exit(EXIT_FAILURE);
    }
    return *leaf_node_key(node, *leaf_node_num_cells(node) - 1, schema);
}

/*
 * Points every child of the internal node `node`, which lives on `page_num`,
 * back at it through their parent fields. Called whenever children arrive in a
 * node wholesale — a split moving half of them to a new sibling, or a root
 * split copying the root's contents to a new page — since each child still
 * names the page it came from.
 */
static void reparent_children(Pager* pager, void* node, uint32_t page_num){
    uint32_t num_keys = *internal_node_num_keys(node);
    for(uint32_t i = 0; i <= num_keys; ++i){
        void* child = pager_get_page(pager, *internal_node_child(node, i));
        *node_parent(child) = page_num;
    }
}

/*
 * Grows the tree by one level after the root has been split.
 *
 * The root has to stay on its page — the table remembers where its root is —
 * so instead of putting the new internal node somewhere fresh, the old root's
 * contents (now the left half of the split) are copied out to a newly allocated
 * page, and the root page itself is reinitialized as an internal node with one
 * key and two children: the copied left half and `right_child_page_num`. The
 * separator key is the left half's maximum, so every key <= it routes left.
 * Both halves record the root page as their parent, which is how a later split
 * of either one finds the node to update.
 *
 * The root that split may be a leaf or an internal node, and the two halves are
 * whatever kind it was. When it was internal, the children of the copied half
 * still name the root page as their parent, so they're pointed at the copy's
 * new page. The right half was built fresh by the split and already has its
 * children pointing at it.
 *
 * num_keys is set before the child is written because internal_node_child
 * resolves "child num_keys" to the right-child slot in the header.
 */
static void create_new_root(Pager* pager, uint32_t root_page_num, uint32_t right_child_page_num,
                            const Schema* schema){
    void* root = pager_get_page(pager, root_page_num);
    void* right_child = pager_get_page(pager, right_child_page_num);
    uint32_t left_child_page_num = pager_allocate_page(pager);
    void* left_child = pager_get_page(pager, left_child_page_num);

    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);
    if(get_node_type(left_child) == NODE_INTERNAL)
        reparent_children(pager, left_child, left_child_page_num);

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_child_page_num;
    *internal_node_key(root, 0) = get_node_max_key(pager, left_child, schema);
    *internal_node_right_child(root) = right_child_page_num;

    *node_parent(left_child) = root_page_num;
    *node_parent(right_child) = root_page_num;
}

/*
 * Rewrites the separator key a parent keeps for one of its children after that
 * child's maximum dropped from `old_key` to `new_key` (a split moved its upper
 * half to a new sibling).
 *
 * The child is found by searching for `old_key`, which lands on the child whose
 * key range contains it — the first child whose separator is >= it. Any key
 * that was stored under the child before the split lies in that range, so the
 * old maximum finds it. It usually equals the separator exactly, but not always:
 * in a cascade of splits, the old node's rightmost leaf may itself have just
 * split, leaving a smaller maximum than the separator above it still records.
 * The range search finds the child either way. When the search lands on
 * num_keys the child is the rightmost one, which has no separator of its own to
 * update. The tutorial writes the new key into the slot one past the last key
 * anyway — a cell that isn't part of the node — so that case is skipped here,
 * and internal_node_insert records the rightmost child's new maximum when it
 * moves that child into a cell.
 */
static void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key){
    uint32_t old_child_index = internal_node_find_child(node, old_key);
    if(old_child_index < *internal_node_num_keys(node))
        *internal_node_key(node, old_child_index) = new_key;
}

/*
 * Declared ahead of internal_node_insert because the two call each other: a
 * full node splits, and a split inserts into the node's parent, which may be
 * full in turn. Each call moves one level up the tree, so the recursion is no
 * deeper than the tree is tall.
 */
static void internal_node_split_and_insert(Pager* pager, uint32_t page_num, uint32_t child_page_num,
                                           const Schema* schema);

/*
 * Adds the node on `child_page_num` as a new child of the internal node on
 * `parent_page_num`, keeping children in key order.
 *
 * The new child's separator is its own maximum key. If that is greater than
 * everything under the current rightmost child, the new child becomes the
 * rightmost one, and the old rightmost child moves down into the last cell with
 * its maximum as the separator — the rightmost child lives in the header, so it
 * can't simply be shifted along with the cells. Otherwise the cells from the
 * insertion point onward move one slot right and the new (child, key) cell
 * fills the gap.
 *
 * A full parent is split instead, and the split places the child in whichever
 * half owns its keys. That check comes before num_keys is touched, unlike the
 * tutorial's first version, which bumped the count first. The count is then
 * raised before any cell is written, because internal_node_child resolves
 * child original_num_keys to the header's right-child slot until the count
 * moves past it.
 *
 * Attaching the child is also where it learns its parent, so every node that
 * gains a parent through here records it the same way — whether it's a leaf
 * fresh from a split or an internal node climbing up a cascade of them.
 */
static void internal_node_insert(Pager* pager, uint32_t parent_page_num, uint32_t child_page_num,
                                 const Schema* schema){
    void* parent = pager_get_page(pager, parent_page_num);
    void* child = pager_get_page(pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(pager, child, schema);
    uint32_t index = internal_node_find_child(parent, child_max_key);

    uint32_t original_num_keys = *internal_node_num_keys(parent);
    if(original_num_keys >= internal_node_max_keys()){
        internal_node_split_and_insert(pager, parent_page_num, child_page_num, schema);
        return;
    }

    uint32_t right_child_page_num = *internal_node_right_child(parent);
    void* right_child = pager_get_page(pager, right_child_page_num);
    uint32_t right_child_max_key = get_node_max_key(pager, right_child, schema);

    *internal_node_num_keys(parent) = original_num_keys + 1;

    if(child_max_key > right_child_max_key){
        /* the new child sorts after everything: it takes over the header slot */
        *internal_node_child(parent, original_num_keys) = right_child_page_num;
        *internal_node_key(parent, original_num_keys) = right_child_max_key;
        *internal_node_right_child(parent) = child_page_num;
    }
    else{
        /* shift trailing cells right to make room for the new one */
        for(uint32_t i = original_num_keys; i > index; --i)
            memcpy(internal_node_cell(parent, i),
                   internal_node_cell(parent, i - 1),
                   INTERNAL_NODE_CELL_SIZE);
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index) = child_max_key;
    }

    *node_parent(child) = parent_page_num;
}

/*
 * Splits the full internal node on `page_num` in two while adding the node on
 * `child_page_num` as a child — the internal-node counterpart of
 * leaf_node_split_and_insert, and shaped the same way: split in place, then
 * either grow a new root or update the parent.
 *
 * With k keys, the node's upper cells (from key k/2 + 1 on) and its right child
 * move to a new sibling in one copy, and the moved children are pointed at
 * their new parent. The node keeps its first k/2 cells, and the child in cell
 * k/2 becomes its right child. Key k/2 itself isn't copied anywhere: it was the
 * maximum of that child, so it's now the maximum of the whole node, and the
 * parent's separator for the node takes over that role. The split uses the
 * node's actual key count rather than the configured cap, so a node filled by
 * a build with a larger cap still divides correctly.
 *
 * The pending child then goes into whichever half owns its keys: the node if
 * its maximum is below the node's new maximum, the sibling otherwise. Neither
 * half is full after the split, so that insert never splits again.
 *
 * Finally the level above learns about the sibling, exactly as for a leaf. A
 * root gets a new root above the two halves. Otherwise the node's separator in
 * its parent drops to the node's new maximum, located by the maximum it had
 * before the split, and the sibling is inserted into the parent — which may be
 * full too, so the split can cascade up to the root.
 *
 * The tutorial reaches the same trees another way: for a root it creates the
 * new root first and splits the copy, and it builds the sibling one
 * internal_node_insert at a time, which is quadratic in the node size. After
 * the recursive insert into the parent it also resets the sibling's parent to
 * the node's own, which is wrong whenever the parent's split sends the two to
 * different halves. Here the sibling's parent is set by whichever insert
 * actually places it.
 */
static void internal_node_split_and_insert(Pager* pager, uint32_t page_num, uint32_t child_page_num,
                                           const Schema* schema){
    void* old_node = pager_get_page(pager, page_num);
    uint32_t old_max = get_node_max_key(pager, old_node, schema);
    void* child = pager_get_page(pager, child_page_num);
    uint32_t child_max = get_node_max_key(pager, child, schema);

    uint32_t new_page_num = pager_allocate_page(pager);
    void* new_node = pager_get_page(pager, new_page_num);
    initialize_internal_node(new_node);
    *node_parent(new_node) = *node_parent(old_node);

    uint32_t num_keys = *internal_node_num_keys(old_node);
    uint32_t split_index = num_keys / 2;
    uint32_t moved_keys = num_keys - split_index - 1;

    memcpy(internal_node_cell(new_node, 0),
           internal_node_cell(old_node, split_index + 1),
           moved_keys * INTERNAL_NODE_CELL_SIZE);
    *internal_node_num_keys(new_node) = moved_keys;
    *internal_node_right_child(new_node) = *internal_node_right_child(old_node);
    reparent_children(pager, new_node, new_page_num);

    *internal_node_right_child(old_node) = *internal_node_child(old_node, split_index);
    *internal_node_num_keys(old_node) = split_index;

    uint32_t destination_page_num =
        child_max < get_node_max_key(pager, old_node, schema) ? page_num : new_page_num;
    internal_node_insert(pager, destination_page_num, child_page_num, schema);

    if(is_node_root(old_node)){
        create_new_root(pager, page_num, new_page_num, schema);
    }
    else{
        uint32_t parent_page_num = *node_parent(old_node);
        void* parent = pager_get_page(pager, parent_page_num);

        update_internal_node_key(parent, old_max, get_node_max_key(pager, old_node, schema));
        internal_node_insert(pager, parent_page_num, new_page_num, schema);
    }
}

/*
 * Splits a full leaf in two while inserting the new cell.
 *
 * The leaf's N existing cells plus the new one — N + 1 in all, in sorted order
 * with the new cell at position `cell_num` — are divided between the old node
 * (the lower half) and a freshly allocated sibling (the upper half), with the
 * left side taking the extra cell when the total is odd. Every key on the right
 * is therefore greater than every key on the left.
 *
 * The cells are placed walking from the highest logical position down to the
 * lowest. That order is what makes it safe to rearrange the old node in place:
 * the old-node cells a position reads from have an index no higher than the
 * position being written, so they haven't been overwritten yet. Old cells below
 * the insertion point that stay in the left half are already where they belong
 * and are skipped rather than copied onto themselves. The new cell gets its key
 * written to the key slot and its row to the value slot — the tutorial instead
 * serializes the row at the start of the cell and never writes the key, a bug
 * hidden by the id happening to be both the key and the row's first field.
 *
 * The new node is spliced into the leaf chain directly after the old one, so a
 * scan still meets the two halves in key order, and it shares the old node's
 * parent. If the node was the root, a new root is created above the two halves.
 * Otherwise the existing parent is brought up to date in two steps: the old
 * node's separator drops to its new, smaller maximum, then the new node is
 * added as a child. The old maximum is read before the cells move, because the
 * parent's separator still holds it and that's what locates the old node there.
 */
static void leaf_node_split_and_insert(Pager* pager, uint32_t page_num, uint32_t cell_num,
                                       uint32_t key, const Record* value, const Schema* schema){
    void* old_node = pager_get_page(pager, page_num);
    uint32_t old_max = get_node_max_key(pager, old_node, schema);
    uint32_t new_page_num = pager_allocate_page(pager);
    void* new_node = pager_get_page(pager, new_page_num);
    initialize_leaf_node(new_node);
    *node_parent(new_node) = *node_parent(old_node);

    /*
     * Linked-list insertion into the leaf chain: the new node inherits the old
     * node's right sibling, then becomes that sibling. The old pointer has to be
     * read before it's overwritten, or the new node would point at itself and
     * every scan would loop forever.
     */
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    uint32_t total_cells = leaf_node_max_cells(schema) + 1;
    uint32_t right_split_count = total_cells / 2;
    uint32_t left_split_count = total_cells - right_split_count;

    for(uint32_t i = total_cells; i-- > 0;){
        void* destination_node = (i >= left_split_count) ? new_node : old_node;
        uint32_t index_within_node = (i >= left_split_count) ? i - left_split_count : i;

        if(i == cell_num){
            *leaf_node_key(destination_node, index_within_node, schema) = key;
            serialize_record(value, leaf_node_value(destination_node, index_within_node, schema));
        }
        else if(i > cell_num){
            memcpy(leaf_node_cell(destination_node, index_within_node, schema),
                   leaf_node_cell(old_node, i - 1, schema),
                   leaf_node_cell_size(schema));
        }
        else if(destination_node != old_node){
            memcpy(leaf_node_cell(destination_node, index_within_node, schema),
                   leaf_node_cell(old_node, i, schema),
                   leaf_node_cell_size(schema));
        }
    }

    *leaf_node_num_cells(old_node) = left_split_count;
    *leaf_node_num_cells(new_node) = right_split_count;

    if(is_node_root(old_node)){
        create_new_root(pager, page_num, new_page_num, schema);
    }
    else{
        uint32_t parent_page_num = *node_parent(old_node);
        uint32_t new_max = get_node_max_key(pager, old_node, schema);
        void* parent = pager_get_page(pager, parent_page_num);

        update_internal_node_key(parent, old_max, new_max);
        internal_node_insert(pager, parent_page_num, new_page_num, schema);
    }
}

/*
 * Inserts a key/row cell at position `cell_num` of the leaf on `page_num`.
 *
 * A full leaf is handed to the split, which places the new cell as part of
 * dividing the node. Otherwise cells at and after `cell_num` are shifted one
 * slot right to open a gap, the key and serialized row are written into it,
 * and the cell count is bumped.
 */
void leaf_node_insert(Pager* pager, uint32_t page_num, uint32_t cell_num, uint32_t key,
                      const Record* value, const Schema* schema){
    void* node = pager_get_page(pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if(num_cells >= leaf_node_max_cells(schema)){
        leaf_node_split_and_insert(pager, page_num, cell_num, key, value, schema);
        return;
    }

    if(cell_num < num_cells){
        /* shift trailing cells right to make room for the new one */
        for(uint32_t i = num_cells; i > cell_num; --i)
            memcpy(leaf_node_cell(node, i, schema),
                   leaf_node_cell(node, i - 1, schema),
                   leaf_node_cell_size(schema));
    }

    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cell_num, schema)) = key;
    serialize_record(value, leaf_node_value(node, cell_num, schema));
}

/*
 * Deletion.
 *
 * Every node but the root keeps a minimum fill, so the tree stays balanced
 * and its height stays logarithmic however rows come and go:
 *
 *   leaves          at least ceil(M/2) cells, M being the leaf capacity
 *   internal nodes  at least ceil(K/2) - 1 keys, K being the key cap
 *
 * These are exactly the sizes a split leaves, so a tree built only by inserts
 * already meets them. Deleting can take a node one below its minimum, and it's
 * then repaired from a sibling under the same parent: by borrowing one entry
 * when the sibling can spare it, and otherwise by merging the two — which a
 * node one short plus a sibling at the minimum always fits. A merge takes a
 * separator and a child out of the parent, which can leave the parent short in
 * turn, so repairs can climb to the root; a root left with a single child
 * gives way to that child.
 *
 * Throughout, every separator stays equal to the largest key in the subtree to
 * its left. Range scans and lookups start from one descent that relies on it,
 * so each step below that changes what a subtree holds also fixes the one
 * separator that describes it.
 */

/* The fewest cells a leaf other than the root may hold: ceil(M/2). */
static uint32_t leaf_node_min_cells(const Schema* schema){
    return (leaf_node_max_cells(schema) + 1) / 2;
}

/* The fewest keys an internal node other than the root may hold: ceil(K/2) - 1. */
static uint32_t internal_node_min_keys(void){
    return (internal_node_max_keys() + 1) / 2 - 1;
}

/*
 * Which child of `parent` the page `child_page_num` is: an index in
 * [0, num_keys], num_keys meaning the right child. Found by page number rather
 * than by key, so it can't be misled by separators that are mid-update. A page
 * missing from its own parent can only mean a corrupt tree.
 */
static uint32_t child_index_in_parent(void* parent, uint32_t child_page_num){
    uint32_t num_keys = *internal_node_num_keys(parent);
    for(uint32_t i = 0; i <= num_keys; ++i){
        if(*internal_node_child(parent, i) == child_page_num)
            return i;
    }
    printf("Corrupt tree: page %u is not a child of its parent\n", child_page_num);
    exit(EXIT_FAILURE);
}

/* Removes cell `cell_num` of a leaf, closing the gap and zeroing the slot it vacates. */
static void leaf_node_remove_cell(void* node, uint32_t cell_num, const Schema* schema){
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t cell_size = leaf_node_cell_size(schema);
    for(uint32_t i = cell_num; i + 1 < num_cells; ++i)
        memcpy(leaf_node_cell(node, i, schema), leaf_node_cell(node, i + 1, schema), cell_size);
    memset(leaf_node_cell(node, num_cells - 1, schema), 0, cell_size);
    *leaf_node_num_cells(node) = num_cells - 1;
}

/* Opens a gap at cell `cell_num` of a leaf and copies `cell` into it. */
static void leaf_node_insert_cell(void* node, uint32_t cell_num, const void* cell, const Schema* schema){
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t cell_size = leaf_node_cell_size(schema);
    for(uint32_t i = num_cells; i > cell_num; --i)
        memcpy(leaf_node_cell(node, i, schema), leaf_node_cell(node, i - 1, schema), cell_size);
    memcpy(leaf_node_cell(node, cell_num, schema), cell, cell_size);
    *leaf_node_num_cells(node) = num_cells + 1;
}

/*
 * After the largest key of the subtree rooted at `page_num` has changed to
 * `new_max`, updates the one separator that records it. That separator sits in
 * the nearest ancestor where the subtree hangs off a child other than the
 * rightmost; climbing while the node is its parent's rightmost child finds it.
 * If the climb reaches the root the key was the whole tree's largest, which no
 * separator records.
 */
static void lower_ancestor_separator(Pager* pager, uint32_t page_num, uint32_t new_max){
    void* node = pager_get_page(pager, page_num);
    while(!is_node_root(node)){
        uint32_t parent_page_num = *node_parent(node);
        void* parent = pager_get_page(pager, parent_page_num);
        uint32_t index = child_index_in_parent(parent, page_num);
        if(index < *internal_node_num_keys(parent)){
            *internal_node_key(parent, index) = new_max;
            return;
        }
        page_num = parent_page_num;
        node = parent;
    }
}

/*
 * Takes child `child_index` out of an internal node after its contents have
 * been merged into child `child_index - 1`, along with the separator between
 * them. The merged node now spans both, so it inherits the separator that used
 * to follow the removed child — the largest key of the pair — or, when the
 * removed child was the rightmost, becomes the rightmost child itself. The
 * vacated cell is zeroed.
 */
static void internal_node_remove_child(void* node, uint32_t child_index){
    uint32_t num_keys = *internal_node_num_keys(node);
    if(child_index == num_keys)
        *internal_node_right_child(node) = *internal_node_child(node, num_keys - 1);
    else{
        *internal_node_key(node, child_index - 1) = *internal_node_key(node, child_index);
        for(uint32_t i = child_index; i + 1 < num_keys; ++i)
            memcpy(internal_node_cell(node, i), internal_node_cell(node, i + 1), INTERNAL_NODE_CELL_SIZE);
    }
    memset(internal_node_cell(node, num_keys - 1), 0, INTERNAL_NODE_CELL_SIZE);
    *internal_node_num_keys(node) = num_keys - 1;
}

/*
 * Replaces a root that has no keys left — one child — with that child. The
 * root has to stay on its page, so the child's contents are copied up into it
 * and the child's own page is freed; the copied children, if any, are pointed
 * at the root page. A child that is a leaf is the only leaf in the tree, so
 * the leaf chain needs no repair.
 */
static void collapse_root(Pager* pager, uint32_t root_page_num){
    void* root = pager_get_page(pager, root_page_num);
    uint32_t child_page_num = *internal_node_right_child(root);
    void* child = pager_get_page(pager, child_page_num);

    memcpy(root, child, PAGE_SIZE);
    set_node_root(root, true);
    *node_parent(root) = 0;
    if(get_node_type(root) == NODE_INTERNAL)
        reparent_children(pager, root, root_page_num);
    pager_free_page(pager, child_page_num);
}

static void internal_node_rebalance(Pager* pager, uint32_t page_num);

/*
 * Called when an internal node has just lost a key to a merge below it. The
 * root needs at least one key; left with none, it collapses onto its child.
 * Any other node below its minimum is repaired from a sibling.
 */
static void after_child_removed(Pager* pager, uint32_t page_num){
    void* node = pager_get_page(pager, page_num);
    if(is_node_root(node)){
        if(*internal_node_num_keys(node) == 0)
            collapse_root(pager, page_num);
        return;
    }
    if(*internal_node_num_keys(node) < internal_node_min_keys())
        internal_node_rebalance(pager, page_num);
}

/*
 * Merges leaf `right_page_num` into its left sibling `left_page_num`: its cells
 * are appended, the left leaf takes over its place in the leaf chain, and its
 * page is freed. The caller removes it from the parent.
 */
static void leaf_node_merge(Pager* pager, uint32_t left_page_num, uint32_t right_page_num, const Schema* schema){
    void* left = pager_get_page(pager, left_page_num);
    void* right = pager_get_page(pager, right_page_num);
    uint32_t left_cells = *leaf_node_num_cells(left);
    uint32_t right_cells = *leaf_node_num_cells(right);

    memcpy(leaf_node_cell(left, left_cells, schema), leaf_node_cell(right, 0, schema),
           right_cells * leaf_node_cell_size(schema));
    *leaf_node_num_cells(left) = left_cells + right_cells;
    *leaf_node_next_leaf(left) = *leaf_node_next_leaf(right);
    pager_free_page(pager, right_page_num);
}

/*
 * Repairs a leaf that has fallen one below its minimum. In order of
 * preference:
 *
 *   - the left sibling can spare a cell: its last cell moves to our front, and
 *     the separator between us drops to the left sibling's new largest key;
 *   - the right sibling can spare one: its first cell moves to our end, and
 *     our separator rises to that key, our new largest;
 *   - neither can: merge with the left sibling, or with the right one if we're
 *     the first child, and take the emptied leaf out of the parent.
 *
 * Borrowing changes nothing above the parent: the pair's combined keys, and so
 * the separator above them, stay the same. A merge removes a key from the
 * parent, which may then need repairing itself.
 */
static void leaf_node_rebalance(Pager* pager, uint32_t page_num, const Schema* schema){
    void* node = pager_get_page(pager, page_num);
    uint32_t parent_page_num = *node_parent(node);
    void* parent = pager_get_page(pager, parent_page_num);
    uint32_t index = child_index_in_parent(parent, page_num);
    uint32_t parent_keys = *internal_node_num_keys(parent);
    uint32_t min_cells = leaf_node_min_cells(schema);

    if(index > 0){
        uint32_t left_page_num = *internal_node_child(parent, index - 1);
        void* left = pager_get_page(pager, left_page_num);
        uint32_t left_cells = *leaf_node_num_cells(left);
        if(left_cells > min_cells){
            leaf_node_insert_cell(node, 0, leaf_node_cell(left, left_cells - 1, schema), schema);
            leaf_node_remove_cell(left, left_cells - 1, schema);
            *internal_node_key(parent, index - 1) = *leaf_node_key(left, left_cells - 2, schema);
            return;
        }
    }
    if(index < parent_keys){
        uint32_t right_page_num = *internal_node_child(parent, index + 1);
        void* right = pager_get_page(pager, right_page_num);
        if(*leaf_node_num_cells(right) > min_cells){
            uint32_t node_cells = *leaf_node_num_cells(node);
            leaf_node_insert_cell(node, node_cells, leaf_node_cell(right, 0, schema), schema);
            leaf_node_remove_cell(right, 0, schema);
            *internal_node_key(parent, index) = *leaf_node_key(node, node_cells, schema);
            return;
        }
    }

    if(index > 0){
        leaf_node_merge(pager, *internal_node_child(parent, index - 1), page_num, schema);
        internal_node_remove_child(parent, index);
    }
    else{
        leaf_node_merge(pager, page_num, *internal_node_child(parent, index + 1), schema);
        internal_node_remove_child(parent, index + 1);
    }
    after_child_removed(pager, parent_page_num);
}

/*
 * Merges internal node `right_page_num` into its left sibling `left_page_num`,
 * given the parent's separator between them. The left node's rightmost child
 * becomes an ordinary cell keyed by that separator — it was the largest key on
 * the left — then the right node's cells follow and its rightmost child
 * becomes the left node's. The moved children are pointed at their new
 * parent, and the right node's page is freed. The caller removes it from the
 * parent.
 */
static void internal_node_merge(Pager* pager, uint32_t left_page_num, uint32_t right_page_num, uint32_t separator){
    void* left = pager_get_page(pager, left_page_num);
    void* right = pager_get_page(pager, right_page_num);
    uint32_t left_keys = *internal_node_num_keys(left);
    uint32_t right_keys = *internal_node_num_keys(right);

    *internal_node_cell(left, left_keys) = *internal_node_right_child(left);
    *internal_node_key(left, left_keys) = separator;
    memcpy(internal_node_cell(left, left_keys + 1), internal_node_cell(right, 0),
           right_keys * INTERNAL_NODE_CELL_SIZE);
    *internal_node_right_child(left) = *internal_node_right_child(right);
    *internal_node_num_keys(left) = left_keys + 1 + right_keys;

    for(uint32_t i = left_keys + 1; i <= left_keys + right_keys + 1; ++i){
        void* child = pager_get_page(pager, *internal_node_child(left, i));
        *node_parent(child) = left_page_num;
    }
    pager_free_page(pager, right_page_num);
}

/*
 * Repairs an internal node that has fallen one below its minimum, like a leaf
 * but moving children with their separators:
 *
 *   - the left sibling can spare a key: its rightmost child becomes our first
 *     child, keyed by the parent's separator between us (that child's largest
 *     key), and the separator drops to the left sibling's new largest;
 *   - the right sibling can spare one: our rightmost child becomes a cell keyed
 *     by our separator, the right sibling's first child becomes our rightmost,
 *     and our separator rises to that child's key;
 *   - neither can: merge with a sibling, pulling the separator between us down.
 *
 * A child that moves is pointed at its new parent. A merge removes a key from
 * the parent, so the repair may continue one level up.
 */
static void internal_node_rebalance(Pager* pager, uint32_t page_num){
    void* node = pager_get_page(pager, page_num);
    uint32_t parent_page_num = *node_parent(node);
    void* parent = pager_get_page(pager, parent_page_num);
    uint32_t index = child_index_in_parent(parent, page_num);
    uint32_t parent_keys = *internal_node_num_keys(parent);
    uint32_t node_keys = *internal_node_num_keys(node);
    uint32_t min_keys = internal_node_min_keys();

    if(index > 0){
        uint32_t left_page_num = *internal_node_child(parent, index - 1);
        void* left = pager_get_page(pager, left_page_num);
        uint32_t left_keys = *internal_node_num_keys(left);
        if(left_keys > min_keys){
            uint32_t moved_child = *internal_node_right_child(left);
            for(uint32_t i = node_keys; i > 0; --i)
                memcpy(internal_node_cell(node, i), internal_node_cell(node, i - 1), INTERNAL_NODE_CELL_SIZE);
            *internal_node_cell(node, 0) = moved_child;
            *internal_node_key(node, 0) = *internal_node_key(parent, index - 1);
            *internal_node_num_keys(node) = node_keys + 1;

            *internal_node_right_child(left) = *internal_node_child(left, left_keys - 1);
            *internal_node_key(parent, index - 1) = *internal_node_key(left, left_keys - 1);
            memset(internal_node_cell(left, left_keys - 1), 0, INTERNAL_NODE_CELL_SIZE);
            *internal_node_num_keys(left) = left_keys - 1;

            *node_parent(pager_get_page(pager, moved_child)) = page_num;
            return;
        }
    }
    if(index < parent_keys){
        uint32_t right_page_num = *internal_node_child(parent, index + 1);
        void* right = pager_get_page(pager, right_page_num);
        uint32_t right_keys = *internal_node_num_keys(right);
        if(right_keys > min_keys){
            uint32_t moved_child = *internal_node_child(right, 0);
            *internal_node_cell(node, node_keys) = *internal_node_right_child(node);
            *internal_node_key(node, node_keys) = *internal_node_key(parent, index);
            *internal_node_num_keys(node) = node_keys + 1;
            *internal_node_right_child(node) = moved_child;

            *internal_node_key(parent, index) = *internal_node_key(right, 0);
            for(uint32_t i = 0; i + 1 < right_keys; ++i)
                memcpy(internal_node_cell(right, i), internal_node_cell(right, i + 1), INTERNAL_NODE_CELL_SIZE);
            memset(internal_node_cell(right, right_keys - 1), 0, INTERNAL_NODE_CELL_SIZE);
            *internal_node_num_keys(right) = right_keys - 1;

            *node_parent(pager_get_page(pager, moved_child)) = page_num;
            return;
        }
    }

    if(index > 0){
        internal_node_merge(pager, *internal_node_child(parent, index - 1), page_num,
                            *internal_node_key(parent, index - 1));
        internal_node_remove_child(parent, index);
    }
    else{
        internal_node_merge(pager, page_num, *internal_node_child(parent, index + 1),
                            *internal_node_key(parent, index));
        internal_node_remove_child(parent, index + 1);
    }
    after_child_removed(pager, parent_page_num);
}

/*
 * Deletes cell `cell_num` from the leaf on `page_num`.
 *
 * The cell is removed and its slot zeroed. A root leaf needs nothing more: it
 * may hold any number of cells, none included. Any other leaf held at least
 * its minimum — two or more, since every leaf fits at least three — so it
 * still has a cell. If the removed key was the leaf's largest, the separator
 * that recorded it is lowered to the new largest first, while the parent
 * pointers still describe the tree as it was; then a leaf that has dropped
 * below its minimum is repaired, which may cascade to the root.
 */
void leaf_node_delete(Pager* pager, uint32_t page_num, uint32_t cell_num, const Schema* schema){
    void* node = pager_get_page(pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    bool removed_largest = cell_num == num_cells - 1;

    leaf_node_remove_cell(node, cell_num, schema);
    if(is_node_root(node))
        return;

    if(removed_largest)
        lower_ancestor_separator(pager, page_num, *leaf_node_key(node, num_cells - 2, schema));
    if(*leaf_node_num_cells(node) < leaf_node_min_cells(schema))
        leaf_node_rebalance(pager, page_num, schema);
}

/*
 * Prints the sizes that define the on-page layout. Handy for eyeballing how
 * many rows fit in a node and as a regression guard (a test pins these values,
 * so an accidental layout change is caught immediately). The node headers and
 * internal-node capacity are the same for every table; the row, cell and leaf
 * capacity depend on a table's schema and are printed only when one is given.
 */
void print_constants(const Schema* schema){
    if(schema != NULL)
        printf("ROW_SIZE: %u\n", schema->row_size);
    printf("COMMON_NODE_HEADER_SIZE: %u\n", COMMON_NODE_HEADER_SIZE);
    printf("LEAF_NODE_HEADER_SIZE: %u\n", LEAF_NODE_HEADER_SIZE);
    if(schema != NULL)
        printf("LEAF_NODE_CELL_SIZE: %u\n", leaf_node_cell_size(schema));
    printf("LEAF_NODE_SPACE_FOR_CELLS: %u\n", PAGE_SIZE - LEAF_NODE_HEADER_SIZE);
    if(schema != NULL)
        printf("LEAF_NODE_MAX_CELLS: %u\n", leaf_node_max_cells(schema));
    printf("INTERNAL_NODE_MAX_KEYS: %u\n", internal_node_max_keys());
}

/* Prints `level` levels of two-space indentation. */
static void indent(uint32_t level){
    for(uint32_t i = 0; i < level; ++i)
        printf("  ");
}

/*
 * Prints the subtree rooted at `page_num`, depth-first and in key order. A leaf
 * prints its size and then its keys one level deeper. An internal node prints
 * its size, then each child followed by the key separating it from the next,
 * and finally its right child, so separators appear between the subtrees they
 * divide. Holding `node` across the recursive calls is safe because the pager
 * never evicts a cached page, so fetching children can't invalidate it.
 */
void print_tree(Pager* pager, const Schema* schema, uint32_t page_num, uint32_t indentation_level){
    void* node = pager_get_page(pager, page_num);
    uint32_t num_keys;
    uint32_t child;

    switch(get_node_type(node)){
        case NODE_LEAF:
            num_keys = *leaf_node_num_cells(node);
            indent(indentation_level);
            printf("- leaf (size %u)\n", num_keys);
            for(uint32_t i = 0; i < num_keys; ++i){
                indent(indentation_level + 1);
                printf("- %u\n", *leaf_node_key(node, i, schema));
            }
            break;
        case NODE_INTERNAL:
            num_keys = *internal_node_num_keys(node);
            indent(indentation_level);
            printf("- internal (size %u)\n", num_keys);
            for(uint32_t i = 0; i < num_keys; ++i){
                child = *internal_node_child(node, i);
                print_tree(pager, schema, child, indentation_level + 1);
                indent(indentation_level + 1);
                printf("- key %u\n", *internal_node_key(node, i));
            }
            child = *internal_node_right_child(node);
            print_tree(pager, schema, child, indentation_level + 1);
            break;
    }
}

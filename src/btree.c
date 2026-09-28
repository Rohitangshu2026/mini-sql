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
 * Leaf node header: the common header followed by a 4-byte cell count.
 */
#define LEAF_NODE_NUM_CELLS_OFFSET COMMON_NODE_HEADER_SIZE
#define LEAF_NODE_HEADER_SIZE      (COMMON_NODE_HEADER_SIZE + 4u)

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

/* Pointer to the leaf's cell-count field, for reading or writing. */
uint32_t* leaf_node_num_cells(void* node){
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
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
 * Prepares a fresh page as an empty leaf. It starts life as a non-root: the one
 * caller that installs a leaf as a tree's root (db_open, for a new file) sets
 * the flag explicitly afterwards. The kind is stamped so the page describes
 * itself — cursors dispatch on it, and a leftover header byte would send them
 * down the wrong branch.
 */
void initialize_leaf_node(void* node){
    set_node_type(node, NODE_LEAF);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
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

/* Prepares a page as an empty, non-root internal node. */
static void initialize_internal_node(void* node){
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
}

/*
 * The largest key stored in a node, used as the separator key a parent keeps
 * for it. For a leaf that's simply its last cell. For an internal node this
 * returns its last separator key, which is not the true maximum of the subtree
 * (that lives under the right child) — correct for the only caller today, which
 * always passes a leaf, and revisited once internal nodes can be split. The
 * caller must not pass an empty node. An unrecognized type byte means the page
 * is corrupt, so it aborts rather than hand back a made-up key.
 */
static uint32_t get_node_max_key(void* node, const Schema* schema){
    switch(get_node_type(node)){
        case NODE_INTERNAL:
            return *internal_node_key(node, *internal_node_num_keys(node) - 1);
        case NODE_LEAF:
            return *leaf_node_key(node, *leaf_node_num_cells(node) - 1, schema);
    }
    printf("Corrupt node: unknown node type\n");
    exit(EXIT_FAILURE);
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
 *
 * num_keys is set before the child is written because internal_node_child
 * resolves "child num_keys" to the right-child slot in the header.
 */
static void create_new_root(Pager* pager, uint32_t root_page_num, uint32_t right_child_page_num,
                            const Schema* schema){
    void* root = pager_get_page(pager, root_page_num);
    uint32_t left_child_page_num = get_unused_page_num(pager);
    void* left_child = pager_get_page(pager, left_child_page_num);

    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_child_page_num;
    *internal_node_key(root, 0) = get_node_max_key(left_child, schema);
    *internal_node_right_child(root) = right_child_page_num;
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
 * If the node was the root, a new root is created above the two halves.
 * Otherwise the existing parent would need a new key and child, which isn't
 * implemented yet; it can't happen while a split root stops further inserts.
 */
static void leaf_node_split_and_insert(Pager* pager, uint32_t page_num, uint32_t cell_num,
                                       uint32_t key, const Record* value, const Schema* schema){
    void* old_node = pager_get_page(pager, page_num);
    uint32_t new_page_num = get_unused_page_num(pager);
    void* new_node = pager_get_page(pager, new_page_num);
    initialize_leaf_node(new_node);

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
        printf("Need to implement updating parent after split\n");
        exit(EXIT_FAILURE);
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
 * Prints the sizes that define the on-page layout. Handy for eyeballing how
 * many rows fit in a node and as a regression guard (a test pins these values,
 * so an accidental layout change is caught immediately).
 */
void print_constants(const Schema* schema){
    printf("ROW_SIZE: %u\n", schema->row_size);
    printf("COMMON_NODE_HEADER_SIZE: %u\n", COMMON_NODE_HEADER_SIZE);
    printf("LEAF_NODE_HEADER_SIZE: %u\n", LEAF_NODE_HEADER_SIZE);
    printf("LEAF_NODE_CELL_SIZE: %u\n", leaf_node_cell_size(schema));
    printf("LEAF_NODE_SPACE_FOR_CELLS: %u\n", PAGE_SIZE - LEAF_NODE_HEADER_SIZE);
    printf("LEAF_NODE_MAX_CELLS: %u\n", leaf_node_max_cells(schema));
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

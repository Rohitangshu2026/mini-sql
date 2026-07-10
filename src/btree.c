#include "btree.h"
#include "pager.h"   /* PAGE_SIZE */

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
 * bytes keep the parent-page field (and, below, every cell key) 4-byte aligned.
 * The accessors dereference uint32_t* pointers into the page, so a misaligned
 * field would be undefined behavior even though x86/ARM happen to tolerate it —
 * UBSan flags it on the packed layout.
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

/* Marks a fresh page as an empty leaf by zeroing its cell count. */
void initialize_leaf_node(void* node){
    *leaf_node_num_cells(node) = 0;
}

/*
 * Inserts a key/row cell at position `cell_num`. Cells at and after that index
 * are shifted one slot to the right to open a gap, then the new key and
 * serialized row are written into it and the cell count is bumped.
 *
 * Splitting a full node isn't implemented yet, so a full node is treated as a
 * fatal error; the executor guards against this by checking capacity first, so
 * in practice this branch is unreachable during normal inserts.
 */
void leaf_node_insert(void* node, uint32_t cell_num, uint32_t key,
                      const Record* value, const Schema* schema){
    uint32_t num_cells = *leaf_node_num_cells(node);
    if(num_cells >= leaf_node_max_cells(schema)){
        printf("Need to implement splitting a leaf node.\n");
        exit(EXIT_FAILURE);
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

/*
 * Dumps a leaf node's cell count and the key of each cell, in stored order. A
 * debugging aid (the .btree meta command) for watching how keys are arranged —
 * currently insertion order, until sorted insert lands.
 */
void print_leaf_node(void* node, const Schema* schema){
    uint32_t num_cells = *leaf_node_num_cells(node);
    printf("leaf (size %u)\n", num_cells);
    for(uint32_t i = 0; i < num_cells; ++i)
        printf("  - %u : %u\n", i, *leaf_node_key(node, i, schema));
}

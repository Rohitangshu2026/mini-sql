#include "cursor.h"
#include "btree.h"

#include<assert.h>
#include<stdio.h>
#include<stdlib.h>

/*
 * Creates a cursor at the first cell of the table: cell 0 of the leftmost leaf.
 *
 * Rather than a separate walk down the left edge of the tree, this searches for
 * key 0. Keys are unsigned and the parser rejects negatives, so 0 is no larger
 * than any key in the table: at every internal node it routes to child 0, and
 * in the leaf it resolves to cell 0 whether or not a row with id 0 exists.
 * table_find already reports end_of_table as "the resolved cell doesn't exist",
 * which at cell 0 means the leaf is empty — exactly the empty-table case.
 */
Cursor* table_start(Table* table){
    return table_find(table, 0);
}

/*
 * Finds where `key` lives, or belongs, in the tree, descending from the root.
 *
 * Each internal node on the way down reports which child owns the key, and the
 * loop follows that child's page until it lands on a leaf. Stopping only at a
 * leaf is what makes every cursor satisfy the leaf invariant by construction —
 * table_start creates its cursors through here too. Inside the leaf, a binary
 * search yields the cell holding the key or the cell it would be inserted at;
 * end_of_table reflects whether that cell actually exists.
 *
 * The walk is iterative rather than recursive: one page fetch and one binary
 * search per level, so a lookup costs O(log n). A node that is neither internal
 * nor a leaf can only be corrupt, so it aborts rather than read garbage as
 * cells.
 */
Cursor* table_find(Table* table, uint32_t key){
    uint32_t page_num = table->root_page_num;
    void* node = pager_get_page(table->pager, page_num);

    while(get_node_type(node) == NODE_INTERNAL){
        uint32_t child_index = internal_node_find_child(node, key);
        page_num = *internal_node_child(node, child_index);
        node = pager_get_page(table->pager, page_num);
    }

    if(get_node_type(node) != NODE_LEAF){
        printf("Corrupt node: unknown node type\n");
        exit(EXIT_FAILURE);
    }

    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = page_num;
    cursor->cell_num = leaf_node_find_cell(node, key, table->schema);
    cursor->end_of_table = (cursor->cell_num >= *leaf_node_num_cells(node));

    return cursor;
}

/*
 * Resolves the cursor to a pointer at the value (serialized row) it addresses,
 * fetching the node's page through the pager. Reads the page as a leaf, which
 * the cursor invariant guarantees.
 */
void* cursor_value(Cursor* cursor){
    void* page = pager_get_page(cursor->table->pager, cursor->page_num);
    assert(get_node_type(page) == NODE_LEAF);
    return leaf_node_value(page, cursor->cell_num, cursor->table->schema);
}

/* Reads the key of the cell under the cursor, which is the first field of the cell. */
uint32_t cursor_key(Cursor* cursor){
    void* page = pager_get_page(cursor->table->pager, cursor->page_num);
    assert(get_node_type(page) == NODE_LEAF);
    return *leaf_node_key(page, cursor->cell_num, cursor->table->schema);
}

/*
 * Advances to the next cell in key order.
 *
 * Within a leaf that is just the next index. Past the last cell, the cursor
 * follows the leaf's next_leaf pointer to cell 0 of its right sibling, so a
 * scan crosses from one leaf to the next without climbing back through the
 * internal nodes. Only the rightmost leaf has no sibling (next_leaf == 0, which
 * can't be a real sibling because page 0 is the file header), and running off
 * it ends the table. Moving strictly along the leaf chain is what
 * keeps the cursor on a leaf, as the invariant requires.
 *
 * Landing on cell 0 of the sibling assumes no leaf is ever empty except the
 * root of an empty table. That holds while rows are only inserted — a split
 * always leaves both halves populated — and is something deletion will have to
 * either preserve or handle.
 */
void cursor_advance(Cursor* cursor){
    void* node = pager_get_page(cursor->table->pager, cursor->page_num);
    assert(get_node_type(node) == NODE_LEAF);

    cursor->cell_num += 1;
    if(cursor->cell_num >= *leaf_node_num_cells(node)){
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if(next_page_num == 0){
            cursor->end_of_table = true;
        }
        else{
            cursor->page_num = next_page_num;
            cursor->cell_num = 0;
        }
    }
}

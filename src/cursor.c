#include "cursor.h"
#include "btree.h"

#include<assert.h>
#include<stdio.h>
#include<stdlib.h>

/*
 * Enforces the leaf invariant (see cursor.h) for table_start, the one cursor
 * constructor that can't descend yet. Returns the root node if it is a leaf,
 * because then the root is the only leaf and a scan starts in it. If the root
 * is internal, the scan would have to begin at the leftmost leaf, which isn't
 * implemented yet, so rather than build a cursor pointing at an internal node
 * this aborts and names the missing piece. The upcoming scan stage replaces the
 * refusal with a descent to the leftmost leaf, like the one table_find does.
 */
static void* root_leaf_or_abort(Table* table, const char* missing_feature){
    void* root_node = pager_get_page(table->pager, table->root_page_num);
    if(get_node_type(root_node) != NODE_LEAF){
        printf("Need to implement %s\n", missing_feature);
        exit(EXIT_FAILURE);
    }
    return root_node;
}

/*
 * Creates a cursor at the first cell of the table. While the root is a leaf,
 * "first" is simply cell 0 of the root; end_of_table is set when that node is
 * empty so a scan over an empty table does nothing.
 */
Cursor* table_start(Table* table){
    void* root_node = root_leaf_or_abort(table, "scanning a multi-level tree");

    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = table->root_page_num;
    cursor->cell_num = 0;
    cursor->end_of_table = (*leaf_node_num_cells(root_node) == 0);

    return cursor;
}

/*
 * Finds where `key` lives, or belongs, in the tree, descending from the root.
 *
 * Each internal node on the way down reports which child owns the key, and the
 * loop follows that child's page until it lands on a leaf. Stopping only at a
 * leaf is what makes a find cursor satisfy the leaf invariant by construction.
 * Inside the leaf, a binary search yields the cell holding the key or the cell
 * it would be inserted at; end_of_table reflects whether that cell actually
 * exists, keeping the flag's meaning the same as it is for a scan cursor.
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

/*
 * Advances to the next cell. Once cell_num reaches the node's cell count the
 * cursor has run off the end, so end_of_table is set and the scan loop stops.
 * Reads the page as a leaf, which the cursor invariant guarantees; stepping
 * across to a sibling leaf arrives with multi-level scans.
 */
void cursor_advance(Cursor* cursor){
    void* node = pager_get_page(cursor->table->pager, cursor->page_num);
    assert(get_node_type(node) == NODE_LEAF);

    cursor->cell_num += 1;
    if(cursor->cell_num >= *leaf_node_num_cells(node))
        cursor->end_of_table = true;
}

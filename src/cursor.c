#include "cursor.h"
#include "btree.h"

#include<assert.h>
#include<stdio.h>
#include<stdlib.h>

/*
 * The single place the leaf invariant (see cursor.h) is enforced when a cursor
 * is created. Returns the root node if it is a leaf, because then the root is
 * the only leaf and every cursor belongs in it. If the root is internal, the
 * caller would have to descend to a leaf first, which isn't implemented yet,
 * so rather than build a cursor pointing at an internal node this aborts and
 * names the missing piece. Descent into internal nodes replaces this refusal
 * in the upcoming search and scan stages.
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
 * Finds where `key` lives, or belongs, in the tree. While the root is a leaf,
 * that's a binary search of the root's cells. end_of_table reflects whether the
 * resolved cell actually exists, keeping the flag's meaning the same as it is
 * for a scan cursor.
 */
Cursor* table_find(Table* table, uint32_t key){
    void* root_node = root_leaf_or_abort(table, "searching an internal node");

    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = table->root_page_num;
    cursor->cell_num = leaf_node_find_cell(root_node, key, table->schema);
    cursor->end_of_table = (cursor->cell_num >= *leaf_node_num_cells(root_node));

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

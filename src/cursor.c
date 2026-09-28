#include "cursor.h"
#include "btree.h"

#include<stdio.h>
#include<stdlib.h>

/*
 * Creates a cursor at the first cell of the table's root leaf. With only a
 * single node, "start" is simply cell 0 of the root; end_of_table is set when
 * that node is empty so a scan over an empty table does nothing.
 */
Cursor* table_start(Table* table){
    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = table->root_page_num;
    cursor->cell_num = 0;

    void* root_node = pager_get_page(table->pager, table->root_page_num);
    cursor->end_of_table = (*leaf_node_num_cells(root_node) == 0);

    return cursor;
}

/*
 * Finds where `key` lives, or belongs, in the tree.
 *
 * Navigation starts at the root and dispatches on the node's kind. A root leaf
 * is searched directly; an internal node would mean descending to the right
 * child first, which has no implementation yet, so that path aborts loudly
 * rather than silently returning a wrong position. end_of_table reflects
 * whether the resolved cell actually exists, keeping the flag's meaning the
 * same as it is for a scan cursor.
 */
Cursor* table_find(Table* table, uint32_t key){
    void* root_node = pager_get_page(table->pager, table->root_page_num);

    if(get_node_type(root_node) != NODE_LEAF){
        printf("Need to implement searching an internal node\n");
        exit(EXIT_FAILURE);
    }

    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = table->root_page_num;
    cursor->cell_num = leaf_node_find_cell(root_node, key, table->schema);
    cursor->end_of_table = (cursor->cell_num >= *leaf_node_num_cells(root_node));

    return cursor;
}

/*
 * Resolves the cursor to a pointer at the value (serialized row) it addresses,
 * fetching the node's page through the pager.
 */
void* cursor_value(Cursor* cursor){
    void* page = pager_get_page(cursor->table->pager, cursor->page_num);
    return leaf_node_value(page, cursor->cell_num, cursor->table->schema);
}

/*
 * Advances to the next cell. Once cell_num reaches the node's cell count the
 * cursor has run off the end, so end_of_table is set (the scan loop stops). A
 * single node has no sibling to step into yet — that arrives with a multi-node
 * tree.
 */
void cursor_advance(Cursor* cursor){
    void* node = pager_get_page(cursor->table->pager, cursor->page_num);

    cursor->cell_num += 1;
    if(cursor->cell_num >= *leaf_node_num_cells(node))
        cursor->end_of_table = true;
}

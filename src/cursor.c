#include "cursor.h"
#include "btree.h"

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
 * Creates a cursor one past the last cell of the root leaf — the position an
 * appended row is written to. end_of_table is true because the cursor does not
 * point at an existing cell.
 */
Cursor* table_end(Table* table){
    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = table->root_page_num;

    void* root_node = pager_get_page(table->pager, table->root_page_num);
    cursor->cell_num = *leaf_node_num_cells(root_node);
    cursor->end_of_table = true;

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

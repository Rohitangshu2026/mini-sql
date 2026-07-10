#include "executor.h"
#include "btree.h"
#include "cursor.h"

#include<stdlib.h>

/* The id column (column_id 1, the first column) is the row's b-tree key. */
#define KEY_COLUMN_ID 1

/*
 * Appends a row to the table's single leaf node. Capacity is checked up front
 * (splitting a full node isn't implemented yet, so we surface TABLE_FULL rather
 * than let leaf_node_insert abort). The key is pulled from the row's id column;
 * a cursor at the end of the table gives the slot to write into.
 */
static ExecuteResult execute_insert(Statement* statement, Table* table){
    void* node = pager_get_page(table->pager, table->root_page_num);
    if(*leaf_node_num_cells(node) >= leaf_node_max_cells(table->schema))
        return EXECUTE_TABLE_FULL;

    uint32_t key = (uint32_t)record_get_int(&statement->record_to_insert, table->schema, KEY_COLUMN_ID);

    Cursor* cursor = table_end(table);
    void* leaf = pager_get_page(table->pager, cursor->page_num);
    leaf_node_insert(leaf, cursor->cell_num, key, &statement->record_to_insert, table->schema);
    free(cursor);
    return EXECUTE_SUCCESS;
}

/*
 * Prints every row in the table. A cursor walks from the start to end_of_table;
 * each cell is deserialized into a temporary Record, printed, and freed. The
 * statement carries no filter yet, hence the (void) cast.
 */
static ExecuteResult execute_select(Statement* statement, Table* table){
    (void)statement;

    Cursor* cursor = table_start(table);
    Record record;
    while(!cursor->end_of_table){
        deserialize_record(cursor_value(cursor), &record, table->schema);
        print_record(&record, table->schema);
        record_free(&record);
        cursor_advance(cursor);
    }
    free(cursor);
    return EXECUTE_SUCCESS;
}

/* Routes a prepared statement to the matching executor. */
ExecuteResult execute_statement(Statement* statement, Table* table){
    switch(statement->type){
        case STATEMENT_INSERT:
            return execute_insert(statement, table);
        case STATEMENT_SELECT:
            return execute_select(statement, table);
    }
    return EXECUTE_SUCCESS;
}

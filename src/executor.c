#include "executor.h"
#include "cursor.h"

#include<stdlib.h>

static ExecuteResult execute_insert(Statement* statement, Table* table){
    uint32_t max_rows = (PAGE_SIZE / table->schema->row_size) * TABLE_MAX_PAGES;
    if(table->num_rows >= max_rows)
        return EXECUTE_TABLE_FULL;

    Cursor* cursor = table_end(table);
    serialize_record(&statement->record_to_insert, cursor_value(cursor));
    table->num_rows += 1;
    free(cursor);
    return EXECUTE_SUCCESS;
}

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

ExecuteResult execute_statement(Statement* statement, Table* table){
    switch(statement->type){
        case (STATEMENT_INSERT):
            return execute_insert(statement,table);
        case (STATEMENT_SELECT):
            return execute_select(statement,table);
    }
    return EXECUTE_SUCCESS;
}

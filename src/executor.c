#include "executor.h"

#include <stdio.h>

static ExecuteResult execute_insert(Statement* statement, Table* table){
    uint32_t max_rows = (PAGE_SIZE / table->schema->row_size) * TABLE_MAX_PAGES;
    if(table->num_rows >= max_rows)
        return EXECUTE_TABLE_FULL;

    serialize_record(&statement->record_to_insert, table_row_slot(table,table->num_rows));
    table->num_rows += 1;
    return EXECUTE_SUCCESS;
}

static ExecuteResult execute_select(Statement* statement, Table* table){
    (void)statement;
    Record record;
    for(uint32_t i = 0; i < table->num_rows; ++i){
        deserialize_record(table_row_slot(table, i), &record, table->schema);
        print_record(&record, table->schema);
        record_free(&record);
    }
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

#include "executor.h"
#include "cursor.h"

#include<stdlib.h>

/*
 * Inserts a row at its sorted position in the statement's table. The key
 * comes from the table's key column; table_insert finds where it belongs and
 * refuses a key that's already taken.
 */
static ExecuteResult execute_insert(Statement* statement){
    if(!table_insert(statement->table, &statement->record_to_insert))
        return EXECUTE_DUPLICATE_KEY;
    return EXECUTE_SUCCESS;
}

/*
 * Prints every row in the table. A cursor walks from the start to end_of_table;
 * each cell is deserialized into a temporary Record, printed, and freed.
 */
static ExecuteResult execute_select(Statement* statement){
    Table* table = statement->table;
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

/* Creates the table the statement defines; the database takes over the definition. */
static ExecuteResult execute_create_table(Statement* statement, Database* db){
    database_create_table(db, &statement->definition);
    return EXECUTE_SUCCESS;
}

/* Routes a prepared statement to the matching executor. */
ExecuteResult execute_statement(Statement* statement, Database* db){
    switch(statement->type){
        case STATEMENT_INSERT:
            return execute_insert(statement);
        case STATEMENT_SELECT:
            return execute_select(statement);
        case STATEMENT_CREATE_TABLE:
            return execute_create_table(statement, db);
    }
    return EXECUTE_SUCCESS;
}

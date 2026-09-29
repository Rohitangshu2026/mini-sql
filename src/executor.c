#include "executor.h"
#include "cursor.h"
#include "expression.h"
#include "planner.h"

#include<stdio.h>
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
 * Runs a SELECT by its plan, or only describes the plan under EXPLAIN.
 *
 * A scan starts at the first row; a point lookup or range starts at the first
 * key at or above `low`, which one descent through the tree finds. That rests
 * on every separator equalling the largest key on its left: the descent then
 * lands in the leaf holding the first such key, never a leaf too early. Rows
 * are read in key order along the leaf chain, and a lookup or range stops at
 * the first key past `high` without decoding it.
 *
 * Every row read is counted — that's what .stats reports — and checked
 * against the whole WHERE, not just the part the plan used, so conditions on
 * other columns, under OR, or with != still filter correctly. Rows that pass
 * print their chosen columns.
 */
static ExecuteResult execute_select(Statement* statement, Database* db){
    Table* table = statement->table;
    const Plan* plan = &statement->plan;
    uint64_t rows_examined = 0;

    if(statement->explain){
        char description[256];
        plan_describe(plan, table, description, sizeof(description));
        printf("%s\n", description);
        db->last_rows_examined = 0;
        return EXECUTE_SUCCESS;
    }

    if(plan->kind != PLAN_EMPTY){
        Cursor* cursor = plan->kind == PLAN_SCAN ? table_start(table) : table_find(table, plan->low);
        Record record;
        while(!cursor->end_of_table){
            if(plan->kind != PLAN_SCAN && cursor_key(cursor) > plan->high)
                break;
            rows_examined++;
            deserialize_record(cursor_value(cursor), &record, table->schema);
            if(statement->where == NULL || expression_evaluate(statement->where, &record, table->schema))
                print_record_columns(&record, table->schema, statement->column_ids, statement->num_columns);
            record_free(&record);
            cursor_advance(cursor);
        }
        free(cursor);
    }

    db->last_rows_examined = rows_examined;
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
            return execute_select(statement, db);
        case STATEMENT_CREATE_TABLE:
            return execute_create_table(statement, db);
    }
    return EXECUTE_SUCCESS;
}

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

/* What to do with each row a statement's WHERE accepts: the row and its key. */
typedef void (*RowVisitor)(const Statement* statement, const Record* record, uint32_t key, void* context);

/*
 * Reads the rows the statement's plan selects, and hands each one the WHERE
 * accepts to `visit`. Returns how many rows were read, accepted or not — the
 * figure .stats reports.
 *
 * A scan starts at the first row; a point lookup or range starts at the first
 * key at or above `low`, which one descent through the tree finds. That rests
 * on every separator equalling the largest key on its left: the descent then
 * lands in the leaf holding the first such key, never a leaf too early. Rows
 * are read in key order along the leaf chain, and a lookup or range stops at
 * the first key past `high` without decoding it.
 *
 * Every row read is checked against the whole WHERE, not just the part the
 * plan used, so conditions on other columns, under OR, or with != still filter
 * correctly. The tree must not change while this runs; a caller that means to
 * change it collects what it needs first.
 */
static uint64_t visit_matching_rows(const Statement* statement, RowVisitor visit, void* context){
    Table* table = statement->table;
    const Plan* plan = &statement->plan;
    uint64_t rows_examined = 0;

    if(plan->kind == PLAN_EMPTY)
        return 0;

    Cursor* cursor = plan->kind == PLAN_SCAN ? table_start(table) : table_find(table, plan->low);
    Record record;
    while(!cursor->end_of_table){
        uint32_t key = cursor_key(cursor);
        if(plan->kind != PLAN_SCAN && key > plan->high)
            break;
        rows_examined++;
        deserialize_record(cursor_value(cursor), &record, table->schema);
        if(statement->where == NULL || expression_evaluate(statement->where, &record, table->schema))
            visit(statement, &record, key, context);
        record_free(&record);
        cursor_advance(cursor);
    }
    free(cursor);
    return rows_examined;
}

/* Prints the plan a SELECT or DELETE would use, for EXPLAIN; it reads nothing. */
static ExecuteResult explain(const Statement* statement, Database* db){
    char description[256];
    plan_describe(&statement->plan, statement->table, description, sizeof(description));
    printf("%s\n", description);
    db->last_rows_examined = 0;
    return EXECUTE_SUCCESS;
}

/* SELECT's visitor: prints the row's chosen columns. */
static void print_row(const Statement* statement, const Record* record, uint32_t key, void* context){
    (void)key;
    (void)context;
    print_record_columns(record, statement->table->schema, statement->column_ids, statement->num_columns);
}

/* Runs a SELECT by its plan — or only describes the plan, under EXPLAIN. */
static ExecuteResult execute_select(Statement* statement, Database* db){
    if(statement->explain)
        return explain(statement, db);
    db->last_rows_examined = visit_matching_rows(statement, print_row, NULL);
    return EXECUTE_SUCCESS;
}

/* The keys a DELETE has chosen, gathered before any of them is removed. */
typedef struct{
    uint32_t* keys;
    uint32_t count;
    uint32_t capacity;
}KeyList;

/* DELETE's visitor: remembers the row's key, growing the list by doubling. */
static void collect_key(const Statement* statement, const Record* record, uint32_t key, void* context){
    (void)statement;
    (void)record;
    KeyList* list = context;
    if(list->count == list->capacity){
        list->capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        list->keys = realloc(list->keys, list->capacity * sizeof(uint32_t));
    }
    list->keys[list->count++] = key;
}

/*
 * Runs a DELETE in two phases. First the rows are chosen exactly as a SELECT
 * with the same WHERE would choose them, keeping only their keys. Then each
 * key is deleted. Deleting reshapes the tree — cells shift, leaves merge,
 * pages are freed, the root can collapse — so no cursor is walking it while
 * that happens: choosing is finished before changing starts.
 */
static ExecuteResult execute_delete(Statement* statement, Database* db){
    if(statement->explain)
        return explain(statement, db);

    KeyList list = {NULL, 0, 0};
    db->last_rows_examined = visit_matching_rows(statement, collect_key, &list);
    for(uint32_t i = 0; i < list.count; ++i)
        table_delete(statement->table, list.keys[i]);
    free(list.keys);
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
        case STATEMENT_DELETE:
            return execute_delete(statement, db);
    }
    return EXECUTE_SUCCESS;
}

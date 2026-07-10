#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "statement.h"
#include "table.h"

/* Outcome of running a statement against a table. */
typedef enum{
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL   /* the (single) leaf node has no room left */
}ExecuteResult;

/*
 * Runs a prepared statement: appends the row for an insert, prints every row
 * for a select. All storage access goes through a cursor, so the executor is
 * unaware of the b-tree layout.
 */
ExecuteResult execute_statement(Statement* statement, Table* table);

#endif

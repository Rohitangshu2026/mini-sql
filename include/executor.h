#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "database.h"
#include "statement.h"

/* Outcome of running a statement. */
typedef enum{
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY   /* a row with that primary key already exists */
}ExecuteResult;

/*
 * Runs a prepared statement: inserts the row at its sorted position for an
 * insert, prints every row for a select, and creates the table for a CREATE
 * TABLE. All row access goes through a cursor, so the executor is unaware of
 * the b-tree layout.
 */
ExecuteResult execute_statement(Statement* statement, Database* db);

#endif

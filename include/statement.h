#ifndef STATEMENT_H
#define STATEMENT_H

#include<stdbool.h>

#include "database.h"
#include "expression.h"
#include "parser.h"
#include "planner.h"
#include "record.h"
#include "table.h"
#include "table_definition.h"

/* The kinds of statement the engine recognizes. */
typedef enum{
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_CREATE_TABLE,
    STATEMENT_DELETE
}StatementType;

/*
 * A prepared statement: its kind, the table it acts on (INSERT, SELECT and
 * DELETE), the row to insert (INSERT), the checked definition of a new table
 * (CREATE TABLE), the columns to print (SELECT), and for a SELECT or DELETE
 * the bound WHERE, the plan for reading the table and whether it's only to be
 * explained. Fields a kind doesn't use stay zeroed, so statement_free is
 * always safe.
 */
typedef struct{
    StatementType type;
    Table* table;                 /* borrowed from the database */
    Record record_to_insert;
    TableDefinition definition;   /* owned until the table is created */
    uint32_t* column_ids;         /* SELECT: heap-owned, the columns to print in order */
    uint32_t num_columns;
    BoundExpr* where;             /* SELECT, DELETE: heap-owned; NULL without a WHERE */
    Plan plan;                    /* SELECT, DELETE: how the table will be read */
    bool explain;                 /* SELECT, DELETE: print the plan instead of running it */
}Statement;

/* Outcome of turning a line of SQL into a Statement. */
typedef enum{
    PREPARE_SUCCESS,    /* the statement is ready to execute */
    PREPARE_EMPTY,      /* a blank line or a lone ';': nothing to run */
    PREPARE_ERROR       /* a syntax, type or other error, described in the SqlError */
}PrepareResult;

/*
 * Parses one line of SQL and checks it against the database: the tables it
 * names must exist (or, for CREATE TABLE, must not), and the values must fit
 * their columns. Fills in `statement` on success. On PREPARE_ERROR the message
 * in `error` is ready to print, and `statement` holds nothing that needs
 * freeing.
 */
PrepareResult prepare_statement(Database* db, const char* sql, Statement* statement, SqlError* error);

/*
 * Frees whatever a statement still owns: the row built for an insert, a table
 * definition that was never executed, a select's column list, and the WHERE of
 * a select or delete. Safe on any prepared statement.
 */
void statement_free(Statement* statement);

#endif

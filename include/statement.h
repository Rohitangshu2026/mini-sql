#ifndef STATEMENT_H
#define STATEMENT_H

#include "parser.h"
#include "record.h"

/* The kinds of statement the engine recognizes. */
typedef enum{
    STATEMENT_INSERT,
    STATEMENT_SELECT
}StatementType;

/*
 * A prepared statement: its kind, plus the row to insert (only meaningful for
 * STATEMENT_INSERT; left zeroed for others so record_free is a safe no-op).
 */
typedef struct{
    StatementType type;
    Record record_to_insert;
}Statement;

/* Outcome of turning a line of SQL into a Statement. */
typedef enum{
    PREPARE_SUCCESS,    /* the statement is ready to execute */
    PREPARE_EMPTY,      /* a blank line or a lone ';': nothing to run */
    PREPARE_ERROR       /* a syntax, type or other error, described in the SqlError */
}PrepareResult;

/*
 * Registers the one table statements can name, and its schema. A stand-in for
 * a catalog: with a single hardcoded table the binder has nowhere else to look
 * a name up. `name` is borrowed and must outlive every prepare_statement call.
 */
void statement_set_default_table(const char* name, Schema* schema);

/*
 * Parses one line of SQL and checks it against the table it names, filling in
 * `statement` on success — including the row for an insert. On PREPARE_ERROR
 * the message in `error` is ready to print, and `statement` holds nothing that
 * needs freeing.
 */
PrepareResult prepare_statement(const char* sql, Statement* statement, SqlError* error);

#endif

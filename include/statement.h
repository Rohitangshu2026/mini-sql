#ifndef STATEMENT_H
#define STATEMENT_H

#include "input_buffer.h"
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

/* Outcome of parsing a line of input into a Statement. */
typedef enum{
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,          /* wrong number of fields */
    PREPARE_NEGATIVE_ID,           /* an integer column got a negative value */
    PREPARE_STRING_TOO_LONG,       /* a text value exceeds its column width */
    PREPARE_UNRECOGNIZED_STATEMENT /* not an insert/select */
}PrepareResult;

/*
 * Registers the schema the parser validates inserts against. A temporary stand
 * -in for a catalog: with one hardcoded table the parser has nowhere else to
 * learn the row shape from.
 */
void statement_set_default_schema(Schema* schema);

/*
 * Parses one line into `statement`. On a successful insert it also builds the
 * row into statement->record_to_insert. Returns a PrepareResult describing the
 * outcome.
 */
PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement);

#endif

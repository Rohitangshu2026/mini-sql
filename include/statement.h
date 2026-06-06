#ifndef STATEMENT_H
#define STATEMENT_H

#include "input_buffer.h"
#include "record.h"

typedef enum{
    STATEMENT_INSERT,
    STATEMENT_SELECT
}StatementType;

typedef struct{
    StatementType type;
    Record record_to_insert;
}Statement;

typedef enum{
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_NEGATIVE_ID,
    PREPARE_STRING_TOO_LONG,
    PREPARE_UNRECOGNIZED_STATEMENT
}PrepareResult;

void statement_set_default_schema(Schema* schema);
PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement);

#endif

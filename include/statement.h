#ifndef STATEMENT_H
#define STATEMENT_H

#include "input_buffer.h"

typedef enum{
    STATEMENT_INSERT,
    STATEMENT_SELECT
}StatementType;

typedef struct{
    StatementType type;
}Statement;

typedef enum{
    PREPARE_SUCCESS,
    PREPARE_UNRECOGNIZED_STATEMENT
}PrepareResult;

PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement);

#endif

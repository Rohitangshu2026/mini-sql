#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "statement.h"
#include "table.h"

typedef enum{
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL
}ExecuteResult;

ExecuteResult execute_statement(Statement* statement, Table* table);

#endif

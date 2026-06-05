#include "executor.h"

#include <stdio.h>

void execute_statement(Statement* statement){
    switch(statement->type){
        case (STATEMENT_INSERT):
            printf("Execute `INSERT`!\n");
            break;
        case (STATEMENT_SELECT):
            printf("Execute `SELECT`!\n");
            break;
    }
}

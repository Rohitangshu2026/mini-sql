#include "executor.h"
#include "input_buffer.h"
#include "meta_command.h"
#include "schema.h"
#include "statement.h"
#include "table.h"

#include<stdbool.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<time.h>

static void print_prompt(void){
    printf("db > ");
}

int main(void){
    ColumnDefinition users_columns[] = { 
        {.name = "id", .type = COLUMN_INT, .size = 4}, 
        {.name = "username",.type = COLUMN_TEXT, .size = 32}, 
        {.name = "email",.type = COLUMN_TEXT, .size = 255},
    };

    uint32_t num_columns = sizeof(users_columns) / sizeof(users_columns[0]);
    Schema* schema = schema_create(users_columns, num_columns);
    Table* table = table_new(schema);
    statement_set_default_schema(schema);

    InputBuffer* input_buffer = new_input_buffer();
    while (true) {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->buffer[0] == '.'){
            switch(do_meta_command(input_buffer, table)){
                case (META_COMMAND_SUCCESS):
                    continue;
                case (META_COMMAND_UNRECOGNIZED_COMMAND):
                     printf("Unrecognized command '%s'.\n", input_buffer->buffer);
                     continue;
            }
        } 

        Statement statement = {0};
        switch(prepare_statement(input_buffer,&statement)){
            case(PREPARE_SUCCESS):
                break;
            case (PREPARE_NEGATIVE_ID):
                printf("ID must be positive.\n");
                continue;
            case (PREPARE_STRING_TOO_LONG):
                printf("String is too long.\n");
                continue;
            case (PREPARE_SYNTAX_ERROR):
                printf("Syntax error. Could not parse statement.\n");
                continue;
            case(PREPARE_UNRECOGNIZED_STATEMENT):
                printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
                continue;
        }

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        ExecuteResult result = execute_statement(&statement, table);
        clock_gettime(CLOCK_MONOTONIC, &end);
        record_free(&statement.record_to_insert);
        long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
        switch(result){
            case (EXECUTE_SUCCESS):
                printf("Executed. (%.3f ms)\n", elapsed_ns / 1e6);
                break;
            case (EXECUTE_TABLE_FULL):
                printf("Error: Table full.\n");
                break;
        }
    }
}

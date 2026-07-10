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

/* Prints the interactive prompt (no newline — input follows on the same line). */
static void print_prompt(void){
    printf("db > ");
}

/*
 * Entry point and REPL loop.
 *
 * Requires the database filename as argv[1]. Builds the hardcoded "users"
 * schema, opens the database against it, and tells the parser which schema to
 * validate inserts with. Then it loops: read a line, route "." lines to the
 * meta-command handler, otherwise prepare and execute a statement, timing
 * execution and reporting the outcome. The loop only ends via ".exit", which
 * exits from inside do_meta_command, so control never falls off the end here.
 */
int main(int argc, char* argv[]){
    if(argc < 2){
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    ColumnDefinition users_columns[] = {
        {.name = "id", .type = COLUMN_INT, .size = 4},
        {.name = "username", .type = COLUMN_TEXT, .size = 32},
        {.name = "email", .type = COLUMN_TEXT, .size = 255},
    };

    uint32_t num_columns = sizeof(users_columns) / sizeof(users_columns[0]);
    Schema* schema = schema_create(users_columns, num_columns);
    Table* table = db_open(argv[1], schema);
    statement_set_default_schema(schema);

    InputBuffer* input_buffer = new_input_buffer();
    while(true){
        print_prompt();
        read_input(input_buffer);

        if(input_buffer->buffer[0] == '.'){
            switch(do_meta_command(input_buffer, table)){
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED_COMMAND:
                    printf("Unrecognized command '%s'.\n", input_buffer->buffer);
                    continue;
            }
        }

        Statement statement = {0};
        switch(prepare_statement(input_buffer, &statement)){
            case PREPARE_SUCCESS:
                break;
            case PREPARE_NEGATIVE_ID:
                printf("ID must be positive.\n");
                continue;
            case PREPARE_STRING_TOO_LONG:
                printf("String is too long.\n");
                continue;
            case PREPARE_SYNTAX_ERROR:
                printf("Syntax error. Could not parse statement.\n");
                continue;
            case PREPARE_UNRECOGNIZED_STATEMENT:
                printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
                continue;
        }

        /* Time just the execution so the reported figure excludes parsing. */
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        ExecuteResult result = execute_statement(&statement, table);
        clock_gettime(CLOCK_MONOTONIC, &end);

        /* Free the row built for an insert (a no-op for select's empty record). */
        record_free(&statement.record_to_insert);

        long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
        switch(result){
            case EXECUTE_SUCCESS:
                printf("Executed. (%.3f ms)\n", elapsed_ns / 1e6);
                break;
            case EXECUTE_TABLE_FULL:
                printf("Error: Table full.\n");
                break;
        }
    }
}

#include "database.h"
#include "executor.h"
#include "input_buffer.h"
#include "meta_command.h"
#include "statement.h"

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
 * Requires the database filename as argv[1], and opens it — creating an
 * empty database, or loading every table an existing one's catalog lists.
 * Then it loops: read a line, route "." lines to the meta-command handler,
 * otherwise prepare and execute a statement, timing execution and reporting
 * the outcome. A statement that fails to prepare prints its error message,
 * and a blank line prints nothing. The loop only ends via ".exit", which exits
 * from inside do_meta_command, so control never falls off the end here.
 */
int main(int argc, char* argv[]){
    if(argc < 2){
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    Database* db = db_open(argv[1]);

    InputBuffer* input_buffer = new_input_buffer();
    while(true){
        print_prompt();
        read_input(input_buffer);

        if(input_buffer->buffer[0] == '.'){
            switch(do_meta_command(input_buffer, db)){
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED_COMMAND:
                    printf("Unrecognized command '%s'.\n", input_buffer->buffer);
                    continue;
            }
        }

        Statement statement = {0};
        SqlError error;
        switch(prepare_statement(db, input_buffer->buffer, &statement, &error)){
            case PREPARE_SUCCESS:
                break;
            case PREPARE_EMPTY:
                continue;
            case PREPARE_ERROR:
                printf("%s\n", error.message);
                continue;
        }

        /* Time just the execution so the reported figure excludes parsing. */
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        ExecuteResult result = execute_statement(&statement, db);
        clock_gettime(CLOCK_MONOTONIC, &end);

        /* Free what the statement still owns: an insert's row, an unused definition. */
        statement_free(&statement);

        long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
        switch(result){
            case EXECUTE_SUCCESS:
                printf("Executed. (%.3f ms)\n", elapsed_ns / 1e6);
                break;
            case EXECUTE_DUPLICATE_KEY:
                printf("Error: Duplicate key.\n");
                break;
        }
    }
}

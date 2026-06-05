#include "executor.h"
#include "input_buffer.h"
#include "meta_command.h"
#include "statement.h"

#include<stdbool.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<time.h>

static void print_prompt(void){
    printf("db > ");
}

int main(void){
    InputBuffer* input_buffer = new_input_buffer();
    while (true) {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->buffer[0] == '.'){
            switch(do_meta_command(input_buffer)){
                case (META_COMMAND_SUCCESS):
                    continue;
                case (META_COMMAND_UNRECOGNIZED_COMMAND):
                     printf("Unrecognized command '%s'.\n", input_buffer->buffer);
                     continue;
            }
        } 

        Statement statement;
        switch(prepare_statement(input_buffer,&statement)){
            case(PREPARE_SUCCESS):
                break;
            case(PREPARE_UNRECOGNIZED_STATEMENT):
                printf("Unrecognized keyword at start of '%s", input_buffer->buffer);
                continue;
        }

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        execute_statement(&statement);
        clock_gettime(CLOCK_MONOTONIC, &end);
        long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
        printf("Executed. (%.3f ms)\n", elapsed_ns / 1e6);
    }
}

#include "meta_command.h"
#include "btree.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/*
 * Handles the "." commands.
 *
 *   .exit       flush and close the database, then terminate the process.
 *   .btree      print the root leaf node (cell count + keys) for debugging.
 *   .constants  print the on-page layout sizes.
 *
 * The debugging commands return META_COMMAND_SUCCESS so the REPL resumes;
 * ".exit" never returns. Anything else is reported as unrecognized so the
 * caller can print an error.
 */
MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table){
    if(strcmp(input_buffer->buffer, ".exit") == 0){
        close_input_buffer(input_buffer);
        db_close(table);   /* flush pages, close file, free pager + schema + table */
        exit(EXIT_SUCCESS);
    }
    if(strcmp(input_buffer->buffer, ".btree") == 0){
        printf("Tree:\n");
        print_leaf_node(pager_get_page(table->pager, table->root_page_num), table->schema);
        return META_COMMAND_SUCCESS;
    }
    if(strcmp(input_buffer->buffer, ".constants") == 0){
        printf("Constants:\n");
        print_constants(table->schema);
        return META_COMMAND_SUCCESS;
    }

    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

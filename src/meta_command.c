#include "meta_command.h"

#include<stdlib.h>
#include<string.h>

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table){
    if(strcmp(input_buffer->buffer, ".exit") == 0){
        close_input_buffer(input_buffer);
        db_close(table);        /* flush pages, close file, free pager + schema + table */
        exit(EXIT_SUCCESS);
    }
    
    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

#include "meta_command.h"

#include<stdlib.h>
#include<string.h>

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table){
    if(strcmp(input_buffer->buffer, ".exit") == 0){
        close_input_buffer(input_buffer);
        schema_free(table->schema);
        table_free(table);
        exit(EXIT_SUCCESS);
    }
    
    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

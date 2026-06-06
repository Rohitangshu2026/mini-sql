#include "statement.h"

#include<stdlib.h>
#include<string.h>

static Schema* default_schema = NULL;

void statement_set_default_schema(Schema* schema){
    default_schema = schema;
}

static PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement){
    statement->type = STATEMENT_INSERT;
    const Schema* schema = default_schema;
    const ColumnDefinition* cols = schema->columns;

    strtok(input_buffer->buffer, " ");

    char* values[schema->num_columns];
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        char* token = strtok(NULL, " ");
        if(token == NULL)
            return PREPARE_SYNTAX_ERROR;

        if(cols[i].type == COLUMN_INT){
            if(atoi(token) < 0)
                return PREPARE_NEGATIVE_ID;
        } 
        else{                               
            if(strlen(token) > cols[i].size)
                return PREPARE_STRING_TOO_LONG;
        }
        values[i] = token;
    }

    record_init(&statement->record_to_insert, schema);
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        if(cols[i].type == COLUMN_INT)
            record_set_int (&statement->record_to_insert, schema, cols[i].column_id, atoi(values[i]));
        else
            record_set_text(&statement->record_to_insert, schema, cols[i].column_id, values[i]);
    }
    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement){
    if(strncmp(input_buffer->buffer, "insert", 6) == 0)
        return prepare_insert(input_buffer, statement);
    if(strncmp(input_buffer->buffer, "select", 6) == 0){
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    }
    return PREPARE_UNRECOGNIZED_STATEMENT;
}

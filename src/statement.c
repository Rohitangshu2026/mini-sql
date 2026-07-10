#include "statement.h"

#include<stdlib.h>
#include<string.h>

/*
 * The schema the parser validates and builds inserts against. A file-scope
 * stand-in for a catalog: with one hardcoded table there is nowhere else for
 * prepare_insert to learn the row shape from. Set once at startup.
 */
static Schema* default_schema = NULL;

/* Records the schema future inserts are parsed against. */
void statement_set_default_schema(Schema* schema){
    default_schema = schema;
}

/*
 * Parses an "insert" line into `statement`, driven entirely by the schema so it
 * works for any table shape.
 *
 * Two passes: first split the line into one token per column with strtok and
 * validate each against its column (integers must be non-negative, text must
 * fit the field width), bailing out before any allocation on the first bad
 * token. Only once every token is valid do we build the Record — this ordering
 * means an error return never leaks a half-built row. Token pointers stay valid
 * because strtok rewrites the input buffer in place.
 */
static PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement){
    statement->type = STATEMENT_INSERT;
    const Schema* schema = default_schema;
    const ColumnDefinition* cols = schema->columns;

    strtok(input_buffer->buffer, " ");   /* skip the "insert" keyword */

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
            record_set_int(&statement->record_to_insert, schema, cols[i].column_id, atoi(values[i]));
        else
            record_set_text(&statement->record_to_insert, schema, cols[i].column_id, values[i]);
    }
    return PREPARE_SUCCESS;
}

/*
 * Dispatches a line to the right parser by leading keyword. "select" needs no
 * arguments yet, so it just sets the type. Anything else is unrecognized.
 */
PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement){
    if(strncmp(input_buffer->buffer, "insert", 6) == 0)
        return prepare_insert(input_buffer, statement);
    if(strncmp(input_buffer->buffer, "select", 6) == 0){
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    }
    return PREPARE_UNRECOGNIZED_STATEMENT;
}

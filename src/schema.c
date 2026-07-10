#define _POSIX_C_SOURCE 200809L   /* strdup under strict -std=c11 */

#include "schema.h"

#include<stdlib.h>
#include<string.h>

/*
 * Deep-copies `columns` into a fresh Schema, assigning each column a 1-based id
 * and its byte offset as it goes.
 *
 * Ownership: names are strdup'd and the array is malloc'd, so the caller keeps
 * whatever it passed in and the Schema owns its own copy (released by
 * schema_free). Offsets accumulate in declaration order, so a row is simply the
 * columns laid end to end and row_size is the running total. Returns NULL if
 * either allocation fails, freeing the struct on the second failure.
 */
Schema* schema_create(const ColumnDefinition* columns, uint32_t num_columns){
    Schema* schema = malloc(sizeof(Schema));
    if(schema == NULL)
        return NULL;

    schema->version = 1;
    schema->num_columns = num_columns;

    schema->columns = malloc(num_columns * sizeof(ColumnDefinition));
    if(schema->columns == NULL){
        free(schema);
        return NULL;
    }

    uint32_t offset = 0;
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        schema->columns[i] = columns[i];
        schema->columns[i].name = strdup(columns[i].name);
        schema->columns[i].column_id = i + 1;
        schema->columns[i].offset = offset;

        offset += schema->columns[i].size;
    }

    schema->row_size = offset;
    schema->next_column_id = num_columns + 1;
    return schema;
}

/*
 * Releases a Schema and every column name it owns. The NULL guard lets a
 * half-opened connection be torn down without the caller checking first.
 */
void schema_free(Schema* schema){
    if(schema == NULL)
        return;

    for(uint32_t i = 0; i < schema->num_columns; ++i)
        free(schema->columns[i].name);

    free(schema->columns);
    free(schema);
}

/*
 * Linear scan for the column with `column_id`. O(num_columns), which is fine
 * for the handful of columns a table has. Returns a borrowed pointer or NULL.
 */
const ColumnDefinition* schema_find_column_by_id(const Schema* schema, uint32_t column_id){
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        if(schema->columns[i].column_id == column_id){
            return &schema->columns[i];
        }
    }
    return NULL;
}

/*
 * Linear scan for an exact name match. Returns a borrowed pointer or NULL. The
 * parser uses this to learn a column's width before validating input against it.
 */
const ColumnDefinition* schema_find_column_by_name(const Schema* schema, const char* name){
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        if(strcmp(schema->columns[i].name, name) == 0){
            return &schema->columns[i];
        }
    }
    return NULL;
}

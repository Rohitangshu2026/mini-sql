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
 * Compares two table or column names the way SQL does: ignoring the case of
 * ASCII letters, so `Users`, `users` and `USERS` all name the same table.
 * Other bytes must match exactly. Written out rather than using strcasecmp,
 * which is POSIX rather than C11 and depends on the locale.
 */
bool schema_names_equal(const char* a, const char* b){
    for(;; ++a, ++b){
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if(ca != cb)
            return false;
        if(ca == '\0')
            return true;
    }
}

/*
 * Linear scan for a column by name, compared with schema_names_equal so the
 * lookup ignores case. Returns a borrowed pointer or NULL. The binder uses it
 * to resolve the column names an INSERT lists.
 */
const ColumnDefinition* schema_find_column_by_name(const Schema* schema, const char* name){
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        if(schema_names_equal(schema->columns[i].name, name)){
            return &schema->columns[i];
        }
    }
    return NULL;
}

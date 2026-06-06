#include "schema.h"

#include<stdlib.h>
#include<string.h>

Schema* schema_create(const ColumnDefinition* columns, uint32_t num_columns){
    Schema* schema = malloc(sizeof(Schema));
    if (schema == NULL)
    return NULL;

    schema->version = 1;
    schema->num_columns = num_columns;

    schema->columns = malloc(num_columns * sizeof(ColumnDefinition));
    if (schema->columns == NULL) {
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

void schema_free(Schema* schema){
    if(schema == NULL)
        return;

    for(uint32_t i = 0; i < schema->num_columns; ++i)
        free(schema->columns[i].name);

    free(schema->columns);
    free(schema);
}

const ColumnDefinition* schema_find_column_by_id(const Schema* schema, uint32_t column_id){
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        if(schema->columns[i].column_id == column_id){
            return &schema->columns[i];
        }
    }
    return NULL;
}

const ColumnDefinition* schema_find_column_by_name(const Schema* schema, const char* name){
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        if(strcmp(schema->columns[i].name, name) == 0){
            return &schema->columns[i];
        }
    }
    return NULL;
}

#ifndef SCHEMA_H
#define SCHEMA_H

#include<stdint.h>

typedef enum{
    COLUMN_INT,
    COLUMN_TEXT
}ColumnType;

typedef struct{
    uint32_t column_id;
    char* name;
    ColumnType type;
    uint32_t size;
    uint32_t offset;
}ColumnDefinition;

typedef struct{
    uint32_t version;
    uint32_t next_column_id;
    
    uint32_t num_columns;
    ColumnDefinition* columns;

    uint32_t row_size;
}Schema;

Schema* schema_create(const ColumnDefinition* columns, uint32_t num_columns);
void schema_free(Schema* schema);

const ColumnDefinition* schema_find_column_by_id(const Schema* schema, uint32_t column_id);
const ColumnDefinition* schema_find_column_by_name(const Schema* schema, const char* name);

#endif

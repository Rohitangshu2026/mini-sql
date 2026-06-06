#include "record.h"

#include<stdlib.h>
#include<string.h>
#include<stdio.h>

void record_init(Record* record, const Schema* schema){
    record->payload_size = schema->row_size;
    record->payload = calloc(1,schema->row_size);
}

void record_free(Record* record){
    if(record == NULL || record->payload == NULL)
        return;
    free(record->payload);
    record->payload = NULL;
    record->payload_size = 0;
}

void record_set_int(Record* record, const Schema* schema, uint32_t column_id, int32_t value){
    const ColumnDefinition* column = schema_find_column_by_id(schema,column_id);
    if(column == NULL)
        return;
    memcpy((char*)record->payload + column->offset, &value, sizeof(int32_t));
}

void record_set_text(Record* record, const Schema* schema, uint32_t column_id, const char* value){
    const ColumnDefinition* column = schema_find_column_by_id(schema,column_id);
    if(column == NULL)
        return;
    strncpy((char*)record->payload + column->offset, value, column->size);
}

int32_t record_get_int(const Record* record, const Schema* schema, uint32_t column_id){
    const ColumnDefinition* column = schema_find_column_by_id(schema,column_id);
    int32_t value = 0;
    if(column == NULL)
        return value;
    memcpy(&value,(const char*)record->payload + column->offset, sizeof(int32_t));
    return value;
}

const char* record_get_text(const Record* record, const Schema* schema, uint32_t column_id){
    const ColumnDefinition* column = schema_find_column_by_id(schema,column_id);
    if(column == NULL)
        return NULL;
    return (const char*)record->payload + column->offset;
}

void serialize_record(const Record* record, void* destination){
    memcpy(destination, record->payload,record->payload_size);
}

void deserialize_record(const void* source, Record* record, const Schema* schema){
    record_init(record,schema);
    memcpy(record->payload,source,schema->row_size);
}

void print_record(const Record* record, const Schema* schema){
    printf("(");
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        const ColumnDefinition* column = &schema->columns[i];
        if(i > 0)
            printf(", ");
        if(column->type == COLUMN_INT)
            printf("%d",record_get_int(record,schema,column->column_id));
        else if(column->type == COLUMN_TEXT)
            printf("%.*s",(int)column->size,record_get_text(record,schema,column->column_id));
    }
    printf(")\n");
}
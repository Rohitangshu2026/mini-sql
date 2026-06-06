#ifndef RECORD_H
#define RECORD_H

#include "schema.h"

#include<stdint.h>

typedef struct{
    void* payload;
    uint32_t payload_size;
}Record;

void record_init(Record* record, const Schema* schema);
void record_free(Record* record);

void record_set_int(Record* record, const Schema* schema, uint32_t column_id, int32_t value);
void record_set_text(Record* record, const Schema* schema, uint32_t column_id, const char* value);

int32_t record_get_int(const Record* record, const Schema* schema, uint32_t column_id);
const char* record_get_text(const Record* record, const Schema* schema, uint32_t column_id);

void serialize_record(const Record* record, void* destination);
void deserialize_record(const void* source, Record* record, const Schema* schema);

void print_record(const Record* record, const Schema* schema);

#endif

#ifndef TABLE_H
#define TABLE_H

#include "schema.h"

#include<stdint.h>

#define TABLE_MAX_PAGES 100
extern const uint32_t PAGE_SIZE;

typedef struct{
    Schema* schema;
    uint32_t num_rows;
    void* pages[TABLE_MAX_PAGES];
}Table;

Table* table_new(Schema* schema);
void table_free(Table* table);
void* table_row_slot(Table* table, uint32_t row_num);

#endif

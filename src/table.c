#include"table.h"

#include<stdlib.h>

const uint32_t PAGE_SIZE = 4096;

Table* table_new(Schema* schema){
    Table* table = malloc(sizeof(Table));
    if(table == NULL)
        return NULL;
    table->schema = schema;
    table->num_rows = 0;
    for(uint32_t i = 0; i < TABLE_MAX_PAGES; ++i)
        table->pages[i] = NULL;
    return table;
}

void table_free(Table* table){
    if(table == NULL)
        return;
    for(uint32_t i = 0; i < TABLE_MAX_PAGES; ++i)
        free(table->pages[i]);
    free(table);
}

void* table_row_slot(Table* table, uint32_t row_num){
    uint32_t rows_per_page = PAGE_SIZE / table->schema->row_size;
    uint32_t page_num = row_num / rows_per_page;

    void* page = table->pages[page_num];
    if(page == NULL)
        page = table->pages[page_num] = malloc(PAGE_SIZE);

    uint32_t row_offset = row_num % rows_per_page;
    uint32_t byte_offset = row_offset * table->schema->row_size;
    return (char*)page + byte_offset;
}

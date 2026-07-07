#include "table.h"

#include<stdlib.h>

Table* db_open(const char* filename, Schema* schema){
    Pager* pager = pager_open(filename);

    Table* table = malloc(sizeof(Table));
    table->pager    = pager;
    table->schema   = schema;
    /* The file length encodes how many rows we have. This is a temporary hack
     * (cstack's) that goes away once the B-tree stores its own row count. */
    table->num_rows = pager->file_length / schema->row_size;

    return table;
}

void db_close(Table* table){
    Pager*   pager          = table->pager;
    uint32_t rows_per_page  = PAGE_SIZE / table->schema->row_size;
    uint32_t num_full_pages = table->num_rows / rows_per_page;

    for(uint32_t i = 0; i < num_full_pages; ++i){
        if(pager->pages[i] == NULL)
            continue;
        pager_flush(pager, i, PAGE_SIZE);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }

    /* A partial page at the end still needs writing (see db_open's num_rows). */
    uint32_t num_additional_rows = table->num_rows % rows_per_page;
    if(num_additional_rows > 0){
        uint32_t page_num = num_full_pages;
        if(pager->pages[page_num] != NULL){
            pager_flush(pager, page_num, num_additional_rows * table->schema->row_size);
            free(pager->pages[page_num]);
            pager->pages[page_num] = NULL;
        }
    }

    pager_close(pager);
    schema_free(table->schema);   /* db_open borrowed it; the connection owns teardown */
    free(table);
}

#include "cursor.h"

#include<stdlib.h>

Cursor* table_start(Table* table){
    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->row_num = 0;
    cursor->end_of_table = (table->num_rows == 0);
    return cursor;
}

Cursor* table_end(Table* table){
    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->row_num = table->num_rows;
    cursor->end_of_table = true;
    return cursor;
}

void* cursor_value(Cursor* cursor){
    uint32_t rows_per_page = PAGE_SIZE / cursor->table->schema->row_size;
    uint32_t page_num = cursor->row_num / rows_per_page;

    void* page = pager_get_page(cursor->table->pager, page_num);

    uint32_t row_offset = cursor->row_num % rows_per_page;
    uint32_t byte_offset = row_offset * cursor->table->schema->row_size;
    return (char*)page + byte_offset;
}

void cursor_advance(Cursor* cursor){
    cursor->row_num += 1;
    if(cursor->row_num >= cursor->table->num_rows)
        cursor->end_of_table = true;
}

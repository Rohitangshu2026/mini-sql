#ifndef CURSOR_H
#define CURSOR_H

#include<stdbool.h>
#include<stdint.h>

#include "table.h"

/* A location within a table. For the current append-only store that's just a
 * row number; the abstraction lets the executor stay ignorant of storage so
 * the B-tree can replace it later. */
typedef struct{
    Table* table;
    uint32_t row_num;
    bool end_of_table;   /* one past the last row */
}Cursor;

Cursor* table_start(Table* table);
Cursor* table_end(Table* table);
void* cursor_value(Cursor* cursor);
void cursor_advance(Cursor* cursor);

#endif

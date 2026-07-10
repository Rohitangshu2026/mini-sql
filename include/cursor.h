#ifndef CURSOR_H
#define CURSOR_H

#include<stdbool.h>
#include<stdint.h>

#include "table.h"

/*
 * A position within a table: the page holding a leaf node and the cell within
 * that node. The executor moves and dereferences a cursor without knowing how
 * the tree is laid out, so the storage engine can grow more levels later
 * without the executor changing.
 */
typedef struct{
    Table* table;
    uint32_t page_num;    /* page of the leaf node the cursor sits in */
    uint32_t cell_num;    /* cell index within that node */
    bool end_of_table;    /* true when positioned one past the last cell */
}Cursor;

/* Cursor at the first cell of the table (end_of_table if the table is empty). */
Cursor* table_start(Table* table);

/* Cursor one past the last cell — the position a new row is appended at. */
Cursor* table_end(Table* table);

/* Pointer to the value (serialized row) the cursor points at. */
void* cursor_value(Cursor* cursor);

/* Moves the cursor to the next cell, setting end_of_table when it runs off. */
void cursor_advance(Cursor* cursor);

#endif

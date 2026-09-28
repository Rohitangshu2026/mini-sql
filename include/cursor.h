#ifndef CURSOR_H
#define CURSOR_H

#include<stdbool.h>
#include<stdint.h>

#include "table.h"

/*
 * A position within a table: the page holding a leaf node and the cell within
 * that node. The executor moves and dereferences a cursor without knowing how
 * the tree is laid out, so the storage engine can grow more levels without the
 * executor changing.
 *
 * Invariant: a Cursor always addresses a leaf node.
 *
 * Rows live only in leaves, and cursor_value / cursor_advance read leaf cells
 * directly, so a cursor on an internal node would read routing data (child page
 * numbers and separator keys) as if it were rows. The invariant is established
 * where cursors are created — table_start and table_find are the only
 * constructors — and asserted where it's relied on. table_find satisfies it by
 * construction, descending until it reaches a leaf. table_start can't descend
 * yet, so it refuses a tree whose root is internal rather than hand back a
 * cursor that would violate it.
 */
typedef struct{
    Table* table;
    uint32_t page_num;    /* page of the leaf node the cursor sits in */
    uint32_t cell_num;    /* cell index within that node */
    bool end_of_table;    /* true when positioned one past the last cell */
}Cursor;

/*
 * Cursor at the first cell of the table (end_of_table if the table is empty).
 * Refuses, for now, a tree whose root is internal; see the invariant above.
 */
Cursor* table_start(Table* table);

/*
 * Cursor at `key`'s position, or at the position it would occupy if absent —
 * which is what makes it serve double duty as "find" and "where to insert".
 * Descends from the root through internal nodes to the leaf that owns `key`,
 * so it works on a tree of any height.
 */
Cursor* table_find(Table* table, uint32_t key);

/* Pointer to the value (serialized row) the cursor points at. */
void* cursor_value(Cursor* cursor);

/* Moves the cursor to the next cell, setting end_of_table when it runs off. */
void cursor_advance(Cursor* cursor);

#endif

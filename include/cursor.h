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
 * numbers and separator keys) as if it were rows. The invariant holds by
 * construction: table_start and table_find are the only constructors, and both
 * reach their leaf through the same descent from the root, which stops only at
 * a leaf. cursor_advance preserves it by moving only along the chain of leaves,
 * and the functions that read cells assert it.
 */
typedef struct{
    Table* table;
    uint32_t page_num;    /* page of the leaf node the cursor sits in */
    uint32_t cell_num;    /* cell index within that node */
    bool end_of_table;    /* true when positioned one past the last cell */
}Cursor;

/*
 * Cursor at the first cell of the table — cell 0 of the leftmost leaf — with
 * end_of_table set if the table is empty.
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

/*
 * The key of the cell the cursor points at, read straight from the leaf, so a
 * range scan can tell it has gone past its end without decoding the row. The
 * cursor must not be at end_of_table.
 */
uint32_t cursor_key(Cursor* cursor);

/*
 * Moves the cursor to the next cell in key order, crossing into the next leaf
 * after the last cell of this one, and setting end_of_table after the last
 * cell of the rightmost leaf.
 */
void cursor_advance(Cursor* cursor);

#endif

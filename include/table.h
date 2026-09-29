#ifndef TABLE_H
#define TABLE_H

#include<stdbool.h>
#include<stdint.h>

#include "pager.h"
#include "record.h"
#include "schema.h"
#include "table_definition.h"

/*
 * A table is a b-tree stored in the database file, identified by the page that
 * holds its root node. It carries everything needed to read and write its
 * rows: the schema describing them, which column is the key the tree is
 * ordered by, and the CREATE TABLE text it was defined with. The file itself
 * belongs to the database, so the pager is only borrowed.
 */
typedef struct{
    char* name;               /* heap-owned, as written in CREATE TABLE */
    Schema* schema;           /* heap-owned row layout */
    uint32_t key_column_id;   /* the INT PRIMARY KEY column; its value is the b-tree key */
    uint32_t root_page_num;   /* page of the b-tree root, which never moves */
    char* sql;                /* heap-owned canonical CREATE TABLE text */
    Pager* pager;             /* borrowed from the database that owns the file */
}Table;

/*
 * Makes a table rooted at `root_page_num` from a checked definition, taking
 * over the definition's name, schema and text — the definition is left empty,
 * so freeing it afterwards is a harmless no-op.
 */
Table* table_create(Pager* pager, uint32_t root_page_num, TableDefinition* definition);

/*
 * Inserts `record` at the position its key belongs, splitting nodes as
 * needed. The key is the value of the table's key column. Returns false,
 * changing nothing, if a row with that key already exists.
 */
bool table_insert(Table* table, const Record* record);

/* Frees the table and everything it owns; the pager is left alone. */
void table_free(Table* table);

#endif

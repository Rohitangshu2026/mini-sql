#ifndef TABLE_H
#define TABLE_H

#include<stdint.h>

#include "pager.h"
#include "schema.h"

/*
 * A table is a b-tree stored in the pager's file, identified by the page that
 * holds its root node. It also carries the Schema describing its rows. The
 * schema pointer is borrowed at open time and freed by db_close(), which owns
 * the whole connection teardown.
 */
typedef struct{
    Schema* schema;          /* row layout; borrowed until db_close frees it */
    uint32_t root_page_num;  /* page number of the b-tree root node */
    Pager* pager;            /* owns the page cache + backing file */
}Table;

/*
 * Opens a database connection: opens the file through the pager and, for a
 * brand-new file, initializes page 0 as an empty root leaf. Returns a Table
 * bound to `schema`.
 */
Table* db_open(const char* filename, Schema* schema);

/*
 * Flushes every cached page to disk, then closes the file and frees the pager,
 * the schema, and the table. This is the single teardown path (called on
 * ".exit"), which is why it also frees the borrowed schema.
 */
void db_close(Table* table);

#endif

#ifndef DATABASE_H
#define DATABASE_H

#include<stdint.h>

#include "pager.h"
#include "table.h"
#include "table_definition.h"

/*
 * An open database file: the pager over it, the catalog, and every table the
 * catalog lists, in the order they were created. The database owns all of
 * them; tables borrow its pager. db_close is the single place they're torn
 * down.
 */
typedef struct{
    char* filename;           /* heap copy, for messages */
    Pager* pager;             /* owns the file and the page cache */
    Table* catalog;           /* the table of tables */
    Table** tables;           /* heap array of the user tables, in creation order */
    uint32_t num_tables;
    uint32_t capacity;        /* slots in `tables` */
    uint32_t next_table_id;   /* catalog id for the next table created */
    uint64_t last_rows_examined;   /* rows the last SELECT read from leaves, for .stats */
}Database;

/*
 * Opens a database file. A new file gets a header on page 0 and an empty
 * catalog on page 1. An existing file must pass every check first — its
 * header, then each catalog entry, whose CREATE TABLE text is parsed and
 * checked again to rebuild the table's schema. A file that fails any of them
 * is refused with a message and an exit, before a byte is written to it.
 */
Database* db_open(const char* filename);

/*
 * Flushes every loaded page to disk, then closes the file and frees the
 * pager, the catalog, every table and the database itself.
 */
void db_close(Database* db);

/* The table called `name`, ignoring case, or NULL if there is none. */
Table* database_find_table(Database* db, const char* name);

/*
 * Creates a table from a checked definition: gives it an empty root leaf on a
 * new page, records it in the catalog, and adds it to the open tables. Takes
 * over the definition's contents. The caller must already have checked that
 * no table has that name.
 */
void database_create_table(Database* db, TableDefinition* definition);

#endif

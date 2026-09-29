#ifndef CATALOG_H
#define CATALOG_H

#include<stdint.h>

#include "pager.h"
#include "table.h"

/*
 * The catalog is the table of tables: one row per table, recording its id, its
 * name, the page its b-tree starts on and the CREATE TABLE text that defines
 * it. It is itself an ordinary b-tree table, and its own definition is written
 * in the same SQL as everyone else's — parsed at open by the same code — so
 * there is one way to describe a table, not two.
 *
 * A row is 1,096 bytes, so a catalog leaf holds 3; with enough tables the
 * catalog splits and grows internal nodes like any other table.
 */
#define CATALOG_SQL \
    "CREATE TABLE mini_sql_catalog (id INT PRIMARY KEY, name TEXT(64), root_page INT, sql TEXT(1024))"

/* One catalog row, decoded. */
typedef struct{
    uint32_t id;              /* the table's id: 1 for the first table created, and so on */
    char* name;               /* heap-owned */
    uint32_t root_page_num;   /* the page the table's b-tree starts on */
    char* sql;                /* heap-owned CREATE TABLE text */
}CatalogEntry;

/* The catalog table itself, rooted at `root_page_num`. */
Table* catalog_open(Pager* pager, uint32_t root_page_num);

/* Records a table in the catalog. The name and text are copied into the row. */
void catalog_add(Table* catalog, uint32_t id, const char* name, uint32_t root_page_num, const char* sql);

/*
 * Every catalog row, in id order — which is the order the tables were created
 * in. Sets `count` and returns a heap array for catalog_entries_free (NULL
 * when the catalog is empty).
 */
CatalogEntry* catalog_load(Table* catalog, uint32_t* count);

/* Frees an array returned by catalog_load. */
void catalog_entries_free(CatalogEntry* entries, uint32_t count);

#endif

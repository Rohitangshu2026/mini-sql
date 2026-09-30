#include "database.h"
#include "btree.h"
#include "catalog.h"
#include "file_header.h"
#include "parser.h"

#include<stdarg.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/* The page a new database puts its catalog's root on: the first after the header. */
#define NEW_CATALOG_ROOT_PAGE_NUM 1u

/*
 * Refuses to open the file: prints the formatted message and exits. It's only
 * ever called before anything has been written, so the file is left exactly
 * as it was found.
 */
static void refuse(const char* format, ...){
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    printf("\n");
    exit(EXIT_FAILURE);
}

/* Appends a table to the open tables, growing the array by doubling. */
static void add_table(Database* db, Table* table){
    if(db->num_tables == db->capacity){
        db->capacity = db->capacity == 0 ? 8 : db->capacity * 2;
        db->tables = realloc(db->tables, db->capacity * sizeof(Table*));
    }
    db->tables[db->num_tables++] = table;
}

/* Makes a fresh page the empty root leaf of a new b-tree. */
static uint32_t allocate_root_leaf(Pager* pager){
    uint32_t page_num = pager_allocate_page(pager);
    void* node = pager_get_page(pager, page_num);
    initialize_leaf_node(node);
    set_node_root(node, true);
    return page_num;
}

/*
 * Rebuilds every table the catalog lists. Each entry's CREATE TABLE text is
 * parsed and checked by exactly the code a new CREATE TABLE goes through, so a
 * table can't come back from disk in a shape it couldn't have been created in.
 * The rebuilt name must match the catalog's own record of it, and the root
 * must be a node page of the file. Any failure means the file is corrupt, and
 * it's refused. The next id handed out is one past the largest seen.
 */
static void load_tables(Database* db){
    uint32_t count;
    CatalogEntry* entries = catalog_load(db->catalog, &count);

    for(uint32_t i = 0; i < count; ++i){
        const CatalogEntry* entry = &entries[i];
        Ast ast;
        SqlError error;
        TableDefinition definition;

        if(!parse_statement(entry->sql, &ast, &error))
            refuse("Error: %s is corrupt: the stored definition of table %s doesn't parse.",
                   db->filename, entry->name);
        if(ast.kind != AST_CREATE_TABLE ||
           !table_definition_from_ast(&ast.create_table, &definition, &error))
            refuse("Error: %s is corrupt: the stored definition of table %s is invalid.",
                   db->filename, entry->name);
        ast_free(&ast);

        if(!schema_names_equal(definition.name, entry->name))
            refuse("Error: %s is corrupt: the catalog lists table %s, but its definition is for %s.",
                   db->filename, entry->name, definition.name);
        if(entry->root_page_num == 0 || entry->root_page_num >= db->pager->num_pages)
            refuse("Error: %s is corrupt: table %s has root page %u, which is not a node page of the file.",
                   db->filename, entry->name, entry->root_page_num);

        add_table(db, table_create(db->pager, entry->root_page_num, &definition));
        if(entry->id >= db->next_table_id)
            db->next_table_id = entry->id + 1;
    }

    catalog_entries_free(entries, count);
}

/*
 * Opens the file through the pager and either creates a database in it or
 * checks and loads the one it holds. A new file's pages are fetched in order
 * — header, then catalog root — since each new page must be the next one past
 * the end of the file.
 */
Database* db_open(const char* filename){
    Database* db = calloc(1, sizeof(Database));
    size_t filename_length = strlen(filename);
    db->filename = malloc(filename_length + 1);
    memcpy(db->filename, filename, filename_length + 1);
    db->pager = pager_open(filename);
    db->next_table_id = 1;

    if(db->pager->num_pages == 0){
        void* header = pager_get_page(db->pager, 0);
        file_header_initialize(header, NEW_CATALOG_ROOT_PAGE_NUM);
        allocate_root_leaf(db->pager);
        db->catalog = catalog_open(db->pager, NEW_CATALOG_ROOT_PAGE_NUM);
        return db;
    }

    void* header = pager_get_page(db->pager, 0);
    char message[512];
    if(!file_header_validate(header, db->pager->num_pages, filename, message, sizeof(message)))
        refuse("%s", message);

    db->pager->free_head = file_header_free_head(header);
    db->catalog = catalog_open(db->pager, file_header_catalog_root_page(header));
    load_tables(db);
    return db;
}

/*
 * Flushes every resident page, then releases everything. The free list's head
 * lives in the pager while the database is open, so it's written back into the
 * header first. Pages are written whether or not they changed; this is the
 * only point data reaches disk.
 */
void db_close(Database* db){
    Pager* pager = db->pager;
    file_header_set_free_head(pager_get_page(pager, 0), pager->free_head);
    for(uint32_t i = 0; i < pager->num_pages; ++i){
        if(pager->pages[i] == NULL)
            continue;
        pager_flush(pager, i);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }
    pager_close(pager);

    for(uint32_t i = 0; i < db->num_tables; ++i)
        table_free(db->tables[i]);
    free(db->tables);
    table_free(db->catalog);
    free(db->filename);
    free(db);
}

/* Linear search by name; a database holds few enough tables for that. */
Table* database_find_table(Database* db, const char* name){
    for(uint32_t i = 0; i < db->num_tables; ++i){
        if(schema_names_equal(db->tables[i]->name, name))
            return db->tables[i];
    }
    return NULL;
}

/*
 * Gives the new table a root leaf, records it in the catalog, then adds it to
 * the open tables. The root page is allocated before the catalog insert, which
 * may itself allocate pages if the catalog splits.
 */
void database_create_table(Database* db, TableDefinition* definition){
    uint32_t root_page_num = allocate_root_leaf(db->pager);
    catalog_add(db->catalog, db->next_table_id++, definition->name, root_page_num, definition->sql);
    add_table(db, table_create(db->pager, root_page_num, definition));
}

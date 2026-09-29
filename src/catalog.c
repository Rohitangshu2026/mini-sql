#include "catalog.h"
#include "cursor.h"
#include "parser.h"
#include "table_definition.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/*
 * Builds the catalog's table from CATALOG_SQL through the same parse-and-check
 * path as any CREATE TABLE. The text is a constant of this build, so a failure
 * can only mean the build itself is broken, and it aborts.
 */
Table* catalog_open(Pager* pager, uint32_t root_page_num){
    Ast ast;
    SqlError error;
    TableDefinition definition;

    if(!parse_statement(CATALOG_SQL, &ast, &error) || ast.kind != AST_CREATE_TABLE){
        printf("Internal error: the catalog's own definition doesn't parse\n");
        exit(EXIT_FAILURE);
    }
    if(!table_definition_from_ast(&ast.create_table, &definition, &error)){
        printf("Internal error: the catalog's own definition is invalid: %s\n", error.message);
        exit(EXIT_FAILURE);
    }
    ast_free(&ast);

    return table_create(pager, root_page_num, &definition);
}

/* The id of the catalog column called `name`, looked up rather than assumed. */
static uint32_t catalog_column(const Table* catalog, const char* name){
    return schema_find_column_by_name(catalog->schema, name)->column_id;
}

/*
 * Writes one catalog row through the ordinary insert path. The id is the key,
 * so rows sit in id order and a duplicate id — which would mean two tables
 * sharing one — is impossible to store; it's treated as a bug.
 */
void catalog_add(Table* catalog, uint32_t id, const char* name, uint32_t root_page_num, const char* sql){
    Record record;
    record_init(&record, catalog->schema);
    record_set_int(&record, catalog->schema, catalog_column(catalog, "id"), (int32_t)id);
    record_set_text(&record, catalog->schema, catalog_column(catalog, "name"), name);
    record_set_int(&record, catalog->schema, catalog_column(catalog, "root_page"), (int32_t)root_page_num);
    record_set_text(&record, catalog->schema, catalog_column(catalog, "sql"), sql);

    if(!table_insert(catalog, &record)){
        printf("Internal error: catalog id %u is already taken\n", id);
        exit(EXIT_FAILURE);
    }
    record_free(&record);
}

/*
 * A heap copy of a fixed-width TEXT field. The field is zero-padded, except
 * when the value fills it exactly, in which case there's no terminator to rely
 * on — so the copy stops at the first NUL or at the width, whichever is first.
 */
static char* copy_text_field(const Record* record, const Schema* schema, uint32_t column_id){
    const ColumnDefinition* column = schema_find_column_by_id(schema, column_id);
    const char* text = record_get_text(record, schema, column_id);
    uint32_t length = 0;
    while(length < column->size && text[length] != '\0')
        ++length;

    char* copy = malloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/*
 * Reads every row with a cursor from the start of the catalog, which visits
 * them in key — that is, id — order, decoding each into an entry.
 */
CatalogEntry* catalog_load(Table* catalog, uint32_t* count){
    uint32_t capacity = 0;
    CatalogEntry* entries = NULL;
    *count = 0;

    Cursor* cursor = table_start(catalog);
    Record record;
    while(!cursor->end_of_table){
        if(*count == capacity){
            capacity = capacity == 0 ? 8 : capacity * 2;
            entries = realloc(entries, capacity * sizeof(CatalogEntry));
        }

        deserialize_record(cursor_value(cursor), &record, catalog->schema);
        CatalogEntry* entry = &entries[*count];
        entry->id = (uint32_t)record_get_int(&record, catalog->schema, catalog_column(catalog, "id"));
        entry->name = copy_text_field(&record, catalog->schema, catalog_column(catalog, "name"));
        entry->root_page_num = (uint32_t)record_get_int(&record, catalog->schema, catalog_column(catalog, "root_page"));
        entry->sql = copy_text_field(&record, catalog->schema, catalog_column(catalog, "sql"));
        record_free(&record);

        (*count)++;
        cursor_advance(cursor);
    }
    free(cursor);
    return entries;
}

/* Frees each entry's strings, then the array. */
void catalog_entries_free(CatalogEntry* entries, uint32_t count){
    for(uint32_t i = 0; i < count; ++i){
        free(entries[i].name);
        free(entries[i].sql);
    }
    free(entries);
}

#ifndef TABLE_DEFINITION_H
#define TABLE_DEFINITION_H

#include<stdbool.h>
#include<stdint.h>

#include "parser.h"
#include "schema.h"

/* The longest table name the catalog can record. */
#define TABLE_NAME_MAX_LENGTH 64u

/* The longest CREATE TABLE text the catalog can record. */
#define TABLE_SQL_MAX_LENGTH 1024u

/*
 * The fewest rows a leaf must be able to hold. A split works with fewer, but
 * rebalancing after deletes needs room to move rows between siblings, and it
 * keeps any one row to at most a third of a page.
 */
#define TABLE_MIN_ROWS_PER_LEAF 3u

/*
 * A checked table definition: everything a CREATE TABLE establishes, ready to
 * become a table. `sql` is the canonical CREATE TABLE text regenerated from
 * the definition itself — what the catalog stores and .schema prints — so it
 * always parses back into the same definition.
 */
typedef struct{
    char* name;               /* heap-owned, as written */
    Schema* schema;           /* heap-owned */
    uint32_t key_column_id;   /* the INT PRIMARY KEY column, the b-tree key */
    char* sql;                /* heap-owned canonical CREATE TABLE text */
}TableDefinition;

/*
 * Checks a parsed CREATE TABLE and builds its definition. Everything that
 * doesn't depend on which tables already exist is checked here — name length,
 * duplicate columns, TEXT widths, exactly one INT PRIMARY KEY, row size and
 * definition length — so a new CREATE TABLE and a definition re-read from the
 * catalog go through the same rules. On failure writes the message into
 * `error`, returns false and leaves nothing to free.
 */
bool table_definition_from_ast(const CreateTableAst* ast, TableDefinition* definition, SqlError* error);

/* Frees what a definition still owns and zeroes it. Safe on a zeroed one. */
void table_definition_free(TableDefinition* definition);

#endif

#ifndef SCHEMA_H
#define SCHEMA_H

#include<stdint.h>

/*
 * The value types a column can hold. COLUMN_INT is a fixed 4-byte signed
 * integer; COLUMN_TEXT is a fixed-width byte field whose capacity is the
 * column's `size` (shorter values are zero-padded to fill it).
 */
typedef enum{
    COLUMN_INT,
    COLUMN_TEXT
}ColumnType;

/*
 * Describes a single column and, crucially, where its bytes live inside a
 * serialized row. The offset is computed once by schema_create() so the rest
 * of the engine reads/writes a field by pointer arithmetic rather than
 * hardcoded constants.
 */
typedef struct{
    uint32_t column_id;    /* stable identity; assigned once, never reused */
    char* name;            /* heap-owned copy of the column name */
    ColumnType type;       /* how the field's bytes are interpreted */
    uint32_t size;         /* width of the field in bytes */
    uint32_t offset;       /* byte position of the field within a row */
}ColumnDefinition;

/*
 * The runtime layout of one table: an ordered set of columns plus the derived
 * total row width. Because the layout is data rather than a compiled C struct,
 * it can change at runtime, which is what keeps CREATE TABLE / ALTER TABLE on
 * the table for later.
 */
typedef struct{
    uint32_t version;          /* bumped on each schema change */
    uint32_t next_column_id;   /* id to hand the next column that is added */
    uint32_t num_columns;
    ColumnDefinition* columns; /* heap-owned array, num_columns long */
    uint32_t row_size;         /* sum of every column's size, in bytes */
}Schema;

/*
 * Builds a Schema from a caller-supplied column template. Column names and the
 * array itself are deep-copied, so `columns` may be a stack array of string
 * literals. Returns NULL if allocation fails.
 */
Schema* schema_create(const ColumnDefinition* columns, uint32_t num_columns);

/* Frees a Schema and every name it owns. Safe to call with NULL. */
void schema_free(Schema* schema);

/*
 * Finds a column by its stable id. Returns a pointer into the schema's own
 * array (do not free) or NULL if no column has that id.
 */
const ColumnDefinition* schema_find_column_by_id(const Schema* schema, uint32_t column_id);

/*
 * Finds a column by exact name. Returns a pointer into the schema's own array
 * (do not free) or NULL if there is no such column.
 */
const ColumnDefinition* schema_find_column_by_name(const Schema* schema, const char* name);

#endif

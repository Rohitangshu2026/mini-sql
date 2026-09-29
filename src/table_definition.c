#include "table_definition.h"
#include "btree.h"

#include<stdarg.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/* Longest stretch of a name an error message quotes before cutting it off. */
#define ERROR_PREVIEW_LENGTH 32

/* Writes a formatted message into `error`, truncating it to fit. */
static void set_error(SqlError* error, const char* format, ...){
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error->message, SQL_ERROR_MESSAGE_SIZE, format, arguments);
    va_end(arguments);
}

/* A NUL-terminated heap copy of `text`. */
static char* copy_string(const char* text){
    size_t length = strlen(text);
    char* copy = malloc(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

/*
 * Appends formatted text to a growable heap buffer. Used to build the
 * canonical CREATE TABLE text one column at a time.
 */
static void append(char** buffer, size_t* length, size_t* capacity, const char* format, ...){
    va_list arguments;
    va_start(arguments, format);
    int needed = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);

    if(*length + (size_t)needed + 1 > *capacity){
        while(*length + (size_t)needed + 1 > *capacity)
            *capacity *= 2;
        *buffer = realloc(*buffer, *capacity);
    }

    va_start(arguments, format);
    vsnprintf(*buffer + *length, *capacity - *length, format, arguments);
    va_end(arguments);
    *length += (size_t)needed;
}

/*
 * The canonical text of a definition: keywords in upper case, one space
 * between tokens, names exactly as written. It's generated from the parsed
 * columns rather than kept from the input, so comments and formatting don't
 * reach the catalog, and what's stored is guaranteed to parse back to the
 * same columns.
 */
static char* canonical_sql(const CreateTableAst* ast, size_t* length){
    size_t capacity = 64;
    char* sql = malloc(capacity);
    *length = 0;

    append(&sql, length, &capacity, "CREATE TABLE %s (", ast->table_name);
    for(uint32_t i = 0; i < ast->num_columns; ++i){
        const ColumnAst* column = &ast->columns[i];
        append(&sql, length, &capacity, "%s%s ", i > 0 ? ", " : "", column->name);
        if(column->type == AST_TYPE_INT)
            append(&sql, length, &capacity, "INT");
        else
            append(&sql, length, &capacity, "TEXT(%lld)", (long long)column->width);
        if(column->primary_key)
            append(&sql, length, &capacity, " PRIMARY KEY");
    }
    append(&sql, length, &capacity, ")");
    return sql;
}

/*
 * Checks the definition in order from the table as a whole down to the space
 * it needs, so the message names the most basic problem:
 *
 *   1. the table name fits the catalog;
 *   2. no column is named twice (ignoring case, as names always do);
 *   3. every TEXT column is at least one byte wide;
 *   4. there is exactly one PRIMARY KEY, and it's INT — the key is what the
 *      b-tree orders rows by, and its keys are unsigned 32-bit integers;
 *   5. a row fits at least TABLE_MIN_ROWS_PER_LEAF times in a leaf, with the
 *      limit taken from the b-tree's own layout and the sum done in 64 bits
 *      so absurd widths can't wrap around to something small;
 *   6. the canonical text fits the catalog.
 *
 * Only then is anything kept: the schema is built, and the name and text are
 * handed to the definition. Column ids are assigned 1, 2, 3... in declaration
 * order by schema_create, so the key's id is its position plus one.
 */
bool table_definition_from_ast(const CreateTableAst* ast, TableDefinition* definition, SqlError* error){
    memset(definition, 0, sizeof(TableDefinition));

    size_t name_length = strlen(ast->table_name);
    if(name_length > TABLE_NAME_MAX_LENGTH){
        set_error(error, "Error: table name '%.*s...' is longer than %u bytes.",
                  ERROR_PREVIEW_LENGTH, ast->table_name, TABLE_NAME_MAX_LENGTH);
        return false;
    }

    for(uint32_t i = 0; i < ast->num_columns; ++i){
        for(uint32_t j = 0; j < i; ++j){
            if(schema_names_equal(ast->columns[i].name, ast->columns[j].name)){
                set_error(error, "Error: column '%s' is defined twice.", ast->columns[i].name);
                return false;
            }
        }
    }

    for(uint32_t i = 0; i < ast->num_columns; ++i){
        const ColumnAst* column = &ast->columns[i];
        if(column->type == AST_TYPE_TEXT && !column->width_out_of_range && column->width < 1){
            set_error(error, "Error: column '%s' must be TEXT(1) or wider.", column->name);
            return false;
        }
    }

    uint32_t num_keys = 0;
    uint32_t key_index = 0;
    for(uint32_t i = 0; i < ast->num_columns; ++i){
        if(ast->columns[i].primary_key){
            num_keys++;
            key_index = i;
        }
    }
    if(num_keys == 0){
        set_error(error, "Error: table %s needs an INT PRIMARY KEY column.", ast->table_name);
        return false;
    }
    if(num_keys > 1){
        set_error(error, "Error: table %s has more than one PRIMARY KEY.", ast->table_name);
        return false;
    }
    if(ast->columns[key_index].type != AST_TYPE_INT){
        set_error(error, "Error: PRIMARY KEY column '%s' must be INT.", ast->columns[key_index].name);
        return false;
    }

    uint32_t max_row_size = leaf_node_max_row_size(TABLE_MIN_ROWS_PER_LEAF);
    uint64_t row_size = 0;
    bool too_wide = false;
    for(uint32_t i = 0; i < ast->num_columns; ++i){
        const ColumnAst* column = &ast->columns[i];
        if(column->type == AST_TYPE_INT)
            row_size += sizeof(int32_t);
        else if(column->width_out_of_range || (uint64_t)column->width > max_row_size)
            too_wide = true;
        else
            row_size += (uint64_t)column->width;
    }
    if(too_wide || row_size > max_row_size){
        if(too_wide)
            set_error(error, "Error: a row of table %s would be wider than %u bytes, the most that fit.",
                      ast->table_name, max_row_size);
        else
            set_error(error, "Error: a row of table %s would take %llu bytes; at most %u fit.",
                      ast->table_name, (unsigned long long)row_size, max_row_size);
        return false;
    }

    size_t sql_length;
    char* sql = canonical_sql(ast, &sql_length);
    if(sql_length > TABLE_SQL_MAX_LENGTH){
        set_error(error, "Error: the definition of table %s is %zu bytes; at most %u fit.",
                  ast->table_name, sql_length, TABLE_SQL_MAX_LENGTH);
        free(sql);
        return false;
    }

    ColumnDefinition columns[ast->num_columns];
    for(uint32_t i = 0; i < ast->num_columns; ++i){
        const ColumnAst* column = &ast->columns[i];
        columns[i].name = column->name;
        columns[i].type = column->type == AST_TYPE_INT ? COLUMN_INT : COLUMN_TEXT;
        columns[i].size = column->type == AST_TYPE_INT ? (uint32_t)sizeof(int32_t) : (uint32_t)column->width;
    }

    definition->name = copy_string(ast->table_name);
    definition->schema = schema_create(columns, ast->num_columns);
    definition->key_column_id = key_index + 1;
    definition->sql = sql;
    return true;
}

/* Frees whatever the definition still owns; ownership may have moved on. */
void table_definition_free(TableDefinition* definition){
    free(definition->name);
    schema_free(definition->schema);
    free(definition->sql);
    memset(definition, 0, sizeof(TableDefinition));
}

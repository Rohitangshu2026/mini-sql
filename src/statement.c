#include "statement.h"

#include<stdarg.h>
#include<stdio.h>
#include<string.h>

/*
 * The binder: it takes the parser's syntax tree and checks it against the
 * table it names, turning names into columns and literals into a row. Every
 * check runs before anything is allocated, so an error never leaves a
 * half-built record behind.
 */

/* Longest stretch of a value an error message quotes before cutting it off. */
#define ERROR_PREVIEW_LENGTH 32

/*
 * The one table statements can name, and its schema. A file-scope stand-in for
 * a catalog: with a single hardcoded table there is nowhere else to look a
 * name up. Set once at startup.
 */
static const char* default_table_name = NULL;
static Schema* default_schema = NULL;

/* Records the table future statements are checked against. */
void statement_set_default_table(const char* name, Schema* schema){
    default_table_name = name;
    default_schema = schema;
}

/* Writes a formatted message into `error`, truncating it to fit. */
static void set_error(SqlError* error, const char* format, ...){
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error->message, SQL_ERROR_MESSAGE_SIZE, format, arguments);
    va_end(arguments);
}

/* How many bytes of a literal an error message shows. */
static int preview_length(const Literal* literal){
    return literal->length > ERROR_PREVIEW_LENGTH ? ERROR_PREVIEW_LENGTH : (int)literal->length;
}

/* What to append after the shown bytes: "..." when the literal was cut short. */
static const char* preview_cut(const Literal* literal){
    return literal->length > ERROR_PREVIEW_LENGTH ? "..." : "";
}

/* Checks that `name` is a table that exists. */
static bool bind_table(const char* name, SqlError* error){
    if(schema_names_equal(name, default_table_name))
        return true;
    set_error(error, "Error: no such table: %s.", name);
    return false;
}

/*
 * Checks one value against the column it's going into. INT columns take
 * integers that fit an int32 and aren't negative (the key column can't be, and
 * for now every INT column follows the same rule). TEXT(n) columns take
 * strings of at most n bytes. The kind of value is checked first, so a string
 * in an INT column is reported as the wrong type rather than as anything
 * about its contents.
 */
static bool bind_value(const Literal* literal, const ColumnDefinition* column, SqlError* error){
    if(column->type == COLUMN_INT){
        if(literal->type != LITERAL_INTEGER){
            set_error(error, "Type error: column '%s' is INT, but '%.*s%s' is text.",
                      column->name, preview_length(literal), literal->text, preview_cut(literal));
            return false;
        }
        if(literal->out_of_range || literal->integer < INT32_MIN || literal->integer > INT32_MAX){
            set_error(error, "Type error: %.*s%s is out of range for INT column '%s'.",
                      preview_length(literal), literal->text, preview_cut(literal), column->name);
            return false;
        }
        if(literal->integer < 0){
            set_error(error, "Error: column '%s' must not be negative.", column->name);
            return false;
        }
        return true;
    }

    if(literal->type != LITERAL_STRING){
        set_error(error, "Type error: column '%s' is TEXT, but %.*s%s is an integer.",
                  column->name, preview_length(literal), literal->text, preview_cut(literal));
        return false;
    }
    if(literal->length > column->size){
        set_error(error, "Type error: column '%s' is TEXT(%u), but the value is %u bytes.",
                  column->name, column->size, literal->length);
        return false;
    }
    return true;
}

/*
 * Binds an INSERT: resolves the table, works out which column each value goes
 * into, checks every value, and only then builds the row.
 *
 * Without a column list, values fill the columns in table order. With one,
 * the list may be in any order but must name every column exactly once:
 * there are no NULLs or defaults to fill a gap with. The checks run from the
 * shape of the statement down to individual values — unknown or repeated
 * column, then the value count, then missing columns, then each value — so the
 * error reported is the most basic thing wrong.
 */
static PrepareResult bind_insert(const InsertAst* insert, Statement* statement, SqlError* error){
    if(!bind_table(insert->table_name, error))
        return PREPARE_ERROR;

    const Schema* schema = default_schema;
    uint32_t num_columns = schema->num_columns;
    uint32_t targets[num_columns];   /* targets[i]: the column value i goes into */
    bool listed[num_columns];
    uint32_t num_targets = num_columns;
    memset(listed, 0, sizeof(listed));

    if(insert->column_names != NULL){
        for(uint32_t i = 0; i < insert->num_columns; ++i){
            const ColumnDefinition* column = schema_find_column_by_name(schema, insert->column_names[i]);
            if(column == NULL){
                set_error(error, "Error: no such column: %s.", insert->column_names[i]);
                return PREPARE_ERROR;
            }
            uint32_t index = (uint32_t)(column - schema->columns);
            if(listed[index]){
                set_error(error, "Error: column '%s' is listed twice.", column->name);
                return PREPARE_ERROR;
            }
            listed[index] = true;
            targets[i] = index;
        }
        num_targets = insert->num_columns;
    }
    else{
        for(uint32_t i = 0; i < num_columns; ++i)
            targets[i] = i;
    }

    if(insert->num_values != num_targets){
        set_error(error, "Error: %u values for %u columns.", insert->num_values, num_targets);
        return PREPARE_ERROR;
    }

    if(insert->column_names != NULL){
        for(uint32_t i = 0; i < num_columns; ++i){
            if(!listed[i]){
                set_error(error, "Error: column '%s' has no value.", schema->columns[i].name);
                return PREPARE_ERROR;
            }
        }
    }

    for(uint32_t i = 0; i < insert->num_values; ++i){
        if(!bind_value(&insert->values[i], &schema->columns[targets[i]], error))
            return PREPARE_ERROR;
    }

    statement->type = STATEMENT_INSERT;
    record_init(&statement->record_to_insert, schema);
    for(uint32_t i = 0; i < insert->num_values; ++i){
        const ColumnDefinition* column = &schema->columns[targets[i]];
        const Literal* literal = &insert->values[i];
        if(column->type == COLUMN_INT)
            record_set_int(&statement->record_to_insert, schema, column->column_id, (int32_t)literal->integer);
        else
            record_set_text(&statement->record_to_insert, schema, column->column_id, literal->text);
    }
    return PREPARE_SUCCESS;
}

/* Binds a SELECT, which for now only has to name a table that exists. */
static PrepareResult bind_select(const SelectAst* select, Statement* statement, SqlError* error){
    if(!bind_table(select->table_name, error))
        return PREPARE_ERROR;
    statement->type = STATEMENT_SELECT;
    return PREPARE_SUCCESS;
}

/*
 * Parses the line, then binds the tree it produced. The tree only lives for
 * the duration of this call: everything the executor needs is copied into the
 * Statement.
 */
PrepareResult prepare_statement(const char* sql, Statement* statement, SqlError* error){
    Ast ast;
    if(!parse_statement(sql, &ast, error))
        return PREPARE_ERROR;

    PrepareResult result = PREPARE_ERROR;
    switch(ast.kind){
        case AST_EMPTY:
            result = PREPARE_EMPTY;
            break;
        case AST_INSERT:
            result = bind_insert(&ast.insert, statement, error);
            break;
        case AST_SELECT:
            result = bind_select(&ast.select, statement, error);
            break;
    }

    ast_free(&ast);
    return result;
}

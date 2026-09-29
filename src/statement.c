#include "statement.h"

#include<stdarg.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/*
 * The binder: it takes the parser's syntax tree and checks it against the
 * database, turning table names into tables, column names into columns and
 * literals into a row. Every check runs before anything is allocated, so an
 * error never leaves a half-built record behind.
 */

/* Longest stretch of a value an error message quotes before cutting it off. */
#define ERROR_PREVIEW_LENGTH 32

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

/* The table called `name`, or NULL with the error set if there is none. */
static Table* bind_table(Database* db, const char* name, SqlError* error){
    Table* table = database_find_table(db, name);
    if(table == NULL)
        set_error(error, "Error: no such table: %s.", name);
    return table;
}

/*
 * Checks one value against the column it's going into. INT columns take any
 * integer that fits an int32, except that the key column's can't be negative:
 * keys are the b-tree's unsigned 32-bit integers. TEXT(n) columns take
 * strings of at most n bytes. The kind of value is checked first, so a string
 * in an INT column is reported as the wrong type rather than as anything
 * about its contents.
 */
static bool bind_value(const Literal* literal, const ColumnDefinition* column, bool is_key, SqlError* error){
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
        if(is_key && literal->integer < 0){
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
static PrepareResult bind_insert(Database* db, const InsertAst* insert, Statement* statement, SqlError* error){
    Table* table = bind_table(db, insert->table_name, error);
    if(table == NULL)
        return PREPARE_ERROR;

    const Schema* schema = table->schema;
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
        const ColumnDefinition* column = &schema->columns[targets[i]];
        if(!bind_value(&insert->values[i], column, column->column_id == table->key_column_id, error))
            return PREPARE_ERROR;
    }

    statement->type = STATEMENT_INSERT;
    statement->table = table;
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

/*
 * Binds a SELECT: the table, then the columns to print — every column in
 * order for '*', or each named one, repeats allowed — then the WHERE, and
 * finally chooses how to read the table. Everything is checked before the
 * statement keeps anything, so an error leaves nothing to free.
 */
static PrepareResult bind_select(Database* db, const SelectAst* select, bool explain, Statement* statement,
                                 SqlError* error){
    Table* table = bind_table(db, select->table_name, error);
    if(table == NULL)
        return PREPARE_ERROR;

    const Schema* schema = table->schema;
    uint32_t num_columns = select->column_names == NULL ? schema->num_columns : select->num_columns;
    uint32_t* column_ids = malloc(num_columns * sizeof(uint32_t));
    for(uint32_t i = 0; i < num_columns; ++i){
        if(select->column_names == NULL){
            column_ids[i] = schema->columns[i].column_id;
            continue;
        }
        const ColumnDefinition* column = schema_find_column_by_name(schema, select->column_names[i]);
        if(column == NULL){
            set_error(error, "Error: no such column: %s.", select->column_names[i]);
            free(column_ids);
            return PREPARE_ERROR;
        }
        column_ids[i] = column->column_id;
    }

    BoundExpr* where = NULL;
    if(select->where != NULL){
        where = expression_bind(select->where, schema, error);
        if(where == NULL){
            free(column_ids);
            return PREPARE_ERROR;
        }
    }

    statement->type = STATEMENT_SELECT;
    statement->table = table;
    statement->column_ids = column_ids;
    statement->num_columns = num_columns;
    statement->where = where;
    statement->plan = plan_select(where, table->key_column_id);
    statement->explain = explain;
    return PREPARE_SUCCESS;
}

/*
 * Binds a CREATE TABLE. The one check that needs the database — that the name
 * is free, ignoring case — comes first; everything else is the table
 * definition's, shared with reloading tables from the catalog.
 */
static PrepareResult bind_create_table(Database* db, const CreateTableAst* create, Statement* statement,
                                       SqlError* error){
    if(database_find_table(db, create->table_name) != NULL){
        set_error(error, "Error: table %s already exists.", create->table_name);
        return PREPARE_ERROR;
    }
    if(!table_definition_from_ast(create, &statement->definition, error))
        return PREPARE_ERROR;
    statement->type = STATEMENT_CREATE_TABLE;
    return PREPARE_SUCCESS;
}

/*
 * Parses the line, then binds the tree it produced. The tree only lives for
 * the duration of this call: everything the executor needs is copied into the
 * Statement.
 */
PrepareResult prepare_statement(Database* db, const char* sql, Statement* statement, SqlError* error){
    Ast ast;
    if(!parse_statement(sql, &ast, error))
        return PREPARE_ERROR;

    PrepareResult result = PREPARE_ERROR;
    switch(ast.kind){
        case AST_EMPTY:
            result = PREPARE_EMPTY;
            break;
        case AST_INSERT:
            result = bind_insert(db, &ast.insert, statement, error);
            break;
        case AST_SELECT:
            result = bind_select(db, &ast.select, ast.explain, statement, error);
            break;
        case AST_CREATE_TABLE:
            result = bind_create_table(db, &ast.create_table, statement, error);
            break;
    }

    ast_free(&ast);
    return result;
}

/*
 * Frees an insert's row, a definition that was never turned into a table, and
 * a select's column list and WHERE.
 */
void statement_free(Statement* statement){
    record_free(&statement->record_to_insert);
    table_definition_free(&statement->definition);
    free(statement->column_ids);
    statement->column_ids = NULL;
    expression_free(statement->where);
    statement->where = NULL;
}

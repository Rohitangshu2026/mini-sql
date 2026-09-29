#ifndef PARSER_H
#define PARSER_H

#include<stdbool.h>
#include<stdint.h>

/*
 * The syntax tree the parser builds. It records what a statement says, not
 * whether it makes sense: table and column names are just text here, and
 * values are just literals. Checking them against a table's schema is the
 * binder's job (statement.c), which is what lets this layer parse a statement
 * before the table it names exists.
 */

/* The two kinds of literal value. */
typedef enum{
    LITERAL_INTEGER,
    LITERAL_STRING
}LiteralType;

/*
 * A literal value. Both kinds keep their text, which is what error messages
 * quote: a string's unescaped content, or an integer exactly as written, sign
 * included, so even a number too long to hold can be shown as typed.
 */
typedef struct{
    LiteralType type;
    char* text;             /* heap-owned, NUL-terminated */
    uint32_t length;        /* bytes in text */
    int64_t integer;        /* LITERAL_INTEGER: the value, unless out_of_range */
    bool out_of_range;      /* LITERAL_INTEGER: more digits than an int64_t holds */
}Literal;

/* INSERT INTO table [(column, ...)] VALUES (value, ...) */
typedef struct{
    char* table_name;       /* heap-owned */
    char** column_names;    /* heap-owned; NULL when there's no column list */
    uint32_t num_columns;
    Literal* values;        /* heap-owned */
    uint32_t num_values;
}InsertAst;

/* SELECT * FROM table */
typedef struct{
    char* table_name;       /* heap-owned */
}SelectAst;

/* A column's declared type, as written: INT, or TEXT with a width. */
typedef enum{
    AST_TYPE_INT,
    AST_TYPE_TEXT
}ColumnTypeAst;

/*
 * One column of a CREATE TABLE: its name, type and whether it's the primary
 * key. A TEXT width is kept exactly as written, flag included, so a width too
 * large to hold is reported as too wide rather than wrapping.
 */
typedef struct{
    char* name;             /* heap-owned */
    ColumnTypeAst type;
    int64_t width;          /* AST_TYPE_TEXT: the declared width, unless out_of_range */
    bool width_out_of_range;
    bool primary_key;
}ColumnAst;

/* CREATE TABLE table (column type [PRIMARY KEY], ...) */
typedef struct{
    char* table_name;       /* heap-owned */
    ColumnAst* columns;     /* heap-owned */
    uint32_t num_columns;
}CreateTableAst;

/* Which statement a line holds. AST_EMPTY is a blank line or a lone ';'. */
typedef enum{
    AST_EMPTY,
    AST_INSERT,
    AST_SELECT,
    AST_CREATE_TABLE
}AstKind;

/* A parsed statement: its kind, and the node for that kind. */
typedef struct{
    AstKind kind;
    union{
        InsertAst insert;
        SelectAst select;
        CreateTableAst create_table;
    };
}Ast;

#define SQL_ERROR_MESSAGE_SIZE 256

/*
 * Where the parser and the binder write the one error a statement gets. The
 * message is complete and ready to print, category prefix included:
 * "Syntax error: ...", "Type error: ..." or "Error: ...".
 */
typedef struct{
    char message[SQL_ERROR_MESSAGE_SIZE];
}SqlError;

/*
 * Parses one line of SQL into `ast`. Returns true on success, with `ast`
 * owning heap memory that ast_free releases. Returns false on the first
 * syntax error, with the message in `error` and nothing left to free.
 */
bool parse_statement(const char* sql, Ast* ast, SqlError* error);

/* Frees everything `ast` owns and resets it to AST_EMPTY. */
void ast_free(Ast* ast);

#endif

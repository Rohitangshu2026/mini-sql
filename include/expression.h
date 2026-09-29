#ifndef EXPRESSION_H
#define EXPRESSION_H

#include<stdbool.h>
#include<stdint.h>

#include "parser.h"
#include "record.h"
#include "schema.h"

/*
 * A WHERE expression checked against a table: every column name resolved to
 * a column of the table, every literal converted to a value, and both sides of
 * every comparison known to have the same type. Evaluating one can't fail —
 * everything that could go wrong was caught when it was bound.
 */

/* The two types a value can have. */
typedef enum{
    VALUE_INT,
    VALUE_TEXT
}ValueType;

/* One side of a bound comparison: a column of the row, or a constant. */
typedef struct{
    bool is_column;
    ValueType type;
    uint32_t column_id;   /* is_column: the column to read from the row */
    uint32_t width;       /* is_column, VALUE_TEXT: the column's width in bytes */
    int64_t integer;      /* constant VALUE_INT */
    char* text;           /* constant VALUE_TEXT: heap-owned */
    uint32_t length;      /* constant VALUE_TEXT: bytes in text */
}BoundOperand;

/* A node of a bound expression; shaped exactly like the ExprAst it came from. */
typedef struct BoundExpr{
    ExprKind kind;
    CompareOp op;                   /* EXPR_COMPARISON */
    BoundOperand left;              /* EXPR_COMPARISON */
    BoundOperand right;             /* EXPR_COMPARISON */
    struct BoundExpr** children;    /* AND, OR, NOT: heap-owned */
    uint32_t num_children;
}BoundExpr;

/*
 * Binds a parsed WHERE expression to a table's schema. Returns the bound tree,
 * or NULL with a ready-to-print message in `error` if a column doesn't exist,
 * a comparison mixes types, or an integer is too large to compare.
 */
BoundExpr* expression_bind(const ExprAst* ast, const Schema* schema, SqlError* error);

/*
 * Whether `record` satisfies the expression. INT values compare as 64-bit
 * integers; TEXT values compare byte by byte, case-sensitively, with a
 * stored value ending at its first zero byte or at the column's width.
 */
bool expression_evaluate(const BoundExpr* expr, const Record* record, const Schema* schema);

/* Frees a bound expression. Safe with NULL. */
void expression_free(BoundExpr* expr);

#endif

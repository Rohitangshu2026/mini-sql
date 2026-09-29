#include "expression.h"

#include<stdarg.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/* Longest stretch of a literal an error message quotes before cutting it off. */
#define ERROR_PREVIEW_LENGTH 32

/* Writes a formatted message into `error`, truncating it to fit. */
static void set_error(SqlError* error, const char* format, ...){
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error->message, SQL_ERROR_MESSAGE_SIZE, format, arguments);
    va_end(arguments);
}

/* The name of a value type as a message says it. */
static const char* type_name(ValueType type){
    return type == VALUE_INT ? "INT" : "TEXT";
}

/*
 * How an operand reads in a type error: "INT column 'id'" for a column, the
 * literal as written for a constant — quoted if it's text, and cut short if
 * it's long.
 */
static void describe_operand(const OperandAst* operand, const BoundOperand* bound, char* buffer, size_t size){
    if(bound->is_column){
        snprintf(buffer, size, "%s column '%s'", type_name(bound->type), operand->column_name);
        return;
    }
    const Literal* literal = &operand->literal;
    int shown = literal->length > ERROR_PREVIEW_LENGTH ? ERROR_PREVIEW_LENGTH : (int)literal->length;
    const char* cut = literal->length > ERROR_PREVIEW_LENGTH ? "..." : "";
    if(literal->type == LITERAL_STRING)
        snprintf(buffer, size, "'%.*s%s'", shown, literal->text, cut);
    else
        snprintf(buffer, size, "%.*s%s", shown, literal->text, cut);
}

/*
 * Binds one operand: a column must exist in the table, and takes its type
 * from the schema; a literal becomes a constant of its own type. A literal
 * integer too long for 64 bits is left marked here, and reported by the caller
 * once it knows what it was being compared with.
 */
static bool bind_operand(const OperandAst* operand, const Schema* schema, BoundOperand* bound, SqlError* error){
    memset(bound, 0, sizeof(BoundOperand));
    if(operand->kind == OPERAND_COLUMN){
        const ColumnDefinition* column = schema_find_column_by_name(schema, operand->column_name);
        if(column == NULL){
            set_error(error, "Error: no such column: %s.", operand->column_name);
            return false;
        }
        bound->is_column = true;
        bound->type = column->type == COLUMN_INT ? VALUE_INT : VALUE_TEXT;
        bound->column_id = column->column_id;
        bound->width = column->size;
        return true;
    }

    const Literal* literal = &operand->literal;
    if(literal->type == LITERAL_INTEGER){
        bound->type = VALUE_INT;
        bound->integer = literal->integer;
        return true;
    }
    bound->type = VALUE_TEXT;
    bound->length = literal->length;
    bound->text = malloc(literal->length + 1);
    memcpy(bound->text, literal->text, literal->length + 1);
    return true;
}

/*
 * Binds a comparison: both operands first (so an unknown column is reported
 * before any type problem), then the types must match, then any integer
 * literal must fit 64 bits. Integers are compared in 64 bits, so a literal
 * beyond int32 — `id < 3000000000` — is fine; only one too long to hold at all
 * is an error.
 */
static bool bind_comparison(const ExprAst* ast, const Schema* schema, BoundExpr* bound, SqlError* error){
    if(!bind_operand(&ast->left, schema, &bound->left, error) ||
       !bind_operand(&ast->right, schema, &bound->right, error))
        return false;

    if(bound->left.type != bound->right.type){
        char left[96];
        char right[96];
        describe_operand(&ast->left, &bound->left, left, sizeof(left));
        describe_operand(&ast->right, &bound->right, right, sizeof(right));
        set_error(error, "Type error: cannot compare %s with %s.", left, right);
        return false;
    }

    for(int side = 0; side < 2; ++side){
        const OperandAst* operand = side == 0 ? &ast->left : &ast->right;
        const OperandAst* other = side == 0 ? &ast->right : &ast->left;
        if(operand->kind != OPERAND_LITERAL || operand->literal.type != LITERAL_INTEGER ||
           !operand->literal.out_of_range)
            continue;
        if(other->kind == OPERAND_COLUMN)
            set_error(error, "Type error: %.*s%s is out of range for comparison with INT column '%s'.",
                      ERROR_PREVIEW_LENGTH, operand->literal.text,
                      operand->literal.length > ERROR_PREVIEW_LENGTH ? "..." : "", other->column_name);
        else
            set_error(error, "Type error: %.*s%s is out of range.",
                      ERROR_PREVIEW_LENGTH, operand->literal.text,
                      operand->literal.length > ERROR_PREVIEW_LENGTH ? "..." : "");
        return false;
    }

    bound->op = ast->op;
    return true;
}

/*
 * Binds a whole expression tree, node for node. A failure anywhere frees what
 * was bound so far and returns NULL. The recursion follows the parse tree,
 * whose nesting the parser already limits.
 */
BoundExpr* expression_bind(const ExprAst* ast, const Schema* schema, SqlError* error){
    BoundExpr* bound = calloc(1, sizeof(BoundExpr));
    bound->kind = ast->kind;

    if(ast->kind == EXPR_COMPARISON){
        if(!bind_comparison(ast, schema, bound, error)){
            expression_free(bound);
            return NULL;
        }
        return bound;
    }

    bound->children = malloc(ast->num_children * sizeof(BoundExpr*));
    for(uint32_t i = 0; i < ast->num_children; ++i){
        BoundExpr* child = expression_bind(ast->children[i], schema, error);
        if(child == NULL){
            expression_free(bound);
            return NULL;
        }
        bound->children[bound->num_children++] = child;
    }
    return bound;
}

/* An INT operand's value: the row's column, widened to 64 bits, or the constant. */
static int64_t int_value(const BoundOperand* operand, const Record* record, const Schema* schema){
    if(operand->is_column)
        return record_get_int(record, schema, operand->column_id);
    return operand->integer;
}

/*
 * A TEXT operand's bytes and length. A column's value is zero-padded to the
 * column's width, except when it fills the width exactly, so it ends at the
 * first zero byte or at the width, whichever comes first.
 */
static const char* text_value(const BoundOperand* operand, const Record* record, const Schema* schema,
                              uint32_t* length){
    if(!operand->is_column){
        *length = operand->length;
        return operand->text;
    }
    const char* text = record_get_text(record, schema, operand->column_id);
    uint32_t n = 0;
    while(n < operand->width && text[n] != '\0')
        ++n;
    *length = n;
    return text;
}

/*
 * Orders the two operands: negative, zero or positive as left is less than,
 * equal to or greater than right. TEXT compares the shared prefix byte by
 * byte, and if that's equal the shorter value comes first — so comparison is
 * case-sensitive and 'n10' sorts before 'n9'.
 */
static int compare_operands(const BoundOperand* left, const BoundOperand* right,
                            const Record* record, const Schema* schema){
    if(left->type == VALUE_INT){
        int64_t a = int_value(left, record, schema);
        int64_t b = int_value(right, record, schema);
        return (a > b) - (a < b);
    }

    uint32_t left_length;
    uint32_t right_length;
    const char* a = text_value(left, record, schema, &left_length);
    const char* b = text_value(right, record, schema, &right_length);
    uint32_t shared = left_length < right_length ? left_length : right_length;
    int order = memcmp(a, b, shared);
    if(order != 0)
        return order;
    return (left_length > right_length) - (left_length < right_length);
}

/*
 * Evaluates the expression against one row. AND stops at the first false
 * child and OR at the first true one, so a condition is only computed as far
 * as it needs to be.
 */
bool expression_evaluate(const BoundExpr* expr, const Record* record, const Schema* schema){
    switch(expr->kind){
        case EXPR_COMPARISON:{
            int order = compare_operands(&expr->left, &expr->right, record, schema);
            switch(expr->op){
                case COMPARE_EQUAL:         return order == 0;
                case COMPARE_NOT_EQUAL:     return order != 0;
                case COMPARE_LESS:          return order < 0;
                case COMPARE_LESS_EQUAL:    return order <= 0;
                case COMPARE_GREATER:       return order > 0;
                case COMPARE_GREATER_EQUAL: return order >= 0;
            }
            return false;
        }
        case EXPR_AND:
            for(uint32_t i = 0; i < expr->num_children; ++i){
                if(!expression_evaluate(expr->children[i], record, schema))
                    return false;
            }
            return true;
        case EXPR_OR:
            for(uint32_t i = 0; i < expr->num_children; ++i){
                if(expression_evaluate(expr->children[i], record, schema))
                    return true;
            }
            return false;
        case EXPR_NOT:
            return !expression_evaluate(expr->children[0], record, schema);
    }
    return false;
}

/* Frees the tree: constants' text, children, then the node. */
void expression_free(BoundExpr* expr){
    if(expr == NULL)
        return;
    free(expr->left.text);
    free(expr->right.text);
    for(uint32_t i = 0; i < expr->num_children; ++i)
        expression_free(expr->children[i]);
    free(expr->children);
    free(expr);
}

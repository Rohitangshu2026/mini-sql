#include "parser.h"
#include "tokenizer.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/*
 * Grammar, one function per rule:
 *
 *   statement  := [ EXPLAIN ] ( insert | select | create | delete ) [ ';' ] END
 *                 (EXPLAIN only before a select or a delete; a blank line is
 *                 also allowed)
 *   insert     := INSERT INTO name [ '(' name { ',' name } ')' ]
 *                 VALUES '(' literal { ',' literal } ')'
 *   select     := SELECT ( '*' | name { ',' name } ) FROM name [ WHERE expr ]
 *   create     := CREATE TABLE name '(' column_def { ',' column_def } ')'
 *   column_def := name ( INT | TEXT '(' INTEGER ')' ) [ PRIMARY KEY ]
 *   delete     := DELETE FROM name [ WHERE expr ]
 *   expr       := and_expr { OR and_expr }
 *   and_expr   := not_expr { AND not_expr }
 *   not_expr   := NOT not_expr | primary
 *   primary    := '(' expr ')' | operand compare_op operand
 *   operand    := name | literal
 *   compare_op := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>='
 *   literal    := [ '-' ] INTEGER | STRING
 *
 * The expression rules give NOT the tightest binding, then AND, then OR.
 */

/* Longest stretch of a token an error message quotes before cutting it off. */
#define ERROR_PREVIEW_LENGTH 32

/*
 * How deeply NOTs and parentheses may nest in an expression. Each level is a
 * recursive call here and later in binding and evaluation, so without a limit
 * a line of ten thousand NOTs would overflow the stack; 64 is far beyond any
 * condition a person writes.
 */
#define MAX_EXPRESSION_DEPTH 64

/*
 * Parser state: the token source, one token of lookahead, and where to report
 * an error. `failed` is set by the first error, after which every expect()
 * fails without overwriting the message, so the error reported is always the
 * first one rather than a cascade of consequences.
 */
typedef struct{
    Tokenizer tokenizer;
    Token current;
    SqlError* error;
    bool failed;
}Parser;

static void advance(Parser* parser){
    parser->current = tokenizer_next(&parser->tokenizer);
}

static bool check(const Parser* parser, TokenType type){
    return parser->current.type == type;
}

/* Consumes the current token if it has type `type`. */
static bool match(Parser* parser, TokenType type){
    if(!check(parser, type))
        return false;
    advance(parser);
    return true;
}

/*
 * Reports a malformed token. These messages describe the token itself rather
 * than what the grammar wanted, since "expected FROM near '7x'" would hide the
 * real problem.
 */
static void report_lexical_error(Parser* parser){
    const Token* token = &parser->current;
    int shown = token->length > ERROR_PREVIEW_LENGTH ? ERROR_PREVIEW_LENGTH : (int)token->length;
    const char* cut = token->length > ERROR_PREVIEW_LENGTH ? "..." : "";

    switch(token->error){
        case LEX_ERROR_UNTERMINATED_STRING:
            snprintf(parser->error->message, SQL_ERROR_MESSAGE_SIZE,
                     "Syntax error: unterminated string starting at column %u.", token->column);
            break;
        case LEX_ERROR_MALFORMED_NUMBER:
            snprintf(parser->error->message, SQL_ERROR_MESSAGE_SIZE,
                     "Syntax error: malformed number '%.*s%s' at column %u.",
                     shown, token->start, cut, token->column);
            break;
        case LEX_ERROR_UNRECOGNIZED_CHARACTER:
        case LEX_ERROR_NONE:
            snprintf(parser->error->message, SQL_ERROR_MESSAGE_SIZE,
                     "Syntax error: unrecognized character '%.*s' at column %u.",
                     shown, token->start, token->column);
            break;
    }
}

/*
 * Records that the grammar wanted `what` but found the current token. The
 * message quotes the token and its column, or says "at end of statement" when
 * the input ran out; a malformed token gets its own message instead. Only the
 * first call has any effect.
 */
static void fail_expected(Parser* parser, const char* what){
    if(parser->failed)
        return;
    parser->failed = true;

    const Token* token = &parser->current;
    if(token->type == TOKEN_ERROR){
        report_lexical_error(parser);
        return;
    }
    if(token->type == TOKEN_END){
        snprintf(parser->error->message, SQL_ERROR_MESSAGE_SIZE,
                 "Syntax error: expected %s at end of statement.", what);
        return;
    }

    int shown = token->length > ERROR_PREVIEW_LENGTH ? ERROR_PREVIEW_LENGTH : (int)token->length;
    snprintf(parser->error->message, SQL_ERROR_MESSAGE_SIZE,
             "Syntax error: expected %s near '%.*s%s' at column %u.",
             what, shown, token->start,
             token->length > ERROR_PREVIEW_LENGTH ? "..." : "", token->column);
}

/*
 * Consumes a token of type `type`, or fails describing what was wanted. The
 * description defaults to the token type's own name; callers pass their own
 * when there's a better one, such as "',' or ')'" at the end of a list item.
 */
static bool expect(Parser* parser, TokenType type, const char* what){
    if(match(parser, type))
        return true;
    fail_expected(parser, what != NULL ? what : token_type_name(type));
    return false;
}

/* A NUL-terminated heap copy of `length` bytes at `start`. */
static char* copy_text(const char* start, uint32_t length){
    char* text = malloc(length + 1);
    memcpy(text, start, length);
    text[length] = '\0';
    return text;
}

/*
 * The content of a string token with its quotes removed and each doubled quote
 * turned back into one. The tokenizer has already checked that every quote
 * inside is doubled, so each one seen here is followed by its twin.
 */
static char* unescape_string(const Token* token, uint32_t* length){
    const char* content = token->start + 1;
    uint32_t content_length = token->length - 2;
    char* text = malloc(content_length + 1);
    uint32_t written = 0;

    for(uint32_t i = 0; i < content_length; ++i){
        text[written++] = content[i];
        if(content[i] == '\'')
            ++i;
    }
    text[written] = '\0';
    *length = written;
    return text;
}

/*
 * Parses a table or column name into a heap copy. A keyword in a name's place
 * fails like any other wrong token: names can't be reserved words.
 */
static bool parse_name(Parser* parser, char** name, const char* what){
    if(!check(parser, TOKEN_IDENTIFIER)){
        fail_expected(parser, what);
        return false;
    }
    *name = copy_text(parser->current.start, parser->current.length);
    advance(parser);
    return true;
}

/*
 * Parses a literal: a string, or an integer with an optional minus sign. The
 * sign is a separate token, so "- 5" is accepted as well as "-5", and the
 * integer's text is rebuilt as "-5" either way. A sign in front of a string
 * is a syntax error, reported as a missing integer.
 */
static bool parse_literal(Parser* parser, Literal* literal){
    memset(literal, 0, sizeof(Literal));
    bool negative = match(parser, TOKEN_MINUS);

    if(check(parser, TOKEN_INTEGER)){
        const Token* token = &parser->current;
        literal->type = LITERAL_INTEGER;
        literal->integer = negative ? -token->integer : token->integer;
        literal->out_of_range = token->out_of_range;
        literal->length = token->length + (negative ? 1 : 0);
        literal->text = malloc(literal->length + 1);
        if(negative)
            literal->text[0] = '-';
        memcpy(literal->text + (negative ? 1 : 0), token->start, token->length);
        literal->text[literal->length] = '\0';
        advance(parser);
        return true;
    }
    if(!negative && check(parser, TOKEN_STRING)){
        literal->type = LITERAL_STRING;
        literal->text = unescape_string(&parser->current, &literal->length);
        advance(parser);
        return true;
    }

    fail_expected(parser, negative ? "an integer" : "a value");
    return false;
}

/*
 * insert := INSERT INTO name [ '(' name { ',' name } ')' ]
 *           VALUES '(' literal { ',' literal } ')'
 *
 * Lists grow by doubling. Counts are only bumped once an element has been
 * parsed, so after an error they cover exactly what needs freeing.
 */
static bool parse_insert(Parser* parser, InsertAst* insert){
    advance(parser);   /* INSERT */
    if(!expect(parser, TOKEN_INTO, NULL))
        return false;
    if(!parse_name(parser, &insert->table_name, "a table name"))
        return false;

    if(match(parser, TOKEN_LEFT_PAREN)){
        uint32_t capacity = 4;
        insert->column_names = malloc(capacity * sizeof(char*));
        do{
            if(insert->num_columns == capacity){
                capacity *= 2;
                insert->column_names = realloc(insert->column_names, capacity * sizeof(char*));
            }
            if(!parse_name(parser, &insert->column_names[insert->num_columns], "a column name"))
                return false;
            insert->num_columns++;
        }while(match(parser, TOKEN_COMMA));
        if(!expect(parser, TOKEN_RIGHT_PAREN, "',' or ')'"))
            return false;
    }

    if(!expect(parser, TOKEN_VALUES, NULL))
        return false;
    if(!expect(parser, TOKEN_LEFT_PAREN, NULL))
        return false;

    uint32_t capacity = 4;
    insert->values = malloc(capacity * sizeof(Literal));
    do{
        if(insert->num_values == capacity){
            capacity *= 2;
            insert->values = realloc(insert->values, capacity * sizeof(Literal));
        }
        if(!parse_literal(parser, &insert->values[insert->num_values]))
            return false;
        insert->num_values++;
    }while(match(parser, TOKEN_COMMA));

    return expect(parser, TOKEN_RIGHT_PAREN, "',' or ')'");
}

/* Reports an expression nested past MAX_EXPRESSION_DEPTH, at the token that did it. */
static void fail_nested(Parser* parser){
    if(parser->failed)
        return;
    parser->failed = true;
    snprintf(parser->error->message, SQL_ERROR_MESSAGE_SIZE,
             "Syntax error: expression nested too deeply at column %u.", parser->current.column);
}

/* A zeroed expression node of the given kind. */
static ExprAst* new_expr(ExprKind kind){
    ExprAst* expr = calloc(1, sizeof(ExprAst));
    expr->kind = kind;
    return expr;
}

/* Appends a child to an AND, OR or NOT node, growing its list by doubling. */
static void add_child(ExprAst* expr, ExprAst* child){
    if(expr->num_children == expr->capacity){
        expr->capacity = expr->capacity == 0 ? 2 : expr->capacity * 2;
        expr->children = realloc(expr->children, expr->capacity * sizeof(ExprAst*));
    }
    expr->children[expr->num_children++] = child;
}

/* Frees an operand's column name or literal text; the operand itself isn't. */
static void free_operand(OperandAst* operand){
    free(operand->column_name);
    free(operand->literal.text);
}

/*
 * Frees an expression tree. Recursion only follows NOT and parentheses, which
 * the parser limits to MAX_EXPRESSION_DEPTH, so the stack depth is bounded.
 */
static void free_expr(ExprAst* expr){
    if(expr == NULL)
        return;
    free_operand(&expr->left);
    free_operand(&expr->right);
    for(uint32_t i = 0; i < expr->num_children; ++i)
        free_expr(expr->children[i]);
    free(expr->children);
    free(expr);
}

/*
 * operand := name | literal
 *
 * A name is kept as text: whether it's a real column, and of what type, is
 * the binder's question.
 */
static bool parse_operand(Parser* parser, OperandAst* operand){
    if(check(parser, TOKEN_IDENTIFIER)){
        operand->kind = OPERAND_COLUMN;
        return parse_name(parser, &operand->column_name, "a column name");
    }
    if(check(parser, TOKEN_INTEGER) || check(parser, TOKEN_MINUS) || check(parser, TOKEN_STRING)){
        operand->kind = OPERAND_LITERAL;
        return parse_literal(parser, &operand->literal);
    }
    fail_expected(parser, "a column or a value");
    return false;
}

/* compare_op := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>=' */
static bool parse_compare_op(Parser* parser, CompareOp* op){
    switch(parser->current.type){
        case TOKEN_EQUAL:         *op = COMPARE_EQUAL; break;
        case TOKEN_NOT_EQUAL:     *op = COMPARE_NOT_EQUAL; break;
        case TOKEN_LESS:          *op = COMPARE_LESS; break;
        case TOKEN_LESS_EQUAL:    *op = COMPARE_LESS_EQUAL; break;
        case TOKEN_GREATER:       *op = COMPARE_GREATER; break;
        case TOKEN_GREATER_EQUAL: *op = COMPARE_GREATER_EQUAL; break;
        default:
            fail_expected(parser, "a comparison operator");
            return false;
    }
    advance(parser);
    return true;
}

static ExprAst* parse_or(Parser* parser, uint32_t depth);

/*
 * primary := '(' expr ')' | operand compare_op operand
 *
 * Opening a parenthesis is one level of nesting, counted against the limit.
 */
static ExprAst* parse_primary(Parser* parser, uint32_t depth){
    if(check(parser, TOKEN_LEFT_PAREN)){
        if(depth >= MAX_EXPRESSION_DEPTH){
            fail_nested(parser);
            return NULL;
        }
        advance(parser);
        ExprAst* inner = parse_or(parser, depth + 1);
        if(inner == NULL)
            return NULL;
        if(!expect(parser, TOKEN_RIGHT_PAREN, NULL)){
            free_expr(inner);
            return NULL;
        }
        return inner;
    }

    ExprAst* comparison = new_expr(EXPR_COMPARISON);
    if(!parse_operand(parser, &comparison->left) ||
       !parse_compare_op(parser, &comparison->op) ||
       !parse_operand(parser, &comparison->right)){
        free_expr(comparison);
        return NULL;
    }
    return comparison;
}

/*
 * not_expr := NOT not_expr | primary
 *
 * Each NOT is one level of nesting, counted against the limit.
 */
static ExprAst* parse_not(Parser* parser, uint32_t depth){
    if(!check(parser, TOKEN_NOT))
        return parse_primary(parser, depth);
    if(depth >= MAX_EXPRESSION_DEPTH){
        fail_nested(parser);
        return NULL;
    }
    advance(parser);

    ExprAst* operand = parse_not(parser, depth + 1);
    if(operand == NULL)
        return NULL;
    ExprAst* not_expr = new_expr(EXPR_NOT);
    add_child(not_expr, operand);
    return not_expr;
}

/*
 * and_expr := not_expr { AND not_expr }
 *
 * A single operand is returned as it is; two or more are gathered under one
 * AND node, however long the chain.
 */
static ExprAst* parse_and(Parser* parser, uint32_t depth){
    ExprAst* first = parse_not(parser, depth);
    if(first == NULL || !check(parser, TOKEN_AND))
        return first;

    ExprAst* and_expr = new_expr(EXPR_AND);
    add_child(and_expr, first);
    while(match(parser, TOKEN_AND)){
        ExprAst* next = parse_not(parser, depth);
        if(next == NULL){
            free_expr(and_expr);
            return NULL;
        }
        add_child(and_expr, next);
    }
    return and_expr;
}

/* expr := and_expr { OR and_expr }, gathered under one OR node like AND's. */
static ExprAst* parse_or(Parser* parser, uint32_t depth){
    ExprAst* first = parse_and(parser, depth);
    if(first == NULL || !check(parser, TOKEN_OR))
        return first;

    ExprAst* or_expr = new_expr(EXPR_OR);
    add_child(or_expr, first);
    while(match(parser, TOKEN_OR)){
        ExprAst* next = parse_and(parser, depth);
        if(next == NULL){
            free_expr(or_expr);
            return NULL;
        }
        add_child(or_expr, next);
    }
    return or_expr;
}

/*
 * select := SELECT ( '*' | name { ',' name } ) FROM name [ WHERE expr ]
 *
 * Without '*', the column list is kept as names, in order, repeats allowed.
 */
static bool parse_select(Parser* parser, SelectAst* select){
    advance(parser);   /* SELECT */
    if(!match(parser, TOKEN_STAR)){
        uint32_t capacity = 4;
        select->column_names = malloc(capacity * sizeof(char*));
        do{
            if(select->num_columns == capacity){
                capacity *= 2;
                select->column_names = realloc(select->column_names, capacity * sizeof(char*));
            }
            const char* what = select->num_columns == 0 ? "'*' or a column name" : "a column name";
            if(!parse_name(parser, &select->column_names[select->num_columns], what))
                return false;
            select->num_columns++;
        }while(match(parser, TOKEN_COMMA));
    }

    if(!expect(parser, TOKEN_FROM, NULL))
        return false;
    if(!parse_name(parser, &select->table_name, "a table name"))
        return false;

    if(match(parser, TOKEN_WHERE)){
        select->where = parse_or(parser, 0);
        if(select->where == NULL)
            return false;
    }
    return true;
}

/*
 * delete := DELETE FROM name [ WHERE expr ]
 *
 * The WHERE is the same expression a SELECT takes, so a DELETE chooses its
 * rows exactly the way a SELECT with the same condition would find them.
 */
static bool parse_delete(Parser* parser, DeleteAst* delete_rows){
    advance(parser);   /* DELETE */
    if(!expect(parser, TOKEN_FROM, NULL))
        return false;
    if(!parse_name(parser, &delete_rows->table_name, "a table name"))
        return false;

    if(match(parser, TOKEN_WHERE)){
        delete_rows->where = parse_or(parser, 0);
        if(delete_rows->where == NULL)
            return false;
    }
    return true;
}

/*
 * column_def := name ( INT | TEXT '(' INTEGER ')' ) [ PRIMARY KEY ]
 *
 * Only the shape is checked here. Whether the width is sensible, or the table
 * has exactly one key, is the table definition's business, which can say so
 * in terms of the table rather than of tokens. The column is zeroed first, so
 * after a failure its name is either owned or NULL and the caller can free it
 * either way.
 */
static bool parse_column_def(Parser* parser, ColumnAst* column){
    memset(column, 0, sizeof(ColumnAst));
    if(!parse_name(parser, &column->name, "a column name"))
        return false;

    if(match(parser, TOKEN_INT))
        column->type = AST_TYPE_INT;
    else if(match(parser, TOKEN_TEXT)){
        column->type = AST_TYPE_TEXT;
        if(!expect(parser, TOKEN_LEFT_PAREN, NULL))
            return false;
        if(!check(parser, TOKEN_INTEGER)){
            fail_expected(parser, "a width");
            return false;
        }
        column->width = parser->current.integer;
        column->width_out_of_range = parser->current.out_of_range;
        advance(parser);
        if(!expect(parser, TOKEN_RIGHT_PAREN, NULL))
            return false;
    }
    else{
        fail_expected(parser, "INT or TEXT");
        return false;
    }

    if(match(parser, TOKEN_PRIMARY)){
        if(!expect(parser, TOKEN_KEY, NULL))
            return false;
        column->primary_key = true;
    }
    return true;
}

/*
 * create := CREATE TABLE name '(' column_def { ',' column_def } ')'
 *
 * A column that fails partway may already own its name, so it's freed here
 * before returning; the count only covers columns parsed in full.
 */
static bool parse_create(Parser* parser, CreateTableAst* create){
    advance(parser);   /* CREATE */
    if(!expect(parser, TOKEN_TABLE, NULL))
        return false;
    if(!parse_name(parser, &create->table_name, "a table name"))
        return false;
    if(!expect(parser, TOKEN_LEFT_PAREN, NULL))
        return false;

    uint32_t capacity = 4;
    create->columns = malloc(capacity * sizeof(ColumnAst));
    do{
        if(create->num_columns == capacity){
            capacity *= 2;
            create->columns = realloc(create->columns, capacity * sizeof(ColumnAst));
        }
        if(!parse_column_def(parser, &create->columns[create->num_columns])){
            free(create->columns[create->num_columns].name);
            return false;
        }
        create->num_columns++;
    }while(match(parser, TOKEN_COMMA));

    return expect(parser, TOKEN_RIGHT_PAREN, "',' or ')'");
}

/*
 * statement := [ EXPLAIN ] ( insert | select | create | delete ) [ ';' ] END
 *
 * An optional EXPLAIN comes first, and only a SELECT or a DELETE may follow it. Then it
 * dispatches on the first token and requires the line to end, allowing one
 * optional ';' first. Anything after the statement — a second statement, or a
 * clause that isn't supported — is a syntax error rather than being silently
 * ignored. On failure the partly built tree is freed here, so callers only
 * ever free a tree from a successful parse.
 */
bool parse_statement(const char* sql, Ast* ast, SqlError* error){
    Parser parser;
    tokenizer_init(&parser.tokenizer, sql);
    parser.error = error;
    parser.failed = false;
    advance(&parser);

    memset(ast, 0, sizeof(Ast));
    ast->kind = AST_EMPTY;

    if(match(&parser, TOKEN_EXPLAIN)){
        ast->explain = true;
        if(!check(&parser, TOKEN_SELECT) && !check(&parser, TOKEN_DELETE))
            fail_expected(&parser, "SELECT or DELETE");
    }

    if(!parser.failed){
        if(check(&parser, TOKEN_INSERT)){
            ast->kind = AST_INSERT;
            parse_insert(&parser, &ast->insert);
        }
        else if(check(&parser, TOKEN_SELECT)){
            ast->kind = AST_SELECT;
            parse_select(&parser, &ast->select);
        }
        else if(check(&parser, TOKEN_CREATE)){
            ast->kind = AST_CREATE_TABLE;
            parse_create(&parser, &ast->create_table);
        }
        else if(check(&parser, TOKEN_DELETE)){
            ast->kind = AST_DELETE;
            parse_delete(&parser, &ast->delete_rows);
        }
        else if(!check(&parser, TOKEN_SEMICOLON) && !check(&parser, TOKEN_END))
            fail_expected(&parser, "INSERT, SELECT, CREATE or DELETE");
    }

    if(!parser.failed){
        match(&parser, TOKEN_SEMICOLON);
        if(!check(&parser, TOKEN_END))
            fail_expected(&parser, "end of statement");
    }

    if(parser.failed){
        ast_free(ast);
        return false;
    }
    return true;
}

/*
 * Frees whatever a tree owns. Safe on a partly built tree: parse_statement
 * zeroes the tree before filling it, and counts only cover elements that were
 * fully parsed, so every pointer here is either owned or NULL.
 */
void ast_free(Ast* ast){
    switch(ast->kind){
        case AST_INSERT:
            free(ast->insert.table_name);
            for(uint32_t i = 0; i < ast->insert.num_columns; ++i)
                free(ast->insert.column_names[i]);
            free(ast->insert.column_names);
            for(uint32_t i = 0; i < ast->insert.num_values; ++i)
                free(ast->insert.values[i].text);
            free(ast->insert.values);
            break;
        case AST_SELECT:
            free(ast->select.table_name);
            for(uint32_t i = 0; i < ast->select.num_columns; ++i)
                free(ast->select.column_names[i]);
            free(ast->select.column_names);
            free_expr(ast->select.where);
            break;
        case AST_CREATE_TABLE:
            free(ast->create_table.table_name);
            for(uint32_t i = 0; i < ast->create_table.num_columns; ++i)
                free(ast->create_table.columns[i].name);
            free(ast->create_table.columns);
            break;
        case AST_DELETE:
            free(ast->delete_rows.table_name);
            free_expr(ast->delete_rows.where);
            break;
        case AST_EMPTY:
            break;
    }
    memset(ast, 0, sizeof(Ast));
    ast->kind = AST_EMPTY;
}

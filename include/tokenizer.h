#ifndef TOKENIZER_H
#define TOKENIZER_H

#include<stdbool.h>
#include<stdint.h>

/*
 * Every kind of token in the SQL dialect. Each keyword has a type of its own,
 * so the parser asks for FROM directly instead of comparing text. The keyword
 * set covers every statement the engine is planned to support, including ones
 * the parser doesn't accept yet, so the lexical rules — and the list of names
 * a table or column can't use — are settled once rather than growing with
 * each feature.
 */
typedef enum{
    /* keywords */
    TOKEN_SELECT,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_CREATE,
    TOKEN_TABLE,
    TOKEN_DELETE,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_PRIMARY,
    TOKEN_KEY,
    TOKEN_INT,
    TOKEN_TEXT,
    TOKEN_EXPLAIN,

    /* names and literals */
    TOKEN_IDENTIFIER,
    TOKEN_INTEGER,
    TOKEN_STRING,

    /* punctuation */
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_COMMA,
    TOKEN_SEMICOLON,
    TOKEN_STAR,
    TOKEN_MINUS,

    /* comparison operators */
    TOKEN_EQUAL,
    TOKEN_NOT_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,

    /* the end of the input, and input that isn't a valid token */
    TOKEN_END,
    TOKEN_ERROR
}TokenType;

/* What made a TOKEN_ERROR token invalid. */
typedef enum{
    LEX_ERROR_NONE,
    LEX_ERROR_UNTERMINATED_STRING,    /* a quote with no closing quote */
    LEX_ERROR_MALFORMED_NUMBER,       /* digits running straight into letters, like 7x */
    LEX_ERROR_UNRECOGNIZED_CHARACTER  /* a character no token starts with */
}LexicalError;

/*
 * One token. It points into the input rather than copying it, so it's only
 * valid while the input is. A string token's text includes its quotes and any
 * doubled quotes inside, exactly as typed; the parser unescapes it.
 */
typedef struct{
    TokenType type;
    const char* start;      /* first byte of the token's text, within the input */
    uint32_t length;        /* bytes of text */
    uint32_t column;        /* 1-based position of the first byte, for error messages */
    int64_t integer;        /* TOKEN_INTEGER: the value, unless out_of_range */
    bool out_of_range;      /* TOKEN_INTEGER: more digits than an int64_t holds */
    LexicalError error;     /* TOKEN_ERROR: what's wrong with it */
}Token;

/* Reads tokens one at a time from a NUL-terminated line of SQL. */
typedef struct{
    const char* sql;        /* the input; borrowed, never modified */
    uint32_t position;      /* byte offset of the next unread character */
}Tokenizer;

/* Starts tokenizing `sql` from its first character. */
void tokenizer_init(Tokenizer* tokenizer, const char* sql);

/*
 * Returns the next token, skipping whitespace and -- comments. At the end of
 * the input it returns TOKEN_END, and keeps returning it if called again.
 * Malformed input comes back as a TOKEN_ERROR token rather than being skipped.
 */
Token tokenizer_next(Tokenizer* tokenizer);

/*
 * How a token type reads in an error message: "FROM" for a keyword, "')'" for
 * punctuation, "a name" for an identifier, and so on.
 */
const char* token_type_name(TokenType type);

#endif

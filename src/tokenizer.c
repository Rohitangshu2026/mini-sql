#include "tokenizer.h"

#include<stddef.h>

/* A reserved word, spelled in lower case, and the token type it produces. */
typedef struct{
    const char* text;
    TokenType type;
}Keyword;

static const Keyword KEYWORDS[] = {
    {"select", TOKEN_SELECT},
    {"insert", TOKEN_INSERT},
    {"into", TOKEN_INTO},
    {"values", TOKEN_VALUES},
    {"from", TOKEN_FROM},
    {"where", TOKEN_WHERE},
    {"create", TOKEN_CREATE},
    {"table", TOKEN_TABLE},
    {"delete", TOKEN_DELETE},
    {"and", TOKEN_AND},
    {"or", TOKEN_OR},
    {"not", TOKEN_NOT},
    {"primary", TOKEN_PRIMARY},
    {"key", TOKEN_KEY},
    {"int", TOKEN_INT},
    {"text", TOKEN_TEXT},
    {"explain", TOKEN_EXPLAIN},
};

#define NUM_KEYWORDS (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static bool is_space(char c){
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_digit(char c){
    return c >= '0' && c <= '9';
}

static bool is_name_start(char c){
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_name_char(char c){
    return is_name_start(c) || is_digit(c);
}

/*
 * Lower-cases an ASCII letter and leaves every other byte alone. Written out
 * rather than using tolower() so the result can't depend on the locale.
 */
static char ascii_lower(char c){
    if(c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

/*
 * Decides whether the name-shaped run of `length` bytes at `start` is a
 * keyword, ignoring case, or an ordinary identifier. A linear scan of the
 * keyword table is plenty for seventeen entries and one short run.
 */
static TokenType keyword_or_identifier(const char* start, uint32_t length){
    for(size_t i = 0; i < NUM_KEYWORDS; ++i){
        const char* keyword = KEYWORDS[i].text;
        uint32_t matched = 0;
        while(matched < length && keyword[matched] != '\0' &&
              ascii_lower(start[matched]) == keyword[matched])
            ++matched;
        if(matched == length && keyword[matched] == '\0')
            return KEYWORDS[i].type;
    }
    return TOKEN_IDENTIFIER;
}

/* Starts tokenizing `sql` from its first character. */
void tokenizer_init(Tokenizer* tokenizer, const char* sql){
    tokenizer->sql = sql;
    tokenizer->position = 0;
}

/*
 * Reads a run of digits as an integer. The value is accumulated in an int64_t
 * with an overflow check before each step, so an absurdly long number is
 * flagged out of range instead of wrapping (signed overflow would be undefined
 * behavior). Digits running straight into letters or an underscore, like 7x,
 * make the whole run a malformed number rather than an integer followed by a
 * name, which would otherwise surface as a confusing error one token later.
 */
static void read_number(const char* sql, uint32_t* position, Token* token){
    uint32_t pos = *position;
    int64_t value = 0;
    bool out_of_range = false;

    while(is_digit(sql[pos])){
        int64_t digit = sql[pos] - '0';
        if(!out_of_range){
            if(value > (INT64_MAX - digit) / 10)
                out_of_range = true;
            else
                value = value * 10 + digit;
        }
        ++pos;
    }

    if(is_name_char(sql[pos])){
        while(is_name_char(sql[pos]))
            ++pos;
        token->type = TOKEN_ERROR;
        token->error = LEX_ERROR_MALFORMED_NUMBER;
    }
    else{
        token->type = TOKEN_INTEGER;
        token->integer = value;
        token->out_of_range = out_of_range;
    }
    *position = pos;
}

/*
 * Reads a single-quoted string starting at its opening quote. Two quotes in a
 * row stand for one quote inside the string, the SQL convention; any other
 * quote ends it. Reaching the end of the line first makes it an unterminated
 * string, whose token then spans from the opening quote to the end.
 */
static void read_string(const char* sql, uint32_t* position, Token* token){
    uint32_t pos = *position + 1;   /* past the opening quote */

    for(;;){
        if(sql[pos] == '\0'){
            token->type = TOKEN_ERROR;
            token->error = LEX_ERROR_UNTERMINATED_STRING;
            break;
        }
        if(sql[pos] == '\''){
            if(sql[pos + 1] == '\''){
                pos += 2;
                continue;
            }
            ++pos;
            token->type = TOKEN_STRING;
            break;
        }
        ++pos;
    }
    *position = pos;
}

/*
 * Reads one- and two-character symbols. Each comparison operator is looked at
 * with one character of lookahead, so <= isn't read as < followed by =. A byte
 * no token starts with becomes an unrecognized character; if it opens a UTF-8
 * sequence, the sequence's continuation bytes are taken with it so the error
 * message shows the whole character rather than a fragment.
 */
static void read_symbol(const char* sql, uint32_t* position, Token* token){
    uint32_t pos = *position;
    char c = sql[pos++];

    switch(c){
        case '(': token->type = TOKEN_LEFT_PAREN; break;
        case ')': token->type = TOKEN_RIGHT_PAREN; break;
        case ',': token->type = TOKEN_COMMA; break;
        case ';': token->type = TOKEN_SEMICOLON; break;
        case '*': token->type = TOKEN_STAR; break;
        case '-': token->type = TOKEN_MINUS; break;
        case '=': token->type = TOKEN_EQUAL; break;
        case '!':
            if(sql[pos] == '='){
                ++pos;
                token->type = TOKEN_NOT_EQUAL;
            }
            else{
                token->type = TOKEN_ERROR;
                token->error = LEX_ERROR_UNRECOGNIZED_CHARACTER;
            }
            break;
        case '<':
            if(sql[pos] == '='){
                ++pos;
                token->type = TOKEN_LESS_EQUAL;
            }
            else if(sql[pos] == '>'){
                ++pos;
                token->type = TOKEN_NOT_EQUAL;
            }
            else
                token->type = TOKEN_LESS;
            break;
        case '>':
            if(sql[pos] == '='){
                ++pos;
                token->type = TOKEN_GREATER_EQUAL;
            }
            else
                token->type = TOKEN_GREATER;
            break;
        default:
            if((unsigned char)c >= 0x80){
                while(((unsigned char)sql[pos] & 0xC0) == 0x80)
                    ++pos;
            }
            token->type = TOKEN_ERROR;
            token->error = LEX_ERROR_UNRECOGNIZED_CHARACTER;
            break;
    }
    *position = pos;
}

/*
 * Skips whitespace and -- comments, then reads one token and records where it
 * starts, how long it is and which column it begins at. The kind of token is
 * decided by its first character: a letter or underscore starts a name, a
 * digit a number, a quote a string, and anything else a symbol.
 */
Token tokenizer_next(Tokenizer* tokenizer){
    const char* sql = tokenizer->sql;
    uint32_t pos = tokenizer->position;

    for(;;){
        if(is_space(sql[pos]))
            ++pos;
        else if(sql[pos] == '-' && sql[pos + 1] == '-'){
            while(sql[pos] != '\0' && sql[pos] != '\n')
                ++pos;
        }
        else
            break;
    }

    Token token = {0};
    uint32_t start = pos;
    token.start = sql + start;
    token.column = start + 1;

    if(sql[pos] == '\0')
        token.type = TOKEN_END;
    else if(is_name_start(sql[pos])){
        while(is_name_char(sql[pos]))
            ++pos;
        token.type = keyword_or_identifier(sql + start, pos - start);
    }
    else if(is_digit(sql[pos]))
        read_number(sql, &pos, &token);
    else if(sql[pos] == '\'')
        read_string(sql, &pos, &token);
    else
        read_symbol(sql, &pos, &token);

    token.length = pos - start;
    tokenizer->position = pos;
    return token;
}

/* How a token type reads in an "expected ..." error message. */
const char* token_type_name(TokenType type){
    switch(type){
        case TOKEN_SELECT: return "SELECT";
        case TOKEN_INSERT: return "INSERT";
        case TOKEN_INTO: return "INTO";
        case TOKEN_VALUES: return "VALUES";
        case TOKEN_FROM: return "FROM";
        case TOKEN_WHERE: return "WHERE";
        case TOKEN_CREATE: return "CREATE";
        case TOKEN_TABLE: return "TABLE";
        case TOKEN_DELETE: return "DELETE";
        case TOKEN_AND: return "AND";
        case TOKEN_OR: return "OR";
        case TOKEN_NOT: return "NOT";
        case TOKEN_PRIMARY: return "PRIMARY";
        case TOKEN_KEY: return "KEY";
        case TOKEN_INT: return "INT";
        case TOKEN_TEXT: return "TEXT";
        case TOKEN_EXPLAIN: return "EXPLAIN";
        case TOKEN_IDENTIFIER: return "a name";
        case TOKEN_INTEGER: return "an integer";
        case TOKEN_STRING: return "a string";
        case TOKEN_LEFT_PAREN: return "'('";
        case TOKEN_RIGHT_PAREN: return "')'";
        case TOKEN_COMMA: return "','";
        case TOKEN_SEMICOLON: return "';'";
        case TOKEN_STAR: return "'*'";
        case TOKEN_MINUS: return "'-'";
        case TOKEN_EQUAL: return "'='";
        case TOKEN_NOT_EQUAL: return "'!='";
        case TOKEN_LESS: return "'<'";
        case TOKEN_LESS_EQUAL: return "'<='";
        case TOKEN_GREATER: return "'>'";
        case TOKEN_GREATER_EQUAL: return "'>='";
        case TOKEN_END: return "end of statement";
        case TOKEN_ERROR: return "a valid token";
    }
    return "a token";
}

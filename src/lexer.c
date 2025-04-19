#include "lexer.h"
#include "utils.h"
#include "globals.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static Keyword keywords[] = {
    {"and", TOKEN_AND}, {"array", TOKEN_ARRAY}, {"begin", TOKEN_BEGIN},
    {"case", TOKEN_CASE}, {"const", TOKEN_CONST}, {"do", TOKEN_DO},
    {"div", TOKEN_INT_DIV}, {"downto", TOKEN_DOWNTO}, {"else", TOKEN_ELSE},
    {"end", TOKEN_END}, {"false", TOKEN_FALSE}, {"for", TOKEN_FOR},
    {"function", TOKEN_FUNCTION}, {"if", TOKEN_IF}, {"implementation", TOKEN_IMPLEMENTATION},
    {"initialization", TOKEN_INITIALIZATION},
    {"interface", TOKEN_INTERFACE}, {"mod", TOKEN_MOD}, {"not", TOKEN_NOT},
    {"of", TOKEN_OF}, {"or", TOKEN_OR}, {"program", TOKEN_PROGRAM},
    {"procedure", TOKEN_PROCEDURE}, {"read", TOKEN_READ}, {"readln", TOKEN_READLN},
    {"record", TOKEN_RECORD}, {"repeat", TOKEN_REPEAT}, {"then", TOKEN_THEN},
    {"to", TOKEN_TO}, {"true", TOKEN_TRUE}, {"type", TOKEN_TYPE},
    {"unit", TOKEN_UNIT}, {"until", TOKEN_UNTIL}, {"uses", TOKEN_USES},
    {"var", TOKEN_VAR}, {"while", TOKEN_WHILE}, {"write", TOKEN_WRITE},
    {"writeln", TOKEN_WRITELN}, {"enum", TOKEN_ENUM}, {"in", TOKEN_IN},
    {"break", TOKEN_BREAK}, {"out", TOKEN_OUT}
};

#define NUM_KEYWORDS (sizeof(keywords)/sizeof(Keyword))

void initLexer(Lexer *lexer, const char *text) {
    if (strncmp(text, "\xEF\xBB\xBF", 3) == 0) {
        text += 3;
    }
    
    lexer->text = text;
    lexer->pos = 0;
    lexer->current_char = text[0];
    lexer->line = 1;
    lexer->column = 1;
}

void advance(Lexer *lexer) {
    if (lexer->current_char == '\n') {
        lexer->line++;
        lexer->column = 0;
    }
    lexer->pos++;
    lexer->column++;
    if (lexer->pos < strlen(lexer->text)) {
        lexer->current_char = lexer->text[lexer->pos];
    } else {
        lexer->current_char = '\0';
    }
}

void skipWhitespace(Lexer *lexer) {
    while (lexer->current_char != '\0' && isspace(lexer->current_char)) {
        advance(lexer);
    }
}

static int isHexDigit(char c) {
    return isdigit(c) ||
           (tolower(c) >= 'a' && tolower(c) <= 'f');
}

Token *number(Lexer *lexer) {
    size_t start = lexer->pos;
    bool is_hex = false;
    bool has_decimal = false;

    // Move these to the top so they're visible to make_number:
    size_t len = 0;
    char *num_str = NULL;
    Token *token = NULL;

    if (lexer->current_char == '#') {
        advance(lexer);
        start++;
        is_hex = true;

        while (isHexDigit(lexer->current_char)) {
            advance(lexer);
        }
    } else {
        bool valid_number = false;

        if (isdigit((unsigned char)lexer->current_char)) {
            valid_number = true;
            do {
                advance(lexer);
            } while (isdigit((unsigned char)lexer->current_char));

            // Peek ahead for '..'
            if (lexer->current_char == '.' && lexer->text[lexer->pos + 1] == '.') {
                // Stop here to let main token scanner see the ..
                goto make_number;
            }

            // Otherwise, check for decimal fraction
            if (lexer->current_char == '.') {
                has_decimal = true;
                advance(lexer);
                while (isdigit((unsigned char)lexer->current_char)) {
                    advance(lexer);
                }
            }
        }

        if (!valid_number) {
            return NULL;
        }
    }

make_number:
    len = lexer->pos - start;
    num_str = malloc(len + 1);
    if (!num_str) {
        fprintf(stderr, "Memory allocation error in number().\n");
        EXIT_FAILURE_HANDLER();
    }
    strncpy(num_str, lexer->text + start, len);
    num_str[len] = '\0';

    if (is_hex) {
        token = newToken(TOKEN_HEX_CONST, num_str);
    } else if (has_decimal) {
        token = newToken(TOKEN_REAL_CONST, num_str);
    } else {
        token = newToken(TOKEN_INTEGER_CONST, num_str);
    }

    free(num_str);
    return token;
}

Token *identifier(Lexer *lexer) {
    size_t start = lexer->pos;
    while (lexer->current_char && (isalnum((unsigned char)lexer->current_char) || lexer->current_char == '_'))
        advance(lexer);
    size_t len = lexer->pos - start;
    char *id_str = malloc(len + 1);
    strncpy(id_str, lexer->text + start, len);
    id_str[len] = '\0';
    for (size_t i = 0; i < len; i++)
        id_str[i] = tolower((unsigned char)id_str[i]);
    Token *token = malloc(sizeof(Token));
    token->type = TOKEN_IDENTIFIER;
    for (int i = 0; i < NUM_KEYWORDS; i++) {
        if (strcmp(id_str, keywords[i].keyword) == 0) {
            token->type = keywords[i].token_type;
            break;
        }
    }
    token->value = id_str;
#ifdef DEBUG
    // Debug: Print the token type
    if (token->type == TOKEN_USES) {
        printf("Lexer: Tokenized 'uses' as TOKEN_USES\n");
    } else if (token->type == TOKEN_UNIT) {
        printf("Lexer: Tokenized 'uses' as TOKEN_UNIT\n");
    }
#endif
    return token;
}

Token *stringLiteral(Lexer *lexer) {
    advance(lexer);  // Skip opening '
    char *buffer = malloc(DEFAULT_STRING_CAPACITY);
    size_t buffer_pos = 0;
    size_t buffer_capacity = DEFAULT_STRING_CAPACITY;

    while (1) {
        if (lexer->current_char == '\'') {
            advance(lexer);
            if (lexer->current_char == '\'') {
                // Escaped apostrophe: add one '
                if (buffer_pos + 1 >= buffer_capacity) {
                    buffer_capacity *= 2;
                    buffer = realloc(buffer, buffer_capacity);
                    if (!buffer) {
                        fprintf(stderr, "Memory error in string literal\n");
                        EXIT_FAILURE_HANDLER();
                    }
                }
                buffer[buffer_pos++] = '\'';
                advance(lexer);
            } else {
                // End of string
                break;
            }
        } else if (lexer->current_char == '\0') {
            fprintf(stderr, "Unterminated string literal\n");
            EXIT_FAILURE_HANDLER();
        } else {
            if (buffer_pos + 1 >= buffer_capacity) {
                buffer_capacity *= 2;
                buffer = realloc(buffer, buffer_capacity);
                if (!buffer) {
                    fprintf(stderr, "Memory error in string literal\n");
                    EXIT_FAILURE_HANDLER();
                }
            }
            buffer[buffer_pos++] = lexer->current_char;
            advance(lexer);
        }
    }

    buffer[buffer_pos] = '\0';
    Token *token = newToken(TOKEN_STRING_CONST, buffer);
    free(buffer);
    return token;
}

Token *getNextToken(Lexer *lexer) {
    while (lexer->current_char) {
        // Skip whitespace
        if (isspace((unsigned char)lexer->current_char)) {
            skipWhitespace(lexer);
            continue;
        }
        
        if (lexer->current_char == '#') {
            // Potential hexadecimal constant - call number()
            return number(lexer);
        }

        // Handle single-line comments (// ...)
        if (lexer->current_char == '/' && lexer->text[lexer->pos + 1] == '/') {
            while (lexer->current_char && lexer->current_char != '\n')
                advance(lexer);
            continue;
        }

        // Handle brace-delimited comments { ... }
        if (lexer->current_char == '{') {
            advance(lexer);  // Skip '{'
            while (lexer->current_char && lexer->current_char != '}')
                advance(lexer);
            if (lexer->current_char == '}')
                advance(lexer);  // Skip '}'
            continue;
        }

        // Handle left bracket token '['
        if (lexer->current_char == '[') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_LBRACKET;
            token->value = strdup("[");
            return token;
        }

        // Handle right bracket token ']'
        if (lexer->current_char == ']') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_RBRACKET;
            token->value = strdup("]");
            return token;
        }

        // Identifiers (and keywords)
        if (isalpha((unsigned char)lexer->current_char) || lexer->current_char == '_')
            return identifier(lexer);

        // Numbers (integer or real)
        if (isdigit((unsigned char)lexer->current_char))
            return number(lexer);

        // String literals
        if (lexer->current_char == '\'')
            return stringLiteral(lexer);

        // Handle colon and assign operator ":="
        if (lexer->current_char == ':') {
            advance(lexer);
            if (lexer->current_char == '=') {
                advance(lexer);
                Token *token = malloc(sizeof(Token));
                token->type = TOKEN_ASSIGN;
                token->value = strdup(":=");
                return token;
            } else {
                Token *token = malloc(sizeof(Token));
                token->type = TOKEN_COLON;
                token->value = strdup(":");
                return token;
            }
        }

        // Semicolon
        if (lexer->current_char == ';') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_SEMICOLON;
            token->value = strdup(";");
            return token;
        }

        // Comma
        if (lexer->current_char == ',') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_COMMA;
            token->value = strdup(",");
            return token;
        }

        // Equal sign
        if (lexer->current_char == '=') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_EQUAL;
            token->value = strdup("=");
            return token;
        }

        // Greater than and greater or equal
        if (lexer->current_char == '>') {
            advance(lexer);
            if (lexer->current_char == '=') {
                advance(lexer);
                Token *token = malloc(sizeof(Token));
                token->type = TOKEN_GREATER_EQUAL;
                token->value = strdup(">=");
                return token;
            } else {
                Token *token = malloc(sizeof(Token));
                token->type = TOKEN_GREATER;
                token->value = strdup(">");
                return token;
            }
        }

        // Less than, less or equal, or not equal
        if (lexer->current_char == '<') {
            advance(lexer);
            if (lexer->current_char == '=') {
                advance(lexer);
                Token *token = malloc(sizeof(Token));
                token->type = TOKEN_LESS_EQUAL;
                token->value = strdup("<=");
                return token;
            } else if (lexer->current_char == '>') {
                advance(lexer);
                Token *token = malloc(sizeof(Token));
                token->type = TOKEN_NOT_EQUAL;
                token->value = strdup("<>");
                return token;
            } else {
                Token *token = malloc(sizeof(Token));
                token->type = TOKEN_LESS;
                token->value = strdup("<");
                return token;
            }
        }

        // Handle dot or dot-dot (..)
        if (lexer->current_char == '.') {
            advance(lexer);
            if (lexer->current_char == '.') {
                advance(lexer);
                return newToken(TOKEN_DOTDOT, "..");
            }
            return newToken(TOKEN_PERIOD, ".");
        }

        // Plus
        if (lexer->current_char == '+') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_PLUS;
            token->value = strdup("+");
            return token;
        }

        // Minus
        if (lexer->current_char == '-') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_MINUS;
            token->value = strdup("-");
            return token;
        }

        // Multiplication
        if (lexer->current_char == '*') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_MUL;
            token->value = strdup("*");
            return token;
        }

        // Division
        if (lexer->current_char == '/') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_SLASH;
            token->value = strdup("/");
            return token;
        }

        // Left parenthesis
        if (lexer->current_char == '(') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_LPAREN;
            token->value = strdup("(");
            return token;
        }

        // Right parenthesis
        if (lexer->current_char == ')') {
            advance(lexer);
            Token *token = malloc(sizeof(Token));
            token->type = TOKEN_RPAREN;
            token->value = strdup(")");
            return token;
        }
        
        if (isdigit((unsigned char)lexer->current_char)) {
            return number(lexer);
        } else if (isalpha((unsigned char)lexer->current_char) || lexer->current_char == '_') {
            return identifier(lexer);
        } else if (lexer->current_char == '\'') {
            return stringLiteral(lexer);
        }

        // If we reach here, the character is unrecognized.
        fprintf(stderr, "Lexer error at line %d, column %d: unrecognized character '%c'\n",
                lexer->line, lexer->column, lexer->current_char);
        advance(lexer);
    }
    // If no characters left, return EOF token.
    Token *token = malloc(sizeof(Token));
    token->type = TOKEN_EOF;
    token->value = strdup("EOF");
    return token;
}

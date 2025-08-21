// src/Pascal/lexer.c

#include "Pascal/lexer.h"
#include "core/utils.h"
#include "globals.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static Keyword keywords[] = {
    {"and", TOKEN_AND}, {"array", TOKEN_ARRAY}, {"begin", TOKEN_BEGIN},
    {"break", TOKEN_BREAK}, // Added break here alphabetically
    {"case", TOKEN_CASE}, {"const", TOKEN_CONST}, {"do", TOKEN_DO},
    {"div", TOKEN_INT_DIV}, {"downto", TOKEN_DOWNTO}, {"else", TOKEN_ELSE},
    {"end", TOKEN_END}, {"enum", TOKEN_ENUM}, // Added enum
    {"false", TOKEN_FALSE}, {"for", TOKEN_FOR},
    {"function", TOKEN_FUNCTION}, {"if", TOKEN_IF}, {"implementation", TOKEN_IMPLEMENTATION},
    {"in", TOKEN_IN}, // Added IN
    {"inline", TOKEN_INLINE}, // Added inline directive keyword
    {"initialization", TOKEN_INITIALIZATION},
    {"interface", TOKEN_INTERFACE}, {"mod", TOKEN_MOD}, {"nil", TOKEN_NIL},
    {"not", TOKEN_NOT}, {"of", TOKEN_OF}, {"or", TOKEN_OR},
    {"out", TOKEN_OUT}, // Added OUT
    {"procedure", TOKEN_PROCEDURE}, {"program", TOKEN_PROGRAM},
    {"read", TOKEN_READ}, {"readln", TOKEN_READLN},
    {"record", TOKEN_RECORD}, {"repeat", TOKEN_REPEAT},
    {"set", TOKEN_SET}, // <--- ADD THIS LINE (alphabetical position)
    {"shl", TOKEN_SHL}, // Added SHL
    {"shr", TOKEN_SHR}, // Added SHR
    {"then", TOKEN_THEN},
    {"to", TOKEN_TO}, {"true", TOKEN_TRUE}, {"type", TOKEN_TYPE},
    {"unit", TOKEN_UNIT}, {"until", TOKEN_UNTIL}, {"uses", TOKEN_USES},
    {"var", TOKEN_VAR}, {"while", TOKEN_WHILE}, {"write", TOKEN_WRITE},
    {"writeln", TOKEN_WRITELN}
};

#define NUM_KEYWORDS (sizeof(keywords)/sizeof(Keyword))

void initLexer(Lexer *lexer, const char *text) {
    lexer->text = text;
    lexer->text_len = text ? strlen(text) : 0; // Calculate length once at the start
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;

    // Check for UTF-8 BOM
    if (lexer->text_len >= 3 && strncmp(lexer->text, "\xEF\xBB\xBF", 3) == 0) {
        lexer->pos = 3; // Start position after BOM
    }

    // Check for Shebang line '#!'
    if (lexer->text_len > lexer->pos + 1 && // Ensure text is long enough from current pos
        lexer->text[lexer->pos] == '#' && lexer->text[lexer->pos + 1] == '!') {
        lexer->pos += 2; // Skip '#' and '!'
        while (lexer->pos < lexer->text_len && lexer->text[lexer->pos] != '\n') {
            lexer->pos++;
        }
        if (lexer->pos < lexer->text_len && lexer->text[lexer->pos] == '\n') {
            lexer->pos++;
            lexer->line = 2;
            lexer->column = 1;
        } else {
            // Reached EOF or line without newline
            // lexer->line remains 1 (or adjust if needed)
            // lexer->column needs to be set based on final lexer->pos
            // This case might need more robust column tracking if shebang is the whole file
        }
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG LEXER] Shebang line detected and skipped. Starting parse at line %d, col %d, pos %zu.\n", lexer->line, lexer->column, lexer->pos);
        #endif
    } else {
        // No shebang, reset pos to after BOM if only BOM was present
        lexer->pos = 0;
        if (lexer->text_len >= 3 && strncmp(lexer->text, "\xEF\xBB\xBF", 3) == 0) {
            lexer->pos = 3;
        }
        lexer->line = 1; // Ensure line/col start correctly if no shebang/BOM
        lexer->column = 1;
    }

    // Set current_char based on final starting position
    lexer->current_char = (lexer->pos < lexer->text_len) ? lexer->text[lexer->pos] : '\0';
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
    bool has_exponent = false;
    
    int token_line = lexer->line; // Capture line/col BEFORE advancing further if number() advances
    int token_column = lexer->column - (lexer->pos - start); // Approximate start column of number


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

            // Exponent part (e.g., 1e-10 or 3.5E+5)
            if (lexer->current_char == 'e' || lexer->current_char == 'E') {
                has_exponent = true;
                advance(lexer);

                // Optional sign after 'e'
                if (lexer->current_char == '+' || lexer->current_char == '-') {
                    advance(lexer);
                }

                // Require at least one digit in exponent
                if (!isdigit((unsigned char)lexer->current_char)) {
                    goto make_number; // Invalid exponent, stop here
                }

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
        token = newToken(TOKEN_HEX_CONST, num_str, token_line, token_column);
    } else if (has_decimal || has_exponent) {
        token = newToken(TOKEN_REAL_CONST, num_str, token_line, token_column);
    } else {
        token = newToken(TOKEN_INTEGER_CONST, num_str, token_line, token_column);
    }

    free(num_str);
    return token;
}

void advance(Lexer *lexer) {
    if (lexer->current_char == '\n') {
        lexer->line++;
        lexer->column = 0;
    }
    lexer->pos++;
    lexer->column++;
    // Use stored length
    if (lexer->pos < lexer->text_len) {
        lexer->current_char = lexer->text[lexer->pos];
    } else {
        lexer->current_char = '\0';
    }
}

Token *identifier(Lexer *lexer) {
    size_t start = lexer->pos;
    
    while (lexer->current_char && (isalnum((unsigned char)lexer->current_char) || lexer->current_char == '_'))
        advance(lexer);
    size_t len = lexer->pos - start;
    char *id_str = malloc(len + 1);
    strncpy(id_str, lexer->text + start, len);
    id_str[len] = '\0';

    // ADD DEBUG PRINT 1: See the raw lexeme before lowercasing
    DEBUG_PRINT("identifier: Raw lexeme formed: \"_%s_\" (len %zu)\n", id_str, len);
    // Optional: print char by char if you suspect invisible characters
    // for (size_t k_debug = 0; k_debug < len; ++k_debug) {
    //     DEBUG_PRINT("identifier: raw_char[%zu] = '%c' (ASCII: %d)\n", k_debug, id_str[k_debug], id_str[k_debug]);
    // }

    for (size_t i = 0; i < len; i++) // Convert to lowercase
        id_str[i] = tolower((unsigned char)id_str[i]);

    // ADD DEBUG PRINT 2: See the lowercase lexeme
    DEBUG_PRINT("identifier: Lowercase lexeme: \"_%s_\"\n", id_str);

    Token *token = malloc(sizeof(Token));
    token->type = TOKEN_IDENTIFIER; // Default to IDENTIFIER
    for (int i = 0; i < (int)NUM_KEYWORDS; i++) {
        // ADD DEBUG PRINT 3: See the comparison being made
        // DEBUG_PRINT("identifier: Comparing \"_%s_\" with keyword \"_%s_\"\n", id_str, keywords[i].keyword);
        if (strcmp(id_str, keywords[i].keyword) == 0) {
            token->type = keywords[i].token_type;
            // ADD DEBUG PRINT 4: Confirm if a keyword was matched
            DEBUG_PRINT("identifier: Matched keyword! Lexeme: \"_%s_\", Type set to %s\n", id_str, tokenTypeToString(token->type));
            break;
        }
    }
    token->value = id_str;

#ifdef DEBUG // Your existing debug block
    if (token->type == TOKEN_USES) {
        // This existing printf uses stdout, so it should appear if hit.
        printf("Lexer: Tokenized 'uses' as TOKEN_USES (via existing printf)\n");
        fflush(stdout); // Ensure it prints immediately
    } else if (token->type == TOKEN_IDENTIFIER && strcasecmp(token->value, "uses") == 0) {
        // ADD THIS ELSE IF: To see if "uses" was tokenized as an identifier
        DEBUG_PRINT("identifier: Lexeme \"uses\" was tokenized as TOKEN_IDENTIFIER, not TOKEN_USES.\n");
    }
#endif
    return token;
}

Token *stringLiteral(Lexer *lexer) {
    advance(lexer);  // Skip opening '
    int token_line = lexer->line;
    int token_column = lexer->column;
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
    Token *token = newToken(TOKEN_STRING_CONST, buffer, token_line, token_column);
    free(buffer);
    return token;
}

Token *getNextToken(Lexer *lexer) {
    int start_line = lexer->line;     // Capture at start
    int start_column = lexer->column; // Capture at start
#ifdef DEBUG
    fprintf(stderr, "LEXER_DEBUG: getNextToken\n"); fflush(stderr);
#endif
    DEBUG_PRINT("getNextToken: Entry. Current char: '%c' (ASCII: %d) at line %d, col %d\n", lexer->current_char, lexer->current_char, lexer->line, lexer->column);
    while (lexer->current_char) {
        // Skip whitespace
        if (isspace((unsigned char)lexer->current_char)) {
            skipWhitespace(lexer);
            continue;
        }

        // Skip single-line comments (// ...)
        if (lexer->current_char == '/' && lexer->pos + 1 < strlen(lexer->text) && lexer->text[lexer->pos + 1] == '/') {
            while (lexer->current_char && lexer->current_char != '\n')
                advance(lexer);
            if (lexer->current_char == '\n') // Consume the newline as well
                advance(lexer);
            continue;
        }

        // Skip brace-delimited comments { ... }
        if (lexer->current_char == '{') {
            advance(lexer);  // Skip '{'
            int comment_level = 1; // Handle nested comments like (* { nested } *)
            while (lexer->current_char && comment_level > 0) {
                 if (lexer->current_char == '}') {
                     comment_level--;
                     advance(lexer);
                 } else if (lexer->current_char == '{') { // Detect nested start
                      comment_level++;
                      advance(lexer);
                 } else {
                     advance(lexer);
                 }
            }
             // Check if comment was terminated
             if (comment_level > 0) {
                  fprintf(stderr, "Lexer error at line %d, column %d: Unterminated brace comment.\n", lexer->line, lexer->column);
                  // Optionally return an error token or advance past the problematic char
             }
            continue; // Resume token search
        }
        
        DEBUG_PRINT("getNextToken: After skip WS/Comment. Current char: '%c' (ASCII: %d)\n", lexer->current_char, lexer->current_char);

         // Skip parenthesis/star comments (* ... *) - Added Nested Handling
         if (lexer->current_char == '(' && lexer->pos + 1 < strlen(lexer->text) && lexer->text[lexer->pos + 1] == '*') {
             advance(lexer); // Skip '('
             advance(lexer); // Skip '*'
             int comment_level = 1;
             while (lexer->current_char && comment_level > 0) {
                 if (lexer->current_char == '*' && lexer->pos + 1 < strlen(lexer->text) && lexer->text[lexer->pos + 1] == ')') {
                      comment_level--;
                      advance(lexer); // Skip '*'
                      advance(lexer); // Skip ')'
                 } else if (lexer->current_char == '(' && lexer->pos + 1 < strlen(lexer->text) && lexer->text[lexer->pos + 1] == '*') { // Detect nested start
                      comment_level++;
                      advance(lexer); // Skip '('
                      advance(lexer); // Skip '*'
                 } else {
                     advance(lexer);
                 }
             }
             // Check if comment was terminated
             if (comment_level > 0) {
                 fprintf(stderr, "Lexer error at line %d, column %d: Unterminated parenthesis-star comment.\n", lexer->line, lexer->column);
             }
             continue; // Resume token search
         }

        // Handle Character Code Constant (# followed by decimal digits)
        if (lexer->current_char == '#') {
            advance(lexer); // Consume '#'
            size_t start = lexer->pos;
            while (isdigit((unsigned char)lexer->current_char)) {
                advance(lexer);
            }
            size_t len = lexer->pos - start;
            if (len == 0) {
                lexerError(lexer, "Character code literal must have at least one digit after '#'");
                return newToken(TOKEN_UNKNOWN, "#", start_line, start_column);
            }
            char *num_str = malloc(len + 1);
            if (!num_str) { fprintf(stderr, "malloc failed in getNextToken for char code\n"); EXIT_FAILURE_HANDLER(); }
            strncpy(num_str, lexer->text + start, len);
            num_str[len] = '\0';
            long val = atol(num_str);
            free(num_str);

            if (val < 0 || val > 255) {
                lexerError(lexer, "Character code value out of range (0-255)");
            }
            // Represent this as a single-character string constant for the parser.
            // The parser's `evaluateCompileTimeValue` will see a STRING_CONST of length 1
            // and correctly convert it to a CHAR value.
            char char_buf[2];
            char_buf[0] = (char)val;
            char_buf[1] = '\0';
            return newToken(TOKEN_STRING_CONST, char_buf, start_line, start_column);
        }
        
        // Handle Hex Constant ($ followed by hex digits)
        if (lexer->current_char == '$') {
            advance(lexer); // Consume '$'
            size_t start = lexer->pos;
            while (isHexDigit(lexer->current_char)) {
                advance(lexer);
            }
            size_t len = lexer->pos - start;
            if (len == 0) {
                lexerError(lexer, "Hex literal must have at least one digit after '$'");
                return newToken(TOKEN_UNKNOWN, "$", start_line, start_column);
            }
            char *hex_str = malloc(len + 1);
            if (!hex_str) { fprintf(stderr, "malloc failed in getNextToken for hex const\n"); EXIT_FAILURE_HANDLER(); }
            strncpy(hex_str, lexer->text + start, len);
            hex_str[len] = '\0';
            Token* token = newToken(TOKEN_HEX_CONST, hex_str, start_line, start_column);
            free(hex_str);
            return token;
        }


        // Handle Identifiers and Keywords
        if (isalpha((unsigned char)lexer->current_char) || lexer->current_char == '_') {
#ifdef DEBUG
            fprintf(stderr, "LEXER_DEBUG: getNextToken(return identifier)\n"); fflush(stderr);
#endif
            return identifier(lexer);
        }

        // Handle Integer or Real Constants (starting with a digit)
        if (isdigit((unsigned char)lexer->current_char)) {
#ifdef DEBUG
            fprintf(stderr, "LEXER_DEBUG: getNextToken(return number)\n"); fflush(stderr);
#endif
            return number(lexer);
        }

        // Handle String Literals
        if (lexer->current_char == '\'') {
#ifdef DEBUG
            fprintf(stderr, "LEXER_DEBUG: getNextToken(return stringLiteral)\n"); fflush(stderr);
#endif
            return stringLiteral(lexer);
        }

        // --- Operators and Punctuation ---

        // Caret (Pointer symbol)
        if (lexer->current_char == '^') { // <<< ADDED
            advance(lexer);
#ifdef DEBUG
            fprintf(stderr, "LEXER_DEBUG: getNextToken(return TOKEN_CARET)\n"); fflush(stderr);
#endif
            return newToken(TOKEN_CARET, "^", start_line, start_column);
        }

        // Colon or Assign
        if (lexer->current_char == ':') {
            advance(lexer);
            if (lexer->current_char == '=') {
                advance(lexer);
#ifdef DEBUG
                fprintf(stderr, "LEXER_DEBUG: getNextToken(return TOKEN_ASSIGN)\n"); fflush(stderr);
#endif
                return newToken(TOKEN_ASSIGN, ":=", start_line, start_column);
            } else {
#ifdef DEBUG
                fprintf(stderr, "LEXER_DEBUG: getNextToken(return TOKEN_COLON)\n"); fflush(stderr);
#endif
                return newToken(TOKEN_COLON, ":", start_line, start_column);
            }
        }

        // Semicolon
        if (lexer->current_char == ';') {
            advance(lexer);
#ifdef DEBUG
            fprintf(stderr, "LEXER_DEBUG: getNextToken(return TOKEN_SEMICOLON)\n"); fflush(stderr);
#endif
            return newToken(TOKEN_SEMICOLON, ";", start_line, start_column);
        }

        // Comma
        if (lexer->current_char == ',') {
            advance(lexer);
#ifdef DEBUG
            fprintf(stderr, "LEXER_DEBUG: getNextToken(return TOKEN_COMMA)\n"); fflush(stderr);
#endif
            return newToken(TOKEN_COMMA, ",", start_line, start_column);
        }

        // Period or DotDot
        if (lexer->current_char == '.') {
            advance(lexer);
            if (lexer->current_char == '.') {
                advance(lexer);
#ifdef DEBUG
                fprintf(stderr, "LEXER_DEBUG: getNextToken(return TOKEN_DOTDOT)\n"); fflush(stderr);
#endif
                return newToken(TOKEN_DOTDOT, "..", start_line, start_column);
            } else {
#ifdef DEBUG
                fprintf(stderr, "LEXER_DEBUG: getNextToken(return TOKEN_PERIOD)\n"); fflush(stderr);
#endif
                return newToken(TOKEN_PERIOD, ".", start_line, start_column);
            }
        }

#ifdef DEBUG
        fprintf(stderr, "LEXER_DEBUG: getNextToken(Math Section)\n"); fflush(stderr);
#endif
        // Simple operators: +, -, *, /
        if (lexer->current_char == '+') { advance(lexer); return newToken(TOKEN_PLUS, "+", start_line, start_column); }
        if (lexer->current_char == '-') { advance(lexer); return newToken(TOKEN_MINUS, "-", start_line, start_column); }
        if (lexer->current_char == '*') { advance(lexer); return newToken(TOKEN_MUL, "*", start_line, start_column); }
        if (lexer->current_char == '/') { advance(lexer); return newToken(TOKEN_SLASH, "/", start_line, start_column); }

        // Parentheses and Brackets
        if (lexer->current_char == '(') { advance(lexer); return newToken(TOKEN_LPAREN, "(", start_line, start_column); }
        if (lexer->current_char == ')') { advance(lexer); return newToken(TOKEN_RPAREN, ")", start_line, start_column); }
        if (lexer->current_char == '[') { advance(lexer); return newToken(TOKEN_LBRACKET, "[", start_line, start_column); }
        if (lexer->current_char == ']') { advance(lexer); return newToken(TOKEN_RBRACKET, "]", start_line, start_column); }

        // Relational Operators: =, <, >
        if (lexer->current_char == '=') { advance(lexer); return newToken(TOKEN_EQUAL, "=", start_line, start_column); }
        if (lexer->current_char == '<') {
            advance(lexer);
            if (lexer->current_char == '=') { advance(lexer); return newToken(TOKEN_LESS_EQUAL, "<=", start_line, start_column); }
            if (lexer->current_char == '>') { advance(lexer); return newToken(TOKEN_NOT_EQUAL, "<>", start_line, start_column); }
            return newToken(TOKEN_LESS, "<", start_line, start_column);
        }
        if (lexer->current_char == '>') {
            advance(lexer);
            if (lexer->current_char == '=') { advance(lexer); return newToken(TOKEN_GREATER_EQUAL, ">=", start_line, start_column); }
            return newToken(TOKEN_GREATER, ">", start_line, start_column);
        }
        // Handle Not Equal (!=)
        if (lexer->current_char == '!') {
            // Check if the next character is '='
            if (lexer->pos + 1 < strlen(lexer->text) && lexer->text[lexer->pos + 1] == '=') {
                advance(lexer); // Consume '!'
                advance(lexer); // Consume '='
                return newToken(TOKEN_NOT_EQUAL, "!=", start_line, start_column); // Recognize !=
            } else {
                // If '!' is not followed by '=', it's an unrecognized character in Pascal.
                // Fall through to the generic unknown character handling below.
            }
        }

        // If character is not recognized by any rule above
        char unknown_char_str[2] = {lexer->current_char, '\0'};
        fprintf(stderr, "Lexer error at line %d, column %d: Unrecognized character '%s'\n",
                lexer->line, lexer->column, unknown_char_str);
        advance(lexer); // Consume the unknown character to prevent infinite loop
        return newToken(TOKEN_UNKNOWN, unknown_char_str, start_line, start_column); // Return an UNKNOWN token

    } // End while (lexer->current_char)

    // End of input reached
    return newToken(TOKEN_EOF, "EOF", start_line, start_column);
}

void lexerError(Lexer *lexer, const char *msg) {
    if (!lexer || !msg) {
        fprintf(stderr, "Lexer error: (null lexer or message passed to handler)\n");
    } else {
        fprintf(stderr, "Lexer error at line %d, column %d: %s (current char: '%c')\n",
                lexer->line, lexer->column, msg, lexer->current_char);
    }
    EXIT_FAILURE_HANDLER();
}

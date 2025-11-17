// parser.c
// The parsing routines for pscal

#include "core/list.h"
#include "compiler/compiler.h"
#include "Pascal/parser.h"
#include "Pascal/type_registry.h"
#include "core/utils.h"
#include "core/types.h"
#include "globals.h"
#include "backend_ast/builtin.h"
#include "symbol/symbol.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <stdint.h>

// Define the helper function *only* when DEBUG is enabled
// No 'static inline' needed here as it's defined only once in this file.
#ifdef DEBUG
void eatDebugWrapper(Parser *parser_ptr, TokenType expected_token_type, const char* func_name) {
    // Test print BEFORE the main fprintf
    fprintf(stderr, "[DEBUG eatDebugWrapper] ENTERED from %s. Expecting: %s. Current token type: %s.\n",
            func_name,
            tokenTypeToString(expected_token_type),
            parser_ptr->current_token ? tokenTypeToString(parser_ptr->current_token->type) : "NULL_TOKEN_TYPE");
    fflush(stderr);

    if (dumpExec) {
        // This is the original complex fprintf that might be hanging
        fprintf(stderr, "[DEBUG eat()] Called from %s() - Expecting: %s, Got: %s ('%s') at Line %d, Col %d\n",
                 func_name, tokenTypeToString(expected_token_type),
                 parser_ptr->current_token ? tokenTypeToString(parser_ptr->current_token->type) : "NULL_TOKEN_TYPE", // Added NULL check
                 (parser_ptr->current_token && parser_ptr->current_token->value) ? parser_ptr->current_token->value : "NULL_TOKEN_VALUE", // Added NULL check
                 parser_ptr->lexer ? parser_ptr->lexer->line : -1, // Added NULL check
                 parser_ptr->lexer ? parser_ptr->lexer->column : -1); // Added NULL check
        fflush(stderr); // Crucial

        if (parser_ptr->current_token && parser_ptr->current_token->type != expected_token_type) { // Added NULL check
             fprintf(stderr, "[DEBUG eat(): *** TOKEN MISMATCH DETECTED by wrapper before calling original eat() ***\n");
             fflush(stderr);
        }
    }

    // Test print AFTER the main fprintf
    fprintf(stderr, "[DEBUG eatDebugWrapper] Calling eatInternal.\n");
    fflush(stderr);

    eatInternal(parser_ptr, expected_token_type);

    fprintf(stderr, "[DEBUG eatDebugWrapper] RETURNED from eatInternal.\n");
    fflush(stderr);
}
#endif // DEBUG

AST *parseWriteArgument(Parser *parser);
static AST *parseStrArgumentList(Parser *parser);
AST *spawnStatement(Parser *parser);
AST *joinStatement(Parser *parser);
static AST *labelDeclarationBlock(Parser *parser);
static bool tokenTerminatesStatement(TokenType type);
static bool tokenTypeIsIdentifierLike(TokenType type);
static bool tokenIsIdentifierLike(const Token *token);
static bool currentTokenIsIdentifierLike(Parser *parser);
static AST *parseInterfaceType(Parser *parser);
static AST *parseInterfaceMethod(Parser *parser, bool isFunction);
static void registerRecordMethods(Parser *parser, const char *recordName, AST *recordType);
static void adoptRoutineParameters(AST *routine, AST *params);
static Token *parseQualifiedRoutineName(Parser *parser, const char *missingNameError);
static AST *parseTypeAssertionTarget(Parser *parser, TokenType keywordToken);

static void appendDependencyPath(Parser *parser, const char *path) {
    if (!parser || !parser->dependency_paths || !path || !*path) {
        return;
    }

    char *canonical = realpath(path, NULL);
    const char *to_store = canonical ? canonical : path;

    for (ListNode *node = parser->dependency_paths->head; node; node = node->next) {
        if (strcmp(node->value, to_store) == 0) {
            if (canonical) free(canonical);
            return;
        }
    }

    listAppend(parser->dependency_paths, to_store);
    if (canonical) free(canonical);
}

static bool tokenTypeIsIdentifierLike(TokenType type) {
    return type == TOKEN_IDENTIFIER || type == TOKEN_LABEL;
}

static bool tokenIsIdentifierLike(const Token *token) {
    return token && tokenTypeIsIdentifierLike(token->type);
}

static bool currentTokenIsIdentifierLike(Parser *parser) {
    return parser && tokenIsIdentifierLike(parser->current_token);
}

static bool tokenMatchesKeyword(const Token *token, const char *keyword) {
    return token && token->value && keyword && strcasecmp(token->value, keyword) == 0;
}

static AST *labelDeclarationBlock(Parser *parser) {
    if (!parser) {
        return NULL;
    }

    eat(parser, TOKEN_LABEL);

    AST *list = newASTNode(AST_COMPOUND, NULL);
    if (!list) {
        return NULL;
    }

    bool saw_label = false;
    while (parser->current_token &&
           (tokenIsIdentifierLike(parser->current_token) ||
            parser->current_token->type == TOKEN_INTEGER_CONST)) {
        Token *label_copy = copyToken(parser->current_token);
        if (!label_copy && parser->current_token) {
            freeAST(list);
            EXIT_FAILURE_HANDLER();
        }

        TokenType label_type = parser->current_token->type;
        eat(parser, label_type);

        AST *decl = newLabelDeclaration(label_copy);
        freeToken(label_copy);
        if (!decl) {
            freeAST(list);
            return NULL;
        }
        addChild(list, decl);
        saw_label = true;

        if (parser->current_token && parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else {
            break;
        }
    }

    if (!saw_label) {
        errorParser(parser, "Expected one or more labels after LABEL");
        freeAST(list);
        return NULL;
    }

    if (!parser->current_token || parser->current_token->type != TOKEN_SEMICOLON) {
        errorParser(parser, "Expected ';' after label declaration");
        freeAST(list);
        return NULL;
    }

    eat(parser, TOKEN_SEMICOLON);
    return list;
}

AST *declarations(Parser *parser, bool in_interface) {
#ifdef DEBUG
    fprintf(stderr, "[DEBUG declarations] ENTER. Current token: %s ('%s')\n",
            parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
            (parser->current_token && parser->current_token->value) ? parser->current_token->value : "NULL_VALUE");
    fflush(stderr);
#endif
    AST *node = newASTNode(AST_COMPOUND, NULL);

    while (1) {
#ifdef DEBUG
    fprintf(stderr, "[DEBUG declarations] Loop start. Current token: %s ('%s')\n",
            parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
            (parser->current_token && parser->current_token->value) ? parser->current_token->value : "NULL_VALUE");
    fflush(stderr);
#endif
        if (parser->current_token == NULL) {
            fprintf(stderr, "Parser error: Unexpected end of file in declarations block.\n");
            fflush(stderr); // Ensure message is printed
            // EXIT_FAILURE_HANDLER(); // Or allow graceful exit if preferred
            break; // Exit the while(1) loop
        }

        if (parser->current_token->type == TOKEN_LABEL) {
            AST *labels = labelDeclarationBlock(parser);
            if (labels && labels->type != AST_NOOP) {
                addChild(node, labels);
            } else if (labels) {
                freeAST(labels);
            }
        } else if (parser->current_token->type == TOKEN_CONST) {
            eat(parser, TOKEN_CONST);
            // Loop for multiple constant declarations within a single CONST block
            while (currentTokenIsIdentifierLike(parser)) {
                AST *constDecl = constDeclaration(parser); // Parses "identifier = value;"
                if (!constDecl || constDecl->type == AST_NOOP) {
                    // constDeclaration should call errorParser if there's a syntax error.
                    // Breaking here means we stop processing further consts in this block.
                    break;
                }
                addChild(node, constDecl);
            }
        } else if (parser->current_token->type == TOKEN_TYPE) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG declarations] Matched TOKEN_TYPE. About to eat.\n");
            fflush(stderr);
#endif
            eat(parser, TOKEN_TYPE);
#ifdef DEBUG
            fprintf(stderr, "[DEBUG declarations] Ate TOKEN_TYPE. Current token: %s ('%s')\n",
                    parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
                    (parser->current_token && parser->current_token->value) ? parser->current_token->value : "NULL_VALUE");
            fflush(stderr);
#endif
            // Loop for multiple type alias lines within a single TYPE block
            while (currentTokenIsIdentifierLike(parser)) {
                AST *typeDecl = typeDeclaration(parser); // Parses "TypeName = TypeSpecifier;"
                if (!typeDecl || typeDecl->type == AST_NOOP) {
                    // typeDeclaration should call errorParser on syntax error.
                    break;
                }
                addChild(node, typeDecl);
                // insertType() is called within typeDeclaration to register the type.
            }
        } else if (parser->current_token->type == TOKEN_VAR) {
            eat(parser, TOKEN_VAR);
            // Loop for multiple variable declaration lines within a single VAR block
            // e.g., VAR a,b: integer; c: char;
            while (currentTokenIsIdentifierLike(parser)) {
                AST *vdecl_result = varDeclaration(parser, parser->current_unit_name_context == NULL);
                if (!vdecl_result || vdecl_result->type == AST_NOOP) {
                    // varDeclaration should call errorParser on syntax error.
                    break;
                }

                if (vdecl_result->type == AST_COMPOUND) {
                    // Transfer children (individual AST_VAR_DECLs) from the compound node
                    for (int k = 0; k < vdecl_result->child_count; ++k) {
                        AST *individual_var_decl = vdecl_result->children[k];
                        if (individual_var_decl) {
                            addChild(node, individual_var_decl);
                        }
                    }
                    vdecl_result->children = NULL; // Prevent double free
                    vdecl_result->child_count = 0;
                    vdecl_result->child_capacity = 0;
                    freeAST(vdecl_result); // Free the AST_COMPOUND wrapper
                } else if (vdecl_result->type == AST_VAR_DECL) {
                    addChild(node, vdecl_result);
                }

                // After "var_id_list : type_spec", a semicolon is expected to either
                // separate it from the next var_id_list or terminate the VAR section
                // if followed by another keyword (CONST, TYPE, PROCEDURE, BEGIN, etc.)
                if (parser->current_token && parser->current_token->type == TOKEN_SEMICOLON) {
                    eat(parser, TOKEN_SEMICOLON);
                    // If after the semicolon, the next token is not an identifier,
                    // this inner while loop for VARs will terminate.
                } else {
                    // If it's not a semicolon, and the next line *does* start with an identifier,
                    // it implies a missing semicolon between var declaration lines.
                    if (currentTokenIsIdentifierLike(parser)) {
                         errorParser(parser, "Expected semicolon to separate variable declarations within VAR block");
                    }
                    // If not a semicolon and not another identifier, this VAR section is ending.
                    // The inner `while` loop condition will handle termination.
                    break; // Break from this inner `while` loop for VAR lines.
                }
            }
        } else if (parser->current_token->type == TOKEN_PROCEDURE ||
                   parser->current_token->type == TOKEN_FUNCTION) {
            AST *decl_routine = (parser->current_token->type == TOKEN_PROCEDURE)
                        ? procedureDeclaration(parser, in_interface)
                        : functionDeclaration(parser, in_interface);
            if (!decl_routine || decl_routine->type == AST_NOOP) {
                // procedureDeclaration/functionDeclaration should handle their own errors.
                // Breaking here exits the outer while(1) loop.
                break;
            }
            addChild(node, decl_routine);
            // `addProcedure` is called from within procedureDeclaration/functionDeclaration.
            
            // A semicolon usually follows a procedure/function declaration (header or full).
            // `procedureDeclaration` (if not in_interface) eats the semicolon after the block's END.
            // This semicolon here would be for the one after a header in an interface,
            // or after a full declaration if not eaten internally.
            if (parser->current_token && parser->current_token->type == TOKEN_SEMICOLON) {
                eat(parser, TOKEN_SEMICOLON);
            }
            // If `in_interface` and no semicolon, the block might be ending.
            // If not `in_interface`, `procedureDeclaration` should have eaten the final semicolon.
            // Further error checking for missing semicolons could be added if needed,
            // depending on how strictly the grammar is enforced here vs. letting the next
            // token check in the outer loop handle it.
        } else {
            // Token is not CONST, TYPE, VAR, PROCEDURE, or FUNCTION.
            // This signifies the end of the declarations section.
            break;
        }
    }
#ifdef DEBUG
    fprintf(stderr, "[DEBUG declarations] EXIT. Next token: %s ('%s')\n",
            parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
            (parser->current_token && parser->current_token->value) ? parser->current_token->value : "NULL_VALUE");
    fflush(stderr);
#endif
    return node;
}

void eatInternal(Parser *parser, TokenType type) {
    if (parser->current_token == NULL) { // Should not happen if called correctly
        fprintf(stderr, "Parser error in eatInternal: current_token is NULL. Expected %s.\n", tokenTypeToString(type));
        fflush(stderr);
        EXIT_FAILURE_HANDLER(); // This is a critical internal error
    }

    if (parser->current_token->type == type ||
        (type == TOKEN_IDENTIFIER && parser->current_token->type == TOKEN_LABEL)) {
        Token *tokenToFree = parser->current_token;
        // Get the next token BEFORE freeing the current one
        parser->current_token = getNextToken(parser->lexer);
        if (tokenToFree) {
            freeToken(tokenToFree);
        }
    } else {
        char err[128];
        snprintf(err, sizeof(err), "Expected token %s, got %s",
                 tokenTypeToString(type), tokenTypeToString(parser->current_token->type));
        errorParser(parser, err); // errorParser should call EXIT_FAILURE_HANDLER
    }
}

// parse_write_argument: parses an expression optionally followed by formatting specifiers.
// The syntax is: <expression> [ : <fieldWidth> [ : <decimalPlaces> ] ]
AST *parseWriteArguments(Parser *parser) {
    AST *argList = newASTNode(AST_COMPOUND, NULL);
    // Check if arguments are present (standard Pascal requires parentheses)
    if (parser->current_token->type != TOKEN_LPAREN) {
        // No arguments provided or non-standard syntax used.
        // Return an empty list. Caller (write/writelnStatement) handles this.
        return argList; // Return empty compound node
    }

    eat(parser, TOKEN_LPAREN); // Consume '('

    if (parser->current_token->type != TOKEN_RPAREN) { // Check if list isn't empty
        while (1) {
            AST *arg = parseWriteArgument(parser); // Parses expr [: width [: prec]]
            addChild(argList, arg);
            if (parser->current_token->type == TOKEN_COMMA) {
                eat(parser, TOKEN_COMMA);
            } else {
                break; // Exit loop if no comma follows
            }
        }
    }
    eat(parser, TOKEN_RPAREN); // Consume ')'
    return argList;
}

// lvalue: Parses variable.field[index] etc. Calls expression for index.
AST *lvalue(Parser *parser) {
    Token *ident_token_snapshot = parser->current_token; // Snapshot the first token

    if (!tokenIsIdentifierLike(ident_token_snapshot)) {
        errorParser(parser, "Expected identifier at start of lvalue");
        return newASTNode(AST_NOOP, NULL); // Return a NOOP node on error
    }

    // Create the base variable node.
    // newASTNode should make its own copy of the token if it needs to persist it.
    AST *node = newASTNode(AST_VARIABLE, ident_token_snapshot);
    eat(parser, ident_token_snapshot->type); // Consume the first identifier-like token

    // Loop for subsequent field access '.', array '[]', or pointer '^'
    while (parser->current_token &&
           (parser->current_token->type == TOKEN_PERIOD ||
            parser->current_token->type == TOKEN_LBRACKET ||
            parser->current_token->type == TOKEN_CARET)) {
        
        if (parser->current_token->type == TOKEN_PERIOD) {
            eat(parser, TOKEN_PERIOD); // Consume '.'
            Token *field_token_snapshot = parser->current_token;
            if (!tokenIsIdentifierLike(field_token_snapshot)) {
                errorParser(parser, "Expected field name after '.'");
                // 'node' is the AST built so far (the record variable); return it.
                return node;
            }
            // For AST_FIELD_ACCESS, the node's token is the field name itself.
            AST *fa_node = newASTNode(AST_FIELD_ACCESS, field_token_snapshot);
            eat(parser, field_token_snapshot->type); // Consume the field identifier-like token

            setLeft(fa_node, node); // The current 'node' (e.g., record variable) becomes the left child
            node = fa_node;         // The new field_access_node becomes the current 'node'

        } else if (parser->current_token->type == TOKEN_LBRACKET) {
            eat(parser, TOKEN_LBRACKET); // Consume '['
            // AST_ARRAY_ACCESS node typically doesn't have its own primary token,
            // the left child is the array, and children are indices.
            AST *aa_node = newASTNode(AST_ARRAY_ACCESS, NULL);
            setLeft(aa_node, node); // Current 'node' (array variable) is the left child

            // Parse index expressions (one or more, comma-separated)
            do {
                AST *index_expr = expression(parser);
                if (!index_expr || index_expr->type == AST_NOOP) {
                    errorParser(parser, "Invalid index expression in lvalue");
                    freeAST(aa_node); // Clean up partially formed AST_ARRAY_ACCESS node
                    return node;      // Return the AST formed before this failed array access
                }
                addChild(aa_node, index_expr); // Add this index expression as a child

                if (parser->current_token && parser->current_token->type == TOKEN_COMMA) {
                    eat(parser, TOKEN_COMMA); // Consume comma, look for next index
                } else {
                    break; // No more commas, so no more indices
                }
            } while (1);

            if (!parser->current_token || parser->current_token->type != TOKEN_RBRACKET) {
                errorParser(parser, "Expected ']' to close array indices in lvalue");
                freeAST(aa_node); // Clean up partially formed AST_ARRAY_ACCESS node
                return node;      // Return the AST formed before this failed array access
            }
            eat(parser, TOKEN_RBRACKET); // Consume ']'
            node = aa_node; // The array_access_node is now the current 'node'

        } else if (parser->current_token->type == TOKEN_CARET) {
            eat(parser, TOKEN_CARET); // Consume '^'
            AST *deref_node = newASTNode(AST_DEREFERENCE, NULL);
            setLeft(deref_node, node);
            // The type of the dereferenced expression will be set during type annotation.
            // For parsing, we just build the structure.
            node = deref_node; // The dereference_node is now the current 'node'
        }
    }
    return node; // Return the fully constructed lvalue AST
}
// parseArrayType: Calls expression for bounds
AST *parseArrayType(Parser *parser) {
    eat(parser, TOKEN_ARRAY);
    // Support open array parameters of the form "array of <type>" by
    // skipping bound parsing when no index list is provided.
    if (parser->current_token && parser->current_token->type == TOKEN_OF) {
        eat(parser, TOKEN_OF);
        AST *elemType = typeSpecifier(parser, 1);
        if (!elemType || elemType->type == AST_NOOP) {
            errorParser(parser, "Invalid element type for array");
            return NULL;
        }
        AST *node = newASTNode(AST_ARRAY_TYPE, NULL);
        setTypeAST(node, TYPE_ARRAY);
        node->children = NULL;
        node->child_count = 0;
        node->child_capacity = 0;
        setRight(node, elemType);
        return node;
    }

    if (!parser->current_token || parser->current_token->type != TOKEN_LBRACKET) {
        errorParser(parser, "Expected '[' after ARRAY"); // Corrected error message
        return NULL;
    }
    eat(parser, TOKEN_LBRACKET);

    AST *indexList = newASTNode(AST_COMPOUND, NULL); // To hold AST_SUBRANGE nodes

    while (1) {
        AST *lower_expr_node = expression(parser); // Parse the lower bound expression
        if (!lower_expr_node || lower_expr_node->type == AST_NOOP) {
            errorParser(parser, "Invalid lower bound expression for array");
            freeAST(indexList);
            return NULL;
        }

        Value lower_eval = evaluateCompileTimeValue(lower_expr_node);
        AST *resolved_lower_ast_node = NULL;

        if (lower_eval.type == TYPE_INTEGER) {
            // Create an AST_NUMBER node for the resolved integer value
            Token temp_token_lower; // Create a temporary token for newASTNode
            char val_str_lower[32];
            snprintf(val_str_lower, sizeof(val_str_lower), "%lld", lower_eval.i_val);
            temp_token_lower.type = TOKEN_INTEGER_CONST;
            temp_token_lower.value = val_str_lower; // Points to stack, but newASTNode copies
            temp_token_lower.length = strlen(val_str_lower);
            temp_token_lower.line = lower_expr_node->token ? lower_expr_node->token->line : parser->lexer->line;
            temp_token_lower.column = lower_expr_node->token ? lower_expr_node->token->column : parser->lexer->column;
            
            resolved_lower_ast_node = newASTNode(AST_NUMBER, &temp_token_lower);
            setTypeAST(resolved_lower_ast_node, TYPE_INTEGER); // Set type for the new AST_NUMBER node
            resolved_lower_ast_node->i_val = (int)lower_eval.i_val; // Store the integer value directly if AST_NUMBER supports it
        }
        // Add similar handling for TYPE_CHAR, TYPE_BOOLEAN, or other ordinals if evaluateCompileTimeValue supports them
        // and if your AST_SUBRANGE expects specific AST node types for resolved bounds.
        // For now, we'll primarily focus on TYPE_INTEGER.
        else {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg), "Array lower bound is not a constant integer expression (got type %s)", varTypeToString(lower_eval.type));
            errorParser(parser, err_msg);
            freeValue(&lower_eval);
            freeAST(lower_expr_node);
            freeAST(indexList);
            return NULL;
        }
        freeValue(&lower_eval);    // Free contents of lower_eval if any (none for TYPE_INTEGER)
        freeAST(lower_expr_node); // Free the original expression AST node

        if (!parser->current_token || parser->current_token->type != TOKEN_DOTDOT) {
            errorParser(parser, "Expected '..' in array range");
            freeAST(resolved_lower_ast_node);
            freeAST(indexList);
            return NULL;
        }
        eat(parser, TOKEN_DOTDOT);

        AST *upper_expr_node = expression(parser); // Parse the upper bound expression
        if (!upper_expr_node || upper_expr_node->type == AST_NOOP) {
            errorParser(parser, "Invalid upper bound expression for array");
            freeAST(resolved_lower_ast_node);
            freeAST(indexList);
            return NULL;
        }

        Value upper_eval = evaluateCompileTimeValue(upper_expr_node);
        AST *resolved_upper_ast_node = NULL;

        if (upper_eval.type == TYPE_INTEGER) {
            Token temp_token_upper;
            char val_str_upper[32];
            snprintf(val_str_upper, sizeof(val_str_upper), "%lld", upper_eval.i_val);
            temp_token_upper.type = TOKEN_INTEGER_CONST;
            temp_token_upper.value = val_str_upper;
            temp_token_upper.length = strlen(val_str_upper);
            temp_token_upper.line = upper_expr_node->token ? upper_expr_node->token->line : parser->lexer->line;
            temp_token_upper.column = upper_expr_node->token ? upper_expr_node->token->column : parser->lexer->column;

            resolved_upper_ast_node = newASTNode(AST_NUMBER, &temp_token_upper);
            setTypeAST(resolved_upper_ast_node, TYPE_INTEGER);
            resolved_upper_ast_node->i_val = (int)upper_eval.i_val;
        } else {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg), "Array upper bound is not a constant integer expression (got type %s)", varTypeToString(upper_eval.type));
            errorParser(parser, err_msg);
            freeValue(&upper_eval);
            freeAST(upper_expr_node);
            freeAST(resolved_lower_ast_node);
            freeAST(indexList);
            return NULL;
        }
        freeValue(&upper_eval);
        freeAST(upper_expr_node);

        AST *range = newASTNode(AST_SUBRANGE, NULL);
        setLeft(range, resolved_lower_ast_node);  // Set left to the resolved AST_NUMBER node
        setRight(range, resolved_upper_ast_node); // Set right to the resolved AST_NUMBER node
        // You might also want to set range->var_type here if subranges have a type, e.g., TYPE_INTEGER if bounds are int.
        setTypeAST(range, TYPE_INTEGER); // Assuming subrange of integers for now

        addChild(indexList, range);

        if (parser->current_token && parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else {
            break;
        }
    }

    if (!parser->current_token || parser->current_token->type != TOKEN_RBRACKET) {
        errorParser(parser, "Expected ']' to close array dimension(s)");
        freeAST(indexList); // indexList contains AST_SUBRANGE nodes whose children (AST_NUMBER) will be freed
        return NULL;
    }
    eat(parser, TOKEN_RBRACKET);

    if (!parser->current_token || parser->current_token->type != TOKEN_OF) {
        errorParser(parser, "Expected 'OF' after array dimensions");
        freeAST(indexList);
        return NULL;
    }
    eat(parser, TOKEN_OF);

    AST *elemType = typeSpecifier(parser, 1); // allowAnonymous = true for element type
    if (!elemType || elemType->type == AST_NOOP) {
        errorParser(parser, "Invalid element type for array");
        freeAST(indexList);
        return NULL;
    }

    AST *node = newASTNode(AST_ARRAY_TYPE, NULL);
    setTypeAST(node, TYPE_ARRAY); // The node itself is of TYPE_ARRAY

    // Transfer children (the AST_SUBRANGE nodes with resolved AST_NUMBER bounds)
    if (indexList->child_count > 0) {
        node->children = indexList->children;
        node->child_count = indexList->child_count;
        node->child_capacity = indexList->child_capacity;
        for (int i = 0; i < node->child_count; ++i) {
            if (node->children[i]) {
                node->children[i]->parent = node;
            }
        }
        indexList->children = NULL; // Nullify to prevent double free
        indexList->child_count = 0;
        indexList->child_capacity = 0;
    } else {
        node->children = NULL;
        node->child_count = 0;
        node->child_capacity = 0;
    }
    freeAST(indexList); // Free the temporary AST_COMPOUND wrapper struct (not its children)

    setRight(node, elemType); // Link to the element type AST

    return node;
}

static void adoptRoutineParameters(AST *routine, AST *params) {
    if (!routine || !params) {
        return;
    }

    if (params->type == AST_COMPOUND && params->child_count > 0) {
        routine->children = params->children;
        routine->child_count = params->child_count;
        routine->child_capacity = params->child_capacity;
        for (int i = 0; i < routine->child_count; i++) {
            if (routine->children[i]) {
                routine->children[i]->parent = routine;
            }
        }
        params->children = NULL;
        params->child_count = 0;
        params->child_capacity = 0;
    }

    freeAST(params);
}

static Token *parseQualifiedRoutineName(Parser *parser, const char *missingNameError) {
    if (!parser) {
        return NULL;
    }

    if (!currentTokenIsIdentifierLike(parser)) {
        if (missingNameError) {
            errorParser(parser, missingNameError);
        }
        return NULL;
    }

    Token *qualified = copyToken(parser->current_token);
    if (!qualified) {
        EXIT_FAILURE_HANDLER();
    }

    size_t currentLen = qualified->value ? strlen(qualified->value) : 0;
    TokenType partType = parser->current_token->type;
    eat(parser, partType);

    while (parser->current_token && parser->current_token->type == TOKEN_PERIOD) {
        eat(parser, TOKEN_PERIOD);

        if (!currentTokenIsIdentifierLike(parser)) {
            freeToken(qualified);
            errorParser(parser, "Expected identifier after '.' in routine name");
            return NULL;
        }

        const char *segment = parser->current_token->value ? parser->current_token->value : "";
        size_t segmentLen = strlen(segment);
        size_t newLen = currentLen + 1 + segmentLen;

        char *newValue = realloc(qualified->value, newLen + 1);
        if (!newValue) {
            freeToken(qualified);
            EXIT_FAILURE_HANDLER();
        }

        newValue[currentLen] = '.';
        memcpy(newValue + currentLen + 1, segment, segmentLen + 1);
        qualified->value = newValue;
        qualified->length = newLen;
        currentLen = newLen;

        partType = parser->current_token->type;
        eat(parser, partType);
    }

    return qualified;
}

static AST *parseInterfaceMethod(Parser *parser, bool isFunction) {
    TokenType keyword = isFunction ? TOKEN_FUNCTION : TOKEN_PROCEDURE;
    eat(parser, keyword);

    if (!currentTokenIsIdentifierLike(parser)) {
        errorParser(parser, isFunction ? "Expected function name after FUNCTION" :
                                         "Expected procedure name after PROCEDURE");
        return NULL;
    }

    Token *originalName = parser->current_token;
    Token *copiedName = copyToken(originalName);
    if (!copiedName) {
        EXIT_FAILURE_HANDLER();
    }

    eat(parser, originalName->type);

    AST *routine = newASTNode(isFunction ? AST_FUNCTION_DECL : AST_PROCEDURE_DECL, copiedName);
    routine->is_forward_decl = true;
    freeToken(copiedName);

    AST *params = NULL;
    if (parser->current_token && parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        if (parser->current_token && parser->current_token->type != TOKEN_RPAREN) {
            params = paramList(parser);
            if (!params) {
                freeAST(routine);
                return NULL;
            }
        }

        if (!parser->current_token || parser->current_token->type != TOKEN_RPAREN) {
            errorParser(parser, "Expected ')' after parameter list");
            freeAST(params);
            freeAST(routine);
            return NULL;
        }
        eat(parser, TOKEN_RPAREN);
    }

    adoptRoutineParameters(routine, params);

    if (isFunction) {
        if (!parser->current_token || parser->current_token->type != TOKEN_COLON) {
            errorParser(parser, "Expected ':' before function return type");
            freeAST(routine);
            return NULL;
        }
        eat(parser, TOKEN_COLON);
        AST *returnType = typeSpecifier(parser, 0);
        if (!returnType) {
            freeAST(routine);
            return NULL;
        }
        setRight(routine, returnType);
        routine->var_type = returnType->var_type;
    } else {
        setTypeAST(routine, TYPE_VOID);
    }

    if (!parser->current_token || parser->current_token->type != TOKEN_SEMICOLON) {
        errorParser(parser, "Expected ';' after routine declaration");
        freeAST(routine);
        return NULL;
    }
    eat(parser, TOKEN_SEMICOLON);

    while (parser->current_token) {
        TokenType t = parser->current_token->type;
        if (t == TOKEN_IDENTIFIER) {
            if (parser->current_token->value &&
                strcasecmp(parser->current_token->value, "virtual") == 0) {
                routine->is_virtual = true;
            }
            eat(parser, TOKEN_IDENTIFIER);
        } else if (t == TOKEN_INLINE) {
            routine->is_inline = true;
            eat(parser, TOKEN_INLINE);
        } else {
            break;
        }

        if (parser->current_token && parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
        } else {
            break;
        }
    }

    return routine;
}

static void registerRecordMethodPrototype(Parser *parser, const char *recordName, AST *method) {
    if (!parser || !recordName || !method || !method->token || !method->token->value) {
        return;
    }

    if (!current_procedure_table) {
        return;
    }

    AST *methodCopy = copyAST(method);
    if (!methodCopy) {
        EXIT_FAILURE_HANDLER();
    }

    methodCopy->is_forward_decl = true;

    if (methodCopy->token && methodCopy->token->value) {
        size_t recordLen = strlen(recordName);
        size_t methodLen = strlen(methodCopy->token->value);
        size_t qualifiedLen = recordLen + 1 + methodLen;
        char *qualifiedName = malloc(qualifiedLen + 1);
        if (!qualifiedName) {
            freeAST(methodCopy);
            EXIT_FAILURE_HANDLER();
        }

        memcpy(qualifiedName, recordName, recordLen);
        qualifiedName[recordLen] = '.';
        memcpy(qualifiedName + recordLen + 1, methodCopy->token->value, methodLen + 1);

        free(methodCopy->token->value);
        methodCopy->token->value = qualifiedName;
        methodCopy->token->length = qualifiedLen;
    }

    addProcedure(parser, methodCopy, NULL, current_procedure_table);
    freeAST(methodCopy);
}

static void registerRecordMethods(Parser *parser, const char *recordName, AST *recordType) {
    if (!parser || !recordName || !recordType || recordType->type != AST_RECORD_TYPE) {
        return;
    }

    for (int i = 0; i < recordType->child_count; i++) {
        AST *child = recordType->children[i];
        if (!child) {
            continue;
        }
        if (child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) {
            registerRecordMethodPrototype(parser, recordName, child);
        }
    }
}

static AST *parseInterfaceType(Parser *parser) {
    AST *node = newASTNode(AST_INTERFACE, parser->current_token);
    eat(parser, TOKEN_INTERFACE);
    setTypeAST(node, TYPE_INTERFACE);

    if (parser->current_token && parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        AST *baseList = newASTNode(AST_LIST, NULL);
        while (parser->current_token && parser->current_token->type != TOKEN_RPAREN) {
            AST *baseType = typeSpecifier(parser, 0);
            if (!baseType) {
                freeAST(baseList);
                freeAST(node);
                return NULL;
            }
            addChild(baseList, baseType);
            if (parser->current_token && parser->current_token->type == TOKEN_COMMA) {
                eat(parser, TOKEN_COMMA);
            } else {
                break;
            }
        }
        if (!parser->current_token || parser->current_token->type != TOKEN_RPAREN) {
            errorParser(parser, "Expected ')' after interface ancestor list");
            freeAST(baseList);
            freeAST(node);
            return NULL;
        }
        eat(parser, TOKEN_RPAREN);
        if (baseList->child_count > 0) {
            setExtra(node, baseList);
        } else {
            freeAST(baseList);
        }
    }

    while (parser->current_token && parser->current_token->type == TOKEN_LBRACKET) {
        eat(parser, TOKEN_LBRACKET);
        while (parser->current_token && parser->current_token->type != TOKEN_RBRACKET) {
            AST *attr = expression(parser);
            if (attr) {
                freeAST(attr);
            }
            if (parser->current_token && parser->current_token->type == TOKEN_COMMA) {
                eat(parser, TOKEN_COMMA);
            } else {
                break;
            }
        }
        if (!parser->current_token || parser->current_token->type != TOKEN_RBRACKET) {
            errorParser(parser, "Expected ']' after interface attribute block");
            freeAST(node);
            return NULL;
        }
        eat(parser, TOKEN_RBRACKET);
    }

    while (parser->current_token && parser->current_token->type != TOKEN_END) {
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
            continue;
        }

        if (parser->current_token->type == TOKEN_PROCEDURE ||
            parser->current_token->type == TOKEN_FUNCTION) {
            bool isFunction = parser->current_token->type == TOKEN_FUNCTION;
            AST *method = parseInterfaceMethod(parser, isFunction);
            if (!method) {
                freeAST(node);
                return NULL;
            }
            method->is_virtual = true;
            addChild(node, method);
            continue;
        }

        errorParser(parser, "Expected method declaration in interface type");
        freeAST(node);
        return NULL;
    }

    if (!parser->current_token || parser->current_token->type != TOKEN_END) {
        errorParser(parser, "Expected END to close interface type");
        freeAST(node);
        return NULL;
    }
    eat(parser, TOKEN_END);

    return node;
}
AST *unitParser(Parser *parser_for_this_unit, int recursion_depth, const char* unit_name_being_parsed, BytecodeChunk* chunk) {
    if (recursion_depth > MAX_RECURSION_DEPTH) { /* error */ EXIT_FAILURE_HANDLER(); }

    Token* unit_keyword_token_copy = copyToken(parser_for_this_unit->current_token);
    eat(parser_for_this_unit, TOKEN_UNIT);
    
    Token *unit_name_token_original = parser_for_this_unit->current_token;
    if (!tokenIsIdentifierLike(unit_name_token_original)) { /* error */ }
    
    Token *unit_name_token_copy_for_ast = copyToken(unit_name_token_original);
    AST *unit_node = newASTNode(AST_UNIT, unit_name_token_copy_for_ast);
    freeToken(unit_name_token_copy_for_ast);
    freeToken(unit_keyword_token_copy);

    char *lower_unit_name_ctx = strdup(unit_name_token_original->value);
    toLowerString(lower_unit_name_ctx);
    parser_for_this_unit->current_unit_name_context = lower_unit_name_ctx;

    eat(parser_for_this_unit, TOKEN_IDENTIFIER);
    eat(parser_for_this_unit, TOKEN_SEMICOLON);

    AST *uses_clause = NULL;
    List *unit_list_for_this_unit = NULL;
    if (parser_for_this_unit->current_token && parser_for_this_unit->current_token->type == TOKEN_USES) {
        eat(parser_for_this_unit, TOKEN_USES);
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        unit_list_for_this_unit = createList();
        while (parser_for_this_unit->current_token &&
               parser_for_this_unit->current_token->type == TOKEN_IDENTIFIER) {
            // ... (logic to parse unit names into list) ...
            char* temp_unit_name = strdup(parser_for_this_unit->current_token->value);
            listAppend(unit_list_for_this_unit, temp_unit_name);
            free(temp_unit_name);
            eat(parser_for_this_unit, TOKEN_IDENTIFIER);
            if (parser_for_this_unit->current_token &&
                parser_for_this_unit->current_token->type == TOKEN_COMMA) {
                eat(parser_for_this_unit, TOKEN_COMMA);
            } else if (parser_for_this_unit->current_token &&
                       parser_for_this_unit->current_token->type == TOKEN_SEMICOLON) {
                Token *lookahead = peekToken(parser_for_this_unit);
                bool continue_after_semicolon = lookahead &&
                                                lookahead->type == TOKEN_IDENTIFIER;
                if (lookahead) {
                    freeToken(lookahead);
                }
                if (continue_after_semicolon) {
                    eat(parser_for_this_unit, TOKEN_SEMICOLON);
                    continue;
                }
                break;
            } else {
                break;
            }
        }
        eat(parser_for_this_unit, TOKEN_SEMICOLON);
        uses_clause->unit_list = unit_list_for_this_unit;
        
        for (int i = 0; i < listSize(unit_list_for_this_unit); i++) {
            char *nested_unit_name = listGet(unit_list_for_this_unit, i);

            char lower_nested_name[MAX_SYMBOL_LENGTH];
            strncpy(lower_nested_name, nested_unit_name, MAX_SYMBOL_LENGTH - 1);
            lower_nested_name[MAX_SYMBOL_LENGTH - 1] = '\0';
            for (int k = 0; lower_nested_name[k]; k++) {
                lower_nested_name[k] = tolower((unsigned char)lower_nested_name[k]);
            }

            char *nested_unit_path = findUnitFile(lower_nested_name);
            if (!nested_unit_path) {
                if (!isUnitDocumented(lower_nested_name)) {
                    fprintf(stderr, "Warning: unit '%s' not found. Skipping.\n", nested_unit_name);
                }
                continue; // skip missing unit regardless
            }

            appendDependencyPath(parser_for_this_unit, nested_unit_path);

            char *unit_source_buffer = NULL;
            FILE *nested_file = fopen(nested_unit_path, "r");
            if (nested_file) {
                fseek(nested_file, 0, SEEK_END);
                long nested_fsize = ftell(nested_file);
                rewind(nested_file);
                unit_source_buffer = malloc(nested_fsize + 1);
                if (!unit_source_buffer) { fclose(nested_file); free(nested_unit_path); EXIT_FAILURE_HANDLER(); }
                size_t bytes_read = fread(unit_source_buffer, 1, nested_fsize, nested_file);
                if (bytes_read != (size_t)nested_fsize) {
                    fprintf(stderr, "Error reading unit file '%s'.\n", nested_unit_path);
                    free(unit_source_buffer);
                    fclose(nested_file);
                    free(nested_unit_path);
                    EXIT_FAILURE_HANDLER();
                }
                unit_source_buffer[nested_fsize] = '\0';
                fclose(nested_file);
            } else {
                fprintf(stderr, "Error opening unit file '%s'.\n", nested_unit_path);
                free(nested_unit_path);
                EXIT_FAILURE_HANDLER();
            }
            free(nested_unit_path);

            Lexer nested_lexer;
            initLexer(&nested_lexer, unit_source_buffer);
            Parser nested_parser_instance;
            nested_parser_instance.lexer = &nested_lexer;
            nested_parser_instance.current_token = getNextToken(&nested_lexer);
            nested_parser_instance.dependency_paths = parser_for_this_unit->dependency_paths;
            
            // --- MODIFICATION: Pass the chunk recursively ---
            AST *parsed_nested_unit_ast = unitParser(&nested_parser_instance, recursion_depth + 1, nested_unit_name, chunk);

            if (nested_parser_instance.current_token) freeToken(nested_parser_instance.current_token);
            if (unit_source_buffer) free(unit_source_buffer);

            if (parsed_nested_unit_ast) {
                // Annotate first so the compiler has accurate type info
                annotateTypes(parsed_nested_unit_ast, NULL, parsed_nested_unit_ast);
                // Compile the unit's implementation before linking so routines
                // receive bytecode addresses that aliases can reference.
                compileUnitImplementation(parsed_nested_unit_ast, chunk);
                // Now link the unit which will insert globals and create
                // unqualified aliases to the compiled routines.
                linkUnit(parsed_nested_unit_ast, recursion_depth + 1);
                freeAST(parsed_nested_unit_ast);
            }
        }
    }
    if(uses_clause) addChild(unit_node, uses_clause);

    eat(parser_for_this_unit, TOKEN_INTERFACE);
    AST *interface_decls = declarations(parser_for_this_unit, true);
    setLeft(unit_node, interface_decls);
    
    Symbol *unitSymTable = buildUnitSymbolTable(interface_decls);
    unit_node->symbol_table = unitSymTable;

    eat(parser_for_this_unit, TOKEN_IMPLEMENTATION);
    AST *impl_decls = declarations(parser_for_this_unit, false);
    setExtra(unit_node, impl_decls);
    
    if (parser_for_this_unit->current_token && parser_for_this_unit->current_token->type == TOKEN_BEGIN) {
        AST *init_block = compoundStatement(parser_for_this_unit);
        setRight(unit_node, init_block);
        eat(parser_for_this_unit, TOKEN_PERIOD);
    } else {
        eat(parser_for_this_unit, TOKEN_END);
        eat(parser_for_this_unit, TOKEN_PERIOD);
    }

    parser_for_this_unit->current_unit_name_context = NULL;
    free(lower_unit_name_ctx);
    return unit_node;
}

void errorParser(Parser *parser, const char *msg) {
    fprintf(stderr, "Parser error at line %d, column %d: %s (found token: %s)\n",
            parser->lexer->line, parser->lexer->column, msg,
            tokenTypeToString(parser->current_token->type));
    pascal_parser_error_count++;
    EXIT_FAILURE_HANDLER();
}

void addProcedure(Parser *parser, AST *proc_decl_ast_original, const char* unit_context_name_param_for_addproc, HashTable *proc_table_param) {
    // Create the name for the symbol table. This might involve mangling
    // with unit_context_name_param_for_addproc if it's not NULL.
    // For simplicity, let's assume for now the name is directly from the token,
    // and unit qualification is handled by lookup.
    // You will need to implement proper name construction here.

    char *proc_name_original = proc_decl_ast_original->token->value;

    if (isBuiltin(proc_name_original)) {
        bool suppress_override_warning = false;
        if (parser && parser->lexer) {
            suppress_override_warning = lexerConsumeOverrideBuiltinDirective(parser->lexer, proc_name_original);
        }
        if (!suppress_override_warning) {
        const char* kind = (proc_decl_ast_original->type == AST_FUNCTION_DECL) ?
                           "function" : "procedure";
        fprintf(stderr,
                "Warning: user-defined %s '%s' overrides builtin of the same name.\n",
                kind, proc_name_original);
        }
    }

    char *name_for_table = strdup(proc_name_original); // Start with a copy
    if (!name_for_table) {
        fprintf(stderr, "Memory allocation error for name_for_table in addProcedure\n");
        EXIT_FAILURE_HANDLER();
    }
    // Convert name_for_table to lowercase for consistent lookup
    for (int i = 0; name_for_table[i]; i++) {
        name_for_table[i] = tolower((unsigned char)name_for_table[i]);
    }

    // If unit_context_name_param_for_addproc is provided, prepend it: "unit.proc"
    if (unit_context_name_param_for_addproc && unit_context_name_param_for_addproc[0] != '\0') {
        // Dynamically create "unit.proc"
        size_t unit_len = strlen(unit_context_name_param_for_addproc);
        size_t proc_len = strlen(name_for_table);
        size_t required_size = unit_len + 1 + proc_len + 1;
        char* mangled_name = malloc(required_size); // Allocate exact required size
          
        if (!mangled_name) {
            fprintf(stderr, "Malloc failed for mangled_name in addProcedure\n");
            free(name_for_table);
            EXIT_FAILURE_HANDLER();
        }
        int chars_written = snprintf(mangled_name, required_size, "%s.%s",
                                             unit_context_name_param_for_addproc, name_for_table);

                // Check for snprintf errors or truncation.
                // snprintf returns the number of characters that *would have been written* if enough space
                // had been available, *excluding* the null terminator.
                // So, if chars_written is negative (error) or >= required_size (truncation would have occurred
                // if buffer was smaller, though here required_size should be exact), it's an issue.
        if (chars_written < 0 || (size_t)chars_written >= required_size) {
            fprintf(stderr, "Error or truncation occurred with snprintf in addProcedure for mangled_name. Chars written: %d, Buffer size: %zu\n",
                    chars_written, required_size);
            free(mangled_name); // Free the buffer allocated for mangled_name
            free(name_for_table); // Free the original name_for_table
            EXIT_FAILURE_HANDLER(); // Or handle error more gracefully
        }
        free(name_for_table); // Free the previously strdup'd name
        name_for_table = mangled_name; // Use the new mangled name
    }

    Symbol* existing_sym = proc_table_param ? hashTableLookup(proc_table_param, name_for_table) : NULL;
    if (existing_sym) {
        // An entry from the interface already exists. Update it with the implementation's AST.
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG addProcedure] Routine '%s' already exists. Updating definition.\n", name_for_table);
        #endif

        // Free the old, incomplete AST from the interface declaration.
        if (existing_sym->type_def) {
            freeAST(existing_sym->type_def);
        }

        // Replace it with a deep copy of the new, complete AST from the implementation.
        existing_sym->type_def = copyAST(proc_decl_ast_original);

        // Also update the type in case it's a function with a forward declaration.
        if (proc_decl_ast_original->type == AST_FUNCTION_DECL) {
            existing_sym->type = proc_decl_ast_original->var_type;
        }

        // Ensure arity matches the declaration's parameter count
        existing_sym->arity = proc_decl_ast_original->child_count;
        existing_sym->is_inline = proc_decl_ast_original->is_inline;
        existing_sym->is_defined = !proc_decl_ast_original->is_forward_decl;

        // The update is complete. Free the constructed name and return.
        free(name_for_table);
        return;
    }


    // THIS IS THE CRITICAL CHANGE - ALLOCATE A Symbol, NOT A Procedure
    Symbol *sym = (Symbol *)malloc(sizeof(Symbol)); // <<< THIS SHOULD BE sizeof(Symbol)
    if (!sym) {
        fprintf(stderr, "Memory allocation error in addProcedure for Symbol struct\n");
        free(name_for_table);
        EXIT_FAILURE_HANDLER();
    }

    sym->name = name_for_table; // name_for_table is already a strdup'd and potentially mangled copy

    // Store a DEEP COPY of the AST declaration node
    sym->type_def = copyAST(proc_decl_ast_original);
    if (!sym->type_def && proc_decl_ast_original) { // If copyAST failed but original AST existed
        fprintf(stderr, "Critical Error: copyAST failed for procedure '%s'. Heap corruption likely.\n", sym->name);
        if (sym->name) free(sym->name);
        free(sym);
        EXIT_FAILURE_HANDLER();
    }

    // Ensure parameter declarations in the copied AST retain full array type information
    if (sym->type_def && proc_decl_ast_original) {
        int param_count = sym->type_def->child_count;
        if (proc_decl_ast_original->child_count < param_count) {
            param_count = proc_decl_ast_original->child_count;
        }
        for (int i = 0; i < param_count; i++) {
            AST *orig_param = proc_decl_ast_original->children[i];
            AST *copied_param = sym->type_def->children[i];
            if (!orig_param || !copied_param) continue;
            if (orig_param->var_type == TYPE_ARRAY) {
                copied_param->var_type = TYPE_ARRAY;
                if (!copied_param->right && orig_param->right) {
                    copied_param->right = copyAST(orig_param->right);
                    if (copied_param->right) {
                        copied_param->right->parent = copied_param;
                    }
                }
                copied_param->type_def = copied_param->right;
            }
        }
    }

    // Perform a local type annotation pass on the copied AST so that
    // downstream compiler stages can rely on accurate var_type
    // information.  This is especially important for nested routines
    // that capture variables from outer scopes (upvalues).
    annotateTypes(sym->type_def, NULL, sym->type_def);

    // Determine the symbol's type based on the original AST declaration node's var_type
    if (proc_decl_ast_original->type == AST_FUNCTION_DECL) {
        // For functions, 'proc_decl_ast_original->var_type' should have been set
        // in registerBuiltinFunction to the function's actual return type.
        if (proc_decl_ast_original->var_type != TYPE_VOID) {
            sym->type = proc_decl_ast_original->var_type;
        } else {
            // This case means registerBuiltinFunction set dummy->var_type to VOID for an AST_FUNCTION_DECL,
            // or it was corrupted. This implies an issue in registerBuiltinFunction's setup or heap corruption.
            fprintf(stderr, "Warning: Function '%s' (AST type: %s) has an effective VOID return type based on its declaration's var_type. Check registerBuiltinFunction setup.\n",
                    sym->name, astTypeToString(proc_decl_ast_original->type));
            sym->type = TYPE_VOID; // Fallback, but this indicates a setup problem.
        }
    } else { // AST_PROCEDURE_DECL
        sym->type = TYPE_VOID;
    }

    sym->value = NULL; // Procedures/functions don't have a 'value' in the same way variables do
    sym->is_const = false;
    sym->is_alias = false;
    sym->is_local_var = false;
    sym->is_inline = proc_decl_ast_original->is_inline;
    sym->closure_captures = false;
    sym->closure_escapes = false;
    sym->next = NULL;
    sym->enclosing = NULL;
    sym->is_defined = !proc_decl_ast_original->is_forward_decl; // Only mark defined when a body is present
    sym->bytecode_address = -1; // -1 can indicate no address assigned yet.
    sym->arity = proc_decl_ast_original->child_count; // Store parameter count for builtins and declarations
    sym->locals_count = 0;      // Will be updated later.

    if (proc_table_param) {
        hashTableInsert(proc_table_param, sym);
    } else {
        fprintf(stderr, "CRITICAL Error: procedure table parameter is NULL before addProcedure call.\n");
        if (sym->name) free(sym->name);
        if (sym->type_def) freeAST(sym->type_def);
        free(sym);
        EXIT_FAILURE_HANDLER();
    }
    
    // Optional Debug Print
    #ifdef DEBUG
    if (dumpExec) {
        fprintf(stderr, "[DEBUG parser.c addProcedure] Added routine '%s' to procedure table %p. Copied AST node at %p. Symbol type: %s\n",
                sym->name, (void*)proc_table_param, (void*)sym->type_def, varTypeToString(sym->type));
    }
    #endif
}


// In emkey1/pscal/pscal-working/src/parser.c
// Make sure DEBUG_PRINT is defined, e.g., in utils.h or globals.h:
// #ifdef DEBUG
// #define DEBUG_PRINT(fmt, ...) fprintf(stdout, "[DEBUG %s:%d] " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
// #else
// #define DEBUG_PRINT(fmt, ...)
// #endif

AST *buildProgramAST(Parser *main_parser, BytecodeChunk* chunk) {
    main_parser->current_unit_name_context = NULL;
    resetCompilerConstants();
    Token *copiedProgToken = copyToken(main_parser->current_token);
    if (!copiedProgToken && main_parser->current_token) { /* Malloc error check */ EXIT_FAILURE_HANDLER(); }
    DEBUG_PRINT("buildProgramAST: About to eat PROGRAM. Current: %s ('%s')\n", main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE", main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
    eat(main_parser, TOKEN_PROGRAM);

    Token *progNameCopied = copyToken(main_parser->current_token);
    if (!progNameCopied && main_parser->current_token) { /* Malloc error check */ freeToken(copiedProgToken); EXIT_FAILURE_HANDLER(); }
    DEBUG_PRINT("buildProgramAST: About to eat IDENTIFIER (prog name). Current: %s ('%s')\n", main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE", main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
    eat(main_parser, TOKEN_IDENTIFIER);

    AST *prog_name_node = newASTNode(AST_VARIABLE, progNameCopied);
    freeToken(progNameCopied); // newASTNode makes its own copy

    if (main_parser->current_token && main_parser->current_token->type == TOKEN_LPAREN) {
        DEBUG_PRINT("buildProgramAST: About to eat LPAREN after program name. Current: %s ('%s')\n",
                    main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                    main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
        eat(main_parser, TOKEN_LPAREN);

        while (currentTokenIsIdentifierLike(main_parser)) {
            DEBUG_PRINT("buildProgramAST: About to eat IDENTIFIER in program file list. Current: %s ('%s')\n",
                        main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                        main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
            eat(main_parser, TOKEN_IDENTIFIER);
            if (main_parser->current_token && main_parser->current_token->type == TOKEN_COMMA) {
                DEBUG_PRINT("buildProgramAST: About to eat COMMA in program file list. Current: %s ('%s')\n",
                            main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                            main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
                eat(main_parser, TOKEN_COMMA);
            } else {
                break;
            }
        }

        DEBUG_PRINT("buildProgramAST: About to eat RPAREN after program file list. Current: %s ('%s')\n",
                    main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                    main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
        eat(main_parser, TOKEN_RPAREN);
    }

    DEBUG_PRINT("buildProgramAST: About to eat SEMICOLON (after prog name). Current: %s ('%s')\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
    eat(main_parser, TOKEN_SEMICOLON);

    AST *uses_clause = NULL;
    List *unit_list = NULL;

    if (main_parser->current_token && main_parser->current_token->type == TOKEN_USES) {
        eat(main_parser, TOKEN_USES);
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        unit_list = createList();

        while (main_parser->current_token &&
               main_parser->current_token->type == TOKEN_IDENTIFIER) {
            char* temp_unit_name_original_case = strdup(main_parser->current_token->value);
            if (!temp_unit_name_original_case) { /* Malloc error */ }

            listAppend(unit_list, temp_unit_name_original_case);
            free(temp_unit_name_original_case);

            eat(main_parser, TOKEN_IDENTIFIER);
            if (main_parser->current_token &&
                main_parser->current_token->type == TOKEN_COMMA) {
                eat(main_parser, TOKEN_COMMA);
            } else if (main_parser->current_token &&
                       main_parser->current_token->type == TOKEN_SEMICOLON) {
                Token *lookahead = peekToken(main_parser);
                bool continue_after_semicolon = lookahead &&
                                                lookahead->type == TOKEN_IDENTIFIER;
                if (lookahead) {
                    freeToken(lookahead);
                }
                if (continue_after_semicolon) {
                    eat(main_parser, TOKEN_SEMICOLON);
                    continue;
                }
                break;
            } else {
                break;
            }
        }
        eat(main_parser, TOKEN_SEMICOLON);

        if (uses_clause) {
            uses_clause->unit_list = unit_list;
        }

        if (unit_list) {
            for (int i = 0; i < listSize(unit_list); i++) {
                char *used_unit_name_str_from_list = listGet(unit_list, i);
                
                char lower_used_unit_name[MAX_SYMBOL_LENGTH];
                strncpy(lower_used_unit_name, used_unit_name_str_from_list, MAX_SYMBOL_LENGTH - 1);
                lower_used_unit_name[MAX_SYMBOL_LENGTH - 1] = '\0';
                for(int k=0; lower_used_unit_name[k]; k++) {
                    lower_used_unit_name[k] = tolower((unsigned char)lower_used_unit_name[k]);
                }

                char *unit_file_path = findUnitFile(lower_used_unit_name);
                if (!unit_file_path) {
                    if (!isUnitDocumented(lower_used_unit_name)) {
                        fprintf(stderr, "Warning: unit '%s' not found. Skipping.\n", used_unit_name_str_from_list);
                    }
                    continue; // skip missing unit regardless
                }

                appendDependencyPath(main_parser, unit_file_path);

                char* unit_source_buffer = NULL;
                FILE *unit_file = fopen(unit_file_path, "r");
                if(unit_file) {
                    fseek(unit_file, 0, SEEK_END);
                    long fsize = ftell(unit_file);
                    rewind(unit_file);
                    unit_source_buffer = malloc(fsize + 1);
                    if (!unit_source_buffer) { fclose(unit_file); free(unit_file_path); EXIT_FAILURE_HANDLER(); }
                    size_t bytes_read = fread(unit_source_buffer, 1, fsize, unit_file);
                    if (bytes_read != (size_t)fsize) {
                        fprintf(stderr, "Error reading unit file '%s'.\n", unit_file_path);
                        free(unit_source_buffer);
                        fclose(unit_file);
                        free(unit_file_path);
                        EXIT_FAILURE_HANDLER();
                    }
                    unit_source_buffer[fsize] = '\0';
                    fclose(unit_file);
                } else {
                    fprintf(stderr, "Error opening unit file '%s'.\n", unit_file_path);
                    free(unit_file_path);
                    EXIT_FAILURE_HANDLER();
                }
                free(unit_file_path);

                Lexer nested_lexer;
                initLexer(&nested_lexer, unit_source_buffer);
                Parser nested_parser_instance;
                nested_parser_instance.lexer = &nested_lexer;
                nested_parser_instance.current_token = getNextToken(&nested_lexer);
                nested_parser_instance.dependency_paths = main_parser->dependency_paths;

                // --- MODIFICATION: Pass the chunk recursively ---
                AST *parsed_unit_ast = unitParser(&nested_parser_instance, 1, lower_used_unit_name, chunk);
                
                if (nested_parser_instance.current_token) freeToken(nested_parser_instance.current_token);
                if (unit_source_buffer) free(unit_source_buffer);

                if (parsed_unit_ast) {
                    // Perform type annotation so the compiler knows symbol types
                    annotateTypes(parsed_unit_ast, NULL, parsed_unit_ast);
                    // Compile the unit's implementation to assign bytecode addresses
                    // to its routines before we create aliases via linkUnit.
                    compileUnitImplementation(parsed_unit_ast, chunk);
                    // Link afterwards to pull in globals and set up aliases.
                    linkUnit(parsed_unit_ast, 1);
                    // The temporary AST is no longer needed once linked.
                    freeAST(parsed_unit_ast);
                }
            }
        }
    }

    AST *block_node = block(main_parser);

    AST *programNode = newASTNode(AST_PROGRAM, copiedProgToken);
    if (!programNode) { /* Malloc error, cleanup... */ }

    setLeft(programNode, prog_name_node);
    setRight(programNode, block_node);
    if(uses_clause) {
        addChild(programNode, uses_clause);
    }
    freeToken(copiedProgToken);

    return programNode;
}

AST *block(Parser *parser) {
    AST *decl = declarations(parser, false); // Pass parser
    AST *comp_stmt = compoundStatement(parser); // Pass parser
    AST *node = newASTNode(AST_BLOCK, NULL);
    addChild(node, decl);
    addChild(node, comp_stmt);
    // node->is_global_scope is set by the caller (buildProgramAST or proc/func declaration)
    return node;
}

static bool tokenTerminatesStatement(TokenType type) {
    switch (type) {
        case TOKEN_SEMICOLON:
        case TOKEN_END:
        case TOKEN_ELSE:
        case TOKEN_UNTIL:
        case TOKEN_EOF:
        case TOKEN_PERIOD:
            return true;
        default:
            return false;
    }
}

AST *procedureDeclaration(Parser *parser, bool in_interface) {
    eat(parser, TOKEN_PROCEDURE);
    Token *copiedProcNameToken = parseQualifiedRoutineName(parser, "Expected procedure name after PROCEDURE");
    if (!copiedProcNameToken) {
        return newASTNode(AST_NOOP, NULL);
    }
    AST *node = newASTNode(AST_PROCEDURE_DECL, copiedProcNameToken);
    node->is_forward_decl = in_interface;
    // freeToken(copiedProcNameToken); // Already handled if newASTNode copies

    // *** ADD DIAGNOSTIC PRINT HERE ***
    #ifdef DEBUG
    if (parser->current_token) {
        fprintf(stderr, "[DEBUG PROC_DECL_ENTRY] After eating proc name '%s', current_token is: Type=%s ('%s'), Value='%s' at Line %d, Col %d\n",
                node->token->value, // Name of the procedure we just parsed
                tokenTypeToString(parser->current_token->type),
                parser->current_token->type == TOKEN_LPAREN ? "LPAREN" : "NOT LPAREN",
                parser->current_token->value ? parser->current_token->value : "NULL",
                parser->lexer->line,
                parser->lexer->column);
    } else {
        fprintf(stderr, "[DEBUG PROC_DECL_ENTRY] After eating proc name '%s', current_token is NULL\n", node->token->value);
    }
    #endif
    // *** END DIAGNOSTIC PRINT ***

    AST *params = NULL;
    // This condition now becomes critical to observe with the diagnostic print above
    if (parser->current_token && parser->current_token->type == TOKEN_LPAREN) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG PROC_DECL_PARAMS] Detected LPAREN, entering parameter parsing for '%s'.\n", node->token->value);
        #endif
        eat(parser, TOKEN_LPAREN);
        if (parser->current_token->type != TOKEN_RPAREN) {
            params = paramList(parser);
        }
        if (parser->current_token && parser->current_token->type == TOKEN_RPAREN) { // Check token existence
             eat(parser, TOKEN_RPAREN);
        } else {
             char err_msg[128];
             snprintf(err_msg, sizeof(err_msg), "Expected ')' to close parameter list for procedure '%s', got %s", node->token->value, parser->current_token ? tokenTypeToString(parser->current_token->type) : "EOF");
             errorParser(parser, err_msg);
             // If errorParser() could return (e.g., if EXIT_FAILURE_HANDLER is suppressed),
             // the following cleanup would be executed.
             if(params) { // Conditionally free params
                 freeAST(params);
             }
             freeAST(node); // Always free node in this error path
             return NULL;  // Always return NULL in this error path
        }
    } else {
        #ifdef DEBUG
        // This block will execute if the LPAREN for parameters was not found or type was wrong
        fprintf(stderr, "[DEBUG PROC_DECL_PARAMS] No LPAREN detected after proc name '%s', skipping parameter parsing. Current token type: %s\n",
                node->token->value,
                parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN");
        #endif
    }

    // ... (Attach params to node) ...
    if (params) {
        if (params->child_count > 0) {
            node->children = params->children;
            node->child_count = params->child_count;
            node->child_capacity = params->child_capacity;
            for(int i=0; i < node->child_count; i++) {
                if(node->children[i]) node->children[i]->parent = node;
            }
            params->children = NULL;
            params->child_count = 0;
        }
        freeAST(params);
    }

    HashTable *outer_table = current_procedure_table;
    HashTable *my_table = NULL;

#ifdef DEBUG
    fprintf(stderr, "[DEBUG PROC_DECL_BODY] Expecting SEMICOLON after header for '%s'. Current token: Type=%s, Value='%s'\n",
            node->token->value,
            parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
            parser->current_token && parser->current_token->value ? parser->current_token->value : "NULL");
#endif
    eat(parser, TOKEN_SEMICOLON);

    if (parser->current_token && parser->current_token->type == TOKEN_INLINE) {
        node->is_inline = true;
        eat(parser, TOKEN_INLINE);
        eat(parser, TOKEN_SEMICOLON);
    }

    if (parser->current_token && parser->current_token->type == TOKEN_FORWARD) {
        node->is_forward_decl = true;
        eat(parser, TOKEN_FORWARD);
        if (!parser->current_token || parser->current_token->type != TOKEN_SEMICOLON) {
            errorParser(parser, "Expected ';' after FORWARD directive");
        }
        eat(parser, TOKEN_SEMICOLON);
    }

    if (!node->is_forward_decl) {
        my_table = pushProcedureTable();
        AST *local_declarations = declarations(parser, false);
        AST *compound_body = compoundStatement(parser);
        AST *blockNode = newASTNode(AST_BLOCK, NULL);
        addChild(blockNode, local_declarations);
        addChild(blockNode, compound_body);
        blockNode->is_global_scope = false;
        setRight(node, blockNode);
        node->symbol_table = (Symbol*)my_table;
        popProcedureTable(false);
    }
    addProcedure(parser, node, parser->current_unit_name_context, outer_table);
    if (copiedProcNameToken)
        freeToken(copiedProcNameToken);

    return node;
}

// constDeclaration: Calls expression or parseArrayInitializer
// In src/Pascal/parser.c

// src/Pascal/parser.c

AST *constDeclaration(Parser *parser) {
#ifdef DEBUG
    if (parser && parser->current_token) {
        fprintf(stderr, "[DEBUG %s] ENTER. Current token: %s ('%s') at L%d C%d\n", __func__,
                tokenTypeToString(parser->current_token->type),
                parser->current_token->value ? parser->current_token->value : "NULL_VAL",
                parser->current_token->line, parser->current_token->column);
    } else {
        fprintf(stderr, "[DEBUG %s] ENTER. Parser or current_token is NULL.\n", __func__);
    }
#endif

    if (!parser || !parser->current_token) {
        return NULL;
    }

    Token *cn = copyToken(parser->current_token); // Keep this copy of the const name token
    if (!cn) {
        errorParser(parser, "Failed to copy constant name token");
        return NULL;
    }
    eat(parser, TOKEN_IDENTIFIER); // Use the MACRO 'eat'

    AST *type_node = NULL;
    if (parser->current_token && parser->current_token->type == TOKEN_COLON) {
        eat(parser, TOKEN_COLON);
        type_node = typeSpecifier(parser, 0);
        if (!type_node || type_node->type == AST_NOOP) {
            errorParser(parser, "Invalid type specification for constant");
            freeToken(cn);
            if (type_node) freeAST(type_node);
            return NULL;
        }
    }

    if (!parser->current_token || parser->current_token->type != TOKEN_EQUAL) {
        errorParser(parser, "Expected '=' after constant name");
        freeToken(cn); // Free 'cn' before returning on error
        if (type_node) freeAST(type_node);
        return NULL;
    }
    eat(parser, TOKEN_EQUAL); // Use the MACRO 'eat'

    AST *val_node = NULL;
    if (parser->current_token && parser->current_token->type == TOKEN_LPAREN) {
        val_node = parseArrayInitializer(parser);
    } else {
        val_node = expression(parser);
    }
    if (!val_node || val_node->type == AST_NOOP) {
        errorParser(parser, "Invalid constant value expression");
        freeToken(cn);
        if (type_node) freeAST(type_node);
        if (val_node) freeAST(val_node);
        return NULL;
    }

    Value const_eval_result = evaluateCompileTimeValue(val_node);

    // BUG WAS HERE: Do NOT freeToken(cn) here. It is used below.

    AST *node = newASTNode(AST_CONST_DECL, cn); // newASTNode makes its own deep copy of 'cn' for node->token
    if (!node) {
        errorParser(parser, "Failed to create AST node for constant declaration");
        freeValue(&const_eval_result);
        freeAST(val_node);
        freeToken(cn); // Free 'cn' before returning on error
        if (type_node) freeAST(type_node);
        return NULL;
    }
    setLeft(node, val_node);
    if (type_node) {
        setRight(node, type_node);
        if (type_node->var_type != TYPE_UNKNOWN && type_node->var_type != TYPE_VOID) {
            setTypeAST(node, type_node->var_type);
        }
    }

    if (const_eval_result.type != TYPE_VOID && const_eval_result.type != TYPE_UNKNOWN) {
        // Use cn->value and cn->line, as 'cn' is still valid.
        addCompilerConstant(cn->value, &const_eval_result, cn->line);
#ifdef DEBUG
        Value* check_val = findCompilerConstant(cn->value);
        if (check_val) {
            fprintf(stderr, "[DEBUG PARSER constDecl] VERIFY ADD: Found '%s' immediately. Type: %s\n", cn->value, varTypeToString(check_val->type));
        } else {
            fprintf(stderr, "[DEBUG PARSER constDecl] VERIFY ADD: FAILED to find '%s' immediately after add!\n", cn->value);
        }
#endif
        if (!type_node) {
            setTypeAST(node, const_eval_result.type);
        }
    } else if (!type_node) {
#ifdef DEBUG
        // Use cn->value and cn->line, as 'cn' is still valid.
        fprintf(stderr, "[DEBUG %s] Parser Info: Constant '%s' value is non-literal or could not be folded by parser at line %d.\n",
                __func__, cn->value, cn->line);
#endif
        if (val_node->var_type != TYPE_UNKNOWN && val_node->var_type != TYPE_VOID) {
            setTypeAST(node, val_node->var_type);
        }
    }

    freeValue(&const_eval_result);

    // <<<< FIX: Move freeToken(cn) here, to the end of the function >>>>
    // Now that newASTNode has made its copy for node->token, and addCompilerConstant/logging
    // have used the original 'cn' and its members, we can free the 'cn' copy.
    freeToken(cn);
    cn = NULL; // Defensive: prevent accidental use of dangling cn later in this function

    if (!parser->current_token || parser->current_token->type != TOKEN_SEMICOLON) {
        errorParser(parser, "Expected ';' after constant declaration");
        freeAST(node); // This will free node->token (the copy of cn) and node->left (val_node)
        // 'cn' was already freed or nulled out.
        return NULL;
    }
    eat(parser, TOKEN_SEMICOLON);

#ifdef DEBUG
    if (node && node->token) {
        fprintf(stderr, "[DEBUG %s] EXIT. Created AST_CONST_DECL for '%s'\n", __func__,
                node->token->value ? node->token->value : "NULL_VAL");
    } else {
        fprintf(stderr, "[DEBUG %s] EXIT. Node or node->token is NULL for AST_CONST_DECL.\n", __func__);
    }
#endif
    return node;
}

// typeSpecifier: Calls expression for string length
// Replace the existing typeSpecifier function with this one

AST *typeSpecifier(Parser *parser, int allowAnonymous) {
    AST *node = NULL;
    // *** Store the token TYPE and the initial token pointer at the start ***
    TokenType initialTokenType = parser->current_token ? parser->current_token->type : TOKEN_UNKNOWN;
    Token *initialToken = parser->current_token; // Store for potential use if needed (e.g., for RECORD, basic types)

    if (initialTokenType == TOKEN_UNKNOWN) {
        errorParser(parser, "Unexpected end of input in typeSpecifier");
        return NULL;
    }

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG typeSpecifier] Entry: Token Type=%s, Value='%s'\n",
            tokenTypeToString(initialTokenType), initialToken->value ? initialToken->value : "NULL");
    #endif

    // --- Check based on the initial token's type ---
    if (initialTokenType == TOKEN_CARET) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG typeSpecifier] Detected CARET (^), parsing pointer type...\n");
        #endif
        node = parsePointerType(parser); // This consumes ^ and the base type identifier
        if (!node) {
            return NULL; // Error already reported by parsePointerType
        }
        // *** Pointer type successfully parsed, RETURN IMMEDIATELY ***
        return node;

    } else if (initialTokenType == TOKEN_POINTER) {
        AST *pointerNode = newASTNode(AST_POINTER_TYPE, initialToken);
        eat(parser, TOKEN_POINTER);
        setTypeAST(pointerNode, TYPE_POINTER);
        return pointerNode;

    } else if (initialTokenType == TOKEN_RECORD) {
        // Use the initialToken captured at the start
        node = newASTNode(AST_RECORD_TYPE, initialToken);
        eat(parser, TOKEN_RECORD); // Consume the RECORD keyword itself

        while (parser->current_token && parser->current_token->type != TOKEN_END) {
            if (parser->current_token->type == TOKEN_SEMICOLON) {
                eat(parser, TOKEN_SEMICOLON);
                continue;
            }

            if (parser->current_token->type == TOKEN_PROCEDURE ||
                parser->current_token->type == TOKEN_FUNCTION) {
                bool isFunction = parser->current_token->type == TOKEN_FUNCTION;
                AST *method = parseInterfaceMethod(parser, isFunction);
                if (!method) {
                    freeAST(node);
                    return NULL;
                }
                addChild(node, method);
                continue;
            }

            if (currentTokenIsIdentifierLike(parser)) {
                AST *fieldDecl = newASTNode(AST_VAR_DECL, NULL);
                while (1) {
                    if (!currentTokenIsIdentifierLike(parser)) {
                        errorParser(parser, "Expected field identifier");
                        freeAST(fieldDecl);
                        freeAST(node);
                        return NULL;
                    }
                    AST *varNode = newASTNode(AST_VARIABLE, parser->current_token);
                    eat(parser, parser->current_token->type);
                    addChild(fieldDecl, varNode);
                    if (parser->current_token && parser->current_token->type == TOKEN_COMMA) {
                        eat(parser, TOKEN_COMMA);
                    } else {
                        break;
                    }
                }
                if (!parser->current_token || parser->current_token->type != TOKEN_COLON) {
                    errorParser(parser, "Expected :");
                    freeAST(fieldDecl);
                    freeAST(node);
                    return NULL;
                }
                eat(parser, TOKEN_COLON);
                AST *fieldType = typeSpecifier(parser, 1);
                if (!fieldType || fieldType->type == AST_NOOP) {
                    errorParser(parser, "Bad field type");
                    freeAST(fieldDecl);
                    freeAST(node);
                    return NULL;
                }
                setTypeAST(fieldDecl, fieldType->var_type);
                setRight(fieldDecl, fieldType);
                addChild(node, fieldDecl);
                if (parser->current_token && parser->current_token->type == TOKEN_SEMICOLON) {
                    eat(parser, TOKEN_SEMICOLON);
                }
                continue;
            }

            errorParser(parser, "Expected field or method declaration in record");
            freeAST(node);
            return NULL;
        }

        if (!parser->current_token || parser->current_token->type != TOKEN_END) {
            errorParser(parser, "Expected END for record");
            freeAST(node);
            return NULL;
        }
        eat(parser, TOKEN_END);
        setTypeAST(node, TYPE_RECORD);
        // Flow continues to the end, return node

    } else if (initialTokenType == TOKEN_INTERFACE) {
        node = parseInterfaceType(parser);
        if (!node) {
            return NULL;
        }
        setTypeAST(node, TYPE_INTERFACE);

    } else if (initialTokenType == TOKEN_ARRAY) {
        // ARRAY logic (parseArrayType consumes tokens)
        node = parseArrayType(parser);
        if(node) setTypeAST(node, TYPE_ARRAY);
        // Flow continues to the end, return node

    } else if (initialTokenType == TOKEN_SET) {
        // SET logic (eats SET, OF, base type)
         eat(parser, TOKEN_SET);
         if (!parser->current_token || parser->current_token->type != TOKEN_OF) { errorParser(parser, "Expected 'of' after 'set'"); return NULL; }
         eat(parser, TOKEN_OF);
         AST *baseTypeNode = typeSpecifier(parser, 1);
         if (!baseTypeNode || baseTypeNode->type == AST_NOOP) { errorParser(parser, "Invalid base type specified for set"); return NULL; }
         VarType baseVarType = baseTypeNode->var_type;
         bool isOrdinal = (baseVarType == TYPE_INTEGER || baseVarType == TYPE_CHAR || baseVarType == TYPE_BOOLEAN || baseVarType == TYPE_ENUM || baseVarType == TYPE_BYTE || baseVarType == TYPE_WORD);
         if (!isOrdinal) { errorParser(parser, "Set base type must be an ordinal type"); freeAST(baseTypeNode); return NULL; }
         node = newASTNode(AST_ARRAY_TYPE, NULL); // Reusing ARRAY_TYPE structure
         setTypeAST(node, TYPE_SET);
         setRight(node, baseTypeNode);
        // Flow continues to the end, return node

    } else if (initialTokenType == TOKEN_FUNCTION || initialTokenType == TOKEN_PROCEDURE) {
        // Procedure/function pointer type: function '(' [named params] ')' ':' returnType | procedure '(' [named params] ')'
        int isFunction = (initialTokenType == TOKEN_FUNCTION);
        Token* kwTok = copyToken(initialToken);
        eat(parser, initialTokenType);

        // Parse optional parameter types list
        AST* paramsList = NULL;
        if (parser->current_token && parser->current_token->type == TOKEN_LPAREN) {
            eat(parser, TOKEN_LPAREN);
            paramsList = newASTNode(AST_LIST, NULL);
            while (parser->current_token && parser->current_token->type != TOKEN_RPAREN) {
                bool byRef = false;
                bool isConst = false;

                while (parser->current_token) {
                    bool isVarModifier = parser->current_token->type == TOKEN_VAR;
                    bool isConstModifier = parser->current_token->type == TOKEN_CONST;
                    bool isOutModifier = parser->current_token->type == TOKEN_IDENTIFIER &&
                                         tokenMatchesKeyword(parser->current_token, "out");
                    if (!(isVarModifier || isConstModifier || isOutModifier)) {
                        break;
                    }

                    if (isVarModifier || isOutModifier) {
                        byRef = true;
                    }
                    if (isConstModifier) {
                        isConst = true;
                    }
                    eat(parser, parser->current_token->type);
                }

                AST* paramDecl = newASTNode(AST_VAR_DECL, NULL);
                if (!paramDecl) {
                    if (kwTok) freeToken(kwTok);
                    freeAST(paramsList);
                    return NULL;
                }

                if (tokenIsIdentifierLike(parser->current_token)) {
                    Token* nextTok = peekToken(parser);
                    bool hasNameThenColon = (nextTok && nextTok->type == TOKEN_COLON);
                    if (nextTok) freeToken(nextTok);
                    if (hasNameThenColon) {
                        AST* nameNode = newASTNode(AST_VARIABLE, parser->current_token);
                        if (!nameNode) {
                            freeAST(paramDecl);
                            if (kwTok) freeToken(kwTok);
                            freeAST(paramsList);
                            return NULL;
                        }
                        eat(parser, parser->current_token->type);
                        addChild(paramDecl, nameNode);
                        if (!parser->current_token || parser->current_token->type != TOKEN_COLON) {
                            errorParser(parser, "Expected ':' after parameter name");
                            freeAST(paramsList);
                            if (kwTok) freeToken(kwTok);
                            freeAST(paramDecl);
                            return NULL;
                        }
                        eat(parser, TOKEN_COLON);
                    }
                }

                AST* paramType = typeSpecifier(parser, 1);
                if (!paramType) {
                    freeAST(paramDecl);
                    freeAST(paramsList);
                    if (kwTok) freeToken(kwTok);
                    return NULL;
                }

                setRight(paramDecl, paramType);
                paramDecl->type_def = paramType;
                setTypeAST(paramDecl, paramType->var_type);
                paramDecl->by_ref = byRef ? 1 : 0;

                (void)isConst;

                addChild(paramsList, paramDecl);

                if (!parser->current_token) {
                    errorParser(parser, "Expected ')' to close parameter type list");
                    if (kwTok) freeToken(kwTok);
                    freeAST(paramsList);
                    return NULL;
                }

                if (parser->current_token->type == TOKEN_COMMA ||
                    parser->current_token->type == TOKEN_SEMICOLON) {
                    eat(parser, parser->current_token->type);
                    continue;
                }

                if (parser->current_token->type == TOKEN_RPAREN) {
                    break;
                }

                errorParser(parser, "Expected ',', ';', or ')' after parameter type");
                if (kwTok) freeToken(kwTok);
                freeAST(paramsList);
                return NULL;
            }
            if (!parser->current_token || parser->current_token->type != TOKEN_RPAREN) {
                errorParser(parser, "Expected ')' to close parameter type list");
                if (kwTok) freeToken(kwTok);
                freeAST(paramsList);
                return NULL;
            }
            eat(parser, TOKEN_RPAREN);
        }

        AST* retType = NULL;
        if (isFunction) {
            if (!parser->current_token || parser->current_token->type != TOKEN_COLON) {
                errorParser(parser, "Expected ':' and return type for function type");
                if (kwTok) {
                    freeToken(kwTok);
                }
                freeAST(paramsList);
                return NULL;
            }
            eat(parser, TOKEN_COLON);
            retType = typeSpecifier(parser, 0);
            if (!retType) { if (kwTok) freeToken(kwTok); freeAST(paramsList); return NULL; }
        }

        AST* procType = newASTNode(AST_PROC_PTR_TYPE, kwTok);
        if (kwTok) freeToken(kwTok);
        if (paramsList) addChild(procType, paramsList);
        if (retType) setRight(procType, retType);
        setTypeAST(procType, TYPE_POINTER);
        return procType;

    } else if (tokenTypeIsIdentifierLike(initialTokenType)) {
        // IDENTIFIER logic for basic types, user types, string[N]
        // Use initialToken for newASTNode calls
        char *typeName = initialToken->value; // Use value from initialToken
        char *typeNameCopy = strdup(typeName);
        if (!typeNameCopy) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }

        if (strcasecmp(typeNameCopy, "string") == 0) {
            node = newASTNode(AST_VARIABLE, initialToken); // Use initialToken
            setTypeAST(node, TYPE_STRING);
            eat(parser, TOKEN_IDENTIFIER); // Consume 'string'
            // Parse optional fixed length
            if (parser->current_token && parser->current_token->type == TOKEN_LBRACKET) {
                 eat(parser, TOKEN_LBRACKET);
                 AST *lengthNode = expression(parser);
                 if(!lengthNode || lengthNode->type == AST_NOOP) { errorParser(parser,"Bad string len expression"); free(typeNameCopy); freeAST(node); return NULL;}
                 if (!parser->current_token || parser->current_token->type != TOKEN_RBRACKET) { errorParser(parser,"Expected ] after string length"); free(typeNameCopy); freeAST(node); freeAST(lengthNode); return NULL;}
                 eat(parser, TOKEN_RBRACKET);
                 setRight(node, lengthNode);
            }
        } else {
            VarType basicType = TYPE_VOID;
            // ... (checks for integer, real, char, etc.) ...
             if (strcasecmp(typeNameCopy, "integer") == 0) basicType = TYPE_INT32;
             else if (strcasecmp(typeNameCopy, "longint") == 0) basicType = TYPE_INT64;
             else if (strcasecmp(typeNameCopy, "cardinal") == 0) basicType = TYPE_UINT32;
             else if (strcasecmp(typeNameCopy, "shortint") == 0) basicType = TYPE_INT8;
             else if (strcasecmp(typeNameCopy, "smallint") == 0) basicType = TYPE_INT16;
             else if (strcasecmp(typeNameCopy, "int64") == 0) basicType = TYPE_INT64;
             else if (strcasecmp(typeNameCopy, "single") == 0) basicType = TYPE_FLOAT;
             else if (strcasecmp(typeNameCopy, "double") == 0) basicType = TYPE_DOUBLE;
             else if (strcasecmp(typeNameCopy, "extended") == 0) basicType = TYPE_LONG_DOUBLE;
             else if (strcasecmp(typeNameCopy, "real") == 0) basicType = TYPE_DOUBLE;
             else if (strcasecmp(typeNameCopy, "char") == 0) basicType = TYPE_CHAR;
             else if (strcasecmp(typeNameCopy, "byte") == 0) basicType = TYPE_BYTE;
             else if (strcasecmp(typeNameCopy, "word") == 0) basicType = TYPE_WORD;
             else if (strcasecmp(typeNameCopy, "boolean") == 0) basicType = TYPE_BOOLEAN;
             else if (strcasecmp(typeNameCopy, "file") == 0 || strcasecmp(typeNameCopy, "text") == 0) basicType = TYPE_FILE;
             else if (strcasecmp(typeNameCopy, "mstream") == 0) basicType = TYPE_MEMORYSTREAM;

            if (basicType != TYPE_VOID) {
                node = newASTNode(AST_VARIABLE, initialToken); // Use initialToken
                setTypeAST(node, basicType);
                eat(parser, parser->current_token->type); // Consume the type identifier

                if (basicType == TYPE_FILE && parser->current_token &&
                    parser->current_token->type == TOKEN_OF) {
                    eat(parser, TOKEN_OF);
                    AST *elementType = typeSpecifier(parser, 1);
                    if (!elementType || elementType->type == AST_NOOP) {
                        errorParser(parser, "Invalid element type for file");
                        free(typeNameCopy);
                        freeAST(node);
                        return NULL;
                    }
                    setRight(node, elementType);
                }
            } else {
                // User-defined type reference
                AST *userType = lookupType(typeNameCopy);
                if (!userType) {
                    char err_msg[128]; snprintf(err_msg, sizeof(err_msg), "Undefined type '%s'", typeNameCopy);
                    errorParser(parser, err_msg);
                    free(typeNameCopy);
                    return NULL;
                }
                node = newASTNode(AST_TYPE_REFERENCE, initialToken); // Use initialToken
                setTypeAST(node, userType->var_type);
                node->right = userType; // Link to definition
                eat(parser, parser->current_token->type); // Consume the type identifier
            }
        }
        free(typeNameCopy);
        // Flow continues to the end, return node

    } else {
        // Error: Unexpected token starting a type specifier
        errorParser(parser, "Expected type identifier, '^', ARRAY, RECORD, or SET");
        return NULL;
    }

    // If node is still NULL after all checks, something went wrong internally
    if (!node) {
        errorParser(parser, "Internal error: typeSpecifier failed to create node");
        return NULL;
    }

    return node; // Return the created type specifier node
}

// Receives the TYPE NAME token (e.g., "tmyenum") as input
AST *parseEnumDefinition(Parser *parser, Token* enumTypeNameToken) {
    // Consume '(', create AST_ENUM_TYPE node using enumTypeNameToken (which is a valid copy)
    eat(parser, TOKEN_LPAREN);
    AST *node = newASTNode(AST_ENUM_TYPE, enumTypeNameToken);
    setTypeAST(node, TYPE_ENUM);

    int ordinal = 0;

    // Parse enumeration values (e.g., cred, cgreen, ...)
    while (currentTokenIsIdentifierLike(parser)) {

        // --- COPY the enum value token BEFORE calling eat ---
        Token *originalValueToken = parser->current_token;
        Token *copiedValueToken = copyToken(originalValueToken); // <<< COPY
        if (!copiedValueToken) {
             fprintf(stderr, "Memory allocation failed in parseEnumDefinition (copyToken)\n");
             EXIT_FAILURE_HANDLER();
        }
        // ---

        // Eat the ORIGINAL enum value token (e.g., "cred"); eatInternal frees it
        eat(parser, TOKEN_IDENTIFIER);

        // --- Create the AST node for this enum VALUE using the COPIED token ---
        // newASTNode will make its own copy of copiedValueToken
        AST *valueNode = newASTNode(AST_ENUM_VALUE, copiedValueToken); // <<< Use copied token
        // ---
        valueNode->i_val = ordinal++;
        setTypeAST(valueNode, TYPE_ENUM);
        
        addChild(node, valueNode); // Add value node as child of AST_ENUM_TYPE node

        // --- Symbol Table Handling (using copied token's value) ---
        insertGlobalSymbol(copiedValueToken->value, TYPE_ENUM, node);
        Symbol *symCheck = lookupGlobalSymbol(copiedValueToken->value);
        if (symCheck && symCheck->value) {
            symCheck->value->enum_val.ordinal = valueNode->i_val;
            /* Enum members are constants so mark them accordingly */
            symCheck->is_const = true;
        }
        // --- End Symbol Table Handling ---

        // --- Free the COPIED enum value token ---
        freeToken(copiedValueToken); // <<< Free the copy made for this loop iteration
        // ---

        // Check for comma or end of list
        if (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA); // eatInternal frees the comma token
        } else {
            break; // Exit loop if no comma
        }
    }

    eat(parser, TOKEN_RPAREN); // eatInternal frees the ')' token

    return node; // Return the main AST_ENUM_TYPE node
}

AST *typeDeclaration(Parser *parser) {
    // --- COPY the token BEFORE calling eat ---
    Token *originalTypeNameToken = parser->current_token;
    // Basic check that we actually have an identifier
    if (!tokenIsIdentifierLike(originalTypeNameToken)) {
         errorParser(parser, "Expected type name identifier");
         // You might want better error handling/recovery here
         return newASTNode(AST_NOOP, NULL);
    }
    // Create a copy of the token that this function will own
    Token *copiedTypeNameToken = copyToken(originalTypeNameToken);
    if (!copiedTypeNameToken) {
        // Handle memory allocation error from copyToken if necessary
        fprintf(stderr, "Memory allocation failed in typeDeclaration (copyToken)\n");
        EXIT_FAILURE_HANDLER();
    }
    // ---

    // Now eat the ORIGINAL token; eatInternal will free it
    eat(parser, TOKEN_IDENTIFIER);
    eat(parser, TOKEN_EQUAL);

    AST *typeDefNode = NULL;

    // --- Create the main TYPE_DECL node using the COPIED token ---
    // newASTNode will make its own copy of copiedTypeNameToken
    AST *node = newASTNode(AST_TYPE_DECL, copiedTypeNameToken);
    // ---

    // Check for enum definition or other type specifier
    if (parser->current_token->type == TOKEN_LPAREN) {
        // Pass the COPIED token to parseEnumDefinition
        // parseEnumDefinition uses it (newASTNode inside makes another copy), but doesn't free it
        typeDefNode = parseEnumDefinition(parser, copiedTypeNameToken);
    } else {
        if (parser->current_token && parser->current_token->type == TOKEN_INTERFACE) {
            reserveTypePlaceholder(copiedTypeNameToken->value, TYPE_INTERFACE);
        }
        // parse typeSpecifier as before
        typeDefNode = typeSpecifier(parser, 1);
    }

    setLeft(node, typeDefNode); // Link the actual type definition (enum, record, etc.)
    // Register the type using the value from the copied token
    insertType(copiedTypeNameToken->value, typeDefNode);
    if (typeDefNode && typeDefNode->type == AST_RECORD_TYPE) {
        registerRecordMethods(parser, copiedTypeNameToken->value, typeDefNode);
    }

    eat(parser, TOKEN_SEMICOLON);

    // --- Free the copy of the token we made at the beginning ---
    freeToken(copiedTypeNameToken); // Clean up the copy owned by this function
    // ---

    return node; // Return the main AST_TYPE_DECL node
}

// variable: Simple variable parsing (e.g., for param list names) - No changes needed here usually
AST *variable(Parser *parser) {
    Token *token = parser->current_token;
    if (!tokenIsIdentifierLike(token)){errorParser(parser,"Expected var name"); return NULL;}
    AST* node = newASTNode(AST_VARIABLE, token); // Uses copy
    eat(parser, token->type);
    // Does NOT parse field/array access
    return node;
}

AST *varDeclaration(Parser *parser, bool isGlobal /* Not used here, but kept */) {
    AST *groupNode = newASTNode(AST_VAR_DECL, NULL); // Temp node for names

    // 1. Parse variable list into groupNode children
    while (currentTokenIsIdentifierLike(parser)) {
        // Pass current_token directly to newASTNode, which handles copying.
        AST *varNode = newASTNode(AST_VARIABLE, parser->current_token);
        if (!varNode) { /* Malloc error */ freeAST(groupNode); EXIT_FAILURE_HANDLER(); }

        // Eat the token *after* it has been copied by newASTNode.
        eat(parser, parser->current_token->type);

        addChild(groupNode, varNode);

        if (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else { break; }
    }

    eat(parser, TOKEN_COLON);
    // 2. Parse the type specifier ONCE for the group
    AST *originalTypeNode = typeSpecifier(parser, 0);
    if (!originalTypeNode) { /* error handling */ freeAST(groupNode); return NULL; }

    // Optional initializer after type
    AST *initNode = NULL;
    if (parser->current_token && parser->current_token->type == TOKEN_EQUAL) {
        eat(parser, TOKEN_EQUAL);
        if (parser->current_token && parser->current_token->type == TOKEN_LPAREN) {
            initNode = parseArrayInitializer(parser);
        } else {
            initNode = expression(parser);
        }
    }

    AST *finalCompoundNode = newASTNode(AST_COMPOUND, NULL);

    // 3. Create final VAR_DECL nodes, creating COPIES of type nodes for each
    for (int i = 0; i < groupNode->child_count; ++i) {
        AST *var_decl_node = newASTNode(AST_VAR_DECL, NULL);
        if (!var_decl_node) { /* Malloc check */ freeAST(groupNode); freeAST(initNode); freeAST(finalCompoundNode); EXIT_FAILURE_HANDLER(); }
        // var_type will be set after copying the type node.

        // Transfer the name node (AST_VARIABLE) from groupNode
        var_decl_node->child_count = 1;
        var_decl_node->child_capacity = 1;
        var_decl_node->children = malloc(sizeof(AST*));
        if (!var_decl_node->children) { /* error handling */ freeAST(groupNode); freeAST(originalTypeNode); freeAST(var_decl_node); freeAST(finalCompoundNode); EXIT_FAILURE_HANDLER(); }

        var_decl_node->children[0] = groupNode->children[i]; // Transfer pointer
        groupNode->children[i] = NULL; // Nullify in groupNode
        if (var_decl_node->children[0]) {
             var_decl_node->children[0]->parent = var_decl_node; // Set parent
        }

        // --- Create a DEEP COPY of the typeNode using copyAST ---
        AST* typeNodeCopy = copyAST(originalTypeNode);
        if (!typeNodeCopy) { /* error handling */ freeAST(groupNode); freeAST(originalTypeNode); freeAST(var_decl_node); freeAST(finalCompoundNode); EXIT_FAILURE_HANDLER(); }
        setRight(var_decl_node, typeNodeCopy); // Link VAR_DECL to the UNIQUE copy

        // Copy initializer, if any
        if (initNode) {
            AST *initCopy = copyAST(initNode);
            if (!initCopy) { freeAST(groupNode); freeAST(originalTypeNode); freeAST(initNode); freeAST(var_decl_node); freeAST(finalCompoundNode); EXIT_FAILURE_HANDLER(); }
            setLeft(var_decl_node, initCopy);
        }

        // Ensure the declared variable's type matches the copied type node.
        // This avoids cases where a previous declaration (e.g., an array)
        // leaks its var_type into a subsequent enum declaration.
        var_decl_node->var_type = typeNodeCopy->var_type;

        // If the resolved type ultimately refers to an enum definition,
        // force the VAR_DECL node to carry TYPE_ENUM rather than whatever
        // var_type happened to be set on the copied node (e.g. TYPE_ARRAY).
        AST *enumCheck = typeNodeCopy;
        if (enumCheck && enumCheck->type == AST_TYPE_REFERENCE && enumCheck->right) {
            enumCheck = enumCheck->right;
        }
        if (enumCheck && enumCheck->type == AST_ENUM_TYPE) {
            var_decl_node->var_type = TYPE_ENUM;
        }
        // ---

        addChild(finalCompoundNode, var_decl_node);
    }

    // --- Free the ORIGINAL typeNode returned by typeSpecifier ---
    freeAST(originalTypeNode);
    if (initNode) freeAST(initNode);

    // Free the temporary groupNode (its children pointers were nulled out)
    freeAST(groupNode);

    // Return single node or compound node
    if (finalCompoundNode->child_count == 1) {
         AST* single_var_decl = finalCompoundNode->children[0];
         single_var_decl->parent = NULL;
         free(finalCompoundNode->children);
         finalCompoundNode->children = NULL;
         finalCompoundNode->child_count = 0;
         free(finalCompoundNode);
         return single_var_decl;
    }

    return finalCompoundNode; // Return compound node containing multiple VAR_DECLs
}

AST *functionDeclaration(Parser *parser, bool in_interface) {
    eat(parser, TOKEN_FUNCTION);
    Token *copiedFuncNameToken = parseQualifiedRoutineName(parser, "Expected function name after FUNCTION");
    if (!copiedFuncNameToken) {
        return newASTNode(AST_NOOP, NULL);
    }

    AST *node = newASTNode(AST_FUNCTION_DECL, copiedFuncNameToken);
    node->is_forward_decl = in_interface;
    // newASTNode makes its own copy, so we can free copiedFuncNameToken after node creation
    // BUT we assign node->token to copiedFuncNameToken in newASTNode, so freeToken(copiedFuncNameToken)
    // at the end of this function is correct if newASTNode's copy is what's stored in node->token.
    // Let's assume newASTNode copies and manages its own token, so free our copy here.
    // freeToken(copiedFuncNameToken); // No, newASTNode uses it. Free at end.

    AST *params = NULL;
    // Check for and parse parameters
    if (parser->current_token && parser->current_token->type == TOKEN_LPAREN) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG FUNC_DECL_PARAMS] Detected LPAREN, entering parameter parsing for function '%s'.\n", copiedFuncNameToken->value);
        fflush(stderr);
#endif
        eat(parser, TOKEN_LPAREN);
        if (parser->current_token && parser->current_token->type != TOKEN_RPAREN) {
            params = paramList(parser); // paramList handles parsing multiple params and their types
        }
        if (parser->current_token && parser->current_token->type == TOKEN_RPAREN) {
             eat(parser, TOKEN_RPAREN);
        } else {
             char err_msg[128];
             snprintf(err_msg, sizeof(err_msg), "Expected ')' to close parameter list for function '%s', got %s",
                      copiedFuncNameToken->value,
                      parser->current_token ? tokenTypeToString(parser->current_token->type) : "EOF");
             errorParser(parser, err_msg);
             if(params) freeAST(params);
             if(node) freeAST(node); // freeAST will handle node->token
             else if(copiedFuncNameToken) freeToken(copiedFuncNameToken); // If node wasn't created
             return NULL;
        }
    }

    // Attach parsed parameters as children to the function declaration node
    if (params) {
        if (params->type == AST_COMPOUND && params->child_count > 0) { // paramList returns AST_COMPOUND
            node->children = params->children;
            node->child_count = params->child_count;
            node->child_capacity = params->child_capacity;
            for(int i=0; i < node->child_count; i++) {
                if(node->children[i]) node->children[i]->parent = node;
            }
            params->children = NULL; // Nullify to prevent double free by freeAST(params)
            params->child_count = 0;
            params->child_capacity = 0;
        }
        freeAST(params); // Free the AST_COMPOUND wrapper node from paramList
    }
    
    // After parameters (if any) are handled, expect a colon for the return type
#ifdef DEBUG
    fprintf(stderr, "[DEBUG FUNC_DECL_RET] Expecting COLON for return type of function '%s'. Current token: %s ('%s')\n",
            copiedFuncNameToken->value,
            parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
            (parser->current_token && parser->current_token->value) ? parser->current_token->value : "NULL_VALUE");
    fflush(stderr);
#endif
    eat(parser, TOKEN_COLON); // Expects ':' for return type
    
    AST *returnType = typeSpecifier(parser, 0); // Parse the return type
    if (!returnType || returnType->type == AST_NOOP) {
        errorParser(parser, "Invalid return type for function");
        if(node) freeAST(node); else if(copiedFuncNameToken) freeToken(copiedFuncNameToken);
        return newASTNode(AST_NOOP, NULL);
    }
    setRight(node, returnType);         // Return type stored in 'right'
    node->var_type = returnType->var_type; // Set the function's var_type to its return type

    // Handle implementation part (body) if not in an interface section
    HashTable *outer_table = current_procedure_table;
    HashTable *my_table = NULL;

#ifdef DEBUG
    fprintf(stderr, "[DEBUG FUNC_DECL_BODY] Expecting SEMICOLON after header for function '%s'. Current token: %s ('%s')\n",
            copiedFuncNameToken->value,
            parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
            (parser->current_token && parser->current_token->value) ? parser->current_token->value : "NULL_VALUE");
    fflush(stderr);
#endif
    eat(parser, TOKEN_SEMICOLON);

    if (parser->current_token && parser->current_token->type == TOKEN_INLINE) {
        node->is_inline = true;
        eat(parser, TOKEN_INLINE);
        eat(parser, TOKEN_SEMICOLON);
    }

    if (parser->current_token && parser->current_token->type == TOKEN_FORWARD) {
        node->is_forward_decl = true;
        eat(parser, TOKEN_FORWARD);
        if (!parser->current_token || parser->current_token->type != TOKEN_SEMICOLON) {
            errorParser(parser, "Expected ';' after FORWARD directive");
        }
        eat(parser, TOKEN_SEMICOLON);
    }

    if (!node->is_forward_decl) {
        my_table = pushProcedureTable();

        AST *local_declarations = declarations(parser, false);
        AST *compound_body = compoundStatement(parser);

        AST *blockNode = newASTNode(AST_BLOCK, NULL);
        addChild(blockNode, local_declarations);
        addChild(blockNode, compound_body);
        blockNode->is_global_scope = false;
        setExtra(node, blockNode);
        node->symbol_table = (Symbol*)my_table;
        popProcedureTable(false);
    }

    addProcedure(parser, node, parser->current_unit_name_context, outer_table); // Registers the function
    
    // copiedFuncNameToken was used by newASTNode which made its own copy if needed,
    // or took ownership if newASTNode doesn't copy.
    // Assuming newASTNode *does* copy via copyToken:
    if (copiedFuncNameToken && (!node || node->token != copiedFuncNameToken)) {
       // This freeToken might be redundant if newASTNode's token is the one pointed to by node->token
       // and freeAST(node) will handle it.
       // For safety, if newASTNode makes a copy, this copiedFuncNameToken is ours to free.
       // Let's rely on freeAST(node) to free node->token.
       // The original copiedFuncNameToken here is distinct from node->token if newASTNode copies.
    }
    // If newASTNode sets node->token = copyToken(passed_token), then freeToken(copiedFuncNameToken)
    // is correct here because copiedFuncNameToken is the one we made with copyToken earlier.
    // The token *inside* the AST (node->token) will be freed when 'node' is freed.
    freeToken(copiedFuncNameToken);


    return node;
}

AST *paramList(Parser *parser) {
    AST *compound = newASTNode(AST_COMPOUND, NULL);
    while (parser->current_token->type != TOKEN_RPAREN) {
        int byRef = 0;
        if (parser->current_token) {
            bool isVarModifier = parser->current_token->type == TOKEN_VAR;
            bool isConstModifier = parser->current_token->type == TOKEN_CONST;
            bool isOutModifier = parser->current_token->type == TOKEN_IDENTIFIER &&
                                 tokenMatchesKeyword(parser->current_token, "out");
            if (isVarModifier || isConstModifier || isOutModifier) {
                if (isVarModifier || isOutModifier) {
                    byRef = 1;
                }
                eat(parser, parser->current_token->type);
            }
        }

        AST *group = newASTNode(AST_VAR_DECL, NULL); // Temp node for names
        while (1) { // Parse identifier names into group->children
            if (!currentTokenIsIdentifierLike(parser)) { errorParser(parser, "Expected identifier in parameter list"); freeAST(group); freeAST(compound); return NULL; }
            
            // Directly use current_token to create the node, which copies it internally.
            AST *id_node = newASTNode(AST_VARIABLE, parser->current_token);
            if (!id_node) { fprintf(stderr, "Memory allocation failed for id_node in paramList\n"); freeAST(group); freeAST(compound); EXIT_FAILURE_HANDLER(); }
            
            // Eat the token AFTER it has been safely copied.
            eat(parser, parser->current_token->type);
            
            addChild(group, id_node);

            if (parser->current_token->type == TOKEN_COMMA) { eat(parser, TOKEN_COMMA); }
            else { break; }
        }

        eat(parser, TOKEN_COLON);
        AST *originalTypeNode = typeSpecifier(parser, 1); // Parse type ONCE
        if (!originalTypeNode) { errorParser(parser, "Failed to parse type specifier in parameter list"); freeAST(group); freeAST(compound); return NULL; }

        setTypeAST(group, originalTypeNode->var_type); // Optional: set type on temp group

        for (int i = 0; i < group->child_count; i++) {
            AST *param_decl = newASTNode(AST_VAR_DECL, NULL);
            if (!param_decl) { /* Malloc Check */ freeAST(originalTypeNode); freeAST(group); freeAST(compound); EXIT_FAILURE_HANDLER(); }
            param_decl->child_count = 1;
            param_decl->child_capacity = 1;
            param_decl->children = malloc(sizeof(AST *));
            if (!param_decl->children) { /* Malloc Check */ freeAST(originalTypeNode); freeAST(group); freeAST(compound); freeAST(param_decl); EXIT_FAILURE_HANDLER(); }

            // Instead of copying the token, just copy the AST node which is safer.
            param_decl->children[0] = copyAST(group->children[i]);
            if (!param_decl->children[0]) { /* Malloc check */ freeAST(originalTypeNode); freeAST(group); freeAST(compound); freeAST(param_decl); EXIT_FAILURE_HANDLER(); }

            param_decl->children[0]->parent = param_decl;

            // Make a unique copy of the parsed type node for this parameter
            AST* typeNodeCopy = copyAST(originalTypeNode);
            if (!typeNodeCopy) {
                fprintf(stderr, "Memory allocation failed copying type node in paramList\n");
                freeAST(group);
                freeAST(compound);
                freeAST(param_decl);
                freeAST(originalTypeNode);
                EXIT_FAILURE_HANDLER();
            }

            setRight(param_decl, typeNodeCopy);      // Store the full type on the right pointer
            param_decl->type_def = typeNodeCopy;      // Mirror on type_def for later lookups

            // Ensure the VAR_DECL reflects the array nature of the parameter
            param_decl->var_type = typeNodeCopy->var_type;
            param_decl->by_ref = byRef;
            if (typeNodeCopy->var_type == TYPE_ARRAY) {
                param_decl->var_type = TYPE_ARRAY;
            }

            addChild(compound, param_decl);
        }

        // --- Free the ORIGINAL typeNode returned by typeSpecifier ---
        freeAST(originalTypeNode); // <<< Free the node returned by typeSpecifier

        freeAST(group); // Free temp name group

        if (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
        } else if (parser->current_token->type != TOKEN_RPAREN) { errorParser(parser, "Expected ';' or ')' after parameter declaration"); freeAST(compound); return NULL; }
        else { break; }
    }
    return compound;
}

AST* compoundStatement(Parser *parser) {
    eat(parser, TOKEN_BEGIN);
    AST *node = newASTNode(AST_COMPOUND, NULL); // Assuming AST_COMPOUND is correct type

    while (1) { // Loop until explicitly broken or END is consumed
        // Skip any optional leading/multiple semicolons
        while (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
        }

        // Check for the end of the block *before* trying to parse a statement
        if (parser->current_token->type == TOKEN_END) {
            break; // Found the end of this compound statement
        }
        if (parser->current_token->type == TOKEN_PERIOD) {
             break;
        }

        // Parse one statement within the block
        AST *stmt = statement(parser);
        if (!stmt) {
             break; // Stop processing compound block if a statement failed
        } else {
             addChild(node, stmt);
        }

        // Now, determine if we expect a semicolon separator or the end of the block
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
             if (parser->current_token->type == TOKEN_END) {
                 break;
             }
             if (parser->current_token->type == TOKEN_PERIOD) {
                 break;
             }

        } else if (parser->current_token->type == TOKEN_END) {
            break;
        } else if (parser->current_token->type == TOKEN_PERIOD) {
             break;
        }
         else {
            // --- CORRECTED DEBUG PRINT HERE ---
            #ifdef DEBUG
            fprintf(stderr, "\n[DEBUG_ERROR] In compoundStatement loop after parsing a statement.\n");
            // *** Access line/column via parser->lexer ***
            fprintf(stderr, "[DEBUG_ERROR] Expected SEMICOLON or END, but found Token Type: %d (%s), Value: '%s' at Line %d, Col %d\n\n",
                    parser->current_token->type,
                    tokenTypeToString(parser->current_token->type),
                    parser->current_token->value ? parser->current_token->value : "NULL",
                    parser->lexer->line,  // <-- Use parser->lexer->line
                    parser->lexer->column // <-- Use parser->lexer->column
                   );
            fflush(stderr);
            #endif
            // --- END CORRECTED DEBUG PRINT ---

            // --- Original Error Reporting ---
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg),
                     "Expected semicolon or END after statement in compound block (found token: %s)",
                     tokenTypeToString(parser->current_token->type));
            errorParser(parser, error_msg);
            // --- End Original Error Reporting ---

            break; // Exit loop on error
        }
    } // End while(1)

    // After the loop, consume the END token (unless it was program end '.')
    if (parser->current_token->type != TOKEN_PERIOD) {
       if (parser->current_token->type == TOKEN_END) {
          eat(parser, TOKEN_END);
       } else {
           char error_msg[128];
           // *** Also update error location here if needed ***
           snprintf(error_msg, sizeof(error_msg), "Expected END or '.', but found %s at Line %d Col %d",
                    tokenTypeToString(parser->current_token->type),
                    parser->lexer->line, parser->lexer->column); // Use lexer location
           errorParser(parser, error_msg);
       }
    }
    return node;
}

AST *statement(Parser *parser) {
    if (!parser || !parser->current_token) {
        return newASTNode(AST_NOOP, NULL);
    }

    if (parser->current_token->type == TOKEN_IDENTIFIER ||
        parser->current_token->type == TOKEN_LABEL ||
        parser->current_token->type == TOKEN_INTEGER_CONST) {
        Token *lookahead = peekToken(parser);
        bool is_label = (lookahead && lookahead->type == TOKEN_COLON);
        if (lookahead) {
            freeToken(lookahead);
        }
        if (is_label) {
            Token *label_copy = copyToken(parser->current_token);
            if (!label_copy && parser->current_token) {
                EXIT_FAILURE_HANDLER();
            }
            TokenType label_type = parser->current_token->type;
            eat(parser, label_type);
            eat(parser, TOKEN_COLON);

            AST *inner_stmt = NULL;
            if (parser->current_token &&
                !tokenTerminatesStatement(parser->current_token->type)) {
                inner_stmt = statement(parser);
            } else {
                inner_stmt = newASTNode(AST_NOOP, NULL);
            }

            AST *label_node = newLabelStatement(label_copy, inner_stmt);
            freeToken(label_copy);
            return label_node;
        }
    }

    AST *node = NULL; // Initialize node

    switch (parser->current_token->type) {
        case TOKEN_BEGIN:
            // Compound statement (BEGIN ... END)
            node = compoundStatement(parser);
            // compoundStatement handles its own END token.
            break; // No semicolon needed after END

        case TOKEN_LABEL:
        case TOKEN_IDENTIFIER: {
            AST *lval_or_proc_id = lvalue(parser); // Parses identifier, field access, array access

            if (!lval_or_proc_id) {
                // lvalue should call errorParser internally if it fails syntactically
                // If it returns NULL for other reasons, that's an issue.
                fprintf(stderr, "Error: lvalue() returned NULL unexpectedly after identifier.\n");
                node = newASTNode(AST_NOOP, NULL); // Error recovery node
                break; // Exit case
            }

            // Check for assignment first
            if (parser->current_token->type == TOKEN_ASSIGN ||
                parser->current_token->type == TOKEN_PLUS_EQUAL ||
                parser->current_token->type == TOKEN_MINUS_EQUAL) {
                // --- Assignment Statement ---
                node = assignmentStatement(parser, lval_or_proc_id); // assignmentStatement handles := and RHS
                                                                     // It will use lval_or_proc_id as is (e.g. AST_VARIABLE or AST_FIELD_ACCESS for LHS)
            }
            // Check if it's a procedure or function call context
            else if (lval_or_proc_id->type == AST_VARIABLE || lval_or_proc_id->type == AST_FIELD_ACCESS) {
                bool has_args = (parser->current_token->type == TOKEN_LPAREN);
                AST *proc_call_node_to_use = NULL;

                if (has_args) {
                    // Call with arguments: proc(...) or unit.proc(...)
                    // Modify the existing lval_or_proc_id node to become the procedure call node.
                    lval_or_proc_id->type = AST_PROCEDURE_CALL;
                    // If lval_or_proc_id was AST_FIELD_ACCESS, its 'left' (unit) and 'token' (proc) are preserved.
                    // If it was AST_VARIABLE, its 'token' (proc) is preserved, 'left' is likely NULL.
                    proc_call_node_to_use = lval_or_proc_id;
                } else {
                    // Parameter-less call: proc; or unit.proc;
                    // Create a new AST_PROCEDURE_CALL node.
                    if (lval_or_proc_id->type == AST_VARIABLE) {
                        Token* name_token_copy = copyToken(lval_or_proc_id->token);
                        if (!name_token_copy) { /* Malloc error */ freeAST(lval_or_proc_id); EXIT_FAILURE_HANDLER(); }
                        proc_call_node_to_use = newASTNode(AST_PROCEDURE_CALL, name_token_copy);
                        if (!proc_call_node_to_use) { /* Malloc error */ freeToken(name_token_copy); freeAST(lval_or_proc_id); EXIT_FAILURE_HANDLER(); }
                        freeToken(name_token_copy); // newASTNode made its own copy
                        freeAST(lval_or_proc_id);   // Free the original AST_VARIABLE node
                    } else { // lval_or_proc_id->type == AST_FIELD_ACCESS
                        Token* proc_name_token_copy = copyToken(lval_or_proc_id->token); // Token for "proc"
                        if (!proc_name_token_copy) { /* Malloc error */ freeAST(lval_or_proc_id); EXIT_FAILURE_HANDLER(); }

                        proc_call_node_to_use = newASTNode(AST_PROCEDURE_CALL, proc_name_token_copy);
                        if (!proc_call_node_to_use) { /* Malloc error */ freeToken(proc_name_token_copy); freeAST(lval_or_proc_id); EXIT_FAILURE_HANDLER(); }
                        freeToken(proc_name_token_copy); // newASTNode made its own copy

                        // Transfer the unit qualifier (left child) from AST_FIELD_ACCESS
                        if (lval_or_proc_id->left) {
                            proc_call_node_to_use->left = lval_or_proc_id->left; // Transfer ownership
                            if (proc_call_node_to_use->left) { // Should always be true if lval_or_proc_id->left was non-NULL
                                 proc_call_node_to_use->left->parent = proc_call_node_to_use;
                            }
                            lval_or_proc_id->left = NULL; // Nullify in original to prevent double free
                        }
                        freeAST(lval_or_proc_id); // Free the original AST_FIELD_ACCESS node (its token is copied, its left child is moved or was NULL)
                    }
                }

                // Parse arguments if present (has_args == true)
                if (has_args) {
                    eat(parser, TOKEN_LPAREN); // Consume '('
                    if (parser->current_token->type != TOKEN_RPAREN) {
                        bool isStrCall = false;
                        if (proc_call_node_to_use && proc_call_node_to_use->token && proc_call_node_to_use->token->value) {
                            isStrCall = (strcasecmp(proc_call_node_to_use->token->value, "str") == 0);
                        }

                        AST* args_compound = isStrCall ? parseStrArgumentList(parser) : exprList(parser); // Parse arguments; exprList returns an AST_COMPOUND node

                        // Argument Transfer Logic (same as your original, applied to proc_call_node_to_use)
                        if (args_compound && args_compound->type == AST_COMPOUND && args_compound->child_count > 0) {
                            #ifdef DEBUG
                            fprintf(stderr, "[DEBUG PARSER STMT] Transferring %d children from args %p to proc_call %p\n",
                                    args_compound->child_count, (void*)args_compound, (void*)proc_call_node_to_use);
                            #endif
                            proc_call_node_to_use->children = args_compound->children;
                            proc_call_node_to_use->child_count = args_compound->child_count;
                            proc_call_node_to_use->child_capacity = args_compound->child_capacity; // Also copy capacity
                            args_compound->children = NULL; args_compound->child_count = 0; args_compound->child_capacity = 0;
                            for(int i=0; i < proc_call_node_to_use->child_count; i++) {
                                if(proc_call_node_to_use->children[i]) {
                                    proc_call_node_to_use->children[i]->parent = proc_call_node_to_use;
                                }
                            }
                        } else { // args_compound was NULL or empty
                            proc_call_node_to_use->children = NULL;
                            proc_call_node_to_use->child_count = 0;
                            proc_call_node_to_use->child_capacity = 0;
                            #ifdef DEBUG
                            if(args_compound) fprintf(stderr, "[DEBUG PARSER STMT] Args node existed but was empty or not compound for proc_call %p\n", (void*)proc_call_node_to_use);
                            else fprintf(stderr, "[DEBUG PARSER STMT] exprList returned NULL for proc_call %p\n", (void*)proc_call_node_to_use);
                            #endif
                        }
                        if(args_compound) freeAST(args_compound); // Free the AST_COMPOUND wrapper
                    } else { // Empty argument list '()'
                        proc_call_node_to_use->children = NULL;
                        proc_call_node_to_use->child_count = 0;
                        proc_call_node_to_use->child_capacity = 0;
                    }
                    eat(parser, TOKEN_RPAREN); // Consume ')'
                } else { // No arguments (parameter-less call that was not AST_VARIABLE initially, or AST_VARIABLE that's now AST_PROCEDURE_CALL)
                    proc_call_node_to_use->children = NULL;
                    proc_call_node_to_use->child_count = 0;
                    proc_call_node_to_use->child_capacity = 0;
                }
                node = proc_call_node_to_use;
            }
            // Error: If it's not an assignment and not a recognizable procedure/function call
            // This means lvalue() returned something like AST_ARRAY_ACCESS that isn't being called or assigned to.
            else {
                char error_msg[256]; // Increased buffer size
                const char* lval_desc = "<unknown_lvalue_structure>";
                char lval_desc_buf[128];
                if (lval_or_proc_id->token && lval_or_proc_id->token->value) {
                    lval_desc = lval_or_proc_id->token->value;
                } else if (lval_or_proc_id->left && lval_or_proc_id->left->token && lval_or_proc_id->left->token->value) {
                    // Attempt to get a more descriptive name for complex lvalues like array access
                    snprintf(lval_desc_buf, sizeof(lval_desc_buf), "%s[...]", lval_or_proc_id->left->token->value);
                    lval_desc = lval_desc_buf;
                }

                snprintf(error_msg, sizeof(error_msg),
                         "Expression starting with '%s' (type %s) cannot be used as a statement here (followed by '%s')",
                         lval_desc,
                         astTypeToString(lval_or_proc_id->type),
                         tokenTypeToString(parser->current_token->type));
                errorParser(parser, error_msg);
                freeAST(lval_or_proc_id); // Free the invalid lvalue node
                node = newASTNode(AST_NOOP, NULL); // Return NOOP for error recovery
            }

            #ifdef DEBUG
            // Enhanced debug print
            if(node) {
                const char* node_name_part = NULL;
                const char* node_qualifier_part = NULL;
                if (node->token && node->token->value) {
                    node_name_part = node->token->value;
                }
                if (node->left && node->left->type == AST_VARIABLE && node->left->token && node->left->token->value) {
                    node_qualifier_part = node->left->token->value;
                }

                fprintf(stderr, "[DEBUG PARSER STMT] Leaving TOKEN_IDENTIFIER case. Node %p: type=%s", (void*)node, astTypeToString(node->type));
                if (node_qualifier_part) fprintf(stderr, ", qualifier='%s'", node_qualifier_part);
                if (node_name_part) fprintf(stderr, ", name/token='%s'", node_name_part);
                fprintf(stderr, ", child_count=%d, children_ptr=%p\n", node->child_count, (void*)node->children);
            } else {
                fprintf(stderr, "[DEBUG PARSER STMT] Leaving TOKEN_IDENTIFIER case with NULL node.\n");
            }
            #endif
            break; // End case TOKEN_IDENTIFIER
        }
        // --- Cases for specific statement keywords ---
        case TOKEN_IF:
            node = ifStatement(parser);
            // IF handles its structure internally.
            break;
        case TOKEN_WHILE:
            node = whileStatement(parser);
            // WHILE handles its structure internally.
            break;
        case TOKEN_FOR:
            node = forStatement(parser);
            // FOR handles its structure internally.
            break;
        case TOKEN_REPEAT:
            node = repeatStatement(parser);
            // REPEAT handles its structure internally.
            break;
        case TOKEN_CASE:
            node = caseStatement(parser);
            // CASE handles its structure internally.
            break;
        case TOKEN_WRITE:
            node = writeStatement(parser); // Parses WRITE (...)
            // *** Semicolon check REMOVED from here ***
            break;
        case TOKEN_WRITELN:
            node = writelnStatement(parser); // Parses WRITELN (...)
            // *** Semicolon check REMOVED from here ***
            break;
        case TOKEN_READ:
             node = readStatement(parser); // Parses READ (...)
             // *** Semicolon check REMOVED from here ***
             break;
        case TOKEN_READLN:
             node = readlnStatement(parser); // Parses READLN (...)
             // *** Semicolon check REMOVED from here ***
             break;
        case TOKEN_SPAWN:
            node = spawnStatement(parser);
            break;
        case TOKEN_JOIN:
            node = joinStatement(parser);
            break;
        case TOKEN_GOTO: {
            eat(parser, TOKEN_GOTO);
            if (!parser->current_token ||
                (!tokenIsIdentifierLike(parser->current_token) &&
                 parser->current_token->type != TOKEN_INTEGER_CONST)) {
                errorParser(parser, "Expected label after GOTO");
                return newASTNode(AST_NOOP, NULL);
            }
            Token *target_copy = copyToken(parser->current_token);
            if (!target_copy && parser->current_token) {
                EXIT_FAILURE_HANDLER();
            }
            TokenType target_type = parser->current_token->type;
            eat(parser, target_type);
            node = newGotoStatement(target_copy);
            freeToken(target_copy);
            break;
        }
        case TOKEN_BREAK:
            eat(parser, TOKEN_BREAK); // Consume BREAK keyword
            node = newASTNode(AST_BREAK, NULL);
            // *** Semicolon check REMOVED from here ***
            break;

        case TOKEN_SEMICOLON: // Empty statement ';'
            eat(parser, TOKEN_SEMICOLON);
            node = newASTNode(AST_NOOP, NULL); // Represent as NOOP
            break; // No semicolon needed after an empty statement

        default:
            // Error for unexpected token starting a statement.
            errorParser(parser, "Unexpected token starting statement");
            node = newASTNode(AST_NOOP, NULL);
            break; // Exit switch
    } // End switch

    #ifdef DEBUG
    if (dumpExec && node) debugAST(node, 0); // Optional debug dump
    #endif
    return node;
} // End statement()

// Parameter renamed to parsedLValue
// assignmentStatement: Calls expression
AST *assignmentStatement(Parser *parser, AST *parsedLValue) {
    if (!parser || !parsedLValue) {
        return newASTNode(AST_NOOP, NULL);
    }

    if (!parser->current_token) {
        errorParser(parser, "Expected assignment operator");
        return newASTNode(AST_NOOP, NULL);
    }

    TokenType opType = parser->current_token->type;
    if (opType != TOKEN_ASSIGN && opType != TOKEN_PLUS_EQUAL && opType != TOKEN_MINUS_EQUAL) {
        errorParser(parser, "Expected assignment operator");
        return newASTNode(AST_NOOP, NULL);
    }

    int opLine = parser->current_token->line;
    int opColumn = parser->current_token->column;

    eat(parser, opType);

    AST *rhs = expression(parser);
    if (!rhs || rhs->type == AST_NOOP) {
        errorParser(parser, "Expected expression after assignment");
        return newASTNode(AST_NOOP, NULL);
    }

    AST *assignNode = newASTNode(AST_ASSIGN, NULL);
    setLeft(assignNode, parsedLValue);

    if (opType == TOKEN_ASSIGN) {
        setRight(assignNode, rhs);
        return assignNode;
    }

    AST *lhsCopy = copyAST(parsedLValue);
    if (!lhsCopy) {
        errorParser(parser, "Failed to duplicate assignment target");
        freeAST(rhs);
        return newASTNode(AST_NOOP, NULL);
    }

    Token *opToken = newToken(
        (opType == TOKEN_PLUS_EQUAL) ? TOKEN_PLUS : TOKEN_MINUS,
        (opType == TOKEN_PLUS_EQUAL) ? "+" : "-",
        opLine,
        opColumn);
    AST *binaryNode = newASTNode(AST_BINARY_OP, opToken);
    freeToken(opToken);
    setLeft(binaryNode, lhsCopy);
    setRight(binaryNode, rhs);
    setRight(assignNode, binaryNode);

    return assignNode;
}

// procedureCall: Calls exprList (which calls expression)
AST *procedureCall(Parser *parser) {
    // Assumes current token is the procedure identifier
    if (!currentTokenIsIdentifierLike(parser)) {
        errorParser(parser, "Expected procedure identifier");
        return newASTNode(AST_NOOP, NULL);
    }
    TokenType proc_token_type = parser->current_token->type;
    AST *node = newASTNode(AST_PROCEDURE_CALL, parser->current_token);
    eat(parser, proc_token_type);
    if(parser->current_token && parser->current_token->type==TOKEN_LPAREN){
        eat(parser,TOKEN_LPAREN); AST* args=NULL;
        if(parser->current_token && parser->current_token->type!=TOKEN_RPAREN) args=exprList(parser); // <<< exprList uses expression()
        if(!args && parser->current_token && parser->current_token->type != TOKEN_RPAREN){errorParser(parser,"Bad arg list"); return node;} // Error if exprList failed
        if(!parser->current_token || parser->current_token->type != TOKEN_RPAREN){errorParser(parser,"Exp )"); return node;} eat(parser,TOKEN_RPAREN);
        // Argument transfer logic (keep existing)
        if(args && args->child_count>0){node->children=args->children;node->child_count=args->child_count; node->child_capacity=args->child_capacity; args->children=NULL;args->child_count=0; args->child_capacity=0; for(int i=0;i<node->child_count;i++)if(node->children[i])node->children[i]->parent=node;}
        else {node->children=NULL;node->child_count=0; node->child_capacity=0;}
        if(args)freeAST(args); // Use freeAST
    } else {node->children=NULL;node->child_count=0; node->child_capacity=0;}
    return node;
}

AST *ifStatement(Parser *parser) {
    eat(parser,TOKEN_IF); AST* c=expression(parser); // <<< Use expression()
    if(!c || c->type==AST_NOOP){errorParser(parser,"Exp cond"); return NULL;}
    if(!parser->current_token || parser->current_token->type!=TOKEN_THEN){errorParser(parser,"Exp THEN"); freeAST(c); return NULL;} eat(parser,TOKEN_THEN);
    AST* t=statement(parser); if(!t || t->type==AST_NOOP){errorParser(parser,"Exp THEN stmt"); freeAST(c); return NULL;}
    AST* n=newASTNode(AST_IF,NULL); setLeft(n,c); setRight(n,t);
    if(parser->current_token && parser->current_token->type==TOKEN_ELSE){eat(parser,TOKEN_ELSE); AST* e=statement(parser); if(!e){errorParser(parser,"Exp ELSE stmt");} setExtra(n,e);}
    return n;
}

AST *whileStatement(Parser *parser) {
    eat(parser,TOKEN_WHILE); AST* c=expression(parser); // <<< Use expression()
    if(!c || c->type==AST_NOOP){errorParser(parser,"Exp cond"); return NULL;}
    if(!parser->current_token || parser->current_token->type!=TOKEN_DO){errorParser(parser,"Exp DO"); freeAST(c); return NULL;} eat(parser,TOKEN_DO);
    AST* b=statement(parser); if(!b || b->type==AST_NOOP){errorParser(parser,"Exp DO stmt"); freeAST(c); return NULL;}
    AST* n=newASTNode(AST_WHILE,NULL); setLeft(n,c); setRight(n,b); return n;
}

// parseCaseLabels: Calls expression
AST *parseCaseLabels(Parser *parser) {
    AST *labels = newASTNode(AST_COMPOUND, NULL);
    while (1) {
        AST *label = NULL;
        AST *start = expression(parser); // <<< Use expression()
        if (!start || start->type == AST_NOOP) { errorParser(parser, "Exp expr for case label"); break; }
        if (parser->current_token && parser->current_token->type == TOKEN_DOTDOT) {
            eat(parser, TOKEN_DOTDOT);
            AST *end = expression(parser); // <<< Use expression()
            if (!end || end->type == AST_NOOP) { errorParser(parser, "Exp expr after .."); freeAST(start); break; }
            label = newASTNode(AST_SUBRANGE, NULL); setLeft(label, start); setRight(label, end);
        } else { label = start; } // Single label
        addChild(labels, label);
        if (parser->current_token && parser->current_token->type == TOKEN_COMMA) eat(parser, TOKEN_COMMA);
        else break;
    }
    if (labels->child_count == 1) { AST *s=labels->children[0]; s->parent=NULL; free(labels->children); free(labels); return s;} // Simplify
    else if (labels->child_count == 0) { freeAST(labels); return newASTNode(AST_NOOP, NULL);} // Handle empty
    return labels;
}

AST *caseStatement(Parser *parser) {
    eat(parser,TOKEN_CASE); AST* ce=expression(parser); // <<< Use expression()
    if(!ce || ce->type==AST_NOOP){errorParser(parser,"Exp CASE expr"); return NULL;}
    AST* n=newASTNode(AST_CASE,NULL); setLeft(n,ce);
    if(!parser->current_token || parser->current_token->type!=TOKEN_OF){errorParser(parser,"Exp OF"); return n;} eat(parser,TOKEN_OF);
    while(parser->current_token && parser->current_token->type!=TOKEN_ELSE && parser->current_token->type!=TOKEN_END){
        AST* br=newASTNode(AST_CASE_BRANCH,NULL);
        AST* lbls = parseCaseLabels(parser); // <<< Calls expression() internally
        if(!lbls || lbls->type==AST_NOOP){errorParser(parser,"Bad case labels"); freeAST(br); break;} setLeft(br,lbls);
        if(!parser->current_token || parser->current_token->type!=TOKEN_COLON){errorParser(parser,"Exp :"); freeAST(br); break;} eat(parser,TOKEN_COLON);
        AST* stmt = statement(parser); if(!stmt || stmt->type==AST_NOOP){errorParser(parser,"Exp stmt after :"); freeAST(br); break;} setRight(br,stmt); addChild(n,br);
        if(parser->current_token && parser->current_token->type==TOKEN_SEMICOLON)eat(parser,TOKEN_SEMICOLON);
        else break;
    }
    if(parser->current_token && parser->current_token->type==TOKEN_ELSE){eat(parser,TOKEN_ELSE); AST* elsestmt = statement(parser); if(!elsestmt){errorParser(parser,"Exp ELSE stmt");} setExtra(n,elsestmt); if(parser->current_token && parser->current_token->type==TOKEN_SEMICOLON)eat(parser,TOKEN_SEMICOLON);}
    if(!parser->current_token || parser->current_token->type!=TOKEN_END){errorParser(parser,"Exp END"); return n;} eat(parser,TOKEN_END); return n;
}

AST *repeatStatement(Parser *parser) {
    eat(parser,TOKEN_REPEAT); AST* b=newASTNode(AST_COMPOUND,NULL);
    while(1){
        if(!parser->current_token){errorParser(parser,"EOF in REPEAT"); break;}
        if(parser->current_token->type==TOKEN_UNTIL) break;
        while(parser->current_token && parser->current_token->type==TOKEN_SEMICOLON) eat(parser,TOKEN_SEMICOLON);
        if(!parser->current_token || parser->current_token->type==TOKEN_UNTIL) break;
        AST* s=statement(parser); if(s&&s->type!=AST_NOOP)addChild(b,s); else if(!s){errorParser(parser,"Bad REPEAT stmt"); break;}
        if(parser->current_token && parser->current_token->type==TOKEN_SEMICOLON) eat(parser,TOKEN_SEMICOLON);
        else if(!parser->current_token || parser->current_token->type!=TOKEN_UNTIL){/*Allow missing semi*/}
    }
    if(!parser->current_token || parser->current_token->type!=TOKEN_UNTIL){errorParser(parser,"Exp UNTIL"); return b;} eat(parser,TOKEN_UNTIL);
    AST* c=expression(parser); // <<< Use expression()
    if(!c || c->type == AST_NOOP){errorParser(parser,"Exp UNTIL cond"); freeAST(b); return NULL;}
    AST* n=newASTNode(AST_REPEAT,NULL); setLeft(n,b); setRight(n,c); return n;
}

AST *forStatement(Parser *parser) {
    eat(parser,TOKEN_FOR); Token* lvt=copyToken(parser->current_token); if(!lvt||lvt->type!=TOKEN_IDENTIFIER){errorParser(parser,"Exp loop var"); return NULL;} eat(parser,TOKEN_IDENTIFIER);
    AST* lvn=newASTNode(AST_VARIABLE,lvt); // Loop var node created with copy
    if(!parser->current_token||parser->current_token->type!=TOKEN_ASSIGN){errorParser(parser,"Exp :="); freeToken(lvt); freeAST(lvn); return NULL;} eat(parser,TOKEN_ASSIGN);
    AST* se=expression(parser); // <<< Use expression() for start
    if(!se || se->type == AST_NOOP){errorParser(parser,"Exp start expr"); freeToken(lvt); freeAST(lvn); return NULL;}
    TokenType dir=parser->current_token? parser->current_token->type : TOKEN_UNKNOWN; // Check NULL
    if(dir!=TOKEN_TO && dir!=TOKEN_DOWNTO){errorParser(parser,"Exp TO/DOWNTO"); freeToken(lvt); freeAST(lvn); freeAST(se); return NULL;} eat(parser,dir);
    AST* ee=expression(parser); // <<< Use expression() for end
    if(!ee || ee->type == AST_NOOP){errorParser(parser,"Exp end expr"); freeToken(lvt); freeAST(lvn); freeAST(se); return NULL;}
    if(!parser->current_token||parser->current_token->type!=TOKEN_DO){errorParser(parser,"Exp DO"); freeToken(lvt); freeAST(lvn); freeAST(se); freeAST(ee); return NULL;} eat(parser,TOKEN_DO);
    AST* bd=statement(parser); if(!bd || bd->type == AST_NOOP){errorParser(parser,"Exp body"); freeToken(lvt); freeAST(lvn); freeAST(se); freeAST(ee); return NULL;}
    ASTNodeType ft=(dir==TOKEN_TO)?AST_FOR_TO:AST_FOR_DOWNTO; AST* n=newASTNode(ft,NULL);
    setLeft(n,se); setRight(n,ee); setExtra(n,bd); addChild(n,lvn); // Loop var is child[0]
    freeToken(lvt); // Free the original copy used for lvn
    return n;
}

AST *writelnStatement(Parser *parser) {
    // Allow calling writeln as identifier for compatibility if needed, otherwise use keyword
    if(parser->current_token && parser->current_token->type==TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value,"writeln")==0)eat(parser,TOKEN_IDENTIFIER);
    else eat(parser,TOKEN_WRITELN);
    AST* args=parseWriteArguments(parser); // Calls parseWriteArgument -> expression
    AST* n=newASTNode(AST_WRITELN,NULL);
    if(args){ // Transfer children safely
        n->children=args->children; n->child_count=args->child_count; n->child_capacity=args->child_capacity;
        args->children=NULL; args->child_count=0; args->child_capacity=0;
        for(int i=0;i<n->child_count;i++)if(n->children[i])n->children[i]->parent=n;
        free(args); // Free the compound wrapper struct
    } else { n->children=NULL;n->child_count=0; n->child_capacity=0; }
    return n;
}

AST *writeStatement(Parser *parser) {
    if(parser->current_token && parser->current_token->type==TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value,"write")==0)eat(parser,TOKEN_IDENTIFIER);
    else eat(parser,TOKEN_WRITE);
    AST* args=parseWriteArguments(parser); // Calls parseWriteArgument -> expression
    AST* n=newASTNode(AST_WRITE,NULL);
    if(args){ // Transfer children safely
        n->children=args->children; n->child_count=args->child_count; n->child_capacity=args->child_capacity;
        args->children=NULL; args->child_count=0; args->child_capacity=0;
        for(int i=0;i<n->child_count;i++)if(n->children[i])n->children[i]->parent=n;
        free(args);
    } else { n->children=NULL;n->child_count=0; n->child_capacity=0; }
    return n;
}

AST *readStatement(Parser *parser) {
    if(parser->current_token && parser->current_token->type==TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value,"read")==0)eat(parser,TOKEN_IDENTIFIER);
    else eat(parser,TOKEN_READ);
    AST* n=newASTNode(AST_READ,NULL); AST* args=NULL;
    if(parser->current_token && parser->current_token->type==TOKEN_LPAREN){
        eat(parser,TOKEN_LPAREN);
        args=exprList(parser); // <<< exprList calls expression
        if(!args || args->type==AST_NOOP){errorParser(parser,"Bad read args"); return n;}
        if(!parser->current_token || parser->current_token->type!=TOKEN_RPAREN){errorParser(parser,"Exp )"); return n;} eat(parser,TOKEN_RPAREN);
    } else {args=newASTNode(AST_COMPOUND,NULL); args->child_count=0; args->child_capacity=0;} // No args if no parens
    if(args){ // Transfer children
        n->children=args->children; n->child_count=args->child_count; n->child_capacity=args->child_capacity;
        args->children=NULL; args->child_count=0; args->child_capacity=0;
        for(int i=0;i<n->child_count;i++)if(n->children[i])n->children[i]->parent=n;
        free(args);
    } else {n->children=NULL;n->child_count=0; n->child_capacity=0;}
    return n;
}

AST *readlnStatement(Parser *parser) {
    if(parser->current_token && parser->current_token->type==TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value,"readln")==0)eat(parser,TOKEN_IDENTIFIER);
    else eat(parser,TOKEN_READLN);
    AST* n=newASTNode(AST_READLN,NULL); AST* args=NULL;
    if(parser->current_token && parser->current_token->type==TOKEN_LPAREN){ // Optional parens
        eat(parser,TOKEN_LPAREN);
        if(parser->current_token && parser->current_token->type!=TOKEN_RPAREN) args=exprList(parser); // <<< exprList calls expression
        else {args=newASTNode(AST_COMPOUND,NULL); args->child_count=0; args->child_capacity=0;} // Empty list inside ()
        if(!args || args->type==AST_NOOP){errorParser(parser,"Bad readln args"); return n;} // Error check
        if(!parser->current_token || parser->current_token->type!=TOKEN_RPAREN){errorParser(parser,"Exp )"); return n;} eat(parser,TOKEN_RPAREN);
    } else {args=newASTNode(AST_COMPOUND,NULL); args->child_count=0; args->child_capacity=0;} // No args if no parens
    if(args){ // Transfer children
        n->children=args->children; n->child_count=args->child_count; n->child_capacity=args->child_capacity;
        args->children=NULL; args->child_count=0; args->child_capacity=0;
        for(int i=0;i<n->child_count;i++)if(n->children[i])n->children[i]->parent=n;
        free(args);
    } else {n->children=NULL;n->child_count=0; n->child_capacity=0;}
    return n;
}

AST *spawnStatement(Parser *parser) {
    eat(parser, TOKEN_SPAWN);
    if (!tokenIsIdentifierLike(parser->current_token)) {
        errorParser(parser, "Expected procedure identifier after SPAWN");
        return newASTNode(AST_NOOP, NULL);
    }
    AST *call = procedureCall(parser);
    AST *node = newThreadSpawn(call);
    setTypeAST(node, TYPE_INTEGER);
    return node;
}

AST *joinStatement(Parser *parser) {
    eat(parser, TOKEN_JOIN);
    AST *exprNode = expression(parser);
    if (!exprNode) return newASTNode(AST_NOOP, NULL);
    AST *node = newThreadJoin(exprNode);
    return node;
}
// exprList: Calls expression
AST *exprList(Parser *parser) {
    AST *node=newASTNode(AST_COMPOUND,NULL);
    AST *arg=expression(parser); // <<< Use expression()
    if(!arg||arg->type==AST_NOOP){errorParser(parser,"Expected expression in list"); freeAST(node); return NULL;} // Check result
    addChild(node,arg);
    while(parser->current_token && parser->current_token->type==TOKEN_COMMA){
        eat(parser,TOKEN_COMMA);
        arg=expression(parser); // <<< Use expression()
        if(!arg||arg->type==AST_NOOP){errorParser(parser,"Expected expression after comma"); return node;} // Return partial list
        addChild(node,arg);
    }
    return node;
}

// --- Modify parseSetConstructor function ---
AST *parseSetConstructor(Parser *parser) {
    if(!parser->current_token || parser->current_token->type != TOKEN_LBRACKET){errorParser(parser,"Exp ["); return NULL;} eat(parser, TOKEN_LBRACKET);
    AST *sn=newASTNode(AST_SET,NULL); setTypeAST(sn,TYPE_SET);
    if(parser->current_token && parser->current_token->type!=TOKEN_RBRACKET){
        while(1){
            AST* el=expression(parser); // <<< Use expression()
            if(!el||el->type==AST_NOOP){errorParser(parser,"Bad set elem"); break;}
            // Runtime checks ordinal compatibility
            if(parser->current_token && parser->current_token->type==TOKEN_DOTDOT){
                eat(parser,TOKEN_DOTDOT); AST* re=expression(parser); // <<< Use expression()
                if(!re||re->type==AST_NOOP){errorParser(parser,"Bad range end"); freeAST(el); break;}
                // Runtime checks compatibility
                AST* rn=newASTNode(AST_SUBRANGE,NULL); setLeft(rn,el); setRight(rn,re); addChild(sn,rn);
            } else { addChild(sn,el); } // Single element
            if(parser->current_token && parser->current_token->type==TOKEN_COMMA)eat(parser,TOKEN_COMMA); else break;
        }
    }
    if(!parser->current_token || parser->current_token->type!=TOKEN_RBRACKET){errorParser(parser,"Exp ]"); return sn;} eat(parser,TOKEN_RBRACKET); return sn;
}

AST *enumDeclaration(Parser *parser) {
    Token *enumToken = parser->current_token; // Store the TYPE NAME token
    if (!tokenIsIdentifierLike(enumToken)) {
         errorParser(parser, "Expected type name for enum declaration");
         return newASTNode(AST_NOOP, NULL); // Error recovery
    }
    eat(parser, enumToken->type); // Consume type name

    eat(parser, TOKEN_EQUAL);
    eat(parser, TOKEN_LPAREN);

    // Node for the overall Enum Type (e.g., tmyenum)
    // *** FIX 1: Use the correct type name token (enumToken) ***
    AST *node = newASTNode(AST_ENUM_TYPE, enumToken);
    setTypeAST(node, TYPE_ENUM); // Mark the type node itself as ENUM type

    int ordinal = 0;
    char *typeNameStr = strdup(enumToken->value); // Store the type name string for symbol table

    // Parse enumeration values (e.g., valone, valtwo, ...)
    while (currentTokenIsIdentifierLike(parser)) {
        Token *valueToken = parser->current_token; // Token for the VALUE (e.g., valone)
        eat(parser, valueToken->type);

        // Create the AST node for this specific enum VALUE
        AST *valueNode = newASTNode(AST_ENUM_VALUE, valueToken);
        valueNode->i_val = ordinal++; // Set its ordinal value

        // *** FIX 2: Ensure var_type is set for ENUM_VALUE node ***
        setTypeAST(valueNode, TYPE_ENUM); // Mark value node as ENUM type

        // Add valueNode directly as child of 'node' (AST_ENUM_TYPE)
        addChild(node, valueNode);

        // --- Symbol Table Handling (Cleaned up version) ---
        // Insert the symbol for the enum VALUE (e.g., "valone")
        // Pass the parent TYPE node ('node') as the type definition hint.
        insertGlobalSymbol(valueToken->value, TYPE_ENUM, node);
        Symbol *symCheck = lookupGlobalSymbol(valueToken->value);
        if (symCheck && symCheck->value) {
            symCheck->value->enum_val.ordinal = valueNode->i_val; // Ensure ordinal is correct
            /* Enum members are constants for global lookup and caching */
            symCheck->is_const = true;
            // enum_name should be set correctly by insertGlobalSymbol now
        }
        // --- End Symbol Table Handling ---

        if (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else {
            break;
        }
    }

    free(typeNameStr); // Free the duplicated type name string
    eat(parser, TOKEN_RPAREN);

    // Remove the incorrect setLeft call (already done in previous fix)

    insertType(enumToken->value, node); // Register the type name ("tmyenum") linked to the AST_ENUM_TYPE node
    return node; // Return the main AST_ENUM_TYPE node
}

// Parses an expression possibly followed by formatting specifiers.
// Syntax: <expression> [ : <fieldWidth> [ : <decimalPlaces> ] ]
AST *parseWriteArgument(Parser *parser) {
    int expr_line = parser->lexer->line;
    int expr_column = parser->lexer->column;
    
    AST *exprNode = expression(parser); // <<< Use expression()
    if (!exprNode || exprNode->type == AST_NOOP) { errorParser(parser, "Expected expression in write argument"); return newASTNode(AST_NOOP, NULL); }

    if (parser->current_token && parser->current_token->type == TOKEN_COLON) {
        eat(parser, TOKEN_COLON); Token *widthTok = copyToken(parser->current_token);
        if(!widthTok || widthTok->type != TOKEN_INTEGER_CONST){errorParser(parser,"Expected integer constant for field width"); if(widthTok)freeToken(widthTok); return exprNode;}
        eat(parser,TOKEN_INTEGER_CONST);
        Token *precTok = NULL;
        if(parser->current_token && parser->current_token->type==TOKEN_COLON){
             eat(parser,TOKEN_COLON); precTok=copyToken(parser->current_token);
             if(!precTok || precTok->type != TOKEN_INTEGER_CONST){errorParser(parser,"Expected integer constant for decimal places"); if(precTok)freeToken(precTok); precTok=NULL;}
             else eat(parser,TOKEN_INTEGER_CONST);
        }
        AST *fmt = newASTNode(AST_FORMATTED_EXPR,NULL); setLeft(fmt,exprNode);
        int w=atoi(widthTok->value); int p=(precTok)?atoi(precTok->value):-1;
        char fs[32]; snprintf(fs,sizeof(fs),"%d,%d",w,p);
        fmt->token=newToken(TOKEN_STRING_CONST,fs, expr_line, expr_column); // Stores "width,precision"
        setTypeAST(fmt, TYPE_STRING);
        freeToken(widthTok); if(precTok)freeToken(precTok);
        return fmt;
    } else {
        return exprNode; // No formatting
    }
}

// Parses the STR built-in argument list, allowing optional width/precision on the first argument.
static AST *parseStrArgumentList(Parser *parser) {
    AST *args = newASTNode(AST_COMPOUND, NULL);
    if (!args) { return NULL; }

    AST *first = parseWriteArgument(parser);
    if (!first || first->type == AST_NOOP) {
        errorParser(parser, "Expected expression for Str argument");
        freeAST(args);
        return NULL;
    }
    addChild(args, first);

    while (parser->current_token && parser->current_token->type == TOKEN_COMMA) {
        eat(parser, TOKEN_COMMA);
        AST *next = expression(parser);
        if (!next || next->type == AST_NOOP) {
            errorParser(parser, "Expected expression after comma in Str arguments");
            return args;
        }
        addChild(args, next);
    }

    return args;
}

AST *parseArrayInitializer(Parser *parser) {
    if(!parser->current_token || parser->current_token->type!=TOKEN_LPAREN){errorParser(parser,"Exp ("); return NULL;} eat(parser, TOKEN_LPAREN);
    AST *n=newASTNode(AST_ARRAY_LITERAL,NULL); setTypeAST(n,TYPE_ARRAY);
    if(parser->current_token && parser->current_token->type!=TOKEN_RPAREN){
        while(1){
            AST* el=expression(parser); // <<< Use expression()
            if(!el || el->type==AST_NOOP){errorParser(parser,"Bad array init expr"); break;} addChild(n,el);
            if(parser->current_token && parser->current_token->type==TOKEN_COMMA)eat(parser,TOKEN_COMMA); else break;
        }
    }
    if(!parser->current_token || parser->current_token->type!=TOKEN_RPAREN){errorParser(parser,"Exp )"); return n;} eat(parser,TOKEN_RPAREN); return n;
}

Token *peekToken(Parser *parser) {
    // 1. Save the current lexer state
    Lexer backupLexerState = *(parser->lexer);

    // 2. Get the next token (this advances the internal lexer state)
    //    getNextToken allocates memory for the returned token.
    Token *peekedToken = getNextToken(parser->lexer);

    // 3. Restore the original lexer state
    *(parser->lexer) = backupLexerState;

    // 4. Return the peeked token.
    //    IMPORTANT: The caller is responsible for freeing this token later!
    return peekedToken;
}

AST *expression(Parser *parser) {
    AST *node = simpleExpression(parser);
    // Check for NULL or NOOP returned from simpleExpression
    if (!node || node->type == AST_NOOP) {
        // Error should have been reported by simpleExpression or below
        // Return NOOP to signify failure at this level
        return newASTNode(AST_NOOP, NULL);
    }

    // Check for a relational operator
    while (parser->current_token && (
           parser->current_token->type == TOKEN_GREATER || parser->current_token->type == TOKEN_GREATER_EQUAL ||
           parser->current_token->type == TOKEN_EQUAL || parser->current_token->type == TOKEN_LESS ||
           parser->current_token->type == TOKEN_LESS_EQUAL || parser->current_token->type == TOKEN_NOT_EQUAL ||
           parser->current_token->type == TOKEN_IN))
    {
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal); // Copy token before eating
        if(!opCopied) { /* Malloc error */ EXIT_FAILURE_HANDLER(); } // Check copy result
        eat(parser, opOriginal->type); // Eat original token

        AST *right;
        // Parse the right-hand side using simpleExpression for ALL relational operators, including IN.
         right = simpleExpression(parser);
         if (!right || right->type == AST_NOOP) {
             // simpleExpression should report errors
             // errorParser(parser, "Failed to parse right operand for relational operator"); // Already reported
             freeToken(opCopied); if(node) freeAST(node); return newASTNode(AST_NOOP, NULL); // Cleanup
         }

        // Create binary operation node using the copied token
        AST *new_node = newASTNode(AST_BINARY_OP, opCopied);
        freeToken(opCopied); // Free the copied token now

        setLeft(new_node, node); // Link existing node as left child
        setRight(new_node, right); // Link new RHS as right child
        setTypeAST(new_node, TYPE_BOOLEAN); // Relational ops result in Boolean
        node = new_node; // Update main node pointer

        // Standard Pascal does not chain relational operators like a = b = c
        // If followed by AND/OR, that's handled at a different precedence level.
        break; // Exit loop after processing one relational operator
    }
    if (parser->current_token && parser->current_token->type == TOKEN_QUESTION) {
        Token *questionOriginal = parser->current_token;
        Token *questionCopy = copyToken(questionOriginal);
        if (!questionCopy) { EXIT_FAILURE_HANDLER(); }
        eat(parser, TOKEN_QUESTION);

        AST *thenExpr = expression(parser);
        if (!thenExpr || thenExpr->type == AST_NOOP) {
            if (questionCopy) freeToken(questionCopy);
            if (thenExpr && thenExpr->type == AST_NOOP) freeAST(thenExpr);
            if (node) freeAST(node);
            return newASTNode(AST_NOOP, NULL);
        }

        if (!parser->current_token || parser->current_token->type != TOKEN_COLON) {
            errorParser(parser, "Expected ':' in ternary expression");
            if (questionCopy) freeToken(questionCopy);
            freeAST(thenExpr);
            if (node) freeAST(node);
            return newASTNode(AST_NOOP, NULL);
        }
        eat(parser, TOKEN_COLON);

        AST *elseExpr = expression(parser);
        if (!elseExpr || elseExpr->type == AST_NOOP) {
            if (questionCopy) freeToken(questionCopy);
            if (elseExpr && elseExpr->type == AST_NOOP) freeAST(elseExpr);
            freeAST(thenExpr);
            if (node) freeAST(node);
            return newASTNode(AST_NOOP, NULL);
        }

        AST *ternaryNode = newASTNode(AST_TERNARY, questionCopy);
        if (questionCopy) freeToken(questionCopy);
        setLeft(ternaryNode, node);
        setRight(ternaryNode, thenExpr);
        setExtra(ternaryNode, elseExpr);
        setTypeAST(ternaryNode, TYPE_UNKNOWN);
        node = ternaryNode;
    }
    return node;
}

// simpleExpression: Parses terms combined with additive operators (+, -, OR, XOR).
// simpleExpression ::= [ sign ] term { additive_op term }
AST *simpleExpression(Parser *parser) {
    AST *node = NULL;
    Token *signToken = NULL; // To store copied leading sign

    // Handle optional leading sign (+ or -)
    if (parser->current_token && (parser->current_token->type == TOKEN_PLUS || parser->current_token->type == TOKEN_MINUS)) {
        signToken = copyToken(parser->current_token); // Copy token before eating
        if(!signToken){EXIT_FAILURE_HANDLER();}
        eat(parser, parser->current_token->type); // Eat original sign token
    }

    // Parse the first term
    node = pascalTerm(parser);
    if (!node || node->type == AST_NOOP) {
        // term should have reported error
        if(signToken) freeToken(signToken); // Free sign token if unused
        return newASTNode(AST_NOOP, NULL); // Propagate error
    }

    // Apply leading sign if present by creating a unary op node
    if (signToken) {
        AST *unaryNode = newASTNode(AST_UNARY_OP, signToken); // Use copied sign token
        freeToken(signToken); // Free the copied sign token now
        setLeft(unaryNode, node); // Link term as child
        setTypeAST(unaryNode, node->var_type); // Tentative type, eval fixes if needed
        node = unaryNode; // Update main node pointer
    }

    // Loop for additive operators (+, -, OR, XOR)
    while (parser->current_token && (
           parser->current_token->type == TOKEN_PLUS || parser->current_token->type == TOKEN_MINUS ||
           parser->current_token->type == TOKEN_OR || parser->current_token->type == TOKEN_XOR ))
    {
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal); // Copy token before eating
        if (!opCopied) { EXIT_FAILURE_HANDLER(); }
        eat(parser, opOriginal->type); // Eat original op token

        AST *right = pascalTerm(parser); // Parse the next term
        if (!right || right->type == AST_NOOP) {
            // term should have reported error
            // errorParser(parser, "Expected term after additive operator"); // Already reported
            freeToken(opCopied); return node; // Return partially built tree on error
        }

        // Create binary op node using copied token
        AST *new_node = newASTNode(AST_BINARY_OP, opCopied);
        freeToken(opCopied); // Free copied token

        setLeft(new_node, node); // Link previous tree as left
        setRight(new_node, right); // Link new term as right
        // Infer type (eval will do final checks)
        setTypeAST(new_node, inferBinaryOpType(node->var_type, right->var_type));
        node = new_node; // Update main node pointer
    }
    return node;
}

// term: Parses factors combined with multiplicative operators (*, /, DIV, MOD, AND, SHL, SHR).
// term ::= factor { multiplicative_op factor }
AST *pascalTerm(Parser *parser) {
    AST *node = factor(parser);
    if (!node || node->type == AST_NOOP) {
       // factor should have reported error
       return newASTNode(AST_NOOP, NULL); // Propagate error
    }

    // Loop for multiplicative operators
    while (parser->current_token && (
           parser->current_token->type == TOKEN_MUL || parser->current_token->type == TOKEN_SLASH ||
           parser->current_token->type == TOKEN_INT_DIV || parser->current_token->type == TOKEN_MOD ||
           parser->current_token->type == TOKEN_AND || parser->current_token->type == TOKEN_SHL ||
           parser->current_token->type == TOKEN_SHR))
    {
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal); // Copy token before eating
        if (!opCopied) { EXIT_FAILURE_HANDLER(); }
        eat(parser, opOriginal->type); // Eat original op token

        AST *right = factor(parser); // Parse the next factor
        if (!right || right->type == AST_NOOP) {
            // factor should have reported error
            // errorParser(parser, "Expected factor after multiplicative operator"); // Already reported
            freeToken(opCopied); return node; // Return partially built tree
        }

        // Create binary op node using copied token
        AST *new_node = newASTNode(AST_BINARY_OP, opCopied);
        freeToken(opCopied); // Free copied token

        setLeft(new_node, node); // Link previous tree as left
        setRight(new_node, right); // Link new factor as right
        // Infer type (eval will do final checks)
        setTypeAST(new_node, inferBinaryOpType(node->var_type, right->var_type));
        node = new_node; // Update main node pointer
    }
    return node;
}

// factor: Parses literals, variables, function calls, NOT, parenthesized expressions, set constructors, and '@' addr-of.
// factor ::= literal | variable | function_call | '(' expression ')' | NOT factor | '[' set_elements ']' | '@' IDENTIFIER
AST *factor(Parser *parser) {
    // *** Use initial token type and pointer like in typeSpecifier ***
    Token *initialToken = parser->current_token;
    TokenType initialTokenType = initialToken ? initialToken->type : TOKEN_UNKNOWN;
    AST *node = NULL;

    if (initialTokenType == TOKEN_UNKNOWN) {
        errorParser(parser, "Unexpected end of input in factor");
        return newASTNode(AST_NOOP, NULL);
    }

    #ifdef DEBUG
    // Optional: Add logging if needed
    // if (dumpExec) fprintf(stderr, "[DEBUG_FACTOR] Entry: Token %s ('%s')\n", tokenTypeToString(initialTokenType), initialToken->value ? initialToken->value : "NULL");
    #endif

    if (initialTokenType == TOKEN_SPAWN) {
        node = spawnStatement(parser);
        return node;
    } else if (initialTokenType == TOKEN_NIL) {
        Token* c = copyToken(initialToken); // Copy the initial NIL token
        eat(parser, TOKEN_NIL);             // Consume NIL, frees original initialToken
        node = newASTNode(AST_NIL, c);      // Create node with the copy
        freeToken(c);                       // Free the copy
        // Set the AST node's type to TYPE_NIL, aligning with the Value representation.
        setTypeAST(node, TYPE_NIL);
        return node; // <<< RETURN IMMEDIATELY
    } else if (initialTokenType == TOKEN_TRUE || initialTokenType == TOKEN_FALSE) {
        Token* c = copyToken(initialToken);
        if (!c && initialToken) { /* Malloc error */ EXIT_FAILURE_HANDLER(); } // Check copyToken result
        eat(parser, initialTokenType);
        node = newASTNode(AST_BOOLEAN, c);
        if (!node && c) { /* Malloc error */ freeToken(c); EXIT_FAILURE_HANDLER(); } // Check newASTNode result
        
        // newASTNode might have made its own copy of c for node->token,
        // so c itself can be freed after newASTNode uses it.
        if (c) freeToken(c);

        setTypeAST(node, TYPE_BOOLEAN);
        
        // >>> ADD THIS LINE BACK <<<
        node->i_val = (initialTokenType == TOKEN_TRUE) ? 1 : 0;

        #ifdef DEBUG
        if(dumpExec && node && node->token) { // Added NULL checks for node and node->token
            fprintf(stderr, "PARSER factor() AST_BOOLEAN: token=%s, node->i_val SET TO %d\n",
                    node->token->value, node->i_val);
        } else if (dumpExec && node) {
             fprintf(stderr, "PARSER factor() AST_BOOLEAN: token=NULL_TOKEN_IN_NODE, node->i_val SET TO %d\n", node->i_val);
        }
        #endif
        return node;

    } else if (initialTokenType == TOKEN_NOT) {
        Token* c = copyToken(initialToken);
        eat(parser, TOKEN_NOT); // Eat NOT
        node = newASTNode(AST_UNARY_OP, c); freeToken(c);
        // Recursively call factor - this will get the *next* token correctly
        AST* op = factor(parser);
        if(!op || op->type == AST_NOOP){ errorParser(parser,"Exp operand after NOT"); freeAST(node); return newASTNode(AST_NOOP, NULL);}
        setLeft(node, op);
        setTypeAST(node, TYPE_BOOLEAN);
        return node; // <<< RETURN IMMEDIATELY

    } else if (initialTokenType == TOKEN_PLUS || initialTokenType == TOKEN_MINUS) {
        Token* c = copyToken(initialToken);
        eat(parser, initialTokenType); // Eat unary +/-
        node = newASTNode(AST_UNARY_OP, c); freeToken(c);
        // Recursively call factor
        AST* op = factor(parser);
        if(!op || op->type == AST_NOOP){ errorParser(parser,"Exp operand after unary +/-"); freeAST(node); return newASTNode(AST_NOOP, NULL);}
        setLeft(node, op);
        // Type depends on operand, set during annotation or eval
        setTypeAST(node, op->var_type); // Tentative type
        return node; // <<< RETURN IMMEDIATELY

    } else if (initialTokenType == TOKEN_INTEGER_CONST || initialTokenType == TOKEN_HEX_CONST || initialTokenType == TOKEN_REAL_CONST) {
        Token* c = copyToken(initialToken);
        eat(parser, initialTokenType); // Eat the number token
        node = newASTNode(AST_NUMBER, c); freeToken(c);
        setTypeAST(node, (initialTokenType == TOKEN_REAL_CONST) ? TYPE_REAL : TYPE_INTEGER);
        return node; // <<< RETURN IMMEDIATELY

    } else if (initialTokenType == TOKEN_STRING_CONST) {
        /* Concatenate immediately adjacent string tokens (including `#nn` char codes)
         * into a single literal.  The lexer represents `#0` with `length == 0`, so
         * treat those as single-byte segments while copying. */
        size_t bufferLen = 0;
        size_t bufferCap = 0;
        char *buffer = NULL;
        bool allCharCodes = true;
        int initialLine = initialToken ? initialToken->line : 0;
        int initialColumn = initialToken ? initialToken->column : 0;

        while (parser->current_token && parser->current_token->type == TOKEN_STRING_CONST) {
            Token *segment = parser->current_token;
            size_t chunkLen = segment->length;
            if (segment->is_char_code && chunkLen == 0) {
                chunkLen = 1;
            }

            size_t required = bufferLen + chunkLen + 1; // keep space for terminator
            if (required > bufferCap) {
                size_t newCap = bufferCap ? bufferCap : 16;
                while (newCap < required) {
                    if (newCap > SIZE_MAX / 2) {
                        newCap = required;
                        break;
                    }
                    newCap *= 2;
                }
                char *resized = realloc(buffer, newCap);
                if (!resized) {
                    free(buffer);
                    fprintf(stderr, "Memory allocation error concatenating string literal\n");
                    EXIT_FAILURE_HANDLER();
                }
                buffer = resized;
                bufferCap = newCap;
            }

            if (chunkLen > 0 && segment->value) {
                memcpy(buffer + bufferLen, segment->value, chunkLen);
                bufferLen += chunkLen;
            }

            if (!segment->is_char_code && chunkLen > 0) {
                allCharCodes = false;
            }

            eat(parser, TOKEN_STRING_CONST);
        }

        if (!buffer) {
            bufferCap = 1;
            buffer = malloc(bufferCap);
            if (!buffer) {
                fprintf(stderr, "Memory allocation error concatenating empty string literal\n");
                EXIT_FAILURE_HANDLER();
            }
        }

        buffer[bufferLen] = '\0';

        Token combinedToken;
        combinedToken.type = TOKEN_STRING_CONST;
        combinedToken.value = buffer;
        combinedToken.length = bufferLen;
        combinedToken.line = initialLine;
        combinedToken.column = initialColumn;
        combinedToken.is_char_code = (bufferLen == 1) && allCharCodes;

        node = newASTNode(AST_STRING, &combinedToken);
        free(buffer);

        setTypeAST(node, (bufferLen == 1) ? TYPE_CHAR : TYPE_STRING);
        return node; // <<< RETURN IMMEDIATELY

    } else if (initialTokenType == TOKEN_IDENTIFIER) {
        // IDENTIFIER case: call lvalue, which handles ., [], ^ and eats tokens internally
        node = lvalue(parser); // lvalue consumes the identifier and potential subsequent tokens
        if (!node || node->type == AST_NOOP) return newASTNode(AST_NOOP, NULL);

        // Check if it's a function call (lvalue doesn't handle the LPAREN)
        if (parser->current_token && parser->current_token->type == TOKEN_LPAREN && node->type == AST_VARIABLE) {
             // It's a function call. Create a NEW procedure call node.
             AST* funcCallNode = newASTNode(AST_PROCEDURE_CALL, node->token);
             freeAST(node); // Free the original AST_VARIABLE node.
             node = funcCallNode; // The new procedure call node is now our main node.

             eat(parser, TOKEN_LPAREN);      // Eat '('
             bool isStrCall = (node->token && node->token->value && strcasecmp(node->token->value, "str") == 0);
             if (parser->current_token && parser->current_token->type != TOKEN_RPAREN) {
                 AST* args = isStrCall ? parseStrArgumentList(parser) : exprList(parser); // Parse argument list
                 if (!args || args->type == AST_NOOP) { errorParser(parser,"Bad arg list"); return node; }
                 // Transfer arguments safely
                 if (args && args->type == AST_COMPOUND && args->child_count > 0) {
                      node->children = args->children; node->child_count = args->child_count; node->child_capacity = args->child_capacity;
                      args->children = NULL; args->child_count = 0; args->child_capacity = 0;
                      for(int i=0; i < node->child_count; i++) if(node->children[i]) node->children[i]->parent = node;
                  } else { node->children = NULL; node->child_count = 0; node->child_capacity=0; }
                  if(args) freeAST(args); // Free compound wrapper
             } else { node->children = NULL; node->child_count = 0; node->child_capacity=0; } // Empty arg list '()'
             if (!parser->current_token || parser->current_token->type != TOKEN_RPAREN) { errorParser(parser,"Expected ) after args"); return node;}
             eat(parser, TOKEN_RPAREN); // Eat ')'

            // Builtin low/high should return the same type as their argument.
            // Previously we only special-cased `char`, leaving other types as VOID,
            // which later triggered "expects type INTEGER but got VOID" errors
            // when low/high were used in expressions.  Infer the type from the
            // provided type identifier so callers see the correct return type.
            if (node->token && isBuiltin(node->token->value) && node->child_count == 1) {
                if (strcasecmp(node->token->value, "low") == 0 ||
                    strcasecmp(node->token->value, "high") == 0) {
                    AST* arg0 = node->children[0];
                    if (arg0 && arg0->type == AST_VARIABLE && arg0->token) {
                        AST* typeDef = lookupType(arg0->token->value);
                        if (typeDef) {
                            if (typeDef->type == AST_TYPE_REFERENCE) typeDef = typeDef->right;
                            setTypeAST(node, typeDef->var_type);
                        }
                    }
                }
            }
             // Type annotation will set the function's return type later
        }
        // Check if lvalue returned a simple variable that might be a parameter-less function
        else if (node->type == AST_VARIABLE) {
                // <<<< MODIFICATION START >>>>
                // Check for built-in functions FIRST, as they are a special case.
                if (node->token && isBuiltin(node->token->value) && getBuiltinType(node->token->value) == BUILTIN_TYPE_FUNCTION) {
                    
                    #ifdef DEBUG
                    fprintf(stderr, "[DEBUG factor] IDENTIFIER '%s' is a built-in FUNCTION. Converting to AST_PROCEDURE_CALL.\n", node->token->value);
                    #endif

                    node->type = AST_PROCEDURE_CALL;
                    node->children = NULL;
                    node->child_count = 0;
                    node->child_capacity = 0;
                    // Set return type from our known built-in return types.
                    // This avoids relying on the dummy AST from the procedure_table.
                    setTypeAST(node, getBuiltinReturnType(node->token->value));
                
                } else {
                    // If not a built-in, check the user-defined procedure table.
                    Symbol *procSym = node->token ? lookupProcedure(node->token->value) : NULL;

                    if (procSym && procSym->type_def && procSym->type_def->type == AST_FUNCTION_DECL) {
                        
                        #ifdef DEBUG
                        fprintf(stderr, "[DEBUG factor] IDENTIFIER '%s' is a user-defined FUNCTION. Converting to AST_PROCEDURE_CALL.\n", node->token->value);
                        #endif

                        node->type = AST_PROCEDURE_CALL; // Change type to parameter-less call
                        node->children = NULL;
                        node->child_count = 0;
                        node->child_capacity = 0;
                        
                        if (procSym->type_def->right) {
                            setTypeAST(node, procSym->type_def->right->var_type);
                        } else {
                            setTypeAST(node, procSym->type);
                        }

                    } else if (procSym) { // It's a procedure (not a function) used as a value
                        // Treat bare procedure identifiers in expression context as their address.
                        // This allows CreateThread(Worker) as a convenience for @Worker.
                        AST* addrNode = newASTNode(AST_ADDR_OF, NULL);
                        setLeft(addrNode, node); // Transfer ownership of variable node as child
                        setTypeAST(addrNode, TYPE_POINTER);
                        node = addrNode;
                    }
                    // Otherwise, it's just a variable/constant reference.
                }
                // <<<< MODIFICATION END >>>>
            }
        // If lvalue returned DEREFERENCE, FIELD_ACCESS, ARRAY_ACCESS, it's used as is.
        // No immediate return needed here as lvalue consumed the relevant tokens.
        // Flow continues to end.

    } else if (initialTokenType == TOKEN_AT) {
        // Address-of operator: @Identifier, @Array[Index], etc.
        Token* atTok = copyToken(initialToken);
        eat(parser, TOKEN_AT);

        AST* target = NULL;
        if (currentTokenIsIdentifierLike(parser)) {
            target = lvalue(parser);
        } else {
            errorParser(parser, "Expected addressable expression after '@'");
            if (atTok) freeToken(atTok);
            return newASTNode(AST_NOOP, NULL);
        }

        if (!target || target->type == AST_NOOP) {
            if (atTok) freeToken(atTok);
            return target ? target : newASTNode(AST_NOOP, NULL);
        }

        AST* addrNode = newASTNode(AST_ADDR_OF, atTok);
        if (atTok) freeToken(atTok);
        setLeft(addrNode, target);
        // Type will be annotated later (typically TYPE_POINTER)
        return addrNode;

    } else if (initialTokenType == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN); // Eat '('
        node = expression(parser); // Parse sub-expression
        if (!node || node->type == AST_NOOP) { return newASTNode(AST_NOOP, NULL); }
        if (!parser->current_token || parser->current_token->type != TOKEN_RPAREN) {
            errorParser(parser,"Expected )");
            // If error, free the parsed sub-expression?
            freeAST(node);
            return newASTNode(AST_NOOP, NULL);
        }
        eat(parser, TOKEN_RPAREN); // Eat ')'
        // Flow continues to end, return node

    } else if (initialTokenType == TOKEN_LBRACKET) {
        // Parse set constructor '[ ... ]'
        node = parseSetConstructor(parser); // Consumes tokens internally
        if (!node || node->type == AST_NOOP) { return newASTNode(AST_NOOP, NULL); }
        setTypeAST(node, TYPE_SET);
        // Flow continues to end, return node

    } else {
        errorParser(parser, "Unexpected token in factor");
        return newASTNode(AST_NOOP, NULL);
    }

    // If node is still NULL after all checks, something went wrong internally
    if (!node) {
        errorParser(parser, "Internal error: factor resulted in NULL node");
        return newASTNode(AST_NOOP, NULL);
    }

    while (parser->current_token &&
           (parser->current_token->type == TOKEN_AS || parser->current_token->type == TOKEN_IS)) {
        TokenType opType = parser->current_token->type;
        Token *opCopy = copyToken(parser->current_token);
        if (!opCopy) {
            EXIT_FAILURE_HANDLER();
        }
        eat(parser, opType);

        AST *targetType = parseTypeAssertionTarget(parser, opType);
        if (!targetType || targetType->type == AST_NOOP) {
            if (opCopy) freeToken(opCopy);
            if (targetType) freeAST(targetType);
            freeAST(node);
            return newASTNode(AST_NOOP, NULL);
        }

        AST *assertNode = newASTNode(AST_TYPE_ASSERT, opCopy);
        if (opCopy) freeToken(opCopy);
        setLeft(assertNode, node);
        setRight(assertNode, targetType);

        AST *resolvedTarget = targetType->type_def ? targetType->type_def : targetType->right;
        if (!resolvedTarget) {
            resolvedTarget = targetType;
        }
        if (resolvedTarget) {
            setTypeAST(assertNode, resolvedTarget->var_type);
        } else {
            setTypeAST(assertNode, targetType->var_type);
        }
        assertNode->type_def = resolvedTarget;
        node = assertNode;
    }

#ifdef DEBUG
if (dumpExec && node && node->token) { // Ensure node and node->token are not NULL
    fprintf(stderr, "[DEBUG_FACTOR_EXIT] Returning from factor(): initialTokenType=%s, node->type=%s, node->token->value='%s', node->token->type=%s\n",
            tokenTypeToString(initialTokenType),
            astTypeToString(node->type),
            node->token->value ? node->token->value : "NULL_VAL",
            tokenTypeToString(node->token->type));
    fflush(stderr);
} else if (dumpExec && node) {
    fprintf(stderr, "[DEBUG_FACTOR_EXIT] Returning from factor(): initialTokenType=%s, node->type=%s, node->token=NULL\n",
            tokenTypeToString(initialTokenType),
            astTypeToString(node->type));
    fflush(stderr);
} else if (dumpExec) {
    fprintf(stderr, "[DEBUG_FACTOR_EXIT] Returning from factor(): initialTokenType=%s, node is NULL\n",
            tokenTypeToString(initialTokenType));
    fflush(stderr);
}
#endif

    return node; // Return the created node
}

// --- Function to specifically parse ^TypeName ---
AST *parsePointerType(Parser *parser) {
    eat(parser, TOKEN_CARET); // Consume '^'

    // The next token *must* be a type identifier
    if (!tokenIsIdentifierLike(parser->current_token)) {
        errorParser(parser, "Expected type identifier after '^'");
        return NULL; // Indicate error
    }

    // --- Create a temporary node representing the base type identifier ---
    // This is needed to look up the type definition later during annotation.
    // We use AST_VARIABLE temporarily, but it signifies a type name here.
    // Alternatively, could use AST_TYPE_REFERENCE directly if preferred. Let's use VARIABLE for now.
    AST *baseTypeNameNode = newASTNode(AST_VARIABLE, parser->current_token); // Uses copy of token
    if (!baseTypeNameNode) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
    // Set its var_type based on lookup or built-in check (optional here, annotation will do it)
    VarType baseVt = TYPE_VOID; // Placeholder
    if (strcasecmp(baseTypeNameNode->token->value, "integer") == 0) baseVt = TYPE_INTEGER;
    else if (strcasecmp(baseTypeNameNode->token->value, "real") == 0) baseVt = TYPE_REAL;
    // ... add other built-ins ...
    else {
        AST* lookedUpType = lookupType(baseTypeNameNode->token->value);
        if (lookedUpType) baseVt = lookedUpType->var_type;
    }
    setTypeAST(baseTypeNameNode, baseVt); // Set tentative base type

    eat(parser, parser->current_token->type); // Consume the base type identifier

    // --- Create the AST_POINTER_TYPE node ---
    AST *pointerTypeNode = newASTNode(AST_POINTER_TYPE, NULL); // No specific token for the pointer type itself
    if (!pointerTypeNode) { freeAST(baseTypeNameNode); EXIT_FAILURE_HANDLER(); }

    // Set the base type node as the 'right' child (consistent with array/record having base type in right)
    setRight(pointerTypeNode, baseTypeNameNode);

    // Set the overall type of this node to TYPE_POINTER
    setTypeAST(pointerTypeNode, TYPE_POINTER);

    return pointerTypeNode;
}
static AST *parseTypeAssertionTarget(Parser *parser, TokenType keywordToken) {
    if (!parser || !parser->current_token || !tokenIsIdentifierLike(parser->current_token)) {
        const char *kw = (keywordToken == TOKEN_IS) ? "'is'" : "'as'";
        char message[128];
        snprintf(message, sizeof(message), "Expected type name after %s", kw);
        errorParser(parser, message);
        return newASTNode(AST_NOOP, NULL);
    }

    Token *typeTokenCopy = copyToken(parser->current_token);
    if (!typeTokenCopy) {
        EXIT_FAILURE_HANDLER();
    }

    const char *typeNameSource = parser->current_token->value;
    char *typeNameCopy = NULL;
    if (typeNameSource) {
        typeNameCopy = strdup(typeNameSource);
        if (!typeNameCopy) {
            if (typeTokenCopy) freeToken(typeTokenCopy);
            EXIT_FAILURE_HANDLER();
        }
    }
    TokenType typeToken = parser->current_token->type;
    eat(parser, typeToken);

    if (typeNameCopy) {
        toLowerString(typeNameCopy);
    }
    AST *resolvedType = typeNameCopy ? lookupType(typeNameCopy) : NULL;
    if (!resolvedType) {
        char message[160];
        snprintf(message, sizeof(message), "Unknown type '%s' in type assertion", typeNameCopy ? typeNameCopy : "<anonymous>");
        errorParser(parser, message);
        if (typeTokenCopy) freeToken(typeTokenCopy);
        if (typeNameCopy) free(typeNameCopy);
        return newASTNode(AST_NOOP, NULL);
    }

    AST *typeRef = newASTNode(AST_TYPE_REFERENCE, typeTokenCopy);
    if (typeTokenCopy) freeToken(typeTokenCopy);
    setTypeAST(typeRef, resolvedType->var_type);
    typeRef->right = resolvedType;
    typeRef->type_def = resolvedType;
    if (typeNameCopy) free(typeNameCopy);
    return typeRef;
}

// parser.c
// The parsing routines for pscal

#include "list.h"
#include "parser.h"
#include "utils.h"
#include "types.h"
#include "globals.h"
#include "builtin.h"
#include "interpreter.h"
#include "symbol.h"
#include <stdbool.h>
#include <stdio.h>  

// Define the helper function *only* when DEBUG is enabled
// No 'static inline' needed here as it's defined only once in this file.
#ifdef DEBUG
void eat_debug_wrapper(Parser *parser_ptr, TokenType expected_token_type, const char* func_name) {
    // Test print BEFORE the main fprintf
    fprintf(stderr, "[DEBUG eat_debug_wrapper] ENTERED from %s. Expecting: %s. Current token type: %s.\n",
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
    fprintf(stderr, "[DEBUG eat_debug_wrapper] Calling eatInternal.\n");
    fflush(stderr);

    eatInternal(parser_ptr, expected_token_type);

    fprintf(stderr, "[DEBUG eat_debug_wrapper] RETURNED from eatInternal.\n");
    fflush(stderr);
}
#endif // DEBUG

AST *parseWriteArgument(Parser *parser);

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

        if (parser->current_token->type == TOKEN_CONST) {
            eat(parser, TOKEN_CONST);
            // Loop for multiple constant declarations within a single CONST block
            while (parser->current_token && parser->current_token->type == TOKEN_IDENTIFIER) {
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
            while (parser->current_token && parser->current_token->type == TOKEN_IDENTIFIER) {
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
            while (parser->current_token && parser->current_token->type == TOKEN_IDENTIFIER) {
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
                    if (parser->current_token && parser->current_token->type == TOKEN_IDENTIFIER) {
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

    if (parser->current_token->type == type) {
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

Symbol *lookupProcedure(const char *name_to_lookup) { // << CHANGED return type
    if (!procedure_table || !name_to_lookup) {
        return NULL;
    }
    // hashTableLookup returns Symbol*, no cast needed.
    return hashTableLookup(procedure_table, name_to_lookup);
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

    if (!ident_token_snapshot || ident_token_snapshot->type != TOKEN_IDENTIFIER) {
        errorParser(parser, "Expected identifier at start of lvalue");
        return newASTNode(AST_NOOP, NULL); // Return a NOOP node on error
    }

    // Create the base variable node.
    // newASTNode should make its own copy of the token if it needs to persist it.
    AST *node = newASTNode(AST_VARIABLE, ident_token_snapshot);
    eat(parser, TOKEN_IDENTIFIER); // Consume the first identifier

    // Loop for subsequent field access '.', array '[]', or pointer '^'
    while (parser->current_token &&
           (parser->current_token->type == TOKEN_PERIOD ||
            parser->current_token->type == TOKEN_LBRACKET ||
            parser->current_token->type == TOKEN_CARET)) {
        
        if (parser->current_token->type == TOKEN_PERIOD) {
            eat(parser, TOKEN_PERIOD); // Consume '.'
            Token *field_token_snapshot = parser->current_token;
            if (!field_token_snapshot || field_token_snapshot->type != TOKEN_IDENTIFIER) {
                errorParser(parser, "Expected field name after '.'");
                // 'node' is the AST built so far (the record variable); return it.
                return node;
            }
            // For AST_FIELD_ACCESS, the node's token is the field name itself.
            AST *fa_node = newASTNode(AST_FIELD_ACCESS, field_token_snapshot);
            eat(parser, TOKEN_IDENTIFIER); // Consume the field identifier

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
    eat(parser, TOKEN_ARRAY); if(!parser->current_token || parser->current_token->type != TOKEN_LBRACKET){errorParser(parser,"Exp ["); return NULL;} eat(parser, TOKEN_LBRACKET);
    AST *indexList = newASTNode(AST_COMPOUND, NULL);
    while (1) {
        AST *lower = expression(parser); // <<< Use expression()
        if (!lower || lower->type == AST_NOOP) { errorParser(parser, "Bad lower bound"); freeAST(indexList); return NULL; }
        if (!parser->current_token || parser->current_token->type != TOKEN_DOTDOT) { errorParser(parser, "Expected .."); freeAST(lower); freeAST(indexList); return NULL; } eat(parser, TOKEN_DOTDOT);
        AST *upper = expression(parser); // <<< Use expression()
        if (!upper || upper->type == AST_NOOP) { errorParser(parser, "Bad upper bound"); freeAST(lower); freeAST(indexList); return NULL; }
        AST *range = newASTNode(AST_SUBRANGE, NULL); setLeft(range, lower); setRight(range, upper); addChild(indexList, range);
        if (parser->current_token && parser->current_token->type == TOKEN_COMMA) eat(parser, TOKEN_COMMA);
        else break;
    }
    if (!parser->current_token || parser->current_token->type != TOKEN_RBRACKET) { errorParser(parser,"Exp ]"); freeAST(indexList); return NULL; } eat(parser, TOKEN_RBRACKET);
    if (!parser->current_token || parser->current_token->type != TOKEN_OF) { errorParser(parser,"Exp OF"); freeAST(indexList); return NULL; } eat(parser, TOKEN_OF);
    AST *elemType = typeSpecifier(parser, 1); if (!elemType || elemType->type == AST_NOOP) { errorParser(parser, "Bad element type"); freeAST(indexList); return NULL; }
    AST *node = newASTNode(AST_ARRAY_TYPE, NULL); setTypeAST(node, TYPE_ARRAY);
    // Transfer children safely (keep existing logic)
    if (indexList->child_count > 0) { node->children=indexList->children; node->child_count=indexList->child_count; node->child_capacity=indexList->child_capacity; for(int i=0;i<node->child_count;++i)if(node->children[i])node->children[i]->parent=node; indexList->children=NULL; indexList->child_count=0; indexList->child_capacity=0; } else { node->children=NULL; node->child_count=0; node->child_capacity=0; }
    free(indexList); // Free temp compound struct
    setRight(node, elemType);
    return node;
}

AST *unitParser(Parser *parser_for_this_unit, int recursion_depth, const char* unit_name_being_parsed) {
    if (recursion_depth > MAX_RECURSION_DEPTH) { /* error */ EXIT_FAILURE_HANDLER(); }

    Token* unit_keyword_token_copy = copyToken(parser_for_this_unit->current_token);
    eat(parser_for_this_unit, TOKEN_UNIT);
    
    Token *unit_name_token_original = parser_for_this_unit->current_token;
    if (!unit_name_token_original || unit_name_token_original->type != TOKEN_IDENTIFIER) { /* error */ freeToken(unit_keyword_token_copy); EXIT_FAILURE_HANDLER(); }
    
    // The unit_node's token should be the unit name.
    Token *unit_name_token_copy_for_ast = copyToken(unit_name_token_original);
    AST *unit_node = newASTNode(AST_UNIT, unit_name_token_copy_for_ast);
    freeToken(unit_name_token_copy_for_ast);
    freeToken(unit_keyword_token_copy);

    // Use the unit_name_being_parsed that was passed in.
    // char *parsed_unit_name_str = strdup(unit_name_being_parsed); // Already lowercased if from listGet
    // This should be passed by the caller (findUnitFile can return the name part)
    // Or, if unit_name_being_parsed is the one from unit_name_token_original->value:
    char *lower_unit_name_ctx = strdup(unit_name_token_original->value);
    for(int k=0; lower_unit_name_ctx[k]; k++) lower_unit_name_ctx[k] = tolower(lower_unit_name_ctx[k]);
    parser_for_this_unit->current_unit_name_context = lower_unit_name_ctx; // SET CONTEXT (will be freed at end)

    eat(parser_for_this_unit, TOKEN_IDENTIFIER);
    eat(parser_for_this_unit, TOKEN_SEMICOLON);

    AST *uses_clause = NULL;
    List *unit_list_for_this_unit = NULL;
    if (parser_for_this_unit->current_token && parser_for_this_unit->current_token->type == TOKEN_USES) {
        eat(parser_for_this_unit, TOKEN_USES);
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        unit_list_for_this_unit = createList(); // <<<< ASSIGN VALUE HERE
        while (parser_for_this_unit->current_token && parser_for_this_unit->current_token->type == TOKEN_IDENTIFIER) {
            char* temp_unit_name = strdup(parser_for_this_unit->current_token->value);
            if (!temp_unit_name) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
            // Convert to lower case for consistent lookup if findUnitFile expects lowercase
            for(int k=0; temp_unit_name[k]; k++) temp_unit_name[k] = tolower(temp_unit_name[k]);
            listAppend(unit_list_for_this_unit, temp_unit_name);
            free(temp_unit_name); // listAppend makes its own copy

            eat(parser_for_this_unit, TOKEN_IDENTIFIER);
            if (parser_for_this_unit->current_token && parser_for_this_unit->current_token->type == TOKEN_COMMA) {
                eat(parser_for_this_unit, TOKEN_COMMA);
            } else {
                break;
            }
        }
        eat(parser_for_this_unit, TOKEN_SEMICOLON);
        uses_clause->unit_list = unit_list_for_this_unit; // Assign to AST node
        // Now, you need to loop through 'unit_list_for_this_unit' to parse nested units
        // This part was in my previous comment as "// ... (Recursively call unitParser for each used unit...)"
        // For example:
        for (int i = 0; i < listSize(unit_list_for_this_unit); i++) {
            char *nested_unit_name = listGet(unit_list_for_this_unit, i); // listGet returns char*
            // Ensure nested_unit_name is lowercased if findUnitFile expects that,
            // (already done before listAppend if findUnitFile needs lowercase)

            char *nested_unit_path = findUnitFile(nested_unit_name); // findUnitFile allocates
            if (!nested_unit_path) {
                fprintf(stderr, "Error: Unit file for '%s' not found.\n", nested_unit_name);
                // freeList(unit_list_for_this_unit); // Clean up list before exit
                // ... other cleanup ...
                EXIT_FAILURE_HANDLER();
            }

            char *unit_source_buffer = NULL;
            FILE *nested_file = fopen(nested_unit_path, "r");
            if (nested_file) {
                fseek(nested_file, 0, SEEK_END);
                long nested_fsize = ftell(nested_file);
                rewind(nested_file);
                unit_source_buffer = malloc(nested_fsize + 1);
                if (!unit_source_buffer) { /* error */ fclose(nested_file); free(nested_unit_path); EXIT_FAILURE_HANDLER(); }
                fread(unit_source_buffer, 1, nested_fsize, nested_file);
                unit_source_buffer[nested_fsize] = '\0';
                fclose(nested_file);
            } else {
                fprintf(stderr, "Error: Could not open unit file '%s'.\n", nested_unit_path);
                free(nested_unit_path);
                // ... other cleanup ...
                EXIT_FAILURE_HANDLER();
            }
            free(nested_unit_path);

            Lexer nested_lexer;
            initLexer(&nested_lexer, unit_source_buffer);
            Parser nested_parser_instance; // Create a NEW parser instance for the nested unit
            nested_parser_instance.lexer = &nested_lexer;
            nested_parser_instance.current_token = getNextToken(&nested_lexer);
            // nested_parser_instance.current_unit_name_context will be set by the recursive call

            AST *parsed_nested_unit_ast = unitParser(&nested_parser_instance, recursion_depth + 1, nested_unit_name);

            if (nested_parser_instance.current_token) freeToken(nested_parser_instance.current_token);
            if (unit_source_buffer) free(unit_source_buffer);

            if (parsed_nested_unit_ast) {
                linkUnit(parsed_nested_unit_ast, recursion_depth + 1);
                // linkUnit should handle freeing parsed_nested_unit_ast if it's temporary
            }
        }
    }
    if(uses_clause) addChild(unit_node, uses_clause);

    eat(parser_for_this_unit, TOKEN_INTERFACE);
    AST *interface_decls = declarations(parser_for_this_unit, true); // Pass this unit's parser
    setLeft(unit_node, interface_decls);
    
    Symbol *unitSymTable = buildUnitSymbolTable(interface_decls);
    unit_node->symbol_table = unitSymTable;

    eat(parser_for_this_unit, TOKEN_IMPLEMENTATION);
    AST *impl_decls = declarations(parser_for_this_unit, false); // Pass this unit's parser
    setExtra(unit_node, impl_decls);
    
    // ... (Process IMPLEMENTATION VAR/CONST, these are global to the program but defined in unit's impl) ...
    // (This part of your code that inserts them into globalSymbols directly is okay)

    if (parser_for_this_unit->current_token && parser_for_this_unit->current_token->type == TOKEN_BEGIN) {
        AST *init_block = compoundStatement(parser_for_this_unit); // Pass parser
        setRight(unit_node, init_block);
        eat(parser_for_this_unit, TOKEN_PERIOD);
    } else {
        eat(parser_for_this_unit, TOKEN_END);
        eat(parser_for_this_unit, TOKEN_PERIOD);
    }

    parser_for_this_unit->current_unit_name_context = NULL; // Clear context
    free(lower_unit_name_ctx); // Free the strdup'd context name
    return unit_node;
}

void errorParser(Parser *parser, const char *msg) {
    fprintf(stderr, "Parser error at line %d, column %d: %s (found token: %s)\n",
            parser->lexer->line, parser->lexer->column, msg,
            tokenTypeToString(parser->current_token->type));
    EXIT_FAILURE_HANDLER();
}

void addProcedure(AST *proc_decl_ast_original, const char* unit_context_name_param_for_addproc) {
    // Create the name for the symbol table. This might involve mangling
    // with unit_context_name_param_for_addproc if it's not NULL.
    // For simplicity, let's assume for now the name is directly from the token,
    // and unit qualification is handled by lookup.
    // You will need to implement proper name construction here.

    char *proc_name_original = proc_decl_ast_original->token->value;
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
    sym->next = NULL;

    if (procedure_table) {
        hashTableInsert(procedure_table, sym);
    } else {
        // This should not happen if main initializes procedure_table
        fprintf(stderr, "CRITICAL Error: procedure_table hash table not initialized before addProcedure call.\n");
        // Proper cleanup if not inserting
        if (sym->name) free(sym->name);
        if (sym->type_def) freeAST(sym->type_def);
        free(sym);
        EXIT_FAILURE_HANDLER();
    }
    
    // Optional Debug Print
    #ifdef DEBUG
    if (dumpExec) {
        fprintf(stderr, "[DEBUG parser.c addProcedure] Added routine '%s' to procedure_table. Copied AST node at %p. Symbol type: %s\n",
                sym->name, (void*)sym->type_def, varTypeToString(sym->type));
    }
    #endif
}

void insertType(const char *name, AST *typeAST) {
    TypeEntry *entry = malloc(sizeof(TypeEntry));
    entry->name = strdup(name);
    entry->typeAST = typeAST;
    entry->next = type_table;
    type_table = entry;
}

AST *lookupType(const char *name) {
    TypeEntry *entry = type_table;
    while (entry) {
        // Ensure entry->name is not NULL before comparing
        if (entry->name && name && strcasecmp(entry->name, name) == 0) { // <<< USE strcasecmp
            return entry->typeAST;
        }
        entry = entry->next;
    }
    return NULL;
}


// In emkey1/pscal/pscal-working/src/parser.c
// Make sure DEBUG_PRINT is defined, e.g., in utils.h or globals.h:
// #ifdef DEBUG
// #define DEBUG_PRINT(fmt, ...) fprintf(stdout, "[DEBUG %s:%d] " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
// #else
// #define DEBUG_PRINT(fmt, ...)
// #endif

AST *buildProgramAST(Parser *main_parser) {
    main_parser->current_unit_name_context = NULL;
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

    DEBUG_PRINT("buildProgramAST: About to eat SEMICOLON (after prog name). Current: %s ('%s')\n", main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE", main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
    eat(main_parser, TOKEN_SEMICOLON); // After this, current_token was proven to be TOKEN_USES by prior lexer logs

    // SPOT 1: Immediately after eat(TOKEN_SEMICOLON)
    DEBUG_PRINT("buildProgramAST SPOT 1: Immediately after eat(SEMICOLON). Token: %s ('%s')\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");

    AST *uses_clause = NULL;
    List *unit_list = NULL; // Will be created if USES is present

    // This is your existing #ifdef DEBUG block with fprintf(stderr, ...).
#ifdef DEBUG
    // SPOT 2: Before the fprintf(stderr,...) block.
    DEBUG_PRINT("buildProgramAST SPOT 2: Before existing stderr DEBUG block. Token: %s ('%s')\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");

    if (main_parser->current_token) { // Check if current_token is not NULL
        fprintf(stderr, "[DEBUG buildProgramAST] After PROGRAM Semicolon. Next token: %s ('%s') (via fprintf)\n",
                tokenTypeToString(main_parser->current_token->type),
                main_parser->current_token->value ? main_parser->current_token->value : "NULL_VAL");
        fflush(stderr);
    } else {
        fprintf(stderr, "[DEBUG buildProgramAST] After PROGRAM Semicolon. Next token is NULL (EOF?) (via fprintf)\n");
        fflush(stderr);
    }

    // SPOT 3: After the fprintf(stderr,...) block.
    DEBUG_PRINT("buildProgramAST SPOT 3: After existing stderr DEBUG block. Token: %s ('%s')\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
#endif

    // SPOT 4: This is the critical check point, just before the 'if' for TOKEN_USES.
    DEBUG_PRINT("buildProgramAST SPOT 4: Just before 'if (current_token->type == TOKEN_USES)'. Token: %s ('%s')\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");

    if (main_parser->current_token && main_parser->current_token->type == TOKEN_USES) {
        DEBUG_PRINT("buildProgramAST SPOT 5: Matched TOKEN_USES. About to eat TOKEN_USES.\n");
#ifdef DEBUG // Your existing stderr print
        fprintf(stderr, "[DEBUG buildProgramAST] Matched TOKEN_USES. About to eat. (via fprintf)\n");
        fflush(stderr);
#endif
        eat(main_parser, TOKEN_USES); // Consumes USES token. current_token is now first unit name or error.
        DEBUG_PRINT("buildProgramAST SPOT 6: After eating TOKEN_USES. Next token for unit name: %s ('%s')\n",
                    main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                    main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
#ifdef DEBUG // Your existing stderr print
        fprintf(stderr, "[DEBUG buildProgramAST] Ate TOKEN_USES. Next token: %s ('%s') (via fprintf)\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN",
                (main_parser->current_token && main_parser->current_token->value) ? main_parser->current_token->value : "NULL_VALUE");
        fflush(stderr);
#endif
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        unit_list = createList();

        // --- START: Corrected USES list parsing (from your code) ---
        while (main_parser->current_token && main_parser->current_token->type == TOKEN_IDENTIFIER) {
            DEBUG_PRINT("buildProgramAST USES_LOOP: Processing unit IDENTIFIER: %s ('%s')\n",
                        main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                        main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
            char* temp_unit_name_original_case = strdup(main_parser->current_token->value);
            if (!temp_unit_name_original_case) {
                fprintf(stderr, "Memory allocation failed for unit name in USES clause.\n");
                freeList(unit_list);
                if(uses_clause) freeAST(uses_clause);
                if(prog_name_node) freeAST(prog_name_node);
                if(copiedProgToken) freeToken(copiedProgToken);
                EXIT_FAILURE_HANDLER();
            }

            listAppend(unit_list, temp_unit_name_original_case);
            free(temp_unit_name_original_case); // listAppend makes its own copy

#ifdef DEBUG // Your existing stderr print
            fprintf(stderr, "[DEBUG buildProgramAST USES_LOOP] Added unit '%s' to list. About to eat IDENTIFIER.\n",
                main_parser->current_token->value);
            fflush(stderr);
#endif
            eat(main_parser, TOKEN_IDENTIFIER); // Consume the unit name identifier
#ifdef DEBUG // Your existing stderr print
            fprintf(stderr, "[DEBUG buildProgramAST USES_LOOP] After eating unit IDENTIFIER. Next token: %s ('%s')\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN",
                (main_parser->current_token && main_parser->current_token->value) ? main_parser->current_token->value : "NULL_VALUE");
            fflush(stderr);
#endif
            if (main_parser->current_token && main_parser->current_token->type == TOKEN_COMMA) {
#ifdef DEBUG // Your existing stderr print
                fprintf(stderr, "[DEBUG buildProgramAST USES_LOOP] Found COMMA in USES list. About to eat.\n");
                fflush(stderr);
#endif
                eat(main_parser, TOKEN_COMMA); // Consume comma
#ifdef DEBUG // Your existing stderr print
                fprintf(stderr, "[DEBUG buildProgramAST USES_LOOP] Ate COMMA. Next token: %s ('%s')\n",
                    main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN",
                    (main_parser->current_token && main_parser->current_token->value) ? main_parser->current_token->value : "NULL_VALUE");
                fflush(stderr);
#endif
            } else {
                break;
            }
        } // End while (parsing unit identifiers)

#ifdef DEBUG // Your existing stderr print
        fprintf(stderr, "[DEBUG buildProgramAST USES_LOOP] Exited identifier list loop. About to eat SEMICOLON. Current token: %s ('%s')\n",
            main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN",
            (main_parser->current_token && main_parser->current_token->value) ? main_parser->current_token->value : "NULL_VALUE");
        fflush(stderr);
#endif
        DEBUG_PRINT("buildProgramAST USES_LOOP: Expecting SEMICOLON to end USES. Current: %s ('%s')\n",
                    main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                    main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
        eat(main_parser, TOKEN_SEMICOLON); // Consumes the semicolon terminating the USES clause.
#ifdef DEBUG // Your existing stderr print
        fprintf(stderr, "[DEBUG buildProgramAST USES_LOOP] Ate USES SEMICOLON. Next token: %s ('%s')\n",
            main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN",
            (main_parser->current_token && main_parser->current_token->value) ? main_parser->current_token->value : "NULL_VALUE");
        fflush(stderr);
#endif
        DEBUG_PRINT("buildProgramAST USES_LOOP: After eating USES SEMICOLON. Next token: %s ('%s')\n",
                    main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                    main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");

        if (uses_clause) { // Should always be true if we are in this block
            uses_clause->unit_list = unit_list; // unit_list can be empty if syntax was "USES ;"
        }

        // Now, process each unit in the populated unit_list (your existing logic)
        if (unit_list) {
            for (int i = 0; i < listSize(unit_list); i++) {
                char *used_unit_name_str_from_list = listGet(unit_list, i);
                DEBUG_PRINT("buildProgramAST: Processing unit from USES list: '%s'\n", used_unit_name_str_from_list ? used_unit_name_str_from_list : "NULL_NAME");
                
                char lower_used_unit_name[MAX_SYMBOL_LENGTH];
                strncpy(lower_used_unit_name, used_unit_name_str_from_list, MAX_SYMBOL_LENGTH - 1);
                lower_used_unit_name[MAX_SYMBOL_LENGTH - 1] = '\0';
                for(int k=0; lower_used_unit_name[k]; k++) {
                    lower_used_unit_name[k] = tolower((unsigned char)lower_used_unit_name[k]);
                }

                char *unit_file_path = findUnitFile(lower_used_unit_name);
                if (!unit_file_path) {
                    fprintf(stderr, "Parser error: Unit file for '%s' not found.\n", lower_used_unit_name);
                    if(uses_clause) freeAST(uses_clause);
                    else if(unit_list) freeList(unit_list);
                    if(prog_name_node) freeAST(prog_name_node);
                    if(copiedProgToken) freeToken(copiedProgToken);
                    EXIT_FAILURE_HANDLER();
                }
                DEBUG_PRINT("buildProgramAST: Found unit file for '%s' at path: '%s'\n", lower_used_unit_name, unit_file_path);
                
                char* unit_source_buffer = NULL;
                FILE *unit_file = fopen(unit_file_path, "r");
                if(unit_file) {
                    // ... (your existing file reading logic) ...
                    fseek(unit_file, 0, SEEK_END);
                    long fsize = ftell(unit_file);
                    rewind(unit_file);
                    unit_source_buffer = malloc(fsize + 1);
                    if (!unit_source_buffer) { fclose(unit_file); free(unit_file_path); EXIT_FAILURE_HANDLER(); }
                    fread(unit_source_buffer, 1, fsize, unit_file);
                    unit_source_buffer[fsize] = '\0';
                    fclose(unit_file);
                } else {
                    fprintf(stderr, "Parser error: Could not open unit file '%s'.\n", unit_file_path);
                    free(unit_file_path);
                    EXIT_FAILURE_HANDLER();
                }
                free(unit_file_path);

                Lexer nested_lexer;
                initLexer(&nested_lexer, unit_source_buffer);
                Parser nested_parser_instance;
                nested_parser_instance.lexer = &nested_lexer;
                nested_parser_instance.current_token = getNextToken(&nested_lexer);
                
                DEBUG_PRINT("buildProgramAST: Calling unitParser for '%s'.\n", lower_used_unit_name);
                AST *parsed_unit_ast = unitParser(&nested_parser_instance, 1, lower_used_unit_name);
                
                if (nested_parser_instance.current_token) freeToken(nested_parser_instance.current_token);
                if (unit_source_buffer) free(unit_source_buffer);

                if (parsed_unit_ast) {
                    DEBUG_PRINT("buildProgramAST: unitParser succeeded for '%s'. Calling linkUnit.\n", lower_used_unit_name);
                    linkUnit(parsed_unit_ast, 1);
                } else {
                    DEBUG_PRINT("buildProgramAST: unitParser FAILED for '%s'.\n", lower_used_unit_name);
                }
            }
        }
    } // End if (main_parser->current_token && main_parser->current_token->type == TOKEN_USES)
    else {
        // This 'else' logs if the 'if (type == TOKEN_USES)' check failed at SPOT 4
        DEBUG_PRINT("buildProgramAST SPOT 5_ELSE: Did NOT match TOKEN_USES. Token at SPOT 4 was: %s ('%s')\n",
                    main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                    main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
    }

#ifdef DEBUG // Your existing stderr print before block()
    // SPOT 7: Before calling block()
    DEBUG_PRINT("buildProgramAST SPOT 7: Before calling block(). Token: %s ('%s')\n",
                main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN_TYPE",
                main_parser->current_token && main_parser->current_token->value ? main_parser->current_token->value : "NULL_TOKEN_VALUE");
    fprintf(stderr, "[DEBUG buildProgramAST] Before calling block(). Current token: %s ('%s') (via fprintf)\n",
            main_parser->current_token ? tokenTypeToString(main_parser->current_token->type) : "NULL_TOKEN",
            (main_parser->current_token && main_parser->current_token->value) ? main_parser->current_token->value : "NULL_VALUE");
    fflush(stderr);
#endif

    AST *block_node = block(main_parser);

#ifdef DEBUG // Your existing stderr print after block()
    fprintf(stderr, "[DEBUG buildProgramAST] Returned from block().\n");
    fflush(stderr);
#endif
    
    AST *programNode = newASTNode(AST_PROGRAM, copiedProgToken);
    if (!programNode) { /* Malloc error, cleanup... */ if(copiedProgToken) freeToken(copiedProgToken); if(prog_name_node) freeAST(prog_name_node); if(block_node) freeAST(block_node); if(uses_clause) freeAST(uses_clause); EXIT_FAILURE_HANDLER(); }

    setLeft(programNode, prog_name_node);
    setRight(programNode, block_node);
    if(uses_clause) { // Only add if it was created
        addChild(programNode, uses_clause);
    }
    freeToken(copiedProgToken); // Was copied by newASTNode for programNode

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

AST *procedureDeclaration(Parser *parser, bool in_interface) {
    eat(parser, TOKEN_PROCEDURE);
    Token *originalProcNameToken = parser->current_token;
    Token *copiedProcNameToken = copyToken(originalProcNameToken);
    eat(parser, TOKEN_IDENTIFIER); // Consumes procedure name
    AST *node = newASTNode(AST_PROCEDURE_DECL, copiedProcNameToken);
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
             if(params) freeAST(params); freeAST(node); return NULL; // Cleanup & exit path
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

    if (!in_interface) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG PROC_DECL_BODY] Expecting SEMICOLON after header for '%s'. Current token: Type=%s, Value='%s'\n",
                node->token->value,
                parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
                parser->current_token && parser->current_token->value ? parser->current_token->value : "NULL");
        #endif
        eat(parser, TOKEN_SEMICOLON); // This is the EAT that your log says is failing
        AST *local_declarations = declarations(parser, false);
        AST *compound_body = compoundStatement(parser);
        AST *blockNode = newASTNode(AST_BLOCK, NULL);
        addChild(blockNode, local_declarations);
        addChild(blockNode, compound_body);
        blockNode->is_global_scope = false;
        setRight(node, blockNode);
    }
    addProcedure(node, parser->current_unit_name_context);
    if (copiedProcNameToken)
        freeToken(copiedProcNameToken);

    return node;
}

// constDeclaration: Calls expression or parseArrayInitializer
AST *constDeclaration(Parser *parser) {
    Token* cn = copyToken(parser->current_token); if(!cn||cn->type!=TOKEN_IDENTIFIER){errorParser(parser,"Exp const name"); return NULL;} eat(parser,TOKEN_IDENTIFIER);
    AST* type_node=NULL, *val_node=NULL;
    if(parser->current_token && parser->current_token->type == TOKEN_COLON){eat(parser,TOKEN_COLON); type_node=typeSpecifier(parser,1); if(!type_node){errorParser(parser,"Invalid type"); freeToken(cn); return NULL;} /*...array check...*/ }
    if(!parser->current_token || parser->current_token->type!=TOKEN_EQUAL){errorParser(parser,"Exp ="); freeToken(cn); if(type_node)freeAST(type_node); return NULL;} eat(parser,TOKEN_EQUAL);
    if(type_node){ // Typed constant (array)
        if(!parser->current_token || parser->current_token->type!=TOKEN_LPAREN){errorParser(parser,"Exp ("); freeToken(cn); freeAST(type_node); return NULL;}
        val_node = parseArrayInitializer(parser); // <<< Uses expression internally
        if(!val_node){errorParser(parser,"Bad array init"); freeToken(cn); freeAST(type_node); return NULL;}
        if(type_node)setRight(val_node,type_node); // Link type info
    } else { // Simple constant
        val_node = expression(parser); // <<< Use expression()
        if(!val_node || val_node->type == AST_NOOP){errorParser(parser,"Exp const value"); freeToken(cn); return NULL;}
    }
    if(!parser->current_token || parser->current_token->type!=TOKEN_SEMICOLON){errorParser(parser,"Exp ;"); freeToken(cn); if(type_node)freeAST(type_node); freeAST(val_node); return NULL;} eat(parser,TOKEN_SEMICOLON);
    AST* node = newASTNode(AST_CONST_DECL, cn); freeToken(cn); setLeft(node,val_node);
    if(type_node){setRight(node,type_node); setTypeAST(node,TYPE_ARRAY);} else {setTypeAST(node,TYPE_VOID);} // Type inferred later for simple const
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

    } else if (initialTokenType == TOKEN_RECORD) {
        // Use the initialToken captured at the start
        node = newASTNode(AST_RECORD_TYPE, initialToken);
        eat(parser, TOKEN_RECORD); // Consume the RECORD keyword itself

        // (Rest of existing RECORD parsing logic - it calls typeSpecifier recursively)
        while (parser->current_token && parser->current_token->type == TOKEN_IDENTIFIER) {
            AST *fieldDecl = newASTNode(AST_VAR_DECL, NULL);
            while (1) { /* Parse field identifiers */ if (!parser->current_token || parser->current_token->type != TOKEN_IDENTIFIER) { errorParser(parser,"Expected field identifier"); freeAST(fieldDecl); return node; } AST *varNode = newASTNode(AST_VARIABLE, parser->current_token); eat(parser, TOKEN_IDENTIFIER); addChild(fieldDecl, varNode); if (parser->current_token && parser->current_token->type == TOKEN_COMMA) eat(parser, TOKEN_COMMA); else break; }
            if (!parser->current_token || parser->current_token->type != TOKEN_COLON) { errorParser(parser,"Expected :"); freeAST(fieldDecl); return node; } eat(parser, TOKEN_COLON);
            AST *fieldType = typeSpecifier(parser, 1); if (!fieldType || fieldType->type == AST_NOOP) { errorParser(parser,"Bad field type"); freeAST(fieldDecl); return node; }
            setTypeAST(fieldDecl, fieldType->var_type);
            setRight(fieldDecl, fieldType); addChild(node, fieldDecl);
            if (parser->current_token && parser->current_token->type == TOKEN_SEMICOLON) { eat(parser, TOKEN_SEMICOLON); if (parser->current_token && parser->current_token->type == TOKEN_END) break; }
            else if (!parser->current_token || parser->current_token->type != TOKEN_END) { errorParser(parser, "Expected ; or END in record"); break; }
        }
        if (!parser->current_token || parser->current_token->type != TOKEN_END) { errorParser(parser,"Expected END for record"); return node; }
        eat(parser, TOKEN_END);
        setTypeAST(node, TYPE_RECORD);
        // Flow continues to the end, return node

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

    } else if (initialTokenType == TOKEN_IDENTIFIER) {
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
             if (strcasecmp(typeNameCopy, "integer") == 0 || strcasecmp(typeNameCopy, "longint") == 0 || strcasecmp(typeNameCopy, "cardinal") == 0) basicType = TYPE_INTEGER;
             else if (strcasecmp(typeNameCopy, "real") == 0) basicType = TYPE_REAL;
             else if (strcasecmp(typeNameCopy, "char") == 0) basicType = TYPE_CHAR;
             else if (strcasecmp(typeNameCopy, "byte") == 0) basicType = TYPE_BYTE;
             else if (strcasecmp(typeNameCopy, "word") == 0) basicType = TYPE_WORD;
             else if (strcasecmp(typeNameCopy, "boolean") == 0) basicType = TYPE_BOOLEAN;
             else if (strcasecmp(typeNameCopy, "file") == 0 || strcasecmp(typeNameCopy, "text") == 0) basicType = TYPE_FILE;
             else if (strcasecmp(typeNameCopy, "mstream") == 0) basicType = TYPE_MEMORYSTREAM;

            if (basicType != TYPE_VOID) {
                node = newASTNode(AST_VARIABLE, initialToken); // Use initialToken
                setTypeAST(node, basicType);
                eat(parser, TOKEN_IDENTIFIER); // Consume the type identifier
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
                eat(parser, TOKEN_IDENTIFIER); // Consume the type identifier
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
    while (parser->current_token->type == TOKEN_IDENTIFIER) {

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
    if (originalTypeNameToken->type != TOKEN_IDENTIFIER) {
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
        // parse typeSpecifier as before
        typeDefNode = typeSpecifier(parser, 1);
    }

    setLeft(node, typeDefNode); // Link the actual type definition (enum, record, etc.)
    // Register the type using the value from the copied token
    insertType(copiedTypeNameToken->value, typeDefNode);

    eat(parser, TOKEN_SEMICOLON);

    // --- Free the copy of the token we made at the beginning ---
    freeToken(copiedTypeNameToken); // Clean up the copy owned by this function
    // ---

    return node; // Return the main AST_TYPE_DECL node
}

// variable: Simple variable parsing (e.g., for param list names) - No changes needed here usually
AST *variable(Parser *parser) {
    Token *token = parser->current_token;
    if (!token || token->type != TOKEN_IDENTIFIER){errorParser(parser,"Expected var name"); return NULL;}
    AST* node = newASTNode(AST_VARIABLE, token); // Uses copy
    eat(parser, TOKEN_IDENTIFIER);
    // Does NOT parse field/array access
    return node;
}

AST *varDeclaration(Parser *parser, bool isGlobal /* Not used here, but kept */) {
    AST *groupNode = newASTNode(AST_VAR_DECL, NULL); // Temp node for names

    // 1. Parse variable list into groupNode children
    while (parser->current_token->type == TOKEN_IDENTIFIER) {
        Token* originalVarToken = parser->current_token;
        Token* copiedVarToken = copyToken(originalVarToken);
        if (!copiedVarToken) { /* Malloc error */ freeAST(groupNode); EXIT_FAILURE_HANDLER(); }
        eat(parser, TOKEN_IDENTIFIER); // Frees original token

        AST *varNode = newASTNode(AST_VARIABLE, copiedVarToken);
        if (!varNode) { /* Malloc error */ freeToken(copiedVarToken); freeAST(groupNode); EXIT_FAILURE_HANDLER(); }
        addChild(groupNode, varNode); // Sets varNode->parent = groupNode
        freeToken(copiedVarToken); // Free the parser's temporary copy

        if (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else { break; }
    }

    eat(parser, TOKEN_COLON);
    // 2. Parse the type specifier ONCE for the group
    AST *originalTypeNode = typeSpecifier(parser, 0);
    if (!originalTypeNode) { /* error handling */ freeAST(groupNode); return NULL; }

    AST *finalCompoundNode = newASTNode(AST_COMPOUND, NULL);

    // 3. Create final VAR_DECL nodes, creating COPIES of type nodes for each
    for (int i = 0; i < groupNode->child_count; ++i) {
        AST *var_decl_node = newASTNode(AST_VAR_DECL, NULL);
        if (!var_decl_node) { /* Malloc check */ freeAST(groupNode); freeAST(finalCompoundNode); EXIT_FAILURE_HANDLER(); }
        var_decl_node->var_type = originalTypeNode->var_type;

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
        // ---

        addChild(finalCompoundNode, var_decl_node);
    }

    // --- Free the ORIGINAL typeNode returned by typeSpecifier ---
    freeAST(originalTypeNode);

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
    Token *originalFuncNameToken = parser->current_token;
    if (!originalFuncNameToken || originalFuncNameToken->type != TOKEN_IDENTIFIER) {
        errorParser(parser,"Expected function name after FUNCTION");
        return newASTNode(AST_NOOP, NULL);
    }
    Token *copiedFuncNameToken = copyToken(originalFuncNameToken);
    if (!copiedFuncNameToken) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
    
    eat(parser, TOKEN_IDENTIFIER); // Consumes function name
    
    AST *node = newASTNode(AST_FUNCTION_DECL, copiedFuncNameToken);
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
    if (!in_interface) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG FUNC_DECL_BODY] Expecting SEMICOLON after header for function '%s'. Current token: %s ('%s')\n",
                copiedFuncNameToken->value,
                parser->current_token ? tokenTypeToString(parser->current_token->type) : "NULL_TOKEN",
                (parser->current_token && parser->current_token->value) ? parser->current_token->value : "NULL_VALUE");
        fflush(stderr);
#endif
        eat(parser, TOKEN_SEMICOLON); // Semicolon after function header "function Name(params): RetType;"
        
        AST *local_declarations = declarations(parser, false);
        AST *compound_body = compoundStatement(parser); // compoundStatement eats its own BEGIN and END.
                                                        // It expects a final '.' or a ';' if nested.
                                                        // For a function body, it should end with END;
        
        AST *blockNode = newASTNode(AST_BLOCK, NULL);
        addChild(blockNode, local_declarations);
        addChild(blockNode, compound_body);
        blockNode->is_global_scope = false;
        setExtra(node, blockNode); // Function body block in 'extra'
    }
    
    addProcedure(node, parser->current_unit_name_context); // Registers the function
    
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
        if (parser->current_token->type == TOKEN_VAR || parser->current_token->type == TOKEN_OUT || parser->current_token->type == TOKEN_CONST) {
            // If it's VAR or OUT, mark as pass-by-reference
            if (parser->current_token->type == TOKEN_VAR || parser->current_token->type == TOKEN_OUT) {
                 byRef = 1;
            }
            // Consume the keyword (VAR, OUT, or CONST)
            eat(parser, parser->current_token->type);
        }

        AST *group = newASTNode(AST_VAR_DECL, NULL); // Temp node for names
        while (1) { // Parse identifier names into group->children
            Token* originalIdToken = parser->current_token;
            if (originalIdToken->type != TOKEN_IDENTIFIER) { errorParser(parser, "Expected identifier in parameter list"); freeAST(group); freeAST(compound); return NULL; }
            Token* copiedIdToken = copyToken(originalIdToken);
            if (!copiedIdToken) { fprintf(stderr, "Memory allocation failed for token copy in paramList\n"); freeAST(group); freeAST(compound); EXIT_FAILURE_HANDLER(); }
            eat(parser, TOKEN_IDENTIFIER); // Frees original token
            AST *id_node = newASTNode(AST_VARIABLE, copiedIdToken);
            if (!id_node) { fprintf(stderr, "Memory allocation failed for id_node in paramList\n"); freeToken(copiedIdToken); freeAST(group); freeAST(compound); EXIT_FAILURE_HANDLER(); }
            addChild(group, id_node); // Sets id_node->parent = group
            freeToken(copiedIdToken); // Frees the parser's temporary copy
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

            Token* nameTokenCopy = copyToken(group->children[i]->token);
            if (!nameTokenCopy) { /* Malloc Check */ freeAST(originalTypeNode); freeAST(group); freeAST(compound); freeAST(param_decl); EXIT_FAILURE_HANDLER(); }
            param_decl->children[0] = newASTNode(AST_VARIABLE, nameTokenCopy);
            if (!param_decl->children[0]) { /* Malloc check */ freeToken(nameTokenCopy); freeAST(originalTypeNode); freeAST(group); freeAST(compound); freeAST(param_decl); EXIT_FAILURE_HANDLER(); }
            freeToken(nameTokenCopy);
            param_decl->children[0]->parent = param_decl;

            param_decl->var_type = originalTypeNode->var_type;
            param_decl->by_ref = byRef;

            // --- Use copyAST for the type node ---
            AST* typeNodeCopy = copyAST(originalTypeNode);
            if (!typeNodeCopy) { fprintf(stderr, "Memory allocation failed copying type node in paramList\n"); freeAST(group); freeAST(compound); freeAST(param_decl); freeAST(originalTypeNode); EXIT_FAILURE_HANDLER(); }
            setRight(param_decl, typeNodeCopy); // Link VAR_DECL to the UNIQUE copy
            // ---

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
    AST *node = NULL; // Initialize node

    switch (parser->current_token->type) {
        case TOKEN_BEGIN:
            // Compound statement (BEGIN ... END)
            node = compoundStatement(parser);
            // compoundStatement handles its own END token.
            break; // No semicolon needed after END

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
            if (parser->current_token->type == TOKEN_ASSIGN) {
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
                        AST* args_compound = exprList(parser); // Parse arguments; exprList returns an AST_COMPOUND node

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
                if (lval_or_proc_id->token && lval_or_proc_id->token->value) {
                    lval_desc = lval_or_proc_id->token->value;
                } else if (lval_or_proc_id->left && lval_or_proc_id->left->token && lval_or_proc_id->left->token->value) {
                    // Attempt to get a more descriptive name for complex lvalues like array access
                    char temp_desc[128];
                    snprintf(temp_desc, sizeof(temp_desc), "%s[...]", lval_or_proc_id->left->token->value);
                    lval_desc = temp_desc; // NOTE: temp_desc is on stack, be careful if error_msg outlives this scope significantly
                                           // For immediate errorParser call, it's okay.
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
    if(!parser->current_token || parser->current_token->type!=TOKEN_ASSIGN){errorParser(parser,"Expected :="); return newASTNode(AST_NOOP,NULL);} eat(parser,TOKEN_ASSIGN);
    AST* r=expression(parser); // <<< Use expression()
    if(!r || r->type == AST_NOOP){errorParser(parser,"Expected expression after :="); return newASTNode(AST_NOOP,NULL);}
    AST* n=newASTNode(AST_ASSIGN,NULL); setLeft(n,parsedLValue); setRight(n,r);
    return n;
}

// procedureCall: Calls exprList (which calls expression)
AST *procedureCall(Parser *parser) {
    // Assumes current token is the procedure identifier
    AST *node = newASTNode(AST_PROCEDURE_CALL, parser->current_token); eat(parser,TOKEN_IDENTIFIER);
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
    if (enumToken->type != TOKEN_IDENTIFIER) {
         errorParser(parser, "Expected type name for enum declaration");
         return newASTNode(AST_NOOP, NULL); // Error recovery
    }
    eat(parser, TOKEN_IDENTIFIER); // Consume type name

    eat(parser, TOKEN_EQUAL);
    eat(parser, TOKEN_LPAREN);

    // Node for the overall Enum Type (e.g., tmyenum)
    // *** FIX 1: Use the correct type name token (enumToken) ***
    AST *node = newASTNode(AST_ENUM_TYPE, enumToken);
    setTypeAST(node, TYPE_ENUM); // Mark the type node itself as ENUM type

    int ordinal = 0;
    char *typeNameStr = strdup(enumToken->value); // Store the type name string for symbol table

    // Parse enumeration values (e.g., valone, valtwo, ...)
    while (parser->current_token->type == TOKEN_IDENTIFIER) {
        Token *valueToken = parser->current_token; // Token for the VALUE (e.g., valone)
        eat(parser, TOKEN_IDENTIFIER);

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
        fmt->token=newToken(TOKEN_STRING_CONST,fs); // Stores "width,precision"
        freeToken(widthTok); if(precTok)freeToken(precTok);
        return fmt;
    } else {
        return exprNode; // No formatting
    }
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
    node = term(parser);
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

    // Loop for additive operators (+, -, OR) - Add XOR later if needed
    while (parser->current_token && (
           parser->current_token->type == TOKEN_PLUS || parser->current_token->type == TOKEN_MINUS ||
           parser->current_token->type == TOKEN_OR /* || TOKEN_XOR */ ))
    {
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal); // Copy token before eating
        if (!opCopied) { EXIT_FAILURE_HANDLER(); }
        eat(parser, opOriginal->type); // Eat original op token

        AST *right = term(parser); // Parse the next term
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
AST *term(Parser *parser) {
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

// factor: Parses literals, variables, function calls, NOT, parenthesized expressions, set constructors.
// factor ::= literal | variable | function_call | '(' expression ')' | NOT factor | '[' set_elements ']'
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

    if (initialTokenType == TOKEN_NIL) {
        Token* c = copyToken(initialToken); // Copy the initial NIL token
        eat(parser, TOKEN_NIL);             // Consume NIL, frees original initialToken
        node = newASTNode(AST_NIL, c);      // Create node with the copy
        freeToken(c);                       // Free the copy
        // Set the AST node's type to TYPE_NIL, aligning with the Value representation.
        setTypeAST(node, TYPE_NIL);
        return node; // <<< RETURN IMMEDIATELY
    } else if (initialTokenType == TOKEN_TRUE || initialTokenType == TOKEN_FALSE) {
        Token* c = copyToken(initialToken);
        eat(parser, initialTokenType); // Eat TRUE or FALSE
        node = newASTNode(AST_BOOLEAN, c); freeToken(c);
        setTypeAST(node, TYPE_BOOLEAN);
        return node; // <<< RETURN IMMEDIATELY

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
        Token* c = copyToken(initialToken);
        eat(parser, initialTokenType); // Eat the string token
        node = newASTNode(AST_STRING, c); freeToken(c);
        setTypeAST(node, TYPE_STRING);
        return node; // <<< RETURN IMMEDIATELY

    } else if (initialTokenType == TOKEN_IDENTIFIER) {
        // IDENTIFIER case: call lvalue, which handles ., [], ^ and eats tokens internally
        node = lvalue(parser); // lvalue consumes the identifier and potential subsequent tokens
        if (!node || node->type == AST_NOOP) return newASTNode(AST_NOOP, NULL);

        // Check if it's a function call (lvalue doesn't handle the LPAREN)
        if (parser->current_token && parser->current_token->type == TOKEN_LPAREN && node->type == AST_VARIABLE) {
             // It's a function call with arguments
             node->type = AST_PROCEDURE_CALL; // Change type
             eat(parser, TOKEN_LPAREN);      // Eat '('
             if (parser->current_token && parser->current_token->type != TOKEN_RPAREN) {
                 AST* args = exprList(parser); // Parse argument list
                 if (!args || args->type == AST_NOOP) { errorParser(parser,"Bad arg list"); return node; }
                 // Transfer arguments safely (ensure your existing logic is robust)
                 if (args && args->type == AST_COMPOUND && args->child_count > 0) {
                      node->children = args->children; node->child_count = args->child_count; node->child_capacity = args->child_capacity;
                      args->children = NULL; args->child_count = 0; args->child_capacity = 0;
                      for(int i=0; i < node->child_count; i++) if(node->children[i]) node->children[i]->parent = node;
                  } else { node->children = NULL; node->child_count = 0; node->child_capacity=0; }
                  if(args) freeAST(args); // Free compound wrapper
             } else { node->children = NULL; node->child_count = 0; node->child_capacity=0; } // Empty arg list '()'
             if (!parser->current_token || parser->current_token->type != TOKEN_RPAREN) { errorParser(parser,"Expected ) after args"); return node;}
             eat(parser, TOKEN_RPAREN); // Eat ')'
             // Type annotation will set the function's return type later
        }
        // Check if lvalue returned a simple variable that might be a parameter-less function
        else if (node->type == AST_VARIABLE) {
            Symbol *procSym = node->token ? lookupProcedure(node->token->value) : NULL; // NEW: Use Symbol*

             // if (proc && proc->proc_decl && proc->proc_decl->type == AST_FUNCTION_DECL) { // OLD
             if (procSym && procSym->type_def && procSym->type_def->type == AST_FUNCTION_DECL) { // NEW: Check procSym and its type_def
                 node->type = AST_PROCEDURE_CALL; // Change type to parameter-less call
                 node->children = NULL; node->child_count = 0; node->child_capacity = 0;
                 // Set return type early if possible
                 // if (proc->proc_decl->right) setTypeAST(node, proc->proc_decl->right->var_type); // OLD
                 if (procSym->type_def->right) setTypeAST(node, procSym->type_def->right->var_type); // NEW
                 // else setTypeAST(node, TYPE_VOID); // OLD: This was an error, should use procSym->type
                 else setTypeAST(node, procSym->type); // NEW: Use the Symbol's stored type
             // } else if (proc) { // Procedure used as value error // OLD
             } else if (procSym) { // NEW: Check procSym
                  char error_msg[128];
                  // snprintf(error_msg, sizeof(error_msg), "Procedure '%s' cannot be used as a value", node->token->value); // OLD
                  snprintf(error_msg, sizeof(error_msg), "Procedure '%s' cannot be used as a value", procSym->name); // NEW
                  errorParser(parser, error_msg);
                  freeAST(node); return newASTNode(AST_NOOP, NULL);
             }
             // Otherwise, it's just a variable/constant reference handled by lvalue
        }
        // If lvalue returned DEREFERENCE, FIELD_ACCESS, ARRAY_ACCESS, it's used as is.
        // No immediate return needed here as lvalue consumed the relevant tokens.
        // Flow continues to end.

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

    return node; // Return the created node
}

// --- Function to specifically parse ^TypeName ---
AST *parsePointerType(Parser *parser) {
    eat(parser, TOKEN_CARET); // Consume '^'

    // The next token *must* be a type identifier
    if (!parser->current_token || parser->current_token->type != TOKEN_IDENTIFIER) {
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

    eat(parser, TOKEN_IDENTIFIER); // Consume the base type identifier

    // --- Create the AST_POINTER_TYPE node ---
    AST *pointerTypeNode = newASTNode(AST_POINTER_TYPE, NULL); // No specific token for the pointer type itself
    if (!pointerTypeNode) { freeAST(baseTypeNameNode); EXIT_FAILURE_HANDLER(); }

    // Set the base type node as the 'right' child (consistent with array/record having base type in right)
    setRight(pointerTypeNode, baseTypeNameNode);

    // Set the overall type of this node to TYPE_POINTER
    setTypeAST(pointerTypeNode, TYPE_POINTER);

    return pointerTypeNode;
}

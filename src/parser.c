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

#ifdef DEBUG
// Define the helper function *only* when DEBUG is enabled
// No 'static inline' needed here as it's defined only once in this file.
void eat_debug_wrapper(Parser *parser_ptr, TokenType expected_token_type, const char* func_name) {
    if (dumpExec) { // Check if debug execution logging is enabled
        fprintf(stderr, "[DEBUG] eat(): Called from %s() - Expecting: %s, Got: %s ('%s') at Line %d, Col %d\n",
                 func_name, // Function name passed by the macro
                 tokenTypeToString(expected_token_type),
                 tokenTypeToString((parser_ptr)->current_token->type),
                 (parser_ptr)->current_token->value ? (parser_ptr)->current_token->value : "NULL",
                 (parser_ptr)->lexer->line, (parser_ptr)->lexer->column);
         // Optional: Check if the types actually match before calling the original eat
         if ((parser_ptr)->current_token->type != (expected_token_type)) {
             fprintf(stderr, "[DEBUG] eat(): *** TOKEN MISMATCH DETECTED by wrapper before calling original eat() ***\n");
         }
    }
    // IMPORTANT: Call the original 'eat' function.
    // The macro substitution for 'eat' doesn't happen inside this function's definition.
    eatInternal(parser_ptr, expected_token_type);
}
#endif // DEBUG

AST *parseWriteArgument(Parser *parser);

AST *declarations(Parser *parser, bool in_interface) {
    AST *node = newASTNode(AST_COMPOUND, NULL);

    while (1) {
        if (parser->current_token->type == TOKEN_CONST) {
            eat(parser, TOKEN_CONST);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *constDecl = constDeclaration(parser); // Parses Name = Value;
                addChild(node, constDecl); // Add AST node as before

                Value constVal = eval(constDecl->left); // Evaluate the constant's value expression

                // --- MODIFICATION START ---
                // Insert the symbol but DO NOT call updateSymbol afterwards for constants
                insertGlobalSymbol(constDecl->token->value, constVal.type, constDecl->right); // Pass type definition if available (constDecl->right)

                // Get the newly inserted symbol
                Symbol *sym = lookupGlobalSymbol(constDecl->token->value);
                if (sym && sym->value) {
                    // Free the default value allocated by insertGlobalSymbol
                    // (Be careful if insertGlobalSymbol changes to not allocate)
                     if (!sym->is_alias) { // Only free if not an alias
                          freeValue(sym->value); // Free the contents of the default value
                     }
                    // Assign the evaluated constant value directly (deep copy)
                    *sym->value = makeCopyOfValue(&constVal);
                    // Set the is_const flag
                    sym->is_const = true;
                     #ifdef DEBUG
                     fprintf(stderr, "[DEBUG_PARSER] Set is_const=TRUE for global constant '%s'\n", sym->name);
                     #endif
                } else {
                     fprintf(stderr, "Parser error: Failed to find or allocate value for global constant '%s'\n", constDecl->token->value);
                     // Handle error appropriately
                }
                // Free the temporary value obtained from eval
                freeValue(&constVal);
                // --- MODIFICATION END ---

                // Original updateSymbol call removed:
                // updateSymbol(constDecl->token->value, constVal); // *** REMOVE THIS LINE ***
            }
        }
        else if (parser->current_token->type == TOKEN_TYPE) {
            eat(parser, TOKEN_TYPE);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *typeDecl = typeDeclaration(parser);
                addChild(node, typeDecl);
                insertType(typeDecl->token->value, typeDecl->left);
            }
        }
        else if (parser->current_token->type == TOKEN_VAR) {
            eat(parser, TOKEN_VAR);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *vdecl = varDeclaration(parser, true);
                addChild(node, vdecl);
                eat(parser, TOKEN_SEMICOLON);
            }
        }
        else if (parser->current_token->type == TOKEN_PROCEDURE ||
                parser->current_token->type == TOKEN_FUNCTION) {
            AST *decl = (parser->current_token->type == TOKEN_PROCEDURE)
                        ? procedureDeclaration(parser, in_interface)
                        : functionDeclaration(parser, in_interface);
            addChild(node, decl);
            addProcedure(decl);
            eat(parser, TOKEN_SEMICOLON);
        } else if (parser->current_token->type == TOKEN_ENUM) {
            AST *enumDecl = enumDeclaration(parser);
            addChild(node, enumDecl);
            insertType(enumDecl->token->value, enumDecl->left);
        }
        else {
            // Exit loop if none of the declaration handlers match
            break;
        }
    }

    return node;
}

void eatInternal(Parser *parser, TokenType type) {
    if (parser->current_token->type == type) {
        // --- Store pointer to the token we are about to replace ---
        Token *tokenToFree = parser->current_token;

        // --- Get the next token ---
        parser->current_token = getNextToken(parser->lexer);

        // --- Free the *previous* token that was just consumed ---
        if (tokenToFree) {
            freeToken(tokenToFree); // freeToken handles freeing value and struct
        }
    } else {
        // Error handling remains the same
        char err[128];
        snprintf(err, sizeof(err), "Expected token %s, got %s",
                 tokenTypeToString(type), tokenTypeToString(parser->current_token->type));
        errorParser(parser, err);
    }
}

Procedure *lookupProcedure(const char *name) {
    Procedure *proc = procedure_table;
    while (proc) {
        if (strcmp(proc->name, name) == 0)
            return proc;
        proc = proc->next;
    }
    return NULL;
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

// lvalue() parses a variable reference without interpreting a following '(' as a call.
AST *lvalue(Parser *parser) {
    // Assume the current token is an identifier.
    Token *token = parser->current_token;
    if (token->type != TOKEN_IDENTIFIER) {
         errorParser(parser, "Expected identifier at start of lvalue");
         return newASTNode(AST_NOOP, NULL); // Return dummy node on error
    }

    AST *node = newASTNode(AST_VARIABLE, token);
    eat(parser, TOKEN_IDENTIFIER); // Consume the base identifier

    // Allow subsequent field accesses (.) and array accesses ([])
    while (parser->current_token->type == TOKEN_PERIOD ||
           parser->current_token->type == TOKEN_LBRACKET) {

        if (parser->current_token->type == TOKEN_PERIOD) {
            eat(parser, TOKEN_PERIOD); // Consume '.'
            Token *fieldToken = parser->current_token;
            if (fieldToken->type != TOKEN_IDENTIFIER) {
                errorParser(parser, "Expected field name after '.'");
                 // Cleanup potentially partially built node? For now, just break/return.
                 return node; // Return partially built node on error
            }
            AST *fieldAccess = newASTNode(AST_FIELD_ACCESS, fieldToken);
            eat(parser, TOKEN_IDENTIFIER); // Consume field identifier
            setLeft(fieldAccess, node); // Previous node becomes left child
            // Type annotation happens later
            node = fieldAccess; // Update node to the field access node
        }
        else if (parser->current_token->type == TOKEN_LBRACKET) {
            eat(parser, TOKEN_LBRACKET); // Consume '['
            AST *arrayAccess = newASTNode(AST_ARRAY_ACCESS, NULL); // No specific token for array access itself
            setLeft(arrayAccess, node); // Previous node is the array variable/expression

            // Parse comma-separated index expressions
            do {
                AST *indexExpr = expr(parser); // Parse one index expression
                addChild(arrayAccess, indexExpr);

                if (parser->current_token->type == TOKEN_COMMA) {
                    eat(parser, TOKEN_COMMA); // Consume comma, look for next index
                } else {
                    break; // No more commas, exit index loop
                }
            } while (1);

            eat(parser, TOKEN_RBRACKET); // Consume ']'
            // Type annotation happens later
            node = arrayAccess; // Update node to the array access node
        }
    }
    // After loop, node points to the complete lvalue AST
    return node;
}


// Update in src/parser.c's parse_array_type function

AST *parseArrayType(Parser *parser) {
    eat(parser, TOKEN_ARRAY);
    eat(parser, TOKEN_LBRACKET);

    AST *indexList = newASTNode(AST_COMPOUND, NULL);

    // Parse first subrange as a binary operation
    AST *lowerExpr = expr(parser);
    if (parser->current_token->type != TOKEN_DOTDOT) {
        errorParser(parser, "Expected DOTDOT in array index range");
    }
    eat(parser, TOKEN_DOTDOT);
    AST *upperExpr = expr(parser);

    // Create a new subrange node
    AST *indexType = newASTNode(AST_SUBRANGE, NULL);
    setLeft(indexType, lowerExpr);
    setRight(indexType, upperExpr);

    addChild(indexList, indexType);

    // Check for additional subranges separated by commas
    while (parser->current_token->type == TOKEN_COMMA) {
        eat(parser, TOKEN_COMMA);

        AST *lowerExpr = expr(parser);
        if (parser->current_token->type != TOKEN_DOTDOT) {
            errorParser(parser, "Expected DOTDOT in array index range");
        }
        eat(parser, TOKEN_DOTDOT);
        AST *upperExpr = expr(parser);

        AST *indexType = newASTNode(AST_SUBRANGE, NULL);
        setLeft(indexType, lowerExpr);
        setRight(indexType, upperExpr);

        addChild(indexList, indexType);
    }

    eat(parser, TOKEN_RBRACKET);

    if (parser->current_token->type != TOKEN_OF) {
        errorParser(parser, "Expected 'of' keyword in array type declaration");
    }
    
    eat(parser, TOKEN_OF);
    
    AST *elemType = typeSpecifier(parser, 1);

    // Create the array type node
    AST *node = newASTNode(AST_ARRAY_TYPE, NULL);
    node->children = indexList->children;
    node->child_count = indexList->child_count;
    free(indexList); // Free temporary compound node

    setRight(node, elemType);

    return node;
}

AST *unitParser(Parser *parser, int recursion_depth) {
    // Prevent infinite recursion
    if (recursion_depth > MAX_RECURSION_DEPTH) {
        fprintf(stderr, "Error: Maximum recursion depth exceeded while parsing units.\n");
        EXIT_FAILURE_HANDLER();
    }

    // The current token should be the 'unit' keyword
    Token *unit_keyword = parser->current_token;
    if (unit_keyword->type != TOKEN_UNIT) {
        fprintf(stderr, "Error: Expected 'unit' keyword at line %d, column %d (found '%s')\n",
                parser->lexer->line, parser->lexer->column, parser->current_token->value);
        EXIT_FAILURE_HANDLER();
    }
    AST *unit_node = newASTNode(AST_UNIT, parser->current_token);
    eat(parser, TOKEN_UNIT); // Consume the 'unit' token

    // The next token should be the unit name
    Token *unit_token = parser->current_token;
    if (unit_token->type != TOKEN_IDENTIFIER) {
        fprintf(stderr, "Error: Expected unit name after 'unit' keyword at line %d\n",
                parser->lexer->line);
        EXIT_FAILURE_HANDLER();
    }
    //char *unit_name = unit_token->value;
    eat(parser, TOKEN_IDENTIFIER); // Consume the unit name (e.g., 'mylib')
    eat(parser, TOKEN_SEMICOLON);  // Consume the semicolon after 'unit mylib;'

    // Handle the uses clause within the unit
    AST *uses_clause = NULL;
    if (parser->current_token->type == TOKEN_USES) {
        eat(parser, TOKEN_USES);
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        List *unit_list = createList();
        do {
            Token *unit_list_token = parser->current_token;
            if (unit_list_token->type != TOKEN_IDENTIFIER) {
                fprintf(stderr, "Error: Expected unit name in uses clause at line %d\n",
                        parser->lexer->line);
                EXIT_FAILURE_HANDLER();
            }
            listAppend(unit_list, unit_list_token->value);
            eat(parser, TOKEN_IDENTIFIER);
            if (parser->current_token->type == TOKEN_COMMA) {
                eat(parser, TOKEN_COMMA);
            } else {
                break;
            }
        } while (1);
        eat(parser, TOKEN_SEMICOLON); // Consume the semicolon after the uses clause
        AST *unit_list_node = newASTNode(AST_LIST, NULL);
        unit_list_node->unit_list = unit_list;
        addChild(uses_clause, unit_list_node);

        // Link the units
        for (int i = 0; i < listSize(unit_list); i++) {
            char *nested_unit_name = listGet(unit_list, i);
            char *nested_unit_path = findUnitFile(nested_unit_name);
            if (nested_unit_path == NULL) {
                fprintf(stderr, "Error: Unit '%s' not found.\n", nested_unit_name);
                EXIT_FAILURE_HANDLER();
            }

            // Initialize a new lexer for the nested unit file
            Lexer nested_unit_lexer;
            initLexer(&nested_unit_lexer, nested_unit_path);

            // Initialize a new parser for the nested unit file
            Parser nested_unit_parser;
            nested_unit_parser.lexer = &nested_unit_lexer;
            nested_unit_parser.current_token = getNextToken(&nested_unit_lexer);

            // Parse the nested unit using the new parser
            AST *nested_unit_ast = unitParser(&nested_unit_parser, recursion_depth + 1); // Recursively parse with incremented depth

            // Link the nested unit symbols into the global scope
            linkUnit(nested_unit_ast, recursion_depth);
        }
    }

    // Parse the interface section
    eat(parser, TOKEN_INTERFACE);
    AST *interface_decls = declarations(parser, true); // Pass a flag to indicate interface section
    addChild(unit_node, interface_decls);
    
    // After parsing the interface section:
    Symbol *unitSymbols = buildUnitSymbolTable(interface_decls);
    unit_node->symbol_table = unitSymbols;


    // Parse IMPLEMENTATION section
    eat(parser, TOKEN_IMPLEMENTATION);
    AST *impl_decls = declarations(parser, false); // Pass a flag to indicate implementation section
    addChild(unit_node, impl_decls);

    // Handle INITIALIZATION block (if present)
    int has_initialization = 0;
    if (parser->current_token->type == TOKEN_BEGIN) {
        AST *init_block = compoundStatement(parser); // Parses BEGIN...END (consumes END)
        addChild(unit_node, init_block);
        has_initialization = 1;
    }

    // Consume the final 'end.' based on initialization presence
    if (has_initialization) {
        // After compoundStatement, current token is '.' from 'end.'
        if (parser->current_token->type != TOKEN_PERIOD) {
            fprintf(stderr, "Error: Expected '.' after unit end at line %d\n", parser->lexer->line);
            EXIT_FAILURE_HANDLER();
        }
        eat(parser, TOKEN_PERIOD);
    } else {
        // No initialization block: consume 'end' then '.'
        eat(parser, TOKEN_END);
        eat(parser, TOKEN_PERIOD);
    }

    return unit_node;
}

void errorParser(Parser *parser, const char *msg) {
    extern AST *globalRoot;
   // printf("===== Global AST Dump START ======\n");
    //dumpAST(globalRoot, 0);
  //  printf("===== Global AST Dump END ======\n");
    fprintf(stderr, "Parser error at line %d, column %d: %s (found token: %s)\n",
            parser->lexer->line, parser->lexer->column, msg,
            tokenTypeToString(parser->current_token->type));
    EXIT_FAILURE_HANDLER();
}

void addProcedure(AST *proc_decl) {
    // Allocate a new Procedure structure.
    Procedure *proc = malloc(sizeof(Procedure));
    if (!proc) {
        fprintf(stderr, "Memory allocation error in add_procedure\n");
        EXIT_FAILURE_HANDLER();
    }
    // Use the procedure name from the token of the declaration.
    const char *originalName = proc_decl->token->value;
    char *lowerName = strdup(originalName); // Duplicate the original name
    if (!lowerName) {
        fprintf(stderr, "Memory allocation error in add_procedure (strdup)\n");
        free(proc); // Free allocated proc struct
        EXIT_FAILURE_HANDLER();
    }
    // Convert the duplicated name to lowercase
    for (int i = 0; lowerName[i]; i++) {
        lowerName[i] = tolower((unsigned char)lowerName[i]);
    }
    proc->name = lowerName; // Store the lowercase name

    // Original code continues...
    proc->proc_decl = proc_decl;
    proc->next = procedure_table;
    procedure_table = proc;

    // Optional Debug Print
    #ifdef DEBUG
    if (dumpExec) {
        fprintf(stderr, "[DEBUG] addProcedure: Added procedure '%s' (original: '%s') to table.\n", proc->name, originalName);
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
        if (strcmp(entry->name, name) == 0) {
            return entry->typeAST;
        }
        entry = entry->next;
    }
    return NULL;
}

AST *buildProgramAST(Parser *main_parser) {
    // --- Copy the PROGRAM token BEFORE calling eat ---
    Token *originalProgToken = main_parser->current_token;
    if (!originalProgToken || originalProgToken->type != TOKEN_PROGRAM) { // Add NULL check
        errorParser(main_parser, "Expected 'program' keyword");
        return newASTNode(AST_NOOP, NULL); // Or handle error appropriately
    }
    Token *copiedProgToken = copyToken(originalProgToken); // <<< COPY
    if (!copiedProgToken) {
        fprintf(stderr, "Memory allocation failed in buildProgramAST (copyToken program)\n");
        EXIT_FAILURE_HANDLER();
    }
    // ---

    // Eat the ORIGINAL program token (eatInternal frees it)
    eat(main_parser, TOKEN_PROGRAM);

    // --- Copy the program name token BEFORE calling eat ---
    Token *progNameOriginal = main_parser->current_token;
    if (!progNameOriginal || progNameOriginal->type != TOKEN_IDENTIFIER) { // Add NULL check
         errorParser(main_parser, "Expected program name identifier");
         freeToken(copiedProgToken); // Clean up first copy
         return newASTNode(AST_NOOP, NULL);
    }
    Token *progNameCopied = copyToken(progNameOriginal); // <<< COPY
    if(!progNameCopied){
         fprintf(stderr, "Memory allocation failed in buildProgramAST (copyToken name)\n");
         freeToken(copiedProgToken); // Clean up first copy
         EXIT_FAILURE_HANDLER();
    }
    // ---
    // Eat the ORIGINAL program name token (eatInternal frees it)
    eat(main_parser, TOKEN_IDENTIFIER);
    // Create the name node using the COPIED name token
    // newASTNode makes its own copy, which is fine
    AST *prog_name_node = newASTNode(AST_VARIABLE, progNameCopied);
    // Free the COPIED name token now that newASTNode is done with it
    freeToken(progNameCopied); // <<< Free copy of name token
    // ---

    // Handle optional parameter list and semicolon
    if (main_parser->current_token && main_parser->current_token->type == TOKEN_LPAREN) { // Add NULL check
        eat(main_parser, TOKEN_LPAREN); // Frees '('
        while (main_parser->current_token && main_parser->current_token->type != TOKEN_RPAREN) { // Add NULL check
             // Assume parameters aren't stored, just consume
             Token* paramToken = main_parser->current_token;
             eat(main_parser, paramToken->type); // Eat whatever token it is (eatInternal frees it)
        }
        if (main_parser->current_token && main_parser->current_token->type == TOKEN_RPAREN) { // Add NULL check
             eat(main_parser, TOKEN_RPAREN); // Frees ')'
        } else {
             errorParser(main_parser, "Expected ')' after program parameters");
             freeToken(copiedProgToken); // Free the copy before returning error node
             return newASTNode(AST_NOOP, NULL);
        }
    }
    // Consume the final semicolon after program header
    if (main_parser->current_token && main_parser->current_token->type == TOKEN_SEMICOLON) { // Add NULL check
        eat(main_parser, TOKEN_SEMICOLON); // Frees ';'
    } else { // Error if missing semicolon (and not EOF)
         // Handle case where EOF might be acceptable after program name (e.g., empty program)
         // For now, require semicolon unless it's EOF right after the name (which is unlikely valid Pascal)
         if (main_parser->current_token) {
              errorParser(main_parser, "Expected ';' after program header");
         } else {
              errorParser(main_parser, "Unexpected end of file after program header");
         }
         freeToken(copiedProgToken); // Free the copy before returning error node
         return newASTNode(AST_NOOP, NULL);
    }


    // Handle USES clause (ensure token handling inside is correct)
    AST *uses_clause = NULL;
    List *unit_list = NULL; // Initialize unit_list to NULL
    if (main_parser->current_token && main_parser->current_token->type == TOKEN_USES) {
        eat(main_parser, TOKEN_USES); // Frees USES
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        unit_list = createList(); // Create list only if USES exists
        do {
            Token *unit_token_original = main_parser->current_token;
            if (!unit_token_original || unit_token_original->type != TOKEN_IDENTIFIER) {
                errorParser(main_parser, "Expected unit name in uses clause");
                // Cleanup needed?
                freeToken(copiedProgToken); // Free the program token copy
                if (uses_clause) freeAST(uses_clause); // Might cause issues if partially built
                if (unit_list) freeList(unit_list); // <<< Corrected Function Call (1 arg)
                return newASTNode(AST_NOOP, NULL);
            }
            // listAppend uses strdup internally [cite: 24]
            listAppend(unit_list, unit_token_original->value);
            eat(main_parser, TOKEN_IDENTIFIER); // Eat original unit name (frees it)

            if (main_parser->current_token && main_parser->current_token->type == TOKEN_COMMA) {
                eat(main_parser, TOKEN_COMMA); // Frees ','
            } else {
                break; // Exit loop
            }
        } while (main_parser->current_token); // Loop condition check

        // Check for semicolon after uses list
        if (main_parser->current_token && main_parser->current_token->type == TOKEN_SEMICOLON) {
            eat(main_parser, TOKEN_SEMICOLON); // Frees ';'
        } else { // Error if not semicolon or EOF
             errorParser(main_parser, "Expected ';' after uses clause");
             // Cleanup list etc.
             freeToken(copiedProgToken);
             if (uses_clause) freeAST(uses_clause); // Might cause issues
             if (unit_list) freeList(unit_list); // <<< Corrected Function Call (1 arg)
             return newASTNode(AST_NOOP, NULL);
        }

        // Attach list to uses_clause node
        uses_clause->unit_list = unit_list; // unit_list now owned by uses_clause

        // Link units (assuming linkUnit/unitParser handle their own memory)
        // Add error handling around findUnitFile, fopen, etc. if needed
        // In src/parser.c -> buildProgramAST() -> within the 'if (uses_clause)' block

                     // --- Start: Process and Link Units from 'uses' list ---
                     for (int i = 0; i < listSize(unit_list); i++) {
                         char *unit_name = listGet(unit_list, i);
         #ifdef DEBUG
                         fprintf(stderr, "[DEBUG USES] Processing unit '%s'...\n", unit_name);
         #endif
                         // Attempt to find the unit source file
                         char *unit_path = findUnitFile(unit_name); // Assumes findUnitFile returns allocated string or NULL

                         if (unit_path == NULL) {
                             // Use errorParser for consistency
                             char error_msg[256];
                             snprintf(error_msg, sizeof(error_msg), "Unit '%s' specified in USES clause not found.", unit_name);
                             errorParser(main_parser, error_msg); // errorParser should exit
                             // Clean up potentially partially built AST? Very complex. Exit for now.
                             // If errorParser doesn't exit, ensure cleanup happens here.
                              if(unit_list) freeList(unit_list); // Free list contents
                              if(uses_clause) freeAST(uses_clause); // Free uses node
                              // Free other potentially allocated nodes like prog_name_node etc.
                              freeToken(copiedProgToken); // Free program token copy
                             return NULL; // Indicate failure
                         }

         #ifdef DEBUG
                         fprintf(stderr, "[DEBUG USES] Found unit '%s' at path: %s\n", unit_name, unit_path);
         #endif

                         // --- Parse the found unit file ---
                         // 1. Read the unit file content
                         FILE *unit_file = fopen(unit_path, "r");
                         if (!unit_file) {
                              char error_msg[512];
                              snprintf(error_msg, sizeof(error_msg), "Could not open unit file '%s' for unit '%s'", unit_path, unit_name);
                              perror(error_msg); // Print system error message too
                              free(unit_path); // Free path returned by findUnitFile
                              // Cleanup...
                              EXIT_FAILURE_HANDLER(); // Exit on file open error
                              return NULL;
                         }
                         fseek(unit_file, 0, SEEK_END);
                         long unit_fsize = ftell(unit_file);
                         rewind(unit_file);
                         char *unit_source = malloc(unit_fsize + 1);
                         if (!unit_source) {
                             fprintf(stderr, "Memory allocation error reading unit '%s'\n", unit_name);
                             fclose(unit_file);
                             free(unit_path);
                             // Cleanup...
                             EXIT_FAILURE_HANDLER();
                              return NULL;
                         }
                         fread(unit_source, 1, unit_fsize, unit_file);
                         unit_source[unit_fsize] = '\0';
                         fclose(unit_file);
                         // ---

                         // 2. Initialize a new lexer and parser for the unit source
                         Lexer unit_lexer;
                         initLexer(&unit_lexer, unit_source);
                         Parser unit_parser; // <<< CHANGE: Use a separate parser instance
                         unit_parser.lexer = &unit_lexer;
                         unit_parser.current_token = getNextToken(&unit_lexer); // Initialize the first token

                         // 3. Parse the unit recursively (use unitParser)
                         // Pass recursion depth (start at 1 for first level units)
         #ifdef DEBUG
                         fprintf(stderr, "[DEBUG USES] Calling unitParser for '%s'...\n", unit_name);
         #endif
                         AST *unit_ast = unitParser(&unit_parser, 1); // <<< Pass unit_parser, start depth 1
                         if (!unit_ast) {
                             // unitParser should have reported an error via errorParser
                             fprintf(stderr, "Error: Failed to parse unit '%s'.\n", unit_name);
                             free(unit_source);
                             free(unit_path);
                             // Cleanup...
                             EXIT_FAILURE_HANDLER();
                              return NULL;
                         }
         #ifdef DEBUG
                         fprintf(stderr, "[DEBUG USES] Finished parsing unit '%s'. AST node: %p\n", unit_name, (void*)unit_ast);
         #endif
                         // ---

                         // 4. Link symbols from the unit's interface into the global scope
                         //    (Requires buildUnitSymbolTable and linkUnit functions)
         #ifdef DEBUG
                          fprintf(stderr, "[DEBUG USES] Building symbol table for unit '%s'...\n", unit_name);
         #endif
                          // Build the symbol table from the interface part (assuming child[0] is interface decls)
                          // Note: buildUnitSymbolTable might need access to the Type Table if units define types
                          // Assuming unit_ast->children[0] holds the INTERFACE declarations compound node
                          Symbol* unitSymTable = buildUnitSymbolTable(unit_ast->children[0]);
                          unit_ast->symbol_table = unitSymTable; // Attach symbol table to unit AST node
         #ifdef DEBUG
                          fprintf(stderr, "[DEBUG USES] Linking unit '%s'...\n", unit_name);
         #endif
                         linkUnit(unit_ast, 1); // <<< Call linkUnit with the parsed unit AST

                         // 5. Cleanup resources for this unit
                         free(unit_source);
                         free(unit_path);
                         // Do NOT free unit_ast here if its symbols/types are now linked globally
                         // Memory management of shared AST nodes (like type definitions) needs care.
                         // If linkUnit copies necessary info, freeAST(unit_ast) might be okay. Assume not for now.

                     } // End for loop processing unit_list
                     // --- End: Process and Link Units ---
    }

    // Parse the main block
    AST *block_node = block(main_parser); // block handles its internal tokens
    if (!block_node) { // block returns NULL on error
        errorParser(main_parser, "Failed to parse main program block"); // <<< Corrected typo: main_parser
        freeToken(copiedProgToken); // Free copy
        // Free uses clause? freeAST should handle it if called later
        // If not, need: if (uses_clause) freeAST(uses_clause); // which also frees unit_list if freeAST is correct
        return newASTNode(AST_NOOP, NULL);
    }

    // Expect final '.'
    if (main_parser->current_token && main_parser->current_token->type == TOKEN_PERIOD) {
         eat(main_parser, TOKEN_PERIOD); // Frees '.'
    } else { // Error if not period or EOF
         errorParser(main_parser, "Expected '.' at end of program");
         freeToken(copiedProgToken); // Free copy
         // Free block_node and uses_clause? Depends on ownership and error handling strategy
         // freeAST(block_node);
         // if (uses_clause) freeAST(uses_clause);
         return newASTNode(AST_NOOP, NULL);
    }


    // --- Create the main PROGRAM node using the COPIED program token ---
    // newASTNode makes its own internal copy of the token passed to it
    AST *programNode = newASTNode(AST_PROGRAM, copiedProgToken);
    // ---

    setLeft(programNode, prog_name_node); // prog_name_node already built
    setRight(programNode, block_node);    // block_node already built
    if (block_node) block_node->is_global_scope = true; // Safety check

    // Add uses_clause node if it was created
    if (uses_clause) {
        addChild(programNode, uses_clause); // addChild handles parent pointers
    }

    // --- Free the COPIED program token ---
    freeToken(copiedProgToken); // <<< Free the copy made at the start
    // ---

    return programNode;
}

AST *block(Parser *parser) {
    bool in_interface = false;
    AST *decl = declarations(parser, in_interface);
    AST *comp_stmt = compoundStatement(parser);
    AST *node = newASTNode(AST_BLOCK, NULL);
    addChild(node, decl);
    addChild(node, comp_stmt);
    DEBUG_DUMP_AST(node, 0);
    return node;
}

AST *procedureDeclaration(Parser *parser, bool in_interface) {
    eat(parser, TOKEN_PROCEDURE); // Frees PROCEDURE token

    // --- COPY the procedure name token BEFORE calling eat ---
    Token *originalProcNameToken = parser->current_token;
    if (originalProcNameToken->type != TOKEN_IDENTIFIER) {
        errorParser(parser, "Expected procedure name identifier");
        return newASTNode(AST_NOOP, NULL);
    }
    Token *copiedProcNameToken = copyToken(originalProcNameToken); // <<< COPY
    if (!copiedProcNameToken) {
        fprintf(stderr, "Memory allocation failed in procedureDeclaration (copyToken)\n");
        EXIT_FAILURE_HANDLER();
    }
    // ---

    // Eat the ORIGINAL procedure name token; eatInternal will free it
    eat(parser, TOKEN_IDENTIFIER);

    // --- Create the AST node using the COPIED token ---
    // newASTNode makes its own copy of the token passed to it
    AST *node = newASTNode(AST_PROCEDURE_DECL, copiedProcNameToken); // <<< Use copied token
    // ---

    // Parse parameters if present
    if (parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN); // Frees '(' token
        AST *params = paramList(parser);
        eat(parser, TOKEN_RPAREN); // Frees ')' token
        // Safely transfer children from params to node
        if (params && params->child_count > 0) {
            node->children = params->children;
            node->child_count = params->child_count;
            params->children = NULL; // Avoid double free
            params->child_count = 0;
            for (int i = 0; i < node->child_count; i++) { // Update parent pointers
                if (node->children[i]) node->children[i]->parent = node;
            }
        } else {
            node->children = NULL;
            node->child_count = 0;
        }
        if (params) free(params); // Free the temporary compound node
    } else {
        node->children = NULL;
        node->child_count = 0;
    }

    // Handle interface vs implementation body
    if (in_interface) {
        // No body needed
    } else { // Implementation part
        eat(parser, TOKEN_SEMICOLON); // Frees ';' token

        AST *local_declarations = declarations(parser, false);
        AST *compound_body = compoundStatement(parser); // Parses BEGIN..END

        AST *blockNode = newASTNode(AST_BLOCK, NULL);
        addChild(blockNode, local_declarations);
        addChild(blockNode, compound_body);
        blockNode->is_global_scope = false;

        setRight(node, blockNode); // Attach block to procedure node
    }

    // --- Free the COPIED procedure name token ---
    freeToken(copiedProcNameToken); // <<< Free the copy made at the start of this function
    // ---

    DEBUG_DUMP_AST(node, 0); // Optional debug dump
    return node;
}

AST *constDeclaration(Parser *parser) {
    // --- Copy the constant name token BEFORE calling eat ---
    Token *originalConstNameToken = parser->current_token;
    if (originalConstNameToken->type != TOKEN_IDENTIFIER) {
        errorParser(parser, "Expected identifier for constant name");
        return newASTNode(AST_NOOP, NULL);
    }
    Token *copiedConstNameToken = copyToken(originalConstNameToken); // <<< COPY
    if (!copiedConstNameToken) {
        fprintf(stderr, "Memory allocation failed in constDeclaration (copyToken name)\n");
        EXIT_FAILURE_HANDLER();
    }
    // ---

    // Eat the ORIGINAL constant name token (eatInternal frees it)
    eat(parser, TOKEN_IDENTIFIER);

    AST *typeNode = NULL; // For typed constants (Pascal extension)
    AST *valueNode = NULL;

    // Check for optional type specifier (Pascal extension for typed constants)
    if (parser->current_token->type == TOKEN_COLON) {
        eat(parser, TOKEN_COLON); // Frees ':'
        typeNode = typeSpecifier(parser, 1); // Parse the type
        // Basic validation (more detailed checks might be needed later)
        if (!typeNode) {
            errorParser(parser, "Failed to parse type specifier in typed constant declaration");
            freeToken(copiedConstNameToken); // Free the copy before returning
            return newASTNode(AST_NOOP, NULL);
        }
        // Assuming typed constants are mainly for arrays currently
        if (typeNode->type != AST_ARRAY_TYPE && typeNode->var_type != TYPE_ARRAY) {
            // Check if it's a reference to an array type
            int is_array_ref = 0;
            if (typeNode->type == AST_TYPE_REFERENCE && typeNode->token) {
                AST* ref_target = lookupType(typeNode->token->value);
                if (ref_target && ref_target->var_type == TYPE_ARRAY) {
                    is_array_ref = 1;
                }
            }
            if (!is_array_ref) {
                 errorParser(parser, "Expected array type specifier for typed constant array declaration");
                 freeToken(copiedConstNameToken); // Free the copy
                 return newASTNode(AST_NOOP, NULL);
            }
        }
    }

    eat(parser, TOKEN_EQUAL); // Frees '='

    // Parse the value: either simple expression or array initializer
    if (typeNode != NULL) { // If type was specified, expect array initializer '('
        if (parser->current_token->type != TOKEN_LPAREN) {
            errorParser(parser, "Expected '(' for array constant initializer list");
            freeToken(copiedConstNameToken); // Free the copy
            // Free typeNode AST if necessary? Depends on ownership. Assuming caller handles.
            return newASTNode(AST_NOOP, NULL);
        }
        valueNode = parseArrayInitializer(parser); // Parses (...)
        if(valueNode && typeNode) setRight(valueNode, typeNode); // Link type to literal node for runtime use
    } else { // No type specified, parse a simple constant expression
        valueNode = expr(parser);
    }

    eat(parser, TOKEN_SEMICOLON); // Frees ';'

    // --- Create the main declaration node using the COPIED token ---
    AST *node = newASTNode(AST_CONST_DECL, copiedConstNameToken); // <<< Use copy
    // ---
    setLeft(node, valueNode); // Link the value/literal node

    if (typeNode) {
       setRight(node, typeNode); // Link explicit type specifier AST if present
       setTypeAST(node, TYPE_ARRAY); // Assume typed const is array for now
    } else {
       // Type will be inferred during semantic analysis/evaluation
       setTypeAST(node, TYPE_VOID);
    }

    // --- Free the COPIED constant name token ---
    freeToken(copiedConstNameToken); // <<< Free the copy made at the start
    // ---

    return node;
}

AST *typeSpecifier(Parser *parser, int allowAnonymous) { // allowAnonymous might not be needed now
    AST *node = NULL;
    Token *typeToken = parser->current_token; // Store current token at the start

    if (parser->current_token->type == TOKEN_RECORD) {
        node = newASTNode(AST_RECORD_TYPE, typeToken); // Use RECORD token
        eat(parser, TOKEN_RECORD);

        while (parser->current_token->type == TOKEN_IDENTIFIER) {
            AST *fieldDecl = newASTNode(AST_VAR_DECL, NULL);

            // Parse potentially comma-separated list of field names for this type
            while (1) {
                AST *varNode = newASTNode(AST_VARIABLE, parser->current_token);
                eat(parser, TOKEN_IDENTIFIER);
                addChild(fieldDecl, varNode);

                if (parser->current_token->type == TOKEN_COMMA) {
                    eat(parser, TOKEN_COMMA);
                } else {
                    break;
                }
            }

            eat(parser, TOKEN_COLON);
            AST *fieldType = typeSpecifier(parser, 1); // Allow anonymous types within records
            // Set the type and definition link for the field declaration group
            setTypeAST(fieldDecl, fieldType->var_type);
            setRight(fieldDecl, fieldType); // Link VAR_DECL to the type definition used
            addChild(node, fieldDecl); // Add this field group to the record type node

            // Fields are separated by semicolons
            if (parser->current_token->type == TOKEN_SEMICOLON) {
                eat(parser, TOKEN_SEMICOLON);
                 // Check if END follows semicolon, if so, break loop
                 if (parser->current_token->type == TOKEN_END) {
                     break;
                 }
            } else if (parser->current_token->type != TOKEN_END) {
                // If it's not a semicolon and not END, it's an error
                errorParser(parser, "Expected semicolon or END in record declaration");
                // Consider breaking or returning error node
                break;
            }
            // If END is next, the outer loop condition will handle it
        } // End while (parsing fields)

        eat(parser, TOKEN_END); // Consume END of record
        setTypeAST(node, TYPE_RECORD); // Mark the main node as RECORD type
    }
    // --- Enum parsing block removed from here ---
    else if (parser->current_token->type == TOKEN_ARRAY) {
        node = parseArrayType(parser); // Delegate to array parsing function
        setTypeAST(node, TYPE_ARRAY);
    }
    else if (strcasecmp(parser->current_token->value, "string") == 0) {
        node = newASTNode(AST_VARIABLE, typeToken); // Use 'string' token
        setTypeAST(node, TYPE_STRING);
        eat(parser, TOKEN_IDENTIFIER); // Consume 'string' identifier
        // Check for fixed-length string specifier: string[length]
        if (parser->current_token->type == TOKEN_LBRACKET) {
            eat(parser, TOKEN_LBRACKET);
            // The length expression should evaluate to an integer constant ideally
            AST *lengthNode = expr(parser); // Parse the length expression
             // Semantic analysis should later verify this is a constant integer > 0
            eat(parser, TOKEN_RBRACKET);
            setRight(node, lengthNode); // Attach length expression node
        }
    }
    else { // Handle basic types (integer, real, etc.) and user-defined type references
        char *typeName = typeToken->value; // Get the identifier string

        if (strcasecmp(typeName, "integer") == 0 ||
            strcasecmp(typeName, "longint") == 0 || // Treat synonyms as INTEGER
            strcasecmp(typeName, "cardinal") == 0) {
            node = newASTNode(AST_VARIABLE, typeToken); // Node represents the type name
            setTypeAST(node, TYPE_INTEGER);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else if (strcasecmp(typeName, "real") == 0) {
            node = newASTNode(AST_VARIABLE, typeToken);
            setTypeAST(node, TYPE_REAL);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else if (strcasecmp(typeName, "char") == 0) {
            node = newASTNode(AST_VARIABLE, typeToken);
            setTypeAST(node, TYPE_CHAR);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else if (strcasecmp(typeName, "byte") == 0) {
            node = newASTNode(AST_VARIABLE, typeToken);
            setTypeAST(node, TYPE_BYTE);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else if (strcasecmp(typeName, "word") == 0) {
            node = newASTNode(AST_VARIABLE, typeToken);
            setTypeAST(node, TYPE_WORD);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else if (strcasecmp(typeName, "boolean") == 0) {
            node = newASTNode(AST_VARIABLE, typeToken);
            setTypeAST(node, TYPE_BOOLEAN);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else if (strcasecmp(typeName, "file") == 0 || // Treat file and text as FILE type
                 strcasecmp(typeName, "text") == 0) {
            node = newASTNode(AST_VARIABLE, typeToken);
            setTypeAST(node, TYPE_FILE);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else if (strcasecmp(typeName, "mstream") == 0) { // If you have this type
            node = newASTNode(AST_VARIABLE, typeToken);
            setTypeAST(node, TYPE_MEMORYSTREAM);
            eat(parser, TOKEN_IDENTIFIER);
        }
        else { // Assume it's a user-defined type reference
            AST *userType = lookupType(typeName); // Look up in the type table
            if (!userType) {
                // This happens if a type is used before it's declared (or is misspelled)
                fprintf(stderr, "Parser Error at Line %d, Col %d: Undefined type '%s'\n",
                        parser->lexer->line, parser->lexer->column, typeName);
                EXIT_FAILURE_HANDLER(); // Or other error handling
            }
            // Create a reference node pointing to the actual definition
            node = newASTNode(AST_TYPE_REFERENCE, typeToken);
            setTypeAST(node, userType->var_type); // Copy VarType (e.g., TYPE_RECORD, TYPE_ENUM) from definition
            setRight(node, userType); // Link reference node to the actual type definition node
            eat(parser, TOKEN_IDENTIFIER); // Consume the type name identifier
        }
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

// This function parses a variable reference (which may have field or array accesses),
// but it does not interpret a following '(' as a procedure call.
AST *variable(Parser *parser) {
    // The current token should be an identifier.
    Token *token = parser->current_token;
    AST *node = newASTNode(AST_VARIABLE, token);
    eat(parser, TOKEN_IDENTIFIER);
    // Now allow field and array accesses.
    while (parser->current_token->type == TOKEN_PERIOD ||
           parser->current_token->type == TOKEN_LBRACKET) {
        if (parser->current_token->type == TOKEN_PERIOD) {
            eat(parser, TOKEN_PERIOD);
            Token *fieldToken = parser->current_token;
            if (fieldToken->type != TOKEN_IDENTIFIER)
                errorParser(parser, "Expected field name after '.'");
            AST *fieldAccess = newASTNode(AST_FIELD_ACCESS, fieldToken);
            eat(parser, TOKEN_IDENTIFIER);
            setLeft(fieldAccess, node);
            node = fieldAccess;
        } else if (parser->current_token->type == TOKEN_LBRACKET) {
            eat(parser, TOKEN_LBRACKET);
            AST *indexExpr = expr(parser);
            eat(parser, TOKEN_RBRACKET);
            AST *arrayAccess = newASTNode(AST_ARRAY_ACCESS, NULL);
            setLeft(arrayAccess, node);
            addChild(arrayAccess, indexExpr);
            node = arrayAccess;
        }
    }
    return node;
}

AST *varDeclaration(Parser *parser, bool isGlobal) {
    AST *node = newASTNode(AST_VAR_DECL, NULL);

    // Parse variable list
    while (parser->current_token->type == TOKEN_IDENTIFIER) {
        AST *varNode = newASTNode(AST_VARIABLE, parser->current_token);
        eat(parser, TOKEN_IDENTIFIER);
        addChild(node, varNode);

        if (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else {
            break;
        }
    }

    eat(parser, TOKEN_COLON);
    AST *typeNode = typeSpecifier(parser, 0);
    setTypeAST(node, typeNode->var_type);
    setRight(node, typeNode);

    // Preserve enum info in AST metadata, if applicable
    if (typeNode->type == AST_TYPE_REFERENCE) {
        AST *actualType = lookupType(typeNode->token->value);
        if (actualType && actualType->type == AST_ENUM_TYPE) {
            // Mark this declaration as using an enum type
            node->var_type = TYPE_ENUM;
            // Optionally, store the enum name somewhere if useful later
            // (e.g., in a custom field if your AST supports it)
        }
    }

    return node;
}

AST *functionDeclaration(Parser *parser, bool in_interface) {
    eat(parser, TOKEN_FUNCTION); // Consume FUNCTION keyword

    Token *funcNameOriginal = parser->current_token;
    if (funcNameOriginal->type != TOKEN_IDENTIFIER) {
        errorParser(parser, "Expected function name identifier");
        return newASTNode(AST_NOOP, NULL); // Error recovery
    }
    Token *funcNameCopied = copyToken(funcNameOriginal); // <<< COPY
    if (!funcNameCopied) {
        fprintf(stderr, "Memory allocation failed in functionDeclaration (copyToken name)\n");
        EXIT_FAILURE_HANDLER();
    }

    // Eat the ORIGINAL identifier token (eatInternal frees it)
    eat(parser, TOKEN_IDENTIFIER);

    // newASTNode makes its own internal copy of funcNameCopied
    AST *node = newASTNode(AST_FUNCTION_DECL, funcNameCopied);

    // Parse parameters if present (Unchanged)
    if (parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        AST *params = paramList(parser);
        eat(parser, TOKEN_RPAREN);
        // Transfer parameter children safely (Your existing logic here)
        if (params && params->child_count > 0) {
            node->children = params->children;
            node->child_count = params->child_count;
            params->children = NULL;
            params->child_count = 0;
             for (int i = 0; i < node->child_count; i++) {
                 if (node->children[i]) node->children[i]->parent = node;
             }
        } else {
             node->children = NULL;
             node->child_count = 0;
        }
        if (params) free(params);
    } else {
         node->children = NULL;
         node->child_count = 0;
    }

    // Parse return type (Unchanged)
    eat(parser, TOKEN_COLON);
    AST *returnType = typeSpecifier(parser, 0); // Allow named types for return
    setRight(node, returnType); // Return type node is stored in 'right'
    node->var_type = returnType->var_type; // Set function's return type early

    // Handle interface vs implementation (Your existing logic here)
    if (in_interface) {
        // Interface section: no body expected
    } else { // Implementation part
        eat(parser, TOKEN_SEMICOLON);
        AST *local_declarations = declarations(parser, false);
        AST *compound_body = compoundStatement(parser);
        AST *blockNode = newASTNode(AST_BLOCK, NULL);
        addChild(blockNode, local_declarations);
        addChild(blockNode, compound_body);
        blockNode->is_global_scope = false;
        setExtra(node, blockNode); // Function block goes in 'extra'
    }

    // --- MODIFICATION START: Free the copied token ---
    freeToken(funcNameCopied); // <<< Free the copy made at the start
    // --- END MODIFICATION ---


    DEBUG_DUMP_AST(node, 0); // Original debug dump
    return node;
} // End of functionDeclaration

AST *paramList(Parser *parser) {
    AST *compound = newASTNode(AST_COMPOUND, NULL); // Final list of parameter AST_VAR_DECL nodes

    // Loop until we see the closing parenthesis.
    while (parser->current_token->type != TOKEN_RPAREN) {
        int byRef = 0;
        // Check for pass-by-reference keyword (VAR or OUT).
        if (parser->current_token->type == TOKEN_VAR || parser->current_token->type == TOKEN_OUT) {
            byRef = 1;
            eat(parser, parser->current_token->type); // Eat either VAR or OUT
        }

        // Temporary group node to hold comma-separated names of the *same* type.
        // This group node itself isn't added to the final AST list.
        AST *group = newASTNode(AST_VAR_DECL, NULL);
        // Parse one or more identifiers separated by commas for this type group.
        while (1) {
            // --- Make a copy of the identifier token for the new AST node ---
            Token* originalIdToken = parser->current_token;
            if (originalIdToken->type != TOKEN_IDENTIFIER) {
                errorParser(parser, "Expected identifier in parameter list");
                freeAST(group); // Clean up temp group
                freeAST(compound); // Clean up compound list being built
                return NULL; // Indicate error
            }
            Token* copiedIdToken = copyToken(originalIdToken);
            if (!copiedIdToken) {
                 fprintf(stderr, "Memory allocation failed for token copy in paramList\n");
                 freeAST(group); freeAST(compound);
                 EXIT_FAILURE_HANDLER();
             }
            // ---

            // Eat the ORIGINAL identifier token (eatInternal frees it)
            eat(parser, TOKEN_IDENTIFIER);

            // Add a new AST_VARIABLE node using the copied token to the temporary group
            // This node will be freed when 'group' is freed later.
            AST *id_node = newASTNode(AST_VARIABLE, copiedIdToken);
            addChild(group, id_node); // addChild sets parent pointer within the group

            // --- Free the copied token, as newASTNode made its own copy ---
            freeToken(copiedIdToken);
            // ---

            // Check if another identifier follows (comma separation)
            if (parser->current_token->type == TOKEN_COMMA) {
                eat(parser, TOKEN_COMMA); // Consume comma, loop for next name
            } else {
                break; // No comma, this group of names is done
            }
        } // End while(1) for parsing identifiers in a group

        // Expect a colon and then the type specifier for this group.
        eat(parser, TOKEN_COLON); // Consume ':'
        // Parse the type definition node (e.g., RECORD_TYPE, VARIABLE for basic types, etc.)
        // typeSpecifier returns the AST node defining the type (e.g., AST_RECORD_TYPE, AST_VARIABLE for 'integer')
        AST *typeNode = typeSpecifier(parser, 1); // Allow anonymous types (like record) here
        if (!typeNode) { // Handle error from typeSpecifier
            errorParser(parser, "Failed to parse type specifier in parameter list");
            freeAST(group); freeAST(compound);
            return NULL; // Indicate error
        }

        // Apply the parsed type info (just the VarType enum) to the temporary group node.
        // This isn't strictly necessary as the group node is temporary, but good for consistency.
        setTypeAST(group, typeNode->var_type);

        // Now, create the *actual* parameter declaration nodes (AST_VAR_DECL)
        // for each identifier collected in the temporary 'group' node.
        for (int i = 0; i < group->child_count; i++) {
            // Create the final AST node (AST_VAR_DECL) that will be added to the procedure's parameter list
            AST *param_decl = newASTNode(AST_VAR_DECL, NULL);
            param_decl->child_count = 1;      // Each param_decl has one child: the variable name node
            param_decl->child_capacity = 1;
            param_decl->children = malloc(sizeof(AST *));
            if (!param_decl->children) {
                 fprintf(stderr, "Memory allocation error for param_decl children\n");
                 // Need more robust cleanup here
                 freeAST(typeNode); freeAST(group); freeAST(compound); freeAST(param_decl);
                 EXIT_FAILURE_HANDLER();
             }

            // Create the AST_VARIABLE node for the parameter name ('s') using the token from the group.
            // We need to make a fresh copy of the token for this new node.
            Token* nameTokenCopy = copyToken(group->children[i]->token);
             if (!nameTokenCopy) {
                  fprintf(stderr, "Memory allocation failed copying parameter name token\n");
                  // Cleanup...
                  EXIT_FAILURE_HANDLER();
             }
            param_decl->children[0] = newASTNode(AST_VARIABLE, nameTokenCopy);
            freeToken(nameTokenCopy); // Free the copy used by newASTNode

            // *** Explicitly set the parent pointer for the child VARIABLE node ***
            if (param_decl->children[0]) {
                param_decl->children[0]->parent = param_decl; // <<< Parent pointer fix
            } else {
                 errorParser(parser, "Failed to create variable node for parameter name");
                 // Cleanup...
                 EXIT_FAILURE_HANDLER();
            }

            // Copy the type enum (e.g., TYPE_RECORD) and by-reference flag
            param_decl->var_type = group->var_type;
            param_decl->by_ref = byRef;

            // Link the VAR_DECL node to the actual Type Definition node parsed earlier
            // This is crucial for the interpreter/compiler to know the structure of the type.
            setRight(param_decl, typeNode); // Links VAR_DECL to RECORD_TYPE, etc.

            // Add the completed parameter declaration node to the compound list returned by this function
            addChild(compound, param_decl);
        }

        // Clean up the temporary group node and its children (the AST_VARIABLE nodes within it).
        // Do not free typeNode here, as it's now referenced by the param_decl nodes via their 'right' pointer.
        // freeAST should handle the children (AST_VARIABLE nodes) recursively.
        freeAST(group);

        // After processing a parameter group (name:type), check for a separator (;) or the end ')'.
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON); // Consume semicolon, continue loop for next parameter group
        } else if (parser->current_token->type != TOKEN_RPAREN) {
            // If it's not a semicolon and not the closing parenthesis, it's an error.
            errorParser(parser, "Expected ';' or ')' after parameter declaration");
            // Need robust cleanup if erroring out here
            freeAST(typeNode); freeAST(compound);
            return NULL;
        } else {
            // Found RPAREN, the loop condition will handle exiting.
            break;
        }
    } // End while != RPAREN

    // No need for debug dump here, let caller dump if needed
    return compound; // Return the compound list of final param_decl nodes
} // End paramList()

AST *compoundStatement(Parser *parser) {
    eat(parser, TOKEN_BEGIN);
    AST *node = newASTNode(AST_COMPOUND, NULL);

    while (1) { // Loop until explicitly broken or END is consumed
        // Skip any optional leading/multiple semicolons
        while (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
        }

        // Check for the end of the block *before* trying to parse a statement
        if (parser->current_token->type == TOKEN_END) {
            break; // Found the end of this compound statement
        }
        // Also check for program end '.' which might terminate a block early
        if (parser->current_token->type == TOKEN_PERIOD) {
             // Let the caller handle the period
             break;
        }

        // Parse one statement within the block
        AST *stmt = statement(parser);
        if (!stmt) {
             // Handle parsing errors from statement() if necessary
        } else {
             addChild(node, stmt);
        }

        // Now, determine if we expect a semicolon separator or the end of the block
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            // Found a semicolon, consume it and expect more statements or END
            eat(parser, TOKEN_SEMICOLON);
             // Handle case like BEGIN statement; END -> check again for END after eating semicolon
             if (parser->current_token->type == TOKEN_END) {
                 break;
             }
             if (parser->current_token->type == TOKEN_PERIOD) {
                  break; // Let caller handle program end
             }
             // Otherwise, loop continues to find next statement after skipping more semicolons

        } else if (parser->current_token->type == TOKEN_END) {
            // Found END immediately after a statement (optional semicolon skipped)
            break; // Exit the loop, END will be consumed below
        } else if (parser->current_token->type == TOKEN_PERIOD) {
             // Found program end immediately after a statement
             break; // Let caller handle program end
        }
         else {
            // <<< START REMOVED BLOCK >>>
            // The previous 'if (parser->current_token->type == TOKEN_ELSE)' block
            // that caused the premature break is removed from here.
            // <<< END REMOVED BLOCK >>>

            // If it's not SEMICOLON, END, or PERIOD, then it's genuinely an unexpected token.
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg),
                     "Expected semicolon or END after statement in compound block (found token: %s)",
                     tokenTypeToString(parser->current_token->type)); // Report the actual found token
            errorParser(parser, error_msg);
            break; // Exit loop on error
        }
    } // End while(1)

    // After the loop, consume the END token (unless it was program end '.')
    if (parser->current_token->type != TOKEN_PERIOD) {
         eat(parser, TOKEN_END);
    }

    #ifdef DEBUG // Keep debug dump if helpful
    if (dumpExec) debugAST(node, 0);
    #endif
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
                    // Could be assignment or procedure call.
                    AST *lval_or_proc_id = lvalue(parser); // Parses identifier and potential .field or [index]

                    // Add check if lvalue parsing failed
                    if (!lval_or_proc_id) {
                         // lvalue should call errorParser internally if it fails syntactically
                         // If it returns NULL for other reasons, that's an issue.
                         fprintf(stderr, "Error: lvalue() returned NULL unexpectedly after identifier.\n");
                         node = newASTNode(AST_NOOP, NULL); // Error recovery node
                         break; // Exit case
                    }

                    // Check what token comes *after* the parsed lvalue
                    if (parser->current_token->type == TOKEN_ASSIGN) {
                        // --- Assignment Statement ---
                        node = assignmentStatement(parser, lval_or_proc_id); // assignmentStatement handles := and RHS
                        // Semicolon separator handled by compoundStatement loop

                    } else if (parser->current_token->type == TOKEN_LPAREN && lval_or_proc_id->type == AST_VARIABLE) {
                        // --- Procedure/Function Call WITH Arguments (...) ---
                         // Convert the AST_VARIABLE node returned by lvalue() to AST_PROCEDURE_CALL.
                        lval_or_proc_id->type = AST_PROCEDURE_CALL;

                        eat(parser, TOKEN_LPAREN); // Consume '('

                        // Check if there are arguments inside the parentheses
                        if (parser->current_token->type != TOKEN_RPAREN) {
                            AST* args = exprList(parser); // Parse arguments; exprList returns an AST_COMPOUND node

                            // --- Start: CORRECTED/COMPLETE Argument Transfer Logic ---
                            if (args && args->type == AST_COMPOUND && args->child_count > 0) {
                                // Arguments were parsed successfully into the 'args' compound node.
                                #ifdef DEBUG
                                fprintf(stderr, "[DEBUG PARSER STMT] Transferring %d children from args %p (%p) to proc_call %p\n",
                                        args->child_count, (void*)args, (void*)args->children, (void*)lval_or_proc_id);
                                #endif

                                // Transfer the array of children AST nodes and the count.
                                lval_or_proc_id->children = args->children; // PROC_CALL node now points to the children array
                                lval_or_proc_id->child_count = args->child_count; // Update count

                                // Nullify the pointers in the temporary 'args' node
                                // so that freeAST(args) doesn't free the children array we just transferred.
                                args->children = NULL;
                                args->child_count = 0;

                                // Set the parent pointer for each transferred argument node
                                // so they correctly point back to the PROCEDURE_CALL node.
                                for(int i=0; i < lval_or_proc_id->child_count; i++){
                                    if(lval_or_proc_id->children[i]) {
                                        lval_or_proc_id->children[i]->parent = lval_or_proc_id;
                                    }
                                }
                                #ifdef DEBUG
                                fprintf(stderr, "[DEBUG PARSER STMT] After transfer: proc_call %p has child_count=%d, children_ptr=%p\n",
                                        (void*)lval_or_proc_id, lval_or_proc_id->child_count, (void*)lval_or_proc_id->children);
                                #endif

                            } else if (args) {
                                // This case handles if exprList returned non-compound (shouldn't happen) or empty compound.
                                lval_or_proc_id->children = NULL;
                                lval_or_proc_id->child_count = 0;
                                #ifdef DEBUG
                                fprintf(stderr, "[DEBUG PARSER STMT] Args node existed but was empty or not compound\n");
                                #endif
                                // We still need to free the 'args' node below.
                            } else { // args was NULL (error during exprList parsing)
                                lval_or_proc_id->children = NULL;
                                lval_or_proc_id->child_count = 0;
                                #ifdef DEBUG
                                fprintf(stderr, "[DEBUG PARSER STMT] exprList returned NULL, setting proc_call children to NULL\n");
                                #endif
                                // If exprList failed, it should have called errorParser already.
                            }

                            // Free the temporary COMPOUND wrapper node returned by exprList (if it existed).
                            // Use freeAST, not free.
                            if (args) {
                                #ifdef DEBUG
                                 fprintf(stderr, "[DEBUG PARSER STMT] Freeing args wrapper node %p\n", (void*)args);
                                #endif
                                 freeAST(args); // <<< USE freeAST here
                            }
                            // --- End: CORRECTED/COMPLETE Argument Transfer Logic ---

                        } else { // Empty argument list '()'
                            lval_or_proc_id->children = NULL;
                            lval_or_proc_id->child_count = 0;
                             #ifdef DEBUG
                             fprintf(stderr, "[DEBUG PARSER STMT] Empty argument list '()': proc_call %p has child_count=0\n", (void*)lval_or_proc_id);
                             #endif
                        }

                        eat(parser, TOKEN_RPAREN); // Consume ')'
                        node = lval_or_proc_id; // Use the modified node as the statement result
                        // Semicolon separator handled by compoundStatement loop

                    } else if (lval_or_proc_id->type == AST_VARIABLE) {
                         // --- Parameter-less Procedure Call ---
                         // (e.g., ClrScr;) Correctly identified if not followed by LPAREN or ASSIGN.
                        Token* procNameToken = copyToken(lval_or_proc_id->token);
                        if (!procNameToken) { /* Malloc error */ freeAST(lval_or_proc_id); EXIT_FAILURE_HANDLER(); }

                        AST *procCallNode = newASTNode(AST_PROCEDURE_CALL, procNameToken);
                        if (!procCallNode) { /* Malloc error */ freeToken(procNameToken); freeAST(lval_or_proc_id); EXIT_FAILURE_HANDLER(); }

                        procCallNode->children = NULL; // No arguments
                        procCallNode->child_count = 0;

                        freeToken(procNameToken); // Free the copy passed to newASTNode
                        freeAST(lval_or_proc_id); // Free the temporary AST_VARIABLE node created by lvalue()
                        node = procCallNode; // Use the new PROC_CALL node as the statement result
                        // Semicolon separator handled by compoundStatement loop

                    } else {
                        // --- Error: Invalid Statement ---
                        // If lval_or_proc_id is FIELD_ACCESS or ARRAY_ACCESS, it cannot stand alone.
                        char error_msg[150];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Expression starting with '%s' cannot be used as a statement here (followed by '%s')",
                                 lval_or_proc_id->token ? lval_or_proc_id->token->value : "<complex_lvalue>",
                                 tokenTypeToString(parser->current_token->type));
                        errorParser(parser, error_msg);
                        freeAST(lval_or_proc_id); // Free the invalid lvalue node
                        node = newASTNode(AST_NOOP, NULL); // Return NOOP for error recovery
                    }

                     // --- Debug print just before leaving case ---
                     #ifdef DEBUG
                     if(node && (node->type == AST_PROCEDURE_CALL || node->type == AST_ASSIGN)) {
                          fprintf(stderr, "[DEBUG PARSER STMT] Leaving TOKEN_IDENTIFIER case, node %p: type=%s, child_count=%d, children_ptr=%p\n",
                                 (void*)node, astTypeToString(node->type), node->child_count, (void*)node->children);
                     }
                     #endif
                     // ---

                    break; // End case TOKEN_IDENTIFIER
                } // End brace for case TOKEN_IDENTIFIER
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
AST *assignmentStatement(Parser *parser, AST *parsedLValue) {
    // Line causing the error is removed.

    // Current token should be ASSIGN (this check might need adjustment
    // depending on where the caller consumes the lvalue)
    eat(parser, TOKEN_ASSIGN);
    AST *right = boolExpr(parser); // Parse the right-hand side expression
    AST *node = newASTNode(AST_ASSIGN, NULL);

    // Use the parameter directly
    setLeft(node, parsedLValue);
    setRight(node, right);

    #ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}

AST *procedureCall(Parser *parser) {
    AST *node = newASTNode(AST_PROCEDURE_CALL, parser->current_token); // node is the new parent
    eat(parser, TOKEN_IDENTIFIER);

    // Handle argument list if present
    if (parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        AST *args = exprList(parser); // exprList returns a temporary AST_COMPOUND
        eat(parser, TOKEN_RPAREN);

        // Check if exprList actually returned arguments
        if (args && args->child_count > 0) {
            // Transfer children array and count
            node->children = args->children;
            node->child_count = args->child_count;

            // Nullify in temporary node to prevent double free issues
            args->children = NULL; // <<< ADD THIS
            args->child_count = 0;  // <<< ADD THIS

            // Update parent pointers of transferred children
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) {
                    node->children[i]->parent = node; // <<< ADD THIS LOOP
                }
            }
        } else {
            // No arguments returned, ensure node reflects this
            node->children = NULL;
            node->child_count = 0;
        }

        // Free the temporary args compound node structure itself
        if (args) {
             free(args); // <<< ADD THIS FREE CALL
        }

    } else {
        // No parentheses, so no arguments
        node->children = NULL;
        node->child_count = 0;
    }

    #ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}

AST *ifStatement(Parser *parser) {
    eat(parser, TOKEN_IF);
    AST *condition = boolExpr(parser);
    eat(parser, TOKEN_THEN);

    DEBUG_PRINT("[DEBUG] ifStatement: Parsing THEN branch...\n"); // Existing or add
    AST *then_branch = statement(parser); // Parses the statement after THEN
    DEBUG_PRINT("[DEBUG] ifStatement: FINISHED parsing THEN branch.\n"); // Existing or add

    AST *node = newASTNode(AST_IF, NULL);
    setLeft(node, condition);
    setRight(node, then_branch);

    // <<< --- ADD THIS DEBUG BLOCK --- >>>
    #ifdef DEBUG
    if (dumpExec) { // Ensure debug flag is checked
        fprintf(stderr, "[DEBUG] ifStatement: After THEN branch, current token is: %s ('%s') at Line %d, Col %d\n",
                 tokenTypeToString(parser->current_token->type),
                 parser->current_token->value ? parser->current_token->value : "NULL",
                 parser->lexer->line, parser->lexer->column);
    }
    #endif
    // <<< --- END ADDED DEBUG BLOCK --- >>>

    // Check for ELSE part
    if (parser->current_token->type == TOKEN_ELSE) {
         #ifdef DEBUG
         if (dumpExec) fprintf(stderr, "[DEBUG] ifStatement: Found ELSE token. Parsing ELSE branch...\n"); // Add/ensure this
         #endif
        eat(parser, TOKEN_ELSE);           // Consumes ELSE
        AST *else_branch = statement(parser); // Parses the statement after ELSE
         #ifdef DEBUG
         if (dumpExec) fprintf(stderr, "[DEBUG] ifStatement: FINISHED parsing ELSE branch.\n"); // Add/ensure this
         #endif
        setExtra(node, else_branch);
    } else {
         #ifdef DEBUG
         if (dumpExec) fprintf(stderr, "[DEBUG] ifStatement: NO ELSE token found after THEN branch.\n"); // Add/ensure this
         #endif
    }

    #ifdef DEBUG
    if (dumpExec) fprintf(stderr, "[DEBUG] ifStatement: Returning IF node.\n"); // Add/ensure this
    #endif

    #ifdef DEBUG // Keep existing dump if helpful
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}

// ... rest of parser.c ...

AST *whileStatement(Parser *parser) {
    eat(parser, TOKEN_WHILE);
    AST *condition = boolExpr(parser);
    eat(parser, TOKEN_DO);
    AST *body = statement(parser);
    AST *node = newASTNode(AST_WHILE, NULL);
    setLeft(node, condition);
    setRight(node, body);
    DEBUG_DUMP_AST(node, 0);
    return node;
}

AST *parseCaseLabels(Parser *parser) {
    AST *labels = newASTNode(AST_COMPOUND, NULL);
    while (1) {
        AST *label = NULL;
        AST *start = expr(parser);

        if (parser->current_token->type == TOKEN_DOTDOT) {
            eat(parser, TOKEN_DOTDOT);
            AST *end = expr(parser);
            label = newASTNode(AST_SUBRANGE, NULL);
            setLeft(label, start);
            setRight(label, end);
        } else {
            label = start;
        }

        addChild(labels, label);

        if (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else {
            break;
        }
    }

    if (labels->child_count == 1) {
        AST *single = labels->children[0];
        single->parent = NULL;
        free(labels->children);
        free(labels);
        return single;
    }

    return labels;
}


AST *caseStatement(Parser *parser) {
    eat(parser, TOKEN_CASE);
    AST *caseExpr = expr(parser);
    AST *node = newASTNode(AST_CASE, NULL);
    setLeft(node, caseExpr);
    eat(parser, TOKEN_OF);
    while ((parser->current_token->type != TOKEN_ELSE) &&
           (parser->current_token->type != TOKEN_END)) {
        AST *branch = newASTNode(AST_CASE_BRANCH, NULL);
        setLeft(branch, parseCaseLabels(parser));
        eat(parser, TOKEN_COLON);
        setRight(branch, statement(parser));
        addChild(node, branch);
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
        } else {
            break;
        }
    }
    if (parser->current_token->type == TOKEN_ELSE) {
        eat(parser, TOKEN_ELSE);
        setExtra(node, statement(parser));
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
        }
    }
    eat(parser, TOKEN_END);
    return node;
}

// In src/parser.c

AST *repeatStatement(Parser *parser) {
    eat(parser, TOKEN_REPEAT); // Consume REPEAT
    AST *body = newASTNode(AST_COMPOUND, NULL);

    // Loop until the UNTIL token is encountered and consumed
    while (1) {
        // Check for UNTIL *before* parsing a statement
        if (parser->current_token->type == TOKEN_UNTIL) {
            break; // Exit the loop, UNTIL will be consumed below
        }

        // Skip any optional empty statements (just semicolons)
        // This prevents calling statement() when it's just a separator
        while (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON);
            // Check for UNTIL immediately after a semicolon
            if (parser->current_token->type == TOKEN_UNTIL) {
                 goto end_loop; // Use goto to break outer loop cleanly
            }
        }
        // Check again for UNTIL after potentially skipping semicolons
        if (parser->current_token->type == TOKEN_UNTIL) {
            break;
        }

        // Parse one statement
        AST *stmt = statement(parser);
        if (stmt && stmt->type != AST_NOOP) {
             addChild(body, stmt);
        } else if (!stmt) {
            // statement() should ideally signal errors via errorParser
            // If it returns NULL without erroring, something is wrong.
            errorParser(parser, "Failed to parse statement within REPEAT loop");
            break; // Exit loop on error
        }
        // Note: We intentionally DO NOT error on AST_NOOP here,
        // as statement() might return that for a valid empty statement ';',
        // which the semicolon skipping loop at the top should handle on the next iteration.

        // After parsing a statement, check if the next token is UNTIL.
        // If not UNTIL, it *should* ideally be a semicolon if more statements follow,
        // but standard Pascal allows omitting the semicolon before UNTIL.
        // So, we only consume a semicolon if it's present.
        if (parser->current_token->type == TOKEN_SEMICOLON) {
             eat(parser, TOKEN_SEMICOLON);
        }
        // If the token after the statement is neither UNTIL nor SEMICOLON,
        // it implies a syntax error *within* the statement list or structure.
        // For example, `if ... end else ... end UNTIL` is valid, the token after
        // the first `end` is `else`. `statement()` should handle parsing the full `if/else`.
        // If `statement()` completed successfully, but the next token isn't UNTIL or ;,
        // it indicates a potential issue. However, directly erroring here might be too strict.
        // Let the main loop condition `while(1)` and the check at the start handle finding UNTIL.

    } // End while(1)

end_loop: // Label for goto

    // Current token must be UNTIL now
    eat(parser, TOKEN_UNTIL);

    // Parse the condition expression AFTER consuming UNTIL
    AST *condition = boolExpr(parser);

    // Create the REPEAT node
    AST *node = newASTNode(AST_REPEAT, NULL);
    setLeft(node, body);
    setRight(node, condition);
    DEBUG_DUMP_AST(node, 0);
    return node;
}

AST *forStatement(Parser *parser) {
    eat(parser, TOKEN_FOR);
    // Parse the loop variable identifier FIRST
    Token *loopVarToken = parser->current_token;
    if (loopVarToken->type != TOKEN_IDENTIFIER) {
         errorParser(parser, "Expected identifier for loop variable");
         return newASTNode(AST_NOOP, NULL); // Error recovery
    }
    AST *loopVarNode = newASTNode(AST_VARIABLE, loopVarToken);
    eat(parser, TOKEN_IDENTIFIER); // Consume loop variable identifier

    // Now parse the rest: := start [to|downto] end do body
    eat(parser, TOKEN_ASSIGN);
    AST *start_expr = expr(parser);
    TokenType direction = parser->current_token->type;
    if (direction == TOKEN_TO)
        eat(parser, TOKEN_TO);
    else if (direction == TOKEN_DOWNTO)
        eat(parser, TOKEN_DOWNTO);
    else
        errorParser(parser, "Expected TO or DOWNTO in for statement");
    AST *end_expr = expr(parser);
    eat(parser, TOKEN_DO);
    AST *body = statement(parser);

    // Create the FOR node (use NULL for token, it's structural)
    ASTNodeType for_type = (direction == TOKEN_TO) ? AST_FOR_TO : AST_FOR_DOWNTO;
    // ---> THE KEY FIX IS HERE <---
    AST *node = newASTNode(for_type, NULL); // <<< Ensure NULL token for FOR node
    // ----------------------------

    // Assign components to the correct places
    setLeft(node, start_expr);   // Start expression
    setRight(node, end_expr);    // End expression
    setExtra(node, body);        // Loop body
    addChild(node, loopVarNode); // <<< Store loop variable node in children[0]

    #ifdef DEBUG // Keep debug dump if helpful
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}

AST *writelnStatement(Parser *parser) {
    if (parser->current_token->type == TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value, "writeln") == 0) {
        eat(parser, TOKEN_IDENTIFIER);
    } else {
        eat(parser, TOKEN_WRITELN);
    }
    AST *args = parseWriteArguments(parser);
    AST *node = newASTNode(AST_WRITELN, NULL);
    if (args) {
        node->children = args->children;
        node->child_count = args->child_count;
        args->children = NULL;
        args->child_count = 0;
        // Update parent pointers of transferred children to point to the new parent (the writeln node)
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) { // Safety check for NULL child
                node->children[i]->parent = node; // <<< UPDATE PARENT POINTER
            }
        }
        free(args);
    } else {
        node->children = NULL;
        node->child_count = 0;
    }
    #ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}

AST *writeStatement(Parser *parser) {
    // Expects current token to be TOKEN_WRITE (or IDENTIFIER 'write' if handled in statement)
    if (parser->current_token->type == TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value, "write") == 0) {
        eat(parser, TOKEN_IDENTIFIER); // Consume identifier 'write'
    } else {
        eat(parser, TOKEN_WRITE); // Consume keyword WRITE
    }

    AST *args = parseWriteArguments(parser); // Parse arguments (handles parentheses)
    AST *node = newASTNode(AST_WRITE, NULL);

    // Assign children from args node safely
    if (args) {
        node->children = args->children;
        node->child_count = args->child_count;
        args->children = NULL; // Prevent double free by args node
        args->child_count = 0;
        // Update parent pointers of transferred children to point to the new parent (the writeln node)
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) { // Safety check for NULL child
                node->children[i]->parent = node; // <<< UPDATE PARENT POINTER
            }
        }
        free(args); // Free the temporary args compound node structure
    } else {
        node->children = NULL;
        node->child_count = 0;
    }

    #ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
    #endif
    return node; // Return the fully formed WRITE node
}

AST *readStatement(Parser *parser) {
     if (parser->current_token->type == TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value, "read") == 0) {
        eat(parser, TOKEN_IDENTIFIER);
    } else {
        eat(parser, TOKEN_READ);
    }
    AST *node = newASTNode(AST_READ, NULL);
    AST *args = NULL; // Assume no args initially

    // Standard Pascal requires parentheses for read arguments
    if (parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        // exprList parses comma-separated expressions (should be variables for read)
        // Need semantic check later to ensure args are assignable lvalues.
        args = exprList(parser);
        eat(parser, TOKEN_RPAREN);
    } else {
         // No parentheses = no arguments (standard behavior)
         args = newASTNode(AST_COMPOUND, NULL);
         args->child_count = 0;
    }

     // Assign children safely
     if (args) {
        node->children = args->children;
        node->child_count = args->child_count;
        args->children = NULL;
        args->child_count = 0;
        // Update parent pointers of transferred children to point to the new parent (the writeln node)
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) { // Safety check for NULL child
                node->children[i]->parent = node; // <<< UPDATE PARENT POINTER
            }
        }
         
        free(args);
    } else {
        node->children = NULL;
        node->child_count = 0;
    }

    #ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}

AST *readlnStatement(Parser *parser) {
     if (parser->current_token->type == TOKEN_IDENTIFIER && strcasecmp(parser->current_token->value, "readln") == 0) {
        eat(parser, TOKEN_IDENTIFIER);
    } else {
        eat(parser, TOKEN_READLN);
    }
    AST *node = newASTNode(AST_READLN, NULL);
     AST *args = NULL;
    // Parentheses are optional for readln; if present, they contain arguments.
    if (parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        // Readln can have zero arguments inside parentheses
        if (parser->current_token->type != TOKEN_RPAREN) {
             args = exprList(parser); // Parse arguments if present
        } else {
             args = newASTNode(AST_COMPOUND, NULL); // Empty arg list inside ()
             args->child_count = 0;
        }
        eat(parser, TOKEN_RPAREN);
    } else {
         // No parentheses = no arguments
         args = newASTNode(AST_COMPOUND, NULL);
         args->child_count = 0;
    }

     // Assign children safely
     if (args) {
        node->children = args->children;
        node->child_count = args->child_count;
        args->children = NULL;
        args->child_count = 0;
        // Update parent pointers of transferred children to point to the new parent (the writeln node)
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) { // Safety check for NULL child
                node->children[i]->parent = node; // <<< UPDATE PARENT POINTER
            }
        }
        free(args);
    } else {
        node->children = NULL;
        node->child_count = 0;
    }

    #ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}


AST *exprList(Parser *parser) {
    AST *node = newASTNode(AST_COMPOUND, NULL);
    AST *arg = expr(parser);
    addChild(node, arg);
    while (parser->current_token->type == TOKEN_COMMA) {
        eat(parser, TOKEN_COMMA);
        arg = expr(parser);
        addChild(node, arg);
    }
    DEBUG_DUMP_AST(node, 0);
    return node;
}

AST *expr(Parser *parser) {
    AST *node = term(parser); // Parse left operand (term)
    while (parser->current_token->type == TOKEN_PLUS ||
           parser->current_token->type == TOKEN_MINUS) {
        // --- Copy the operator token BEFORE eat ---
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal);
        if(!opCopied) {
             fprintf(stderr, "Memory allocation failed in expr (copyToken op)\n");
             EXIT_FAILURE_HANDLER();
        }
        // ---

        // Eat the ORIGINAL operator token (eatInternal frees it)
        eat(parser, opOriginal->type);

        // Parse right operand (term)
        AST *right = term(parser);
        if (!right) { // Handle error from term parsing
             freeToken(opCopied); // Clean up copied token
             errorParser(parser, "Failed to parse right operand for +/-");
             // Free left operand 'node'? Risky. Return NULL or NOOP.
             return newASTNode(AST_NOOP, NULL);
        }


        // --- Create the new binary operation node using the COPIED token ---
        // newASTNode makes its own internal copy of the token passed to it
        AST *new_node = newASTNode(AST_BINARY_OP, opCopied);
        // ---

        setLeft(new_node, node);
        setRight(new_node, right);
        // Assuming inferBinaryOpType handles type inference correctly
        setTypeAST(new_node, inferBinaryOpType(node->var_type, right->var_type));

        node = new_node; // Update 'node' for the next loop iteration or return

        // --- Free the COPIED operator token ---
        freeToken(opCopied); // <<< Free the copy made for this loop iteration
        // ---

    } // End while loop
    return node; // Return the final expression tree
}

// --- Modify parseSetConstructor function ---
AST *parseSetConstructor(Parser *parser) {
    eat(parser, TOKEN_LBRACKET);
    AST *setNode = newASTNode(AST_SET, NULL);
    setTypeAST(setNode, TYPE_SET);

    if (parser->current_token->type != TOKEN_RBRACKET) {
        while (1) {
            AST *element = expr(parser); // Parse element or range start

            // --- Modification Start: Relax parser validation slightly ---
            // Basic check: Allow numbers or string literals (runtime will check length)
            bool element_syntax_ok = (element->type == AST_NUMBER || element->type == AST_STRING);


            if (parser->current_token->type == TOKEN_DOTDOT) { // Range
                eat(parser, TOKEN_DOTDOT);
                AST *rangeEnd = expr(parser);
                bool end_syntax_ok = (rangeEnd->type == AST_NUMBER || rangeEnd->type == AST_STRING);

                if (!element_syntax_ok || !end_syntax_ok) {
                     // Simplified parser error - runtime handles detailed type/length check
                     errorParser(parser, "Set range elements must be constants of ordinal types (e.g., integer or char literal)");
                 }

                AST *rangeNode = newASTNode(AST_SUBRANGE, NULL);
                setLeft(rangeNode, element);
                setRight(rangeNode, rangeEnd);
                addChild(setNode, rangeNode);
            } else { // Single element
                if (!element_syntax_ok) {
                     // Simplified parser error - runtime handles detailed type/length check
                     errorParser(parser, "Set elements must be constants of an ordinal type (e.g., integer or char literal)");
                 }
                 addChild(setNode, element);
            }
            // --- Modification End ---

            if (parser->current_token->type == TOKEN_COMMA) {
                eat(parser, TOKEN_COMMA);
            } else {
                break;
            }
        }
    }
    eat(parser, TOKEN_RBRACKET);
    return setNode;
}

AST *relExpr(Parser *parser) {
    AST *node = expr(parser); // Parses left operand

    // Loop for relational operators
    while (parser->current_token->type == TOKEN_GREATER ||
           parser->current_token->type == TOKEN_GREATER_EQUAL ||
           parser->current_token->type == TOKEN_EQUAL ||
           parser->current_token->type == TOKEN_LESS ||
           parser->current_token->type == TOKEN_LESS_EQUAL ||
           parser->current_token->type == TOKEN_NOT_EQUAL ||
           parser->current_token->type == TOKEN_IN)
           {
        // --- Copy the operator token BEFORE calling eat ---
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal);
        if(!opCopied) {
             fprintf(stderr, "Memory allocation failed in relExpr (copyToken op)\n");
             EXIT_FAILURE_HANDLER();
        }
        // ---

        // Eat the ORIGINAL operator token (eatInternal frees it)
        eat(parser, opOriginal->type);

        // Parse the right operand
        AST *right;
        if (opCopied->type == TOKEN_IN) { // <<< Use copied token for type check
             right = parseSetConstructor(parser); // Parse the set part
        } else {
             right = expr(parser); // Parse regular expression for other ops
        }

        // --- Create the new binary operation node using the COPIED token ---
        // newASTNode makes its own internal copy of the token passed to it
        AST *new_node = newASTNode(AST_BINARY_OP, opCopied);
        // ---

        setLeft(new_node, node);
        setRight(new_node, right);
        setTypeAST(new_node, TYPE_BOOLEAN); // Relational ops result in Boolean

        node = new_node; // Update 'node' for the next loop iteration or return

        // --- Free the COPIED operator token ---
        freeToken(opCopied); // <<< Free the copy made for this loop iteration
        // ---

    } // End while loop

    DEBUG_DUMP_AST(node, 0);
    return node; // Return the final expression tree
}

AST *boolExpr(Parser *parser) {
    AST *node = relExpr(parser); // Parses relational expression
    while (parser->current_token->type == TOKEN_AND || parser->current_token->type == TOKEN_OR) {
        // --- Copy the operator token BEFORE eat ---
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal);
        if(!opCopied) {
             fprintf(stderr, "Memory allocation failed in boolExpr (copyToken op)\n");
             EXIT_FAILURE_HANDLER();
        }
        // ---

        // Eat the ORIGINAL operator token (eatInternal frees it)
        eat(parser, opOriginal->type);

        // Parse right operand (relational expression)
        AST *right = relExpr(parser);
         if (!right) { // Handle error from relExpr parsing
             freeToken(opCopied); // Clean up copied token
             errorParser(parser, "Failed to parse right operand for AND/OR");
             // Free left operand 'node'? Risky. Return NULL or NOOP.
             return newASTNode(AST_NOOP, NULL);
         }

        // --- Create the new binary operation node using the COPIED token ---
        // newASTNode makes its own internal copy of the token passed to it
        AST *new_node = newASTNode(AST_BINARY_OP, opCopied);
        // ---

        setLeft(new_node, node);
        setRight(new_node, right);
        setTypeAST(new_node, TYPE_BOOLEAN); // AND/OR result is boolean

        node = new_node; // Update 'node' for the next loop iteration or return

        // --- Free the COPIED operator token ---
        freeToken(opCopied); // <<< Free the copy made for this loop iteration
        // ---

    } // End while loop
    return node; // Return the final expression tree
}

AST *term(Parser *parser) {
    AST *node = factor(parser); // Parse left operand (factor)

    while (parser->current_token->type == TOKEN_MUL ||
           parser->current_token->type == TOKEN_SLASH ||
           parser->current_token->type == TOKEN_INT_DIV ||
           parser->current_token->type == TOKEN_MOD) {

        // --- Copy the operator token BEFORE eat ---
        Token *opOriginal = parser->current_token;
        Token *opCopied = copyToken(opOriginal);
        if(!opCopied) {
             fprintf(stderr, "Memory allocation failed in term (copyToken op)\n");
             EXIT_FAILURE_HANDLER();
        }
        // ---

        // Eat the ORIGINAL operator token (eatInternal frees it)
        eat(parser, opOriginal->type);

        // Parse right operand (factor)
        AST *right = factor(parser);
         if (!right) { // Handle error from factor parsing
             freeToken(opCopied); // Clean up copied token
             errorParser(parser, "Failed to parse right operand for */DIV/MOD");
             // Free left operand 'node'? Risky. Return NULL or NOOP.
             return newASTNode(AST_NOOP, NULL);
         }

        // --- Create the new binary operation node using the COPIED token ---
        // newASTNode makes its own internal copy of the token passed to it
        AST *new_node = newASTNode(AST_BINARY_OP, opCopied);
        // ---

        setLeft(new_node, node);
        setRight(new_node, right);
        // Assuming inferBinaryOpType handles type inference correctly
        setTypeAST(new_node, inferBinaryOpType(node->var_type, right->var_type));

        node = new_node; // Update 'node' for the next loop iteration or return

        // --- Free the COPIED operator token ---
        freeToken(opCopied); // <<< Free the copy made for this loop iteration
        // ---

    } // End while loop
    return node; // Return the final expression tree
}

AST *factor(Parser *parser) {
    Token *token = parser->current_token; // Original token pointer
    AST *node = NULL; // Initialize resulting AST node

#ifdef DEBUG
    // ADDED: Debug print at the ENTRY of factor
    if (dumpExec) { // Check dumpExec flag
        fprintf(stderr, "[DEBUG_FACTOR] Entry: Current token is %s ('%s')\n",
                tokenTypeToString(token->type),
                token->value ? token->value : "NULL");
    }
#endif

    if (token->type == TOKEN_TRUE || token->type == TOKEN_FALSE) {
        // --- Apply copy-before-eat for boolean literals ---
        Token* copiedToken = copyToken(token);
        if(!copiedToken) {
             fprintf(stderr, "Memory allocation failed in factor (copyToken boolean)\n");
             EXIT_FAILURE_HANDLER();
        }
        eat(parser, token->type); // Eat original (eatInternal frees it)
        node = newASTNode(AST_BOOLEAN, copiedToken); // Use copy for AST node
        freeToken(copiedToken); // Free the copy made here
        // ---
    } else if (token->type == TOKEN_PLUS ||
               token->type == TOKEN_MINUS ||
               token->type == TOKEN_NOT) {
        // --- Apply copy-before-eat for unary operator ---
        Token *opToken = token; // Original operator token
        Token *copiedOpToken = copyToken(opToken);
        if(!copiedOpToken) {
             fprintf(stderr, "Memory allocation failed in factor (copyToken unary op)\n");
             EXIT_FAILURE_HANDLER();
        }
        eat(parser, opToken->type); // Eat original operator (eatInternal frees it)
        node = newASTNode(AST_UNARY_OP, copiedOpToken); // Use copy for the operator node
        freeToken(copiedOpToken); // Free the copy of the operator token
        setLeft(node, factor(parser)); // Recursively parse the operand
        // Type annotation for unary op would happen later or be inferred
        // ---
    } else if (token->type == TOKEN_INTEGER_CONST ||
               token->type == TOKEN_HEX_CONST ||
               token->type == TOKEN_REAL_CONST) {
        // --- Apply copy-before-eat for number literals ---
        Token* copiedToken = copyToken(token);
        if(!copiedToken) {
             fprintf(stderr, "Memory allocation failed in factor (copyToken number)\n");
             EXIT_FAILURE_HANDLER();
        }
        eat(parser, token->type); // Eat original (eatInternal frees it)
        node = newASTNode(AST_NUMBER, copiedToken); // Use copy for AST node
        freeToken(copiedToken); // Free the copy made here
        // ---
    } else if (token->type == TOKEN_STRING_CONST) {
        // --- Apply copy-before-eat for string literals ---
        Token* copiedToken = copyToken(token);
        if(!copiedToken) {
             fprintf(stderr, "Memory allocation failed in factor (copyToken string)\n");
             EXIT_FAILURE_HANDLER();
        }
        eat(parser, token->type); // Eat original (eatInternal frees it)
        node = newASTNode(AST_STRING, copiedToken); // Use copy for AST node
        freeToken(copiedToken); // Free the copy made here
        // ---
    } else if (token->type == TOKEN_IDENTIFIER) {
        // --- Logic for identifiers (variable, function call, field/array base) ---
        Token *identifierToken = token; // Keep pointer to original identifier

        // Always treat "result" as a variable - needs copy-before-eat
        if (strcasecmp(identifierToken->value, "result") == 0) {
             Token* copiedToken = copyToken(identifierToken);
             if(!copiedToken){
                  fprintf(stderr, "Memory allocation failed in factor (copyToken result)\n");
                  EXIT_FAILURE_HANDLER();
             }
             eat(parser, TOKEN_IDENTIFIER); // Eat original 'result'
             node = newASTNode(AST_VARIABLE, copiedToken); // Use copy
             freeToken(copiedToken); // Free copy
             // Note: Standard Pascal wouldn't allow field/array access on 'Result'
             // but you might add checks here if your dialect allows it.
             return node; // Return directly for result
        }

        // Check if next token is '(', requires peekToken
        Token *peek = peekToken(parser);
        if (!peek) {
             errorParser(parser, "Failed to peek token in factor");
             return newASTNode(AST_NOOP, NULL);
        }
        bool next_is_lparen = (peek->type == TOKEN_LPAREN);
        freeToken(peek); // Free the token returned by peekToken immediately after check

        if (next_is_lparen) {
            // It's a function/procedure call with parentheses.
            // procedureCall should handle its own token management internally now.
            // (We assume procedureCall copies the name token before eating it).
            node = procedureCall(parser);
        } else {
             // Not followed by '('. Could be var, param-less func, field/array base.
             Procedure *proc = lookupProcedure(identifierToken->value);
             if (proc != NULL && proc->proc_decl && proc->proc_decl->type == AST_FUNCTION_DECL) {
                  // Parameterless function call
                  Token* copiedToken = copyToken(identifierToken); // Copy before eat
                  if(!copiedToken){
                       fprintf(stderr, "Memory allocation failed in factor (copyToken paramless func)\n");
                       EXIT_FAILURE_HANDLER();
                  }
                  eat(parser, TOKEN_IDENTIFIER); // Eat original function name
                  node = newASTNode(AST_PROCEDURE_CALL, copiedToken); // Use copy
                  freeToken(copiedToken); // Free copy
                  // Set up node for parameterless call
                  node->children = NULL;
                  node->child_count = 0;
                  node->var_type = proc->proc_decl->var_type; // Set function's return type
             } else if (proc != NULL) {
                 // It's a procedure name used where a value/factor is expected. Error.
                 char error_msg[128];
                 snprintf(error_msg, sizeof(error_msg), "Procedure '%s' found where a value (factor) is expected", identifierToken->value);
                 errorParser(parser, error_msg);
                 return newASTNode(AST_NOOP, NULL); // Return dummy node
             }
             else {
                   // Variable, or base of field/array access
                   Token* copiedIdentifierToken = copyToken(identifierToken); // Copy base identifier
                   if(!copiedIdentifierToken){
                        fprintf(stderr, "Memory allocation failed in factor (copyToken variable base)\n");
                        EXIT_FAILURE_HANDLER();
                   }
                   eat(parser, TOKEN_IDENTIFIER); // Eat original base identifier

                   // Start with AST_VARIABLE using the copied token
                   node = newASTNode(AST_VARIABLE, copiedIdentifierToken);
                   // We are done with the copied base token's use in this node, free it.
                   freeToken(copiedIdentifierToken);

                   // Now handle subsequent field/array access
                   while (parser->current_token->type == TOKEN_PERIOD ||
                          parser->current_token->type == TOKEN_LBRACKET) {
                        if (parser->current_token->type == TOKEN_PERIOD) {
                             eat(parser, TOKEN_PERIOD); // Frees '.'
                             Token *fieldTokenOriginal = parser->current_token;
                             if (fieldTokenOriginal->type != TOKEN_IDENTIFIER) {
                                 errorParser(parser, "Expected field name after '.'");
                                 break; // Exit loop on error
                             }
                             Token *fieldTokenCopied = copyToken(fieldTokenOriginal); // Copy field name
                             if(!fieldTokenCopied){
                                  fprintf(stderr, "Memory allocation failed in factor (copyToken field name)\n");
                                  EXIT_FAILURE_HANDLER();
                             }
                             eat(parser, TOKEN_IDENTIFIER); // Eat original field name (frees it)
                             AST *fieldAccess = newASTNode(AST_FIELD_ACCESS, fieldTokenCopied); // Use copied token
                             freeToken(fieldTokenCopied); // Free copy of field token
                             setLeft(fieldAccess, node); // Link previous node (var or prior access)
                             node = fieldAccess; // Update node for next iteration
                        } else if (parser->current_token->type == TOKEN_LBRACKET) {
                             eat(parser, TOKEN_LBRACKET); // Frees '['
                             AST *arrayAccess = newASTNode(AST_ARRAY_ACCESS, NULL);
                             setLeft(arrayAccess, node); // Link previous node
                             // Parse index expressions
                             do {
                                 AST *indexExpr = expr(parser); // expr handles its tokens
                                 if (!indexExpr) { // Handle potential error from expr
                                      errorParser(parser, "Failed to parse array index expression");
                                      // Cleanup needed? Depends on expr error handling.
                                      // For now, break outer loop.
                                      node = arrayAccess; // Keep partially built node? Or free it?
                                      goto error_exit; // Use goto to avoid complex nested breaks
                                 }
                                 addChild(arrayAccess, indexExpr);
                                 if (parser->current_token->type == TOKEN_COMMA) {
                                     eat(parser, TOKEN_COMMA); // Frees ','
                                 } else { break; } // Exit index loop
                             } while (1);
                             // Check if loop exited because of error or end of indices
                             if (parser->current_token->type != TOKEN_RBRACKET) {
                                  errorParser(parser, "Expected ']' after array indices");
                                  node = arrayAccess; // Keep partially built node?
                                  goto error_exit; // Use goto
                             }
                             eat(parser, TOKEN_RBRACKET); // Frees ']'
                             node = arrayAccess; // Update node for next iteration
                        }
                   } // end while for field/array access
error_exit:; // Label for error jumps within loop
             }
        }
         // --- End IDENTIFIER logic ---
    } else if (token->type == TOKEN_LPAREN) {
        // Parenthesized expression
        eat(parser, TOKEN_LPAREN); // Frees '('
        node = boolExpr(parser);    // boolExpr parses its sub-expressions
        // Check for parsing errors within boolExpr before eating ')'
        if (!node) {
             errorParser(parser, "Failed to parse expression inside parentheses");
             return newASTNode(AST_NOOP, NULL);
        }
        if (parser->current_token->type != TOKEN_RPAREN) {
             errorParser(parser, "Expected ')' after expression");
             // Node might be valid but incomplete, free it?
             // freeAST(node); // Risky if boolExpr had errors
             return newASTNode(AST_NOOP, NULL);
        }
        eat(parser, TOKEN_RPAREN); // Frees ')'
    } else {
        // If none of the above match, it's an error.
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Unexpected token '%s' in factor",
                 token->value ? token->value : tokenTypeToString(token->type));
        errorParser(parser, error_msg);
        return newASTNode(AST_NOOP, NULL); // Return dummy node
    }

    // Ensure node is not NULL before returning (should be handled by error paths)
    if (!node) {
       // This case might indicate an unhandled path or error
       errorParser(parser, "Internal error: factor resulted in NULL node");
       return newASTNode(AST_NOOP, NULL);
    }

    return node; // Return the created AST node
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
    // 1. Parse the main expression to be printed
    AST *exprNode = expr(parser); // Assumes expr() parses a standard Pascal expression

    // 2. Check for optional formatting specifiers (starting with ':')
    if (parser->current_token && parser->current_token->type == TOKEN_COLON) { // Add NULL check
        eat(parser, TOKEN_COLON); // Frees ':'

        // --- Copy width token BEFORE eat ---
        Token *widthTokenOriginal = parser->current_token;
        if (!widthTokenOriginal || widthTokenOriginal->type != TOKEN_INTEGER_CONST) { // Add NULL check
            errorParser(parser, "Expected integer constant for field width after ':'");
            return exprNode; // Return base expression on error
        }
        Token *widthTokenCopied = copyToken(widthTokenOriginal); // <<< COPY
        if(!widthTokenCopied){
            fprintf(stderr, "Memory allocation failed in parseWriteArgument (copyToken width)\n");
            EXIT_FAILURE_HANDLER();
        }
        // ---

        // Eat original width token (eatInternal frees it)
        eat(parser, TOKEN_INTEGER_CONST);

        Token *decimalsTokenOriginal = NULL;
        Token *decimalsTokenCopied = NULL; // Initialize copied decimals token ptr to NULL

        // Check for optional decimal places specifier (starting with ':')
        if (parser->current_token && parser->current_token->type == TOKEN_COLON) { // Add NULL check
            eat(parser, TOKEN_COLON); // Frees second ':'

            // --- Copy decimals token BEFORE eat (if present) ---
            decimalsTokenOriginal = parser->current_token;
            if (decimalsTokenOriginal && decimalsTokenOriginal->type == TOKEN_INTEGER_CONST) { // Add NULL check
                decimalsTokenCopied = copyToken(decimalsTokenOriginal); // <<< COPY
                if(!decimalsTokenCopied){
                     fprintf(stderr, "Memory allocation failed in parseWriteArgument (copyToken decimals)\n");
                     // Clean up width copy before exiting
                     freeToken(widthTokenCopied);
                     EXIT_FAILURE_HANDLER();
                }
                // Eat original decimals token (eatInternal frees it)
                eat(parser, TOKEN_INTEGER_CONST);
            } else if (decimalsTokenOriginal) {
                 // Error: Expected integer but got something else
                 errorParser(parser, "Expected integer constant for decimal places after ':'");
                 // Do not eat the incorrect token, proceed without decimals
                 decimalsTokenCopied = NULL;
            } else {
                 // Error: Unexpected end of input
                 errorParser(parser, "Unexpected end of input after second ':' in write format");
                 decimalsTokenCopied = NULL;
            }
            // ---
        }

        // Create the formatting node
        AST *formatNode = newASTNode(AST_FORMATTED_EXPR, NULL);
        setLeft(formatNode, exprNode);

        // --- Use COPIED tokens' values for atoi ---
        int width = atoi(widthTokenCopied->value); // <<< Use copy
        int decimals = (decimalsTokenCopied != NULL) ? atoi(decimalsTokenCopied->value) : -1; // <<< Use copy
        // ---

        // Create and store the format string
        char formatStr[64]; // Buffer for "width,decimals" string
        snprintf(formatStr, sizeof(formatStr), "%d,%d", width, decimals);
        // newToken makes its own copy of formatStr
        formatNode->token = newToken(TOKEN_STRING_CONST, formatStr);

        // --- Free the COPIED tokens ---
        freeToken(widthTokenCopied); // <<< Free width copy
        if (decimalsTokenCopied) {
            freeToken(decimalsTokenCopied); // <<< Free decimals copy (if it was created)
        }
        // ---

        return formatNode; // Return the node representing the formatted expression

    } else {
        // No formatting specifiers found (no ':' after expression),
        // return the plain expression node.
        return exprNode;
    }
}

AST *parseArrayInitializer(Parser *parser) {
    eat(parser, TOKEN_LPAREN); // Consume '('
    AST *node = newASTNode(AST_ARRAY_LITERAL, NULL);
    setTypeAST(node, TYPE_ARRAY); // Mark the literal node itself as array type

    if (parser->current_token->type != TOKEN_RPAREN) { // Check for non-empty list
        while (1) {
            // Parse one element expression. IMPORTANT: These should evaluate
            // to constants during interpretation for a const declaration.
            // The parser itself just ensures valid expression syntax here.
            AST *elementExpr = expr(parser);
            addChild(node, elementExpr);

            if (parser->current_token->type == TOKEN_COMMA) {
                eat(parser, TOKEN_COMMA);
            } else {
                break; // Exit loop if no comma follows
            }
        }
    }

    eat(parser, TOKEN_RPAREN); // Consume ')'
    return node;
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

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
    eat_internal(parser_ptr, expected_token_type);
}
#endif // DEBUG

AST *parseWriteArgument(Parser *parser);

AST *declarations(Parser *parser, bool in_interface) {
    AST *node = newASTNode(AST_COMPOUND, NULL);

    while (1) {
        if (parser->current_token->type == TOKEN_CONST) {
            eat(parser, TOKEN_CONST);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *constDecl = constDeclaration(parser);
                addChild(node, constDecl);
                Value constVal = eval(constDecl->left);
                insertGlobalSymbol(constDecl->token->value, constVal.type, NULL);
                updateSymbol(constDecl->token->value, constVal);
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

void eat_internal(Parser *parser, TokenType type) {
    if (parser->current_token->type == type)
        parser->current_token = getNextToken(parser->lexer);
    else {
        char err[128];
        snprintf(err, sizeof(err), "Expected token %s, got %s",
                 tokenTypeToString(type), tokenTypeToString(parser->current_token->type));
        errorParser(parser, err);
    }
}

Token *peekToken(Parser *parser) {
    Lexer backup = *(parser->lexer);
    Token *token = getNextToken(parser->lexer);
    *(parser->lexer) = backup;
    return token;
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
    Token *prog_token = main_parser->current_token;
    eat(main_parser, TOKEN_PROGRAM);
    AST *prog_name = newASTNode(AST_VARIABLE, main_parser->current_token);
    eat(main_parser, TOKEN_IDENTIFIER);
    
    // Handle optional parameter list (e.g., (input, output))
    if (main_parser->current_token->type == TOKEN_LPAREN) {
        eat(main_parser, TOKEN_LPAREN);
        // Skip tokens until we reach the closing right parenthesis.
        while (main_parser->current_token->type != TOKEN_RPAREN) {
            eat(main_parser, main_parser->current_token->type);
        }
        eat(main_parser, TOKEN_RPAREN);
        eat(main_parser, TOKEN_SEMICOLON);
    } else {
        eat(main_parser, TOKEN_SEMICOLON);
    }
    
    // Handle the uses clause
    AST *uses_clause = NULL;
    AST *unit_ast = NULL;
    if (main_parser->current_token->type == TOKEN_USES) {
        eat(main_parser, TOKEN_USES);
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        List *unit_list = createList();
        do {
            Token *unit_token = main_parser->current_token;
            eat(main_parser, TOKEN_IDENTIFIER);
            listAppend(unit_list, unit_token->value);
            if (main_parser->current_token->type == TOKEN_COMMA) {
                eat(main_parser, TOKEN_COMMA);
            } else {
                break;
            }
        } while (1);
        eat(main_parser, TOKEN_SEMICOLON);  // Consume the semicolon after the uses clause
        AST *unit_list_node = newASTNode(AST_LIST, NULL);
        unit_list_node->unit_list = unit_list;
        addChild(uses_clause, unit_list_node);
        
        // Link the units
        for (int i = 0; i < listSize(unit_list); i++) {
            char *unit_name = listGet(unit_list, i);
            char *unit_path = findUnitFile(unit_name);
            if (unit_path == NULL) {
                fprintf(stderr, "Error: Unit '%s' not found.\n", unit_name);
                EXIT_FAILURE_HANDLER();
            }

            // Open and read the unit file
            FILE *file = fopen(unit_path, "rb");
            if (!file) {
                fprintf(stderr, "Error: Unable to open unit file '%s'.\n", unit_path);
                EXIT_FAILURE_HANDLER();
            }

            fseek(file, 0, SEEK_END);
            long fsize = ftell(file);
            rewind(file);
            char *source = malloc(fsize + 1);
            if (!source) {
                fprintf(stderr, "Memory allocation error in program.\n");
                fclose(file);
                EXIT_FAILURE_HANDLER();
            }
            fread(source, 1, fsize, file);
            fclose(file);
            source[fsize] = '\0';

            // Initialize the lexer with the unit's content
            Lexer unit_lexer;
            initLexer(&unit_lexer, source);

            // Initialize a new parser for the unit file
            Parser unit_parser_instance;
            unit_parser_instance.lexer = &unit_lexer;
            unit_parser_instance.current_token = getNextToken(&unit_lexer);

            unit_ast = unitParser(&unit_parser_instance, 0); // Pass recursion depth

            // Link the unit symbols into the global scope
            linkUnit(unit_ast, 1);

            // Clean up
            free(source);
        }
    }
    
    AST *block_node = block(main_parser);
    eat(main_parser, TOKEN_PERIOD);
    AST *node = newASTNode(AST_PROGRAM, prog_token);
    setLeft(node, prog_name);
    setRight(node, block_node);
    block_node->is_global_scope = true;
    
    // If there's a uses_clause, add it as a child
    if (uses_clause) {
        addChild(node, uses_clause);
    }
    
    return node;
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
    eat(parser, TOKEN_PROCEDURE);
    Token *procName = parser->current_token;
    eat(parser, TOKEN_IDENTIFIER);
    AST *node = newASTNode(AST_PROCEDURE_DECL, procName);
    if (parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        AST *params = paramList(parser);
        eat(parser, TOKEN_RPAREN);
        node->children = params->children;
        node->child_count = params->child_count;
    }
    if (in_interface) {
        // Do not expect a BEGIN or body
        // Add to symbol table as forward declaration
        // (Assuming add_procedure handles this)
    } else {
        eat(parser, TOKEN_SEMICOLON);
        if (parser->current_token->type == TOKEN_VAR) {
            eat(parser, TOKEN_VAR);
            AST *localDecls = newASTNode(AST_COMPOUND, NULL);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *vdecl = varDeclaration(parser, false);
                addChild(localDecls, vdecl);
                eat(parser, TOKEN_SEMICOLON);
            }
            AST *compound = compoundStatement(parser);
            AST *blockNode = newASTNode(AST_BLOCK, NULL);
            addChild(blockNode, localDecls);
            addChild(blockNode, compound);
            setRight(node, blockNode);
        } else {
            setRight(node, compoundStatement(parser));
        }
    }
    DEBUG_DUMP_AST(node, 0);
    return node;
}

AST *constDeclaration(Parser *parser) {
    Token *constNameToken = parser->current_token;
    eat(parser, TOKEN_IDENTIFIER);

    AST *typeNode = NULL;
    AST *valueNode = NULL;

    // Check for optional type specifier (Pascal extension)
    if (parser->current_token->type == TOKEN_COLON) {
        eat(parser, TOKEN_COLON);
        typeNode = typeSpecifier(parser, 1); // Parse the type (e.g., array[1..10] of string)
        // We expect an ARRAY type here for array constants
        if (!typeNode || (typeNode->type != AST_ARRAY_TYPE && typeNode->var_type != TYPE_ARRAY)) {
             // If it's a named type, var_type might be ARRAY, check that too.
             // Check if it's a reference to an array type
             int is_array_ref = 0;
             if (typeNode->type == AST_TYPE_REFERENCE) {
                  AST* ref_target = lookupType(typeNode->token->value);
                  if (ref_target && ref_target->var_type == TYPE_ARRAY) {
                       is_array_ref = 1;
                       // Optionally link typeNode directly to the target definition
                       // setRight(typeNode, ref_target); // Could be useful
                  }
             }
             if (!is_array_ref) {
                  errorParser(parser, "Expected array type specifier in typed constant array declaration");
                  // Basic error recovery: return a NOOP node
                   return newASTNode(AST_NOOP, NULL);
             }
        }
    }

    eat(parser, TOKEN_EQUAL); // Expect '=' after name or type

    // Parse the value: either a simple expression or an array initializer
    if (typeNode != NULL) { // If type was specified, expect array initializer
        if (parser->current_token->type != TOKEN_LPAREN) {
            errorParser(parser, "Expected '(' for array constant initializer list");
             return newASTNode(AST_NOOP, NULL);
        }
        valueNode = parseArrayInitializer(parser); // Parse (...)
        // Link the type definition to the literal for later use in interpreter
        setRight(valueNode, typeNode);
    } else { // No type specified, parse a simple constant expression
        valueNode = expr(parser);
    }

    eat(parser, TOKEN_SEMICOLON); // Expect ';' at the end

    // Create the main declaration node
    AST *node = newASTNode(AST_CONST_DECL, constNameToken);
    setLeft(node, valueNode); // Link the value/literal node

    // If an explicit type was parsed, store it.
    // Storing it on the valueNode might be more direct if eval needs it.
    // Let's attach it to the main CONST_DECL node for now.
    if (typeNode) {
       setRight(node, typeNode); // Link type specifier AST if present
       setTypeAST(node, TYPE_ARRAY); // Mark the CONST_DECL node as array type
    } else {
       // Type will be inferred during evaluation for simple constants
       setTypeAST(node, TYPE_VOID); // Mark as VOID initially for simple const
    }


    return node;
}

// In parser.c

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
        else if (strcasecmp(typeName, "memorystream") == 0) { // If you have this type
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
    // Assumes '(' was already consumed by the caller (typeDeclaration)
    if (parser->current_token->type == TOKEN_LPAREN) { // Optional: Consume here if not done by caller
        eat(parser, TOKEN_LPAREN);
    }

    // Node for the overall Enum Type (e.g., tmyenum)
    AST *node = newASTNode(AST_ENUM_TYPE, enumTypeNameToken); // Use the correct type name token
    setTypeAST(node, TYPE_ENUM); // Mark the type node itself as ENUM type

    int ordinal = 0;
    char *typeNameStr = strdup(enumTypeNameToken->value);

    // Parse enumeration values (e.g., valone, valtwo, ...)
    while (parser->current_token->type == TOKEN_IDENTIFIER) {
        Token *valueToken = parser->current_token;
        eat(parser, TOKEN_IDENTIFIER);

        AST *valueNode = newASTNode(AST_ENUM_VALUE, valueToken);
        valueNode->i_val = ordinal++;
        setTypeAST(valueNode, TYPE_ENUM); // Mark value node as ENUM type

        // --- ENSURE THIS LINE IS REMOVED OR COMMENTED ---
        // setRight(valueNode, node); // REMOVE THIS LINE - It creates the cycle!
        // ----------------------------------------------

        addChild(node, valueNode); // Add value node as child of AST_ENUM_TYPE node

        // --- Symbol Table Handling ---
        insertGlobalSymbol(valueToken->value, TYPE_ENUM, node);
        Symbol *symCheck = lookupGlobalSymbol(valueToken->value);
         if (symCheck && symCheck->value) {
             symCheck->value->enum_val.ordinal = valueNode->i_val;
         }
        // --- End Symbol Table Handling ---

        if (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
        } else {
            break;
        }
    }

    free(typeNameStr);
    eat(parser, TOKEN_RPAREN); // Consume ')'

    // Type name is registered by the caller (typeDeclaration)
    return node; // Return the main AST_ENUM_TYPE node
}

AST *typeDeclaration(Parser *parser) {
    Token *typeNameToken = parser->current_token; // Get the type name (e.g., "tmyenum")
    eat(parser, TOKEN_IDENTIFIER);
    eat(parser, TOKEN_EQUAL);

    AST *typeDefNode = NULL;
    AST *node = newASTNode(AST_TYPE_DECL, typeNameToken); // Main TYPE_DECL node

    // --- Check if it's an enum definition starting with '(' ---
    if (parser->current_token->type == TOKEN_LPAREN) {
        // It's an enum type like TMyEnum = (...)
        typeDefNode = parseEnumDefinition(parser, typeNameToken); // Pass the correct type name token
    } else {
        // It's another type (record, array, existing type) handled by typeSpecifier
        typeDefNode = typeSpecifier(parser, 1); // allowAnonymous=1 might not be needed here
    }
    // --- End check ---

    setLeft(node, typeDefNode); // Link the type definition AST to the TYPE_DECL node
    insertType(typeNameToken->value, typeDefNode); // Register the type name

    eat(parser, TOKEN_SEMICOLON);
    return node;
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
    eat(parser, TOKEN_FUNCTION);
    Token *funcName = parser->current_token;
    eat(parser, TOKEN_IDENTIFIER);
    AST *node = newASTNode(AST_FUNCTION_DECL, funcName);
    if (parser->current_token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        AST *params = paramList(parser);
        eat(parser, TOKEN_RPAREN);
        node->children = params->children;
        node->child_count = params->child_count;
    }
    eat(parser, TOKEN_COLON);
    AST *returnType = typeSpecifier(parser, 0);
    setRight(node, returnType);
    if (in_interface) {
        // Do not expect a BEGIN or body
        // Add to symbol table as forward declaration
        // (Assuming add_procedure handles this)
    } else {
        eat(parser, TOKEN_SEMICOLON);
        if (parser->current_token->type == TOKEN_VAR) {
            eat(parser, TOKEN_VAR);
            AST *localDecls = newASTNode(AST_COMPOUND, NULL);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *vdecl = varDeclaration(parser, false);
                addChild(localDecls, vdecl);
                eat(parser, TOKEN_SEMICOLON);
            }
            AST *compound = compoundStatement(parser);
            AST *blockNode = newASTNode(AST_BLOCK, NULL);
            addChild(blockNode, localDecls);
            addChild(blockNode, compound);
            setExtra(node, blockNode);
        } else {
            setExtra(node, compoundStatement(parser));
        }
    }
    DEBUG_DUMP_AST(node, 0);
    return node;
}

AST *paramList(Parser *parser) {
    AST *compound = newASTNode(AST_COMPOUND, NULL);
    // Loop until we see the closing parenthesis.
    while (parser->current_token->type != TOKEN_RPAREN) {
        int byRef = 0;
        // Check for pass-by-reference keyword.
        if (parser->current_token->type == TOKEN_VAR || parser->current_token->type == TOKEN_OUT) { // <<< CHECK FOR TOKEN_OUT
            byRef = 1;
            eat(parser, parser->current_token->type); // Eat either VAR or OUT
        }
        // Parse one or more identifiers separated by commas.
        AST *group = newASTNode(AST_VAR_DECL, NULL);
        // Parse the first identifier.
        AST *id_node = newASTNode(AST_VARIABLE, parser->current_token);
        eat(parser, TOKEN_IDENTIFIER);
        addChild(group, id_node);
        while (parser->current_token->type == TOKEN_COMMA) {
            eat(parser, TOKEN_COMMA);
            id_node = newASTNode(AST_VARIABLE, parser->current_token);
            eat(parser, TOKEN_IDENTIFIER);
            addChild(group, id_node);
        }
        // Expect a colon and then a type specifier.
        eat(parser, TOKEN_COLON);
        AST *typeNode = typeSpecifier(parser, 0);
        setRight(group, typeNode);
        setTypeAST(group, typeNode->var_type);

        // For each identifier in this group, create a separate parameter declaration.
        for (int i = 0; i < group->child_count; i++) {
            AST *param_decl = newASTNode(AST_VAR_DECL, NULL);
            param_decl->child_count = 1;
            param_decl->child_capacity = 1;
            param_decl->children = malloc(sizeof(AST *));
            if (!param_decl->children) {
                fprintf(stderr, "Memory allocation error in param_list\n");
                EXIT_FAILURE_HANDLER();
            }
            param_decl->children[0] = newASTNode(AST_VARIABLE, group->children[i]->token);
            param_decl->var_type = group->var_type;
            param_decl->by_ref = byRef;
            addChild(compound, param_decl);
        }
        // Clean up the temporary group node.
        free(group->children);
        free(group);
        // If there’s a semicolon, consume it and continue; otherwise, break out.
        if (parser->current_token->type == TOKEN_SEMICOLON)
            eat(parser, TOKEN_SEMICOLON);
        else
            break;
    }
    DEBUG_DUMP_AST(compound, 0);
    return compound;
}


// file: pscal/parser.c

// ... (other functions remain the same) ...

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
    AST *node = NULL;
    switch (parser->current_token->type) {
        case TOKEN_CASE:
            node = caseStatement(parser);
            break;
        case TOKEN_BREAK: // <<< ADD THIS CASE
            eat(parser, TOKEN_BREAK);
            node = newASTNode(AST_BREAK, NULL); // Create a specific AST node
            // Break doesn't need children, left, right, or extra nodes.
            break;
        case TOKEN_IDENTIFIER: {
            Token *currentIdToken = parser->current_token; // Save the identifier token
            Token *peek = peekToken(parser);
            Procedure *proc = lookupProcedure(currentIdToken->value);
            bool is_builtin_proc = isBuiltin(currentIdToken->value); // isBuiltin should check names like 'write', 'writeln', etc.

            // --- Handle specific built-ins first if they are parsed as IDENTIFIER ---
            // This covers non-standard syntax where keywords might be treated as identifiers.
            if (strcasecmp(currentIdToken->value, "write") == 0) {
                node = writeStatement(parser); // Delegate fully to writeStatement
            } else if (strcasecmp(currentIdToken->value, "writeln") == 0) {
                node = writelnStatement(parser); // Delegate fully to writelnStatement
            } else if (strcasecmp(currentIdToken->value, "read") == 0) {
                 node = readStatement(parser);
            } else if (strcasecmp(currentIdToken->value, "readln") == 0) {
                 node = readlnStatement(parser);
            }
            // --- End specific built-in handling ---

            // --- Handle explicit procedure call with parentheses ---
            else if (peek->type == TOKEN_LPAREN) {
                node = procedureCall(parser); // procedureCall consumes IDENTIFIER and args
            }
            // --- Handle assignment ---
            else if (peek->type == TOKEN_ASSIGN ||
                     peek->type == TOKEN_LBRACKET || // Could be array element assignment: arr[i] := ...
                     peek->type == TOKEN_PERIOD)    // Could be record field assignment: rec.fld := ...
            {
                 // It looks like an assignment statement starting with an identifier.
                 // Let assignmentStatement handle parsing the lvalue and expression.
                node = assignmentStatement(parser);
            }
             // --- Handle known parameter-less procedures (built-in or user-defined) ---
             else if (proc != NULL && proc->proc_decl->child_count == 0 && proc->proc_decl->type != AST_FUNCTION_DECL) {
                 node = newASTNode(AST_PROCEDURE_CALL, currentIdToken);
                 eat(parser, TOKEN_IDENTIFIER); // Consume the procedure name identifier
                 node->child_count = 0;
             }
             // --- Handle other known built-ins that might be called without parens (e.g., randomize) ---
             // Note: Builtins requiring args (like inc/dec) generally need explicit calls or keywords.
             else if (is_builtin_proc) {
                  // Example: Check for randomize, halt (no args)
                  if (strcasecmp(currentIdToken->value, "randomize") == 0 ||
                      strcasecmp(currentIdToken->value, "halt") == 0) {
                      node = newASTNode(AST_PROCEDURE_CALL, currentIdToken);
                      eat(parser, TOKEN_IDENTIFIER);
                      node->child_count = 0;
                  } else {
                       // Builtin exists but isn't handled above (maybe needs args or different context?)
                       // Treat as error or simple variable access for now.
                       char error_msg[128];
                       snprintf(error_msg, sizeof(error_msg), "Ambiguous use of identifier '%s' as statement", currentIdToken->value);
                       errorParser(parser, error_msg);
                       // Fallback (optional): node = lvalue(parser);
                       node = newASTNode(AST_NOOP, NULL); // Avoid parsing if ambiguous
                  }
             }
            // --- Fallback: Assume it's a variable/field access (or undeclared identifier) ---
            // This path is taken if it's not an explicit call, assignment, or known parameterless proc.
            else {
                 // It might be an undeclared identifier, or a variable access used as a statement (invalid Pascal).
                 // Parse it as an lvalue to consume the identifier (and potential indices/fields).
                 // The semantic analysis phase would later flag this as an error if it's not valid.
                 // node = lvalue(parser); // Parse as variable access
                 // Or signal error immediately:
                 char error_msg[128];
                 snprintf(error_msg, sizeof(error_msg), "Identifier '%s' cannot start a statement here", currentIdToken->value);
                 errorParser(parser, error_msg);
                 node = newASTNode(AST_NOOP, NULL); // Avoid parsing further
            }
            break;
        }
        // <<< END REVISED TOKEN_IDENTIFIER CASE >>>

        case TOKEN_LPAREN: {
            // An expression used as a statement, likely a function call like func(x);
            node = expr(parser); // expr should handle parsing the call correctly
            break;
        }
        case TOKEN_BEGIN:
            node = compoundStatement(parser);
            break;
        case TOKEN_IF:
            node = ifStatement(parser); // Handles the full IF..THEN..[ELSE..] structure
            break;
        case TOKEN_WHILE:
            node = whileStatement(parser);
            break;
        case TOKEN_REPEAT:
            node = repeatStatement(parser);
            break;
        case TOKEN_FOR:
            node = forStatement(parser);
            break;
        // --- Use explicit TOKEN types for built-ins ---
        case TOKEN_WRITELN:
            node = writelnStatement(parser);
            break;
        case TOKEN_WRITE:
            node = writeStatement(parser);
            break;
        case TOKEN_READLN:
            node = readlnStatement(parser);
            break;
        case TOKEN_READ:
            node = readStatement(parser);
            break;
        // --- End explicit TOKEN types ---
        default:
            // If the token cannot start any known statement type
            node = newASTNode(AST_NOOP, NULL);
             // DO NOT consume the token here; let the caller (compoundStatement) handle errors
             // related to unexpected tokens between statements.
#ifdef DEBUG
            DEBUG_PRINT("Unexpected token %s found in statement()\n", tokenTypeToString(parser->current_token->type));
#endif
            break;
    }
    #ifdef DEBUG
    if (dumpExec && node) debugAST(node, 0);
    #endif
    return node;
}
AST *assignmentStatement(Parser *parser) {
    // The current token is the start of the left-hand side (identifier, array access start, etc.)
    AST *left = lvalue(parser);  // Use lvalue() to parse variable, field access, array access
    // After lvalue returns, current_token should be ASSIGN
    eat(parser, TOKEN_ASSIGN);
    AST *right = boolExpr(parser); // Parse the right-hand side expression
    AST *node = newASTNode(AST_ASSIGN, NULL);
    setLeft(node, left);
    setRight(node, right);
    #ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
    #endif
    return node;
}

// In pscal/parser.c

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

AST *repeatStatement(Parser *parser) {
    eat(parser, TOKEN_REPEAT); // Consume REPEAT
    AST *body = newASTNode(AST_COMPOUND, NULL);

    // Parse the sequence of statements until UNTIL is encountered
    while (parser->current_token->type != TOKEN_UNTIL) {
        // Skip leading semicolons (optional empty statements)
        // Moved this inside the loop to handle multiple empty statements potentially
        while (parser->current_token->type == TOKEN_SEMICOLON) {
             eat(parser, TOKEN_SEMICOLON);
        }
        // Check again for UNTIL after skipping semicolons
        if (parser->current_token->type == TOKEN_UNTIL) {
            break;
        }

        // Parse one statement
        AST *stmt = statement(parser);
        if (stmt && stmt->type != AST_NOOP) {
             addChild(body, stmt);
        } else if (!stmt || stmt->type == AST_NOOP) {
            // If statement() failed or returned NOOP when not expected (e.g., not a simple ';')
            if (parser->current_token->type != TOKEN_UNTIL && parser->current_token->type != TOKEN_SEMICOLON) {
                 errorParser(parser, "Invalid statement or structure within REPEAT loop");
                 // Attempt to recover might be complex, exiting loop is safer
                 break;
            }
            // If it was just an empty statement ';', we loop back and skip it.
        }

        // After a valid statement, we might have a semicolon separator, or directly UNTIL
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            eat(parser, TOKEN_SEMICOLON); // Consume the separator
            // The loop condition `while (parser->current_token->type != TOKEN_UNTIL)` will handle the next step.
        } else if (parser->current_token->type != TOKEN_UNTIL) {
             // If it's not a semicolon AND not UNTIL after a statement, it's an error.
             errorParser(parser, "Expected semicolon or UNTIL after statement in REPEAT loop");
             break; // Exit loop on error
        }
        // If it *was* UNTIL, the main loop condition will catch it next iteration.
    }

    // Now the current token MUST be UNTIL
    eat(parser, TOKEN_UNTIL); // Consume the UNTIL token

    // Now parse the condition expression AFTER consuming UNTIL
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
    AST *var_node = newASTNode(AST_VARIABLE, parser->current_token);
    eat(parser, TOKEN_IDENTIFIER);
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
    ASTNodeType for_type = (direction == TOKEN_TO) ? AST_FOR_TO : AST_FOR_DOWNTO;
    AST *node = newASTNode(for_type, var_node->token);
    setLeft(node, start_expr);
    setRight(node, end_expr);
    setExtra(node, body);
    DEBUG_DUMP_AST(node, 0);
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
    AST *node = term(parser);
    while (parser->current_token->type == TOKEN_PLUS ||
           parser->current_token->type == TOKEN_MINUS) {
        Token *op = parser->current_token;
        eat(parser, op->type);
        AST *right = term(parser);
        AST *new_node = newASTNode(AST_BINARY_OP, op);
        setLeft(new_node, node);
        setRight(new_node, right);
        setTypeAST(new_node, inferBinaryOpType(node->var_type, right->var_type));
        node = new_node;
    }
    DEBUG_DUMP_AST(node, 0);
    return node;
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
    AST *node = expr(parser);
    while (parser->current_token->type == TOKEN_GREATER ||
           parser->current_token->type == TOKEN_GREATER_EQUAL ||
           parser->current_token->type == TOKEN_EQUAL ||
           parser->current_token->type == TOKEN_LESS ||
           parser->current_token->type == TOKEN_LESS_EQUAL ||
           parser->current_token->type == TOKEN_NOT_EQUAL ||
           parser->current_token->type == TOKEN_IN) {
        Token *op = parser->current_token;
        eat(parser, op->type);
        
        AST *right;
        // Special handling for 'in': right side must be a set constructor
        if (op->type == TOKEN_IN) {
            right = parseSetConstructor(parser); // Parse the set part
        } else {
            right = expr(parser); // Parse regular expression for other ops
        }
        
        AST *new_node = newASTNode(AST_BINARY_OP, op);
        setLeft(new_node, node);
        setRight(new_node, right);
        setTypeAST(new_node, TYPE_BOOLEAN);
        node = new_node;
    }
    DEBUG_DUMP_AST(node, 0);
    return node;
}

AST *boolExpr(Parser *parser) {
    AST *node = relExpr(parser);
    while (parser->current_token->type == TOKEN_AND || parser->current_token->type == TOKEN_OR) {
        Token *op = parser->current_token;
        eat(parser, op->type);
        AST *right = relExpr(parser);
        AST *new_node = newASTNode(AST_BINARY_OP, op);
        setLeft(new_node, node);
        setRight(new_node, right);
        node = new_node;
    }
    return node;
}

AST *term(Parser *parser) {
    AST *node = factor(parser); // Parse left operand

#ifdef DEBUG
    // ADDED: Check token immediately after parsing the first factor
    if (dumpExec) {
        fprintf(stderr, "[DEBUG_TERM] After Factor: Next token is %s ('%s')\n",
                tokenTypeToString(parser->current_token->type),
                parser->current_token->value ? parser->current_token->value : "NULL");
    }
#endif

    while (parser->current_token->type == TOKEN_MUL ||
           parser->current_token->type == TOKEN_SLASH ||
           parser->current_token->type == TOKEN_INT_DIV ||
           parser->current_token->type == TOKEN_MOD) {
        
        Token *op = parser->current_token; // Get operator token
        
#ifdef DEBUG
        // Existing debug print before eat
        if (dumpExec) {
             fprintf(stderr, "[DEBUG_TERM] Before eat: Loop Op=%s, Current token is %s ('%s')\n",
                     tokenTypeToString(op->type),
                     tokenTypeToString(parser->current_token->type),
                     parser->current_token->value ? parser->current_token->value : "NULL");
        }
#endif

        eat(parser, op->type); // Consume the operator token (*, /, div, mod)

#ifdef DEBUG
        // Existing debug print after eat
        if (dumpExec) {
             fprintf(stderr, "[DEBUG_TERM] After eat: Expecting Factor, Current token is %s ('%s')\n",
                     tokenTypeToString(parser->current_token->type),
                     parser->current_token->value ? parser->current_token->value : "NULL");
        }
#endif

        AST *right = factor(parser); // Call factor to parse the right operand

        // --- Rest of the loop remains the same ---
        AST *new_node = newASTNode(AST_BINARY_OP, op);
        setLeft(new_node, node);
        setRight(new_node, right);
        // Assuming inferBinaryOpType exists and works:
        setTypeAST(new_node, inferBinaryOpType(node->var_type, right->var_type));
        node = new_node;
        // --- End of loop body changes ---
    }
    // DEBUG_DUMP_AST(node, 0); // Original debug dump
    return node;
}

AST *factor(Parser *parser) {
    Token *token = parser->current_token;

#ifdef DEBUG
    // ADDED: Debug print at the ENTRY of factor
    if (dumpExec) { // Check dumpExec flag
        fprintf(stderr, "[DEBUG_FACTOR] Entry: Current token is %s ('%s')\n",
                tokenTypeToString(token->type),
                token->value ? token->value : "NULL");
    }
#endif

    if (token->type == TOKEN_TRUE) {
        eat(parser, TOKEN_TRUE);
        return newASTNode(AST_BOOLEAN, token);
    } else if (token->type == TOKEN_FALSE) {
        eat(parser, TOKEN_FALSE);
        return newASTNode(AST_BOOLEAN, token);
    } else if (token->type == TOKEN_PLUS ||
               token->type == TOKEN_MINUS ||
               token->type == TOKEN_NOT) {
        Token *op = token;
        eat(parser, token->type);
        AST *node = newASTNode(AST_UNARY_OP, op);
        setLeft(node, factor(parser)); // Recursively parse the factor after the operator
        // Type annotation for unary op would happen later or be inferred
        return node;
    } else if (token->type == TOKEN_INTEGER_CONST ||
               token->type == TOKEN_HEX_CONST ||
               token->type == TOKEN_REAL_CONST) {
        eat(parser, token->type); // Consume the number token
        return newASTNode(AST_NUMBER, token); // Return AST_NUMBER node
    } else if (token->type == TOKEN_STRING_CONST) {
        eat(parser, TOKEN_STRING_CONST); // Consume the string token
        return newASTNode(AST_STRING, token); // Return AST_STRING node
    } else if (token->type == TOKEN_IDENTIFIER) {
        // Always treat "result" as a variable, not as a procedure call.
        if (strcasecmp(token->value, "result") == 0) {
             AST *node = newASTNode(AST_VARIABLE, token);
             eat(parser, TOKEN_IDENTIFIER);
             // Note: Standard Pascal wouldn't allow field/array access on 'Result'
             // but you might add checks here if your dialect allows it.
             return node;
         }
        // If the next token is a left parenthesis, always parse a call.
        if (peekToken(parser)->type == TOKEN_LPAREN) {
            return procedureCall(parser); // procedureCall handles parsing the call
        } else {
            // Not followed by LPAREN.
            // Check if it's a known procedure/function first.
            // NOTE: Standard Pascal requires () for function calls even with no args.
            // This logic allows calling parameter-less procedures without ().
            Procedure *proc = lookupProcedure(token->value); // lookupProcedure checks lowercase
            if (proc != NULL) {
                // It's a known procedure or function name.
                // Decide if it's being used as a function call (needs return value)
                // or potentially a procedure call statement (handled earlier).
                // Here in 'factor', we usually expect something returning a value.
                 if (proc->proc_decl && proc->proc_decl->type == AST_FUNCTION_DECL) {
                     // It's a function, parse as a call (even without parens here)
                     AST *node = newASTNode(AST_PROCEDURE_CALL, token);
                     eat(parser, TOKEN_IDENTIFIER);
                     node->children = NULL;
                     node->child_count = 0;
                     node->var_type = proc->proc_decl->var_type; // Set return type
                     return node;
                 } else {
                      // It's a procedure name used where a value/factor is expected.
                      // This is generally a syntax error in standard Pascal.
                      char error_msg[128];
                      snprintf(error_msg, sizeof(error_msg), "Procedure '%s' found where a value (factor) is expected", token->value);
                      errorParser(parser, error_msg);
                      return newASTNode(AST_NOOP, NULL); // Return dummy node
                 }

            } else {
                // Otherwise, treat it as a variable reference (potentially with field/array access).
                AST *node = newASTNode(AST_VARIABLE, token);
                eat(parser, TOKEN_IDENTIFIER); // Consume the base identifier

                // Handle any field or array accesses that follow the variable.
                while (parser->current_token->type == TOKEN_PERIOD ||
                       parser->current_token->type == TOKEN_LBRACKET) {
                    if (parser->current_token->type == TOKEN_PERIOD) {
                        eat(parser, TOKEN_PERIOD);
                        Token *fieldToken = parser->current_token;
                        if (fieldToken->type != TOKEN_IDENTIFIER)
                            errorParser(parser, "Expected field name after '.'");
                        AST *fieldAccess = newASTNode(AST_FIELD_ACCESS, fieldToken);
                        eat(parser, TOKEN_IDENTIFIER);
                        setLeft(fieldAccess, node); // Previous node becomes left child
                        node = fieldAccess; // Update node to the field access node
                    } else if (parser->current_token->type == TOKEN_LBRACKET) {
                        eat(parser, TOKEN_LBRACKET);
                        AST *arrayAccess = newASTNode(AST_ARRAY_ACCESS, NULL);
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
                        node = arrayAccess; // Update node to the array access node
                    }
                }
                return node; // Return the complete variable/access node
            }
        }
    } else if (token->type == TOKEN_LPAREN) {
        eat(parser, TOKEN_LPAREN);
        AST *node = boolExpr(parser); // Parse the expression inside parentheses
        eat(parser, TOKEN_RPAREN);
        return node;
    }
    // If none of the above match, it's an error.
    errorParser(parser, "Unexpected token in factor");
    return NULL; // Should not be reached if errorParser exits
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
    if (parser->current_token->type == TOKEN_COLON) {
        eat(parser, TOKEN_COLON); // Consume ':'

        // 3. Parse the field width (must be integer constant)
        if (parser->current_token->type != TOKEN_INTEGER_CONST) {
            errorParser(parser, "Expected integer constant for field width after ':'");
            // On error, just return the base expression node; runtime can handle default formatting.
            return exprNode;
        }
        Token *widthToken = parser->current_token;
        eat(parser, TOKEN_INTEGER_CONST); // Consume width token

        // 4. Check for optional decimal places specifier (starting with ':')
        Token *decimalsToken = NULL; // Initialize to NULL
        if (parser->current_token->type == TOKEN_COLON) {
            eat(parser, TOKEN_COLON); // Consume second ':'

            // 5. Parse the decimal places (must be integer constant)
            if (parser->current_token->type != TOKEN_INTEGER_CONST) {
                errorParser(parser, "Expected integer constant for decimal places after ':'");
                // On error, proceed with only width formatting (decimalsToken remains NULL)
            } else {
                decimalsToken = parser->current_token; // Store token if present
                eat(parser, TOKEN_INTEGER_CONST);      // Consume decimals token
            }
        }

        // 6. Create a dedicated formatting node (AST_FORMATTED_EXPR)
        //    - Original expression goes in the 'left' child.
        //    - Formatting info ("width,decimals") is stored in the format node's token->value.
        AST *formatNode = newASTNode(AST_FORMATTED_EXPR, NULL); // No specific token needed for the node itself
        setLeft(formatNode, exprNode); // Link original expression

        // Create the format string (e.g., "10,-1" or "8,2")
        char formatStr[64]; // Buffer for "width,decimals" string
        int width = atoi(widthToken->value);
        int decimals = (decimalsToken != NULL) ? atoi(decimalsToken->value) : -1; // Use -1 if no decimals token
        snprintf(formatStr, sizeof(formatStr), "%d,%d", width, decimals);

        // Store the format string in the format node's token (create a dummy token)
        // Use TOKEN_STRING_CONST type for convenience in storing the string value.
        formatNode->token = newToken(TOKEN_STRING_CONST, formatStr);

        // Return the formatNode, which represents the formatted expression
        return formatNode;

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

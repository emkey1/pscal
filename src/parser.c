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
    AST *node = newASTNode(AST_COMPOUND, NULL); // Main compound for all declarations in this scope

    while (1) {
        // --- CONST ---
        if (parser->current_token->type == TOKEN_CONST) {
            eat(parser, TOKEN_CONST);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *constDecl = constDeclaration(parser); // Parses Name = Value;
                if (!constDecl || constDecl->type == AST_NOOP) { /* Handle error from constDeclaration */ break; }
                addChild(node, constDecl);

                // Logic to evaluate and insert const symbol (existing correct logic)
                if (constDecl->left) { // Ensure there is a value node to evaluate
                     Value constVal = eval(constDecl->left);
                     insertGlobalSymbol(constDecl->token->value, constVal.type, constDecl->right);
                     Symbol *sym = lookupGlobalSymbol(constDecl->token->value);
                     if (sym && sym->value) {
                         if (!sym->is_alias) freeValue(sym->value);
                         *sym->value = makeCopyOfValue(&constVal);
                         sym->is_const = true;
                         #ifdef DEBUG
                         fprintf(stderr, "[DEBUG_PARSER] Set is_const=TRUE for global constant '%s'\n", sym->name);
                         #endif
                     } else { /* Handle error */ }
                     freeValue(&constVal);
                } else { /* Handle constDecl without a value node? Error? */ }
            }
        }
        // --- TYPE ---
        else if (parser->current_token->type == TOKEN_TYPE) {
            eat(parser, TOKEN_TYPE);
            while (parser->current_token->type == TOKEN_IDENTIFIER) {
                AST *typeDecl = typeDeclaration(parser);
                 if (!typeDecl || typeDecl->type == AST_NOOP) { /* Handle error */ break; }
                addChild(node, typeDecl);
                // insertType needs the actual definition (left child of TYPE_DECL)
                if (typeDecl->left) insertType(typeDecl->token->value, typeDecl->left);
                else { /* Handle error: TYPE decl missing definition */ }
            }
        }
        // --- VAR ---
        else if (parser->current_token->type == TOKEN_VAR) {
            eat(parser, TOKEN_VAR);
            while (parser->current_token && parser->current_token->type == TOKEN_IDENTIFIER) { // Check token exists
                AST *vdecl_result = varDeclaration(parser, false); // isGlobal flag seems unused?
                if (!vdecl_result || vdecl_result->type == AST_NOOP) {
                     // varDeclaration should call errorParser on failure
                     // If it returns NULL/NOOP without erroring, that's an issue.
                     // Assume error was handled, break loop.
                     break;
                }

                // --- Refined logic to add VAR_DECL nodes to the main compound 'node' ---
                if (vdecl_result->type == AST_COMPOUND) {
                    // Transfer children from the compound returned by varDeclaration
                    while (vdecl_result->child_count > 0) {
                         AST* child_to_transfer = vdecl_result->children[--vdecl_result->child_count];
                         vdecl_result->children[vdecl_result->child_count] = NULL; // Nullify in source
                         if (child_to_transfer) {
                             addChild(node, child_to_transfer); // Adds child to 'node', sets parent
                         }
                    }
                    // Free the now empty compound wrapper (struct + NULL children array)
                    freeAST(vdecl_result);
                } else if (vdecl_result->type == AST_VAR_DECL) {
                     // varDeclaration returned a single VAR_DECL node
                     addChild(node, vdecl_result); // Add single VAR_DECL to 'node'
                } else {
                     // Should not happen if varDeclaration works correctly
                     char err_msg[100];
                     snprintf(err_msg, sizeof(err_msg), "Unexpected node type %s returned by varDeclaration", astTypeToString(vdecl_result->type));
                     errorParser(parser, err_msg);
                     freeAST(vdecl_result); // Free the unexpected node
                     break;
                }
                // --- End refined logic ---

                 // Ensure semicolon follows the var declaration group
                 if (parser->current_token && parser->current_token->type == TOKEN_SEMICOLON) {
                     eat(parser, TOKEN_SEMICOLON);
                 } else {
                      errorParser(parser, "Expected semicolon after var declaration");
                      break; // Exit the inner while loop
                 }
            } // end while (parsing var groups for this VAR keyword)
        }
        // --- PROCEDURE/FUNCTION ---
        else if (parser->current_token->type == TOKEN_PROCEDURE ||
                 parser->current_token->type == TOKEN_FUNCTION) {
            AST *decl = (parser->current_token->type == TOKEN_PROCEDURE)
                        ? procedureDeclaration(parser, in_interface)
                        : functionDeclaration(parser, in_interface);
             if (!decl || decl->type == AST_NOOP) { /* Handle error */ break; }
            addChild(node, decl);
            addProcedure(decl); // Assumes addProcedure handles potential errors/duplicates
             // Ensure semicolon follows proc/func declaration
             if (parser->current_token && parser->current_token->type == TOKEN_SEMICOLON) {
                 eat(parser, TOKEN_SEMICOLON);
             } else {
                  // In interface section, semicolon might be optional if it's the last item before IMPLEMENTATION
                  // In implementation, it should generally be required before next decl or BEGIN/END.
                  // Add stricter checking if needed based on context (in_interface). For now, allow missing.
                  // errorParser(parser, "Expected semicolon after procedure/function declaration");
                  // break;
             }
        }
        // --- ENUM (If handled separately, otherwise part of TYPE) ---
        // else if (parser->current_token->type == TOKEN_ENUM) { ... } // Assuming handled within TYPE
        // --- Exit loop ---
        else {
            break; // Exit loop if no more declaration keywords found
        }
    } // End while(1)

    return node; // Returns the compound node containing all declarations for this scope
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

// lvalue: Parses variable.field[index] etc. Calls expression for index.
AST *lvalue(Parser *parser) {
    Token *token = parser->current_token;
    // Check for NULL token added for robustness
    if (!token || token->type != TOKEN_IDENTIFIER) {
        errorParser(parser, "Expected identifier at start of lvalue");
        return newASTNode(AST_NOOP, NULL);
    }
    AST *node = newASTNode(AST_VARIABLE, token); // Uses copy
    eat(parser, TOKEN_IDENTIFIER); // Consumes original identifier

    while (parser->current_token &&
           (parser->current_token->type == TOKEN_PERIOD ||
            parser->current_token->type == TOKEN_LBRACKET ||
            parser->current_token->type == TOKEN_CARET)) { // <<< ADDED CARET CHECK >>>

        if (parser->current_token->type == TOKEN_PERIOD) {
            // Field Access Logic (existing)
            eat(parser, TOKEN_PERIOD);
            Token *field = copyToken(parser->current_token);
            if(!field || field->type != TOKEN_IDENTIFIER){ errorParser(parser,"Expected field name"); if(field) freeToken(field); /* Return partial node? */ return node; }
            eat(parser, TOKEN_IDENTIFIER);
            AST *fa = newASTNode(AST_FIELD_ACCESS, field);
            freeToken(field);
            setLeft(fa, node); // Previous node becomes left child
            node = fa;         // Update node to the field access node
        } else if (parser->current_token->type == TOKEN_LBRACKET) {
            // Array Access Logic (existing)
            eat(parser, TOKEN_LBRACKET);
            AST *aa = newASTNode(AST_ARRAY_ACCESS, NULL);
            setLeft(aa, node); // Previous node becomes left child
            do {
                AST *idx = expression(parser);
                if(!idx || idx->type == AST_NOOP){ errorParser(parser,"Bad index expression"); freeAST(aa); /* Return partial node? */ return node; }
                addChild(aa, idx); // Add index as child
                if(parser->current_token && parser->current_token->type == TOKEN_COMMA) eat(parser, TOKEN_COMMA);
                else break;
            } while(1);
            if(!parser->current_token || parser->current_token->type != TOKEN_RBRACKET){ errorParser(parser,"Expected ']' after array indices"); /* Return partial node? */ return node; }
            eat(parser, TOKEN_RBRACKET);
            node = aa; // Update node to the array access node
        } else { // TOKEN_CARET - Pointer Dereference <<< ADDED >>>
            eat(parser, TOKEN_CARET); // Consume '^'
            AST *derefNode = newASTNode(AST_DEREFERENCE, NULL); // No token associated with deref op itself
            setLeft(derefNode, node); // The node *being dereferenced* is the left child
            // Type annotation will set the var_type of derefNode later
            setTypeAST(derefNode, TYPE_VOID); // Placeholder type
            node = derefNode; // Update node to the dereference node
        }
    } // End while loop for ., [, ^
    return node;
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

AST *unitParser(Parser *parser, int recursion_depth) {
    // Prevent infinite recursion
    if (recursion_depth > MAX_RECURSION_DEPTH) {
        fprintf(stderr, "Error: Maximum recursion depth exceeded while parsing units.\n");
        EXIT_FAILURE_HANDLER();
    }

    // The current token should be the 'unit' keyword
    Token *unit_keyword = parser->current_token;
    if (!unit_keyword || unit_keyword->type != TOKEN_UNIT) { /* ... error handling ... */ EXIT_FAILURE_HANDLER(); }
    AST *unit_node = newASTNode(AST_UNIT, parser->current_token);
    eat(parser, TOKEN_UNIT);

    // The next token should be the unit name
    Token *unit_token = parser->current_token;
    if (!unit_token || unit_token->type != TOKEN_IDENTIFIER) { /* ... error handling ... */ freeAST(unit_node); EXIT_FAILURE_HANDLER(); }
    char *unit_name_for_debug = strdup(unit_token->value);
    if (!unit_name_for_debug) { /* Malloc error */ freeAST(unit_node); EXIT_FAILURE_HANDLER(); }
    eat(parser, TOKEN_IDENTIFIER);

    // Check for semicolon
    if (!parser->current_token || parser->current_token->type != TOKEN_SEMICOLON) { /* ... error handling ... */ free(unit_name_for_debug); freeAST(unit_node); EXIT_FAILURE_HANDLER(); }
    eat(parser, TOKEN_SEMICOLON);

    // Handle the uses clause
    AST *uses_clause = NULL;
    if (parser->current_token && parser->current_token->type == TOKEN_USES) {
        eat(parser, TOKEN_USES);
        uses_clause = newASTNode(AST_USES_CLAUSE, NULL);
        List *unit_list = createList();
        do { // Parse unit list
            Token *unit_list_token = parser->current_token;
            if (!unit_list_token || unit_list_token->type != TOKEN_IDENTIFIER) { /* ... error cleanup ... */ EXIT_FAILURE_HANDLER(); }
            listAppend(unit_list, unit_list_token->value);
            eat(parser, TOKEN_IDENTIFIER);
            if (parser->current_token && parser->current_token->type == TOKEN_COMMA) { eat(parser, TOKEN_COMMA); }
            else { break; }
        } while (1);
        if (!parser->current_token || parser->current_token->type != TOKEN_SEMICOLON) { /* ... error cleanup ... */ EXIT_FAILURE_HANDLER(); }
        eat(parser, TOKEN_SEMICOLON);
        AST *unit_list_node = newASTNode(AST_LIST, NULL);
        unit_list_node->unit_list = unit_list;
        addChild(uses_clause, unit_list_node);

        // Link the units
        for (int i = 0; i < listSize(unit_list); i++) {
            char *nested_unit_name = listGet(unit_list, i);
            char *nested_unit_path = findUnitFile(nested_unit_name);
            if (nested_unit_path == NULL) { /* ... error cleanup ... */ EXIT_FAILURE_HANDLER(); }
            // Read file content
            FILE *nested_file = fopen(nested_unit_path, "r");
            if (!nested_file) { /* ... error cleanup ... */ EXIT_FAILURE_HANDLER(); }
            fseek(nested_file, 0, SEEK_END); long nested_fsize = ftell(nested_file); rewind(nested_file);
            char *nested_source = malloc(nested_fsize + 1);
            if (!nested_source) { /* ... error cleanup ... */ EXIT_FAILURE_HANDLER(); }
            fread(nested_source, 1, nested_fsize, nested_file); nested_source[nested_fsize] = '\0'; fclose(nested_file);
            // Parse recursively
            Lexer nested_unit_lexer; initLexer(&nested_unit_lexer, nested_source);
            Parser nested_unit_parser; nested_unit_parser.lexer = &nested_unit_lexer; nested_unit_parser.current_token = getNextToken(&nested_unit_lexer);
            AST *nested_unit_ast = unitParser(&nested_unit_parser, recursion_depth + 1);
            if (!nested_unit_ast) { /* ... error cleanup ... */ EXIT_FAILURE_HANDLER(); }
            linkUnit(nested_unit_ast, recursion_depth + 1);
            free(nested_source); free(nested_unit_path);
        }
         if (uses_clause) { addChild(unit_node, uses_clause); } // Add uses_clause if it exists
    } // End if (TOKEN_USES)

    // Parse the interface section
    if (!parser->current_token || parser->current_token->type != TOKEN_INTERFACE) { /* ... error handling ... */ EXIT_FAILURE_HANDLER(); }
    eat(parser, TOKEN_INTERFACE);
    AST *interface_decls = declarations(parser, true);
    setLeft(unit_node, interface_decls); // Store interface decls in left

    // Build INTERFACE symbol table
    Symbol *unitSymbols = buildUnitSymbolTable(interface_decls);
    unit_node->symbol_table = unitSymbols;

    // Parse IMPLEMENTATION section
    if (!parser->current_token || parser->current_token->type != TOKEN_IMPLEMENTATION) { /* ... error handling ... */ EXIT_FAILURE_HANDLER(); }
    eat(parser, TOKEN_IMPLEMENTATION);
    AST *impl_decls = declarations(parser, false);
    setExtra(unit_node, impl_decls); // Store impl decls in extra

    // --- MODIFICATION START: Process IMPLEMENTATION VAR/CONST as Globals ---
    if (impl_decls && impl_decls->type == AST_COMPOUND) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG UNIT_IMPL] Processing IMPLEMENTATION declarations for Unit '%s'\n", unit_name_for_debug);
        #endif
        for (int i = 0; i < impl_decls->child_count; i++) {
            AST *declNode = impl_decls->children[i];
            if (!declNode) continue;

            if (declNode->type == AST_VAR_DECL) {
                 AST *typeNode = declNode->right;
                 if (!typeNode) { /* ... warning ... */ continue; }
                 for (int j = 0; j < declNode->child_count; j++) {
                    AST *varNode = declNode->children[j];
                    if (!varNode || !varNode->token) continue;
                    const char *varName = varNode->token->value;

                    // --- REINSTATED Global Insert ---
                    #ifdef DEBUG
                    fprintf(stderr, "[DEBUG UNIT_IMPL]   Attempting global insert for VAR: %s (Type: %s)\n", varName, varTypeToString(declNode->var_type));
                    #endif
                    // Insert into GLOBAL symbol table (insertGlobalSymbol handles internal duplicate check)
                    insertGlobalSymbol(varName, declNode->var_type, typeNode);
                    // ---

                    // --- Array Initialization (IF symbol was successfully added/found and is an array) ---
                    Symbol *sym = lookupGlobalSymbol(varName); // Look up *after* attempting insert
                    if (sym && sym->value && sym->type == TYPE_ARRAY) {
                         if (sym->value->array_val == NULL) { // Check if uninitialized
                             #ifdef DEBUG
                             fprintf(stderr, "[DEBUG UNIT_IMPL]     Initializing global array VAR: %s\n", varName);
                             #endif
                            // (Array initialization logic using makeArrayND - kept from previous step)
                            AST *actualArrayTypeNode = typeNode;
                             if (actualArrayTypeNode->type == AST_TYPE_REFERENCE) { /* ... resolve reference ... */
                                 actualArrayTypeNode = lookupType(actualArrayTypeNode->token->value);
                                 if (!actualArrayTypeNode) { /* Error */ continue; }
                             }
                             if (actualArrayTypeNode->type != AST_ARRAY_TYPE) { /* Error */ continue; }
                             int dimensions = actualArrayTypeNode->child_count;
                             if (dimensions <= 0) { /* Error */ continue; }
                             int *lower_bounds = malloc(sizeof(int) * dimensions);
                             int *upper_bounds = malloc(sizeof(int) * dimensions);
                             if (!lower_bounds || !upper_bounds) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
                             bool bounds_ok = true;
                              for (int dim = 0; dim < dimensions; dim++) { /* ... evaluate bounds (ensure eval works here) ... */
                                  AST *subrange = actualArrayTypeNode->children[dim];
                                  if (!subrange || subrange->type != AST_SUBRANGE || !subrange->left || !subrange->right) { bounds_ok = false; break; }
                                  Value low_val = eval(subrange->left); Value high_val = eval(subrange->right);
                                  if (low_val.type != TYPE_INTEGER || high_val.type != TYPE_INTEGER) { bounds_ok = false; break; }
                                  lower_bounds[dim] = (int)low_val.i_val; upper_bounds[dim] = (int)high_val.i_val;
                                  if (lower_bounds[dim] > upper_bounds[dim]) { bounds_ok = false; break; }
                              }
                             if (!bounds_ok) { free(lower_bounds); free(upper_bounds); continue; }
                             AST *elemTypeNode = actualArrayTypeNode->right; VarType elemType = TYPE_VOID;
                             if (!elemTypeNode) { /* Error */ free(lower_bounds); free(upper_bounds); continue; }
                             elemType = elemTypeNode->var_type;
                             if (elemType == TYPE_VOID && elemTypeNode->type == AST_VARIABLE && elemTypeNode->token) { /* ... fallback type lookup ... */
                                const char *elemTypeName = elemTypeNode->token->value;
                                if (strcasecmp(elemTypeName, "integer")==0) elemType = TYPE_INTEGER;
                                else { AST* userTypeDef = lookupType(elemTypeName); if(userTypeDef) elemType = userTypeDef->var_type; else elemType = TYPE_VOID;}
                             } else if (elemType == TYPE_VOID) { /* Error */ elemType = TYPE_VOID; }

                             if (elemType != TYPE_VOID) {
                                 Value initialized_array = makeArrayND(dimensions, lower_bounds, upper_bounds, elemType, elemTypeNode);
                                 freeValue(sym->value); // Free default Value struct content
                                 *sym->value = initialized_array; // Assign new array Value
                             }
                             free(lower_bounds); free(upper_bounds);
                         }
                    } else if (!sym) {
                         // This case should ideally not happen if insertGlobalSymbol succeeded or handled duplicate.
                         // If insertGlobalSymbol printed an error and returned due to duplicate, sym would be NULL here.
                         fprintf(stderr, "Warning: Could not find global symbol for IMPLEMENTATION var '%s' after insertion attempt.\n", varName);
                    }
                    // --- END Array Initialization ---
                 } // End loop j (var names)
            } else if (declNode->type == AST_CONST_DECL) {
                if (!declNode->token || !declNode->left) continue;
                const char *constName = declNode->token->value;

                // --- REINSTATED Global Insert (without prior check) ---
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG UNIT_IMPL]   Attempting global insert for CONST: %s\n", constName);
                #endif
                Value constVal;
                 if (declNode->left->type == AST_NUMBER || declNode->left->type == AST_STRING || declNode->left->type == AST_BOOLEAN) {
                      constVal = eval(declNode->left); // Evaluate simple literals
                 } else {
                      fprintf(stderr, "Warning: Cannot evaluate complex IMPLEMENTATION constant '%s' during parse phase in unit '%s'. Skipping.\n", constName, unit_name_for_debug);
                      continue; // Skip if not simple
                 }
                 // Insert into GLOBAL symbol table (insertGlobalSymbol handles internal duplicate check)
                 insertGlobalSymbol(constName, constVal.type, declNode->right);
                 // ---

                 // --- Update value and set const flag (IF symbol exists after insert attempt) ---
                 Symbol *sym = lookupGlobalSymbol(constName);
                 if (sym && sym->value) {
                      if (!sym->is_const) { // Only update if not already marked const
                          freeValue(sym->value);
                         *sym->value = makeCopyOfValue(&constVal);
                         sym->is_const = true;
                         #ifdef DEBUG
                         fprintf(stderr, "[DEBUG UNIT_IMPL]     Set value and is_const=TRUE for global constant '%s'\n", constName);
                         #endif
                      } else {
                           #ifdef DEBUG
                           fprintf(stderr, "[DEBUG UNIT_IMPL]     Skipping value update for already const global '%s'\n", constName);
                           #endif
                      }
                 } else if (!sym) {
                      // This implies insertGlobalSymbol failed (likely duplicate based on its internal check)
                       #ifdef DEBUG
                       fprintf(stderr, "[DEBUG UNIT_IMPL]     Skipping value update for CONST '%s' (likely duplicate prevented insert)\n", constName);
                       #endif
                 } else { /* sym exists but sym->value is NULL - Should not happen after insertGlobalSymbol */ }
                 freeValue(&constVal); // Free temp value from eval
                 // ---
            } // End if CONST_DECL
        } // End loop i (declarations)
    }
    // --- MODIFICATION END ---

    // Handle INITIALIZATION block
    int has_initialization = 0;
    if (parser->current_token && parser->current_token->type == TOKEN_BEGIN) {
        AST *init_block = compoundStatement(parser);
         if (!init_block) { /* ... error handling ... */ EXIT_FAILURE_HANDLER(); }
        setRight(unit_node, init_block); // Store init_block in right
        has_initialization = 1;
    }

    // Consume final 'end.'
    if (has_initialization) {
        if (!parser->current_token || parser->current_token->type != TOKEN_PERIOD) { /* ... error handling ... */ EXIT_FAILURE_HANDLER(); }
        eat(parser, TOKEN_PERIOD);
    } else {
        if (!parser->current_token || parser->current_token->type != TOKEN_END) { /* ... error handling ... */ EXIT_FAILURE_HANDLER(); }
        eat(parser, TOKEN_END);
        if (!parser->current_token || parser->current_token->type != TOKEN_PERIOD) { /* ... error handling ... */ EXIT_FAILURE_HANDLER(); }
        eat(parser, TOKEN_PERIOD);
    }

    free(unit_name_for_debug);
    return unit_node;
}

void errorParser(Parser *parser, const char *msg) {
    fprintf(stderr, "Parser error at line %d, column %d: %s (found token: %s)\n",
            parser->lexer->line, parser->lexer->column, msg,
            tokenTypeToString(parser->current_token->type));
    EXIT_FAILURE_HANDLER();
}

void addProcedure(AST *proc_decl) {
    // Allocate a new Procedure structure.
    Procedure *proc = malloc(sizeof(Procedure));
    if (!proc) {
        fprintf(stderr, "Memory allocation error in addProcedure\n"); // Corrected function name
        EXIT_FAILURE_HANDLER();
    }
    // Use the procedure name from the token of the declaration.
    const char *originalName = proc_decl->token->value;
    char *lowerName = strdup(originalName); // Duplicate the original name
    if (!lowerName) {
        fprintf(stderr, "Memory allocation error in addProcedure (strdup)\n");
        free(proc); // Free allocated proc struct
        EXIT_FAILURE_HANDLER();
    }
    // Convert the duplicated name to lowercase
    for (int i = 0; lowerName[i]; i++) {
        lowerName[i] = tolower((unsigned char)lowerName[i]);
    }
    proc->name = lowerName; // Store the lowercase name

    // ADDED: Make a deep copy of the procedure declaration AST node.
    // This copy is now owned by the Procedure struct in the global table.
    // This is crucial to avoid use-after-free when the original AST is freed.
    AST *proc_decl_copy = copyAST(proc_decl); // <<< THIS IS THE CRUCIAL ADDITION
    if (!proc_decl_copy) {
        fprintf(stderr, "Memory allocation error in addProcedure (copyAST)\n");
        // Need to free allocated proc and lowerName before exiting
        free(lowerName);
        free(proc);
        EXIT_FAILURE_HANDLER();
    }

    // Use the COPY in the procedure table entry.
    proc->proc_decl = proc_decl_copy; // <<< Now stores the pointer to the COPY

    // Link the new procedure into the head of the procedure table list.
    proc->next = procedure_table;
    procedure_table = proc;

    // Optional Debug Print
    #ifdef DEBUG
    if (dumpExec) {
        fprintf(stderr, "[DEBUG] addProcedure: Added procedure '%s' (original: '%s') to table. Copied AST node at %p\n", proc->name, originalName, (void*)proc->proc_decl);
    }
    #endif

    // The original proc_decl node remains part of the main AST or unit AST structure.
    // It will be freed when that AST is freed by the general cleanup (freeAST on the root).
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
             // Need to free prog_name_node too
             freeAST(prog_name_node);
             return newASTNode(AST_NOOP, NULL);
        }
    }
    // Consume the final semicolon after program header
    if (main_parser->current_token && main_parser->current_token->type == TOKEN_SEMICOLON) { // Add NULL check
        eat(main_parser, TOKEN_SEMICOLON); // Frees ';'
    } else { // Error if missing semicolon (and not EOF)
         if (main_parser->current_token) {
              errorParser(main_parser, "Expected ';' after program header");
         } else {
              errorParser(main_parser, "Unexpected end of file after program header");
         }
         freeToken(copiedProgToken); // Free the copy before returning error node
         // Need to free prog_name_node too
         freeAST(prog_name_node);
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
                // Cleanup needed
                freeToken(copiedProgToken);
                freeAST(prog_name_node);
                if (uses_clause) freeAST(uses_clause); // Might cause issues if partially built
                if (unit_list) freeList(unit_list);
                return newASTNode(AST_NOOP, NULL);
            }
            // listAppend uses strdup internally
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
             freeAST(prog_name_node);
             if (uses_clause) freeAST(uses_clause); // Might cause issues
             if (unit_list) freeList(unit_list);
             return newASTNode(AST_NOOP, NULL);
        }

        // Attach list to uses_clause node
        uses_clause->unit_list = unit_list; // unit_list now owned by uses_clause

        // --- Start: Process and Link Units from 'uses' list ---
        for (int i = 0; i < listSize(unit_list); i++) {
            char *unit_name = listGet(unit_list, i);
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG USES] Processing unit '%s'...\n", unit_name);
            #endif
            // Attempt to find the unit source file
            char *nested_unit_path = findUnitFile(unit_name); // Assumes findUnitFile returns allocated string or NULL

            // --- MODIFICATION START: Add Check After findUnitFile ---
            if (nested_unit_path == NULL) {
                // Use errorParser for consistency
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg), "Unit '%s' specified in USES clause not found (findUnitFile returned NULL).", unit_name);
                errorParser(main_parser, error_msg); // errorParser should exit
                // If errorParser doesn't exit, ensure cleanup happens here.
                // uses_clause owns unit_list, freeing uses_clause handles both
                if(uses_clause) freeAST(uses_clause);
                freeAST(prog_name_node);
                freeToken(copiedProgToken);
                return NULL; // Indicate failure
            }
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG USES] Found unit '%s' at path: %s (ptr: %p)\n", unit_name, nested_unit_path, (void*)nested_unit_path);
            #endif
            // --- MODIFICATION END ---


            // --- Parse the found unit file ---
            // 1. Read the unit file content
            FILE *unit_file = fopen(nested_unit_path, "r");
            if (!unit_file) {
                 char error_msg[512];
                 snprintf(error_msg, sizeof(error_msg), "Could not open unit file '%s' for unit '%s'", nested_unit_path, unit_name);
                 perror(error_msg); // Print system error message too
                 free(nested_unit_path); // Free path returned by findUnitFile
                 // Cleanup...
                 if(uses_clause) freeAST(uses_clause);
                 freeAST(prog_name_node);
                 freeToken(copiedProgToken);
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
                free(nested_unit_path);
                // Cleanup...
                 if(uses_clause) freeAST(uses_clause);
                 freeAST(prog_name_node);
                 freeToken(copiedProgToken);
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
            Parser unit_parser;
            unit_parser.lexer = &unit_lexer;
            unit_parser.current_token = getNextToken(&unit_lexer); // Initialize the first token

            // 3. Parse the unit recursively
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG USES] >>> Calling unitParser for '%s'...\n", unit_name);
            #endif
            AST *unit_ast = unitParser(&unit_parser, 1); // Pass unit_parser, start depth 1
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG USES] <<< Returned from unitParser for '%s'. AST node: %p\n", unit_name, (void*)unit_ast);
            #endif
            if (!unit_ast) {
                // unitParser should have reported an error via errorParser
                fprintf(stderr, "Error: Failed to parse unit '%s'.\n", unit_name);
                free(unit_source);
                free(nested_unit_path);
                // Cleanup...
                 if(uses_clause) freeAST(uses_clause);
                 freeAST(prog_name_node);
                 freeToken(copiedProgToken);
                 EXIT_FAILURE_HANDLER();
                 return NULL;
            }
            // ---

            // 4. Link symbols from the unit's interface into the global scope
            #ifdef DEBUG
             fprintf(stderr, "[DEBUG USES] Building symbol table for unit '%s'...\n", unit_name);
            #endif
             // Build symbol table from interface (left child holds interface decls)
             Symbol* unitSymTable = NULL;
             if (unit_ast->left) { // Check if interface node exists
                 unitSymTable = buildUnitSymbolTable(unit_ast->left);
                 unit_ast->symbol_table = unitSymTable; // Attach symbol table to unit AST node
             } else {
                  fprintf(stderr,"Warning: No interface declarations found for unit '%s' to build symbol table.\n", unit_name);
             }

            #ifdef DEBUG
             fprintf(stderr, "[DEBUG USES] Linking unit '%s'...\n", unit_name);
            #endif
            linkUnit(unit_ast, 1); // Call linkUnit with the parsed unit AST


            // 5. Cleanup resources for this unit
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG USES] Freeing nested_source at %p for unit '%s'\n", (void*)unit_source, unit_name);
             #endif
             free(unit_source); // Free the source buffer first
             unit_source = NULL; // Prevent double free


            // --- MODIFICATION START: Add Check Before free(nested_unit_path) ---
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG USES] Attempting to free nested_unit_path (ptr: %p) for unit '%s'\n", (void*)nested_unit_path, unit_name);
             if (nested_unit_path == NULL) {
                  fprintf(stderr, "[DEBUG USES] *** CRITICAL: nested_unit_path IS NULL before free()! ***\n");
                  // Optionally trigger debugger or dump stack here if needed
                  // For now, the check below will prevent the crash by skipping free(NULL)
             }
             #endif
             // ---

             // --- Original free call that might crash, now guarded ---
             if (nested_unit_path != NULL) {
                 free(nested_unit_path); // Free the path string
             } else {
                 // Log if it was NULL
                 fprintf(stderr, "Warning: nested_unit_path was NULL before free in buildProgramAST loop for unit '%s'.\n", unit_name);
             }
             nested_unit_path = NULL; // Prevent potential double-free within the loop

             #ifdef DEBUG
             fprintf(stderr, "[DEBUG USES] Successfully processed free for nested_unit_path for unit '%s'\n", unit_name);
             #endif
             // --- MODIFICATION END ---


             // Do NOT free unit_ast here if its symbols/types are now linked globally

        } // End for loop processing unit_list
        // --- End: Process and Link Units ---
    } // End if (uses_clause)

    // Parse the main block
    AST *block_node = block(main_parser); // block handles its internal tokens
    if (!block_node) { // block returns NULL on error
        // errorParser(main_parser, "Failed to parse main program block"); // Error reported in block()
        freeToken(copiedProgToken);
        freeAST(prog_name_node);
        if (uses_clause) freeAST(uses_clause);
        return newASTNode(AST_NOOP, NULL);
    }

    // Expect final '.'
    if (main_parser->current_token && main_parser->current_token->type == TOKEN_PERIOD) {
         eat(main_parser, TOKEN_PERIOD); // Frees '.'
    } else { // Error if not period or EOF
         errorParser(main_parser, "Expected '.' at end of program");
         freeToken(copiedProgToken);
         freeAST(prog_name_node);
         if (uses_clause) freeAST(uses_clause);
         freeAST(block_node);
         return newASTNode(AST_NOOP, NULL);
    }


    // --- Create the main PROGRAM node using the COPIED program token ---
    // newASTNode makes its own internal copy of the token passed to it
    AST *programNode = newASTNode(AST_PROGRAM, copiedProgToken);
    // ---

    setLeft(programNode, prog_name_node); // prog_name_node already built
    setRight(programNode, block_node);    // block_node already built
    if (block_node) block_node->is_global_scope = true; // Mark main block as global

    // Add uses_clause node if it was created
    // NOTE: uses_clause was already added as a child to unit_node in the uses loop,
    //       this might be redundant or incorrect depending on desired final AST structure.
    //       If uses_clause should be a direct child of PROGRAM, modify the uses loop logic.
    //       For now, assuming uses_clause memory is managed via unit_node which becomes part of programNode?
    //       Let's add it directly to programNode IF it wasn't added elsewhere.
    //       Revisiting the structure: uses_clause is local to this function and *not* added elsewhere.
    //       So, it should be added as a child to the programNode.
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
        setTypeAST(node, TYPE_POINTER);
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
             Procedure *proc = node->token ? lookupProcedure(node->token->value) : NULL;
             if (proc && proc->proc_decl && proc->proc_decl->type == AST_FUNCTION_DECL) {
                 node->type = AST_PROCEDURE_CALL; // Change type to parameter-less call
                 node->children = NULL; node->child_count = 0; node->child_capacity = 0;
                 // Set return type early if possible
                 if (proc->proc_decl->right) setTypeAST(node, proc->proc_decl->right->var_type);
                 else setTypeAST(node, TYPE_VOID);
             } else if (proc) { // Procedure used as value error
                  char error_msg[128]; snprintf(error_msg, sizeof(error_msg), "Procedure '%s' cannot be used as a value", node->token->value); errorParser(parser, error_msg); freeAST(node); return newASTNode(AST_NOOP, NULL);
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

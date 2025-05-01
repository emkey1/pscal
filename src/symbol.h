//
//  symbol.h
//  pscal
//
//  Created by Michael Miller on 3/25/25.
//

#ifndef symbol_h
#define symbol_h

#include <stdio.h>
#include "globals.h"


typedef struct Symbol {
    char *name;
    VarType type;
    Value *value;
    bool is_alias;  // indicates value is not owned by this symbol
    bool is_local_var;
    bool is_const;
    AST *type_def;
    struct Symbol *next;
} Symbol;

Symbol *lookupSymbol(const char *name);
Symbol *lookupGlobalSymbol(const char *name);
Symbol *lookupLocalSymbol(const char *name);
void updateSymbol(const char *name, Value val);
Symbol *lookupSymbolIn(Symbol *env, const char *name);
void insertGlobalSymbol(const char *name, VarType type, AST *type_def);
Symbol *insertLocalSymbol(const char *name, VarType type, AST* type_def, bool is_variable_declaration);

bool hasSeenGlobal(const char *name);
void markGlobalAsSeen(const char *name);
Procedure *getProcedureTable(void);
void assignToRecord(FieldValue *record, const char *fieldName, Value val);

#endif /* symbol_h */

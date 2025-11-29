#pragma once

#include <stdbool.h>
#include <stdatomic.h>

#include "Pascal/globals.h"
#include "symbol/symbol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HashTable *globalSymbols;
    HashTable *constGlobalSymbols;
    HashTable *localSymbols;
    Symbol *current_function_symbol;
    HashTable *procedure_table;
    HashTable *current_procedure_table;
    TypeEntry *type_table;
    int gCurrentTextColor;
    int gCurrentTextBackground;
    bool gCurrentTextBold;
    bool gCurrentColorIsExt;
    bool gCurrentBgIsExt;
    bool gCurrentTextUnderline;
    bool gCurrentTextBlink;
    bool gConsoleAttrDirty;
    bool gConsoleAttrDirtyFromReset;
    bool gTextAttrInitialized;
    int gWindowLeft;
    int gWindowTop;
    int gWindowRight;
    int gWindowBottom;
    int last_io_error;
    int typeWarn;
    int gSuppressWriteSpacing;
    int gUppercaseBooleans;
    int pascal_semantic_error_count;
    int pascal_parser_error_count;
    int break_requested_value;
    int exit_requested_value;
#ifdef DEBUG
    List *inserted_global_names;
    int dumpExec;
#endif
} PascalGlobalState;

void pascalPushGlobalState(PascalGlobalState *state);
void pascalPopGlobalState(PascalGlobalState *state);
void pascalInvalidateGlobalState(void);

#ifdef __cplusplus
}
#endif

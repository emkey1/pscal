#include "common/pascal_state.h"
#include "compiler/compiler.h"

#include <string.h>

#include "core/list.h"
#include "core/utils.h"
#include "ast/ast.h"

static void restoreHashTable(HashTable **target, HashTable *previous) {
    if (*target && *target != previous) {
        freeHashTable(*target);
    }
    *target = previous;
}

void pascalPushGlobalState(PascalGlobalState *state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));

    state->globalSymbols = globalSymbols;
    state->constGlobalSymbols = constGlobalSymbols;
    state->localSymbols = localSymbols;
    state->current_function_symbol = current_function_symbol;
    state->procedure_table = procedure_table;
    state->current_procedure_table = current_procedure_table;
    state->type_table = type_table;

    globalSymbols = NULL;
    constGlobalSymbols = NULL;
    localSymbols = NULL;
    current_function_symbol = NULL;
    procedure_table = NULL;
    current_procedure_table = NULL;
    type_table = NULL;

    state->gCurrentTextColor = gCurrentTextColor;
    state->gCurrentTextBackground = gCurrentTextBackground;
    state->gCurrentTextBold = gCurrentTextBold;
    state->gCurrentColorIsExt = gCurrentColorIsExt;
    state->gCurrentBgIsExt = gCurrentBgIsExt;
    state->gCurrentTextUnderline = gCurrentTextUnderline;
    state->gCurrentTextBlink = gCurrentTextBlink;
    state->gConsoleAttrDirty = gConsoleAttrDirty;
    state->gConsoleAttrDirtyFromReset = gConsoleAttrDirtyFromReset;
    state->gTextAttrInitialized = gTextAttrInitialized;
    state->gWindowLeft = gWindowLeft;
    state->gWindowTop = gWindowTop;
    state->gWindowRight = gWindowRight;
    state->gWindowBottom = gWindowBottom;

    gCurrentTextColor = 7;
    gCurrentTextBackground = 0;
    gCurrentTextBold = false;
    gCurrentColorIsExt = false;
    gCurrentBgIsExt = false;
    gCurrentTextUnderline = false;
    gCurrentTextBlink = false;
    gConsoleAttrDirty = false;
    gConsoleAttrDirtyFromReset = false;
    gTextAttrInitialized = false;
    gWindowLeft = 1;
    gWindowTop = 1;
    gWindowRight = 80;
    gWindowBottom = 24;

    state->last_io_error = last_io_error;
    state->typeWarn = typeWarn;
    state->gSuppressWriteSpacing = gSuppressWriteSpacing;
    state->gUppercaseBooleans = gUppercaseBooleans;
    state->pascal_semantic_error_count = pascal_semantic_error_count;
    state->pascal_parser_error_count = pascal_parser_error_count;
    state->break_requested_value = atomic_load(&break_requested);
    state->exit_requested_value = exit_requested;

    last_io_error = 0;
    typeWarn = 1;
    gSuppressWriteSpacing = 0;
    gUppercaseBooleans = 0;
    pascal_semantic_error_count = 0;
    pascal_parser_error_count = 0;
    atomic_store(&break_requested, 0);
    exit_requested = 0;

#ifdef DEBUG
    state->inserted_global_names = inserted_global_names;
    state->dumpExec = dumpExec;
    inserted_global_names = NULL;
    dumpExec = 1;
#endif
}

void pascalPopGlobalState(PascalGlobalState *state) {
    if (!state) {
        return;
    }

    restoreHashTable(&globalSymbols, state->globalSymbols);
    restoreHashTable(&constGlobalSymbols, state->constGlobalSymbols);
    restoreHashTable(&localSymbols, state->localSymbols);
    restoreHashTable(&procedure_table, state->procedure_table);
    restoreHashTable(&current_procedure_table, state->current_procedure_table);
    current_function_symbol = state->current_function_symbol;

    if (type_table && type_table != state->type_table) {
        freeTypeTableASTNodes();
        freeTypeTable();
    }
    type_table = state->type_table;

    gCurrentTextColor = state->gCurrentTextColor;
    gCurrentTextBackground = state->gCurrentTextBackground;
    gCurrentTextBold = state->gCurrentTextBold;
    gCurrentColorIsExt = state->gCurrentColorIsExt;
    gCurrentBgIsExt = state->gCurrentBgIsExt;
    gCurrentTextUnderline = state->gCurrentTextUnderline;
    gCurrentTextBlink = state->gCurrentTextBlink;
    gConsoleAttrDirty = state->gConsoleAttrDirty;
    gConsoleAttrDirtyFromReset = state->gConsoleAttrDirtyFromReset;
    gTextAttrInitialized = state->gTextAttrInitialized;
    gWindowLeft = state->gWindowLeft;
    gWindowTop = state->gWindowTop;
    gWindowRight = state->gWindowRight;
    gWindowBottom = state->gWindowBottom;

    last_io_error = state->last_io_error;
    typeWarn = state->typeWarn;
    gSuppressWriteSpacing = state->gSuppressWriteSpacing;
    gUppercaseBooleans = state->gUppercaseBooleans;
    pascal_semantic_error_count = state->pascal_semantic_error_count;
    pascal_parser_error_count = state->pascal_parser_error_count;
    atomic_store(&break_requested, state->break_requested_value);
    exit_requested = state->exit_requested_value;

#ifdef DEBUG
    if (inserted_global_names && inserted_global_names != state->inserted_global_names) {
        freeList(inserted_global_names);
    }
    inserted_global_names = state->inserted_global_names;
    dumpExec = state->dumpExec;
#endif
}

void pascalInvalidateGlobalState(void) {
    globalSymbols = NULL;
    constGlobalSymbols = NULL;
    localSymbols = NULL;
    procedure_table = NULL;
    current_procedure_table = NULL;
    current_function_symbol = NULL;
    type_table = NULL;
#ifdef DEBUG
    inserted_global_names = NULL;
#endif
    compilerResetState();
}

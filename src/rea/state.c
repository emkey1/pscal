#include "rea/state.h"

#include "Pascal/globals.h"
#include "ast/ast.h"
#include "compiler/compiler.h"
#include "core/utils.h"
#include "rea/parser.h"
#include "rea/semantic.h"
#include "symbol/symbol.h"

static void reaResetParserState(void) {
    reaSetStrictMode(0);
}

void reaResetSymbolState(void) {
    if (globalSymbols) {
        freeHashTable(globalSymbols);
        globalSymbols = NULL;
    }
    if (constGlobalSymbols) {
        freeHashTable(constGlobalSymbols);
        constGlobalSymbols = NULL;
    }
    if (procedure_table) {
        freeHashTable(procedure_table);
        procedure_table = NULL;
    }
    current_procedure_table = NULL;
    if (type_table) {
        freeTypeTableASTNodes();
        freeTypeTable();
        type_table = NULL;
    }
    pascal_semantic_error_count = 0;
    pascal_parser_error_count = 0;
}

void reaInvalidateGlobalState(void) {
    reaResetParserState();
    reaSemanticResetState();
    reaResetSymbolState();
    compilerResetState();
}

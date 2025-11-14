#include "clike/state.h"

#include "clike/codegen.h"
#include "clike/errors.h"
#include "clike/parser.h"
#include "clike/semantics.h"
#include "Pascal/globals.h"
#include "ast/ast.h"
#include "compiler/compiler.h"
#include "core/utils.h"
#include "symbol/symbol.h"

void clikeResetSymbolState(void) {
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
}

void clikeInvalidateGlobalState(void) {
    clikeResetParserState();
    clikeResetSemanticsState();
    clikeResetCodegenState();
    clikeResetSymbolState();
    clike_error_count = 0;
    clike_warning_count = 0;
    compilerResetState();
}

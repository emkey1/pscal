// Minimal loader for AST JSON produced by dumpASTJSON
#ifndef AST_JSON_LOADER_H
#define AST_JSON_LOADER_H

#include "ast/ast.h"

// Parse an AST from a JSON string produced by dumpASTJSON.
// Returns NULL on parse error.
AST* loadASTFromJSON(const char* json_text);

#endif // AST_JSON_LOADER_H


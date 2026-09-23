// Minimal loader for AST JSON produced by dumpASTJSON
#ifndef AST_JSON_LOADER_H
#define AST_JSON_LOADER_H

#include "ast/ast.h"

// Parse an AST from a JSON string produced by dumpASTJSON.
// Returns NULL on parse error.
AST* loadASTFromJSON(const char* json_text);

// Parse an AST from JSON exactly as written. loadASTFromJSON reshapes what it
// reads into what pscaljson2bc's compiler expects (a clike VAR_DECL's name, a
// function body stored on the right, `exit` as `halt`, doubly escaped strings,
// a PROGRAM without a block); this applies none of that. pscalasm uses it for
// the ASTs pscald writes, which carry every field the bytecode cache stores.
AST* loadASTFromJSONExact(const char* json_text);

#endif // AST_JSON_LOADER_H


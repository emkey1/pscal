#pragma once

#include "Pascal/globals.h"
#include "ast/ast.h"

TypeEntry *findTypeEntry(const char *name);
void reserveTypePlaceholder(const char *name, VarType kind);
void insertType(const char *name, AST *typeAST);
AST *lookupType(const char *name);

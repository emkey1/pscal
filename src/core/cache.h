#ifndef PSCAL_CACHE_H
#define PSCAL_CACHE_H

#include <stdbool.h>
#include "compiler/bytecode.h"
#include "symbol/symbol.h"

bool loadBytecodeFromCache(const char* source_path, BytecodeChunk* chunk, HashTable* procedure_table);
void saveBytecodeToCache(const char* source_path, const BytecodeChunk* chunk, HashTable* procedure_table);

#endif // PSCAL_CACHE_H

#ifndef PSCAL_CACHE_H
#define PSCAL_CACHE_H

#include <stdbool.h>
#include "compiler/bytecode.h"

bool loadBytecodeFromCache(const char* source_path, BytecodeChunk* chunk);
void saveBytecodeToCache(const char* source_path, const BytecodeChunk* chunk);
bool loadBytecodeFromFile(const char* file_path, BytecodeChunk* chunk);

#endif // PSCAL_CACHE_H

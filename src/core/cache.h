#ifndef PSCAL_CACHE_H
#define PSCAL_CACHE_H

#include <stdbool.h>
#include "compiler/bytecode.h"

bool loadBytecodeFromCache(const char* source_path,
                           const char* frontend_path,
                           const char** dependencies,
                           int dep_count,
                           BytecodeChunk* chunk);
void saveBytecodeToCache(const char* source_path, const BytecodeChunk* chunk);
bool saveBytecodeToFile(const char* file_path, const char* source_path, const BytecodeChunk* chunk);
bool loadBytecodeFromFile(const char* file_path, BytecodeChunk* chunk);

// Build the canonical path for the cache file corresponding to a source path.
// Caller is responsible for freeing the returned string.
char* buildCachePath(const char* source_path);

#endif // PSCAL_CACHE_H

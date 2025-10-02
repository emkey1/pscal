#ifndef EXT_BUILTIN_DUMP_H
#define EXT_BUILTIN_DUMP_H

#include <stdio.h>

/*
 * Write a machine-readable listing of the registered extended builtin
 * categories, their groups, and their functions.  The output format is a
 * series of lines in the form:
 *   category <name>
 *   group <category> <group>
 *   function <category> <group> <name>
 * The listing is stable across front ends so regression harnesses can parse
 * it regardless of which interpreter produced the data.
 */
void extBuiltinDumpInventory(FILE *out);

#endif /* EXT_BUILTIN_DUMP_H */

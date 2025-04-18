// globals.c
#include "globals.h"

int last_io_error = 0;
int typeWarn = 1;

Symbol *globalSymbols = NULL;
Symbol *localSymbols = NULL;
Symbol *current_function_symbol = NULL;
Procedure *procedure_table = NULL;
TypeEntry *type_table = NULL;

int break_requested = 0;


#ifdef DEBUG
int dumpExec = 1;  // Set to 1 by default in debug mode
#endif

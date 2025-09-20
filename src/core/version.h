#ifndef PSCAL_VERSION_H
#define PSCAL_VERSION_H

#define PSCAL_VM_VERSION 7  // Bump for expanded locals metadata in cache format

#include <stdint.h>

// Retrieve the VM bytecode version at runtime. Using a function rather than
// the preprocessor constant ensures binaries pick up version changes even if
// their object files aren't rebuilt.
uint32_t pscal_vm_version(void);

#endif // PSCAL_VERSION_H

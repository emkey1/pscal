#ifndef PSCAL_AETHER_STATE_H
#define PSCAL_AETHER_STATE_H

#include "common/frontend_kind.h"

void aetherResetSymbolState(void);
void aetherInvalidateGlobalState(void);

#if defined(PSCAL_FRONTEND_KIND) && PSCAL_FRONTEND_KIND == FRONTEND_KIND_AETHER
void aetherSetVerboseCompatibilityDiagnostics(int enable);
int aetherGetVerboseCompatibilityDiagnostics(void);
#else
static inline void aetherSetVerboseCompatibilityDiagnostics(int enable) {
    (void)enable;
}

static inline int aetherGetVerboseCompatibilityDiagnostics(void) {
    return 0;
}
#endif

#endif

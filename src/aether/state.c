#include "aether/state.h"

#include "aether/parser.h"
#include "rea/state.h"

void aetherResetSymbolState(void) {
    reaResetSymbolState();
}

void aetherInvalidateGlobalState(void) {
    aetherClearLastSource();
    reaInvalidateGlobalState();
}

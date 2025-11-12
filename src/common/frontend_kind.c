#include "common/frontend_kind.h"

static FrontendKind gFrontendKind = FRONTEND_KIND_PASCAL;

FrontendKind frontendPushKind(FrontendKind kind) {
    FrontendKind previous = gFrontendKind;
    gFrontendKind = kind;
    return previous;
}

void frontendPopKind(FrontendKind previous) {
    gFrontendKind = previous;
}

FrontendKind frontendGetKind(void) {
    return gFrontendKind;
}

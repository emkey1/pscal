#pragma once

#include <stdbool.h>

typedef enum FrontendKind {
    FRONTEND_KIND_UNKNOWN = 0,
    FRONTEND_KIND_PASCAL,
    FRONTEND_KIND_REA,
    FRONTEND_KIND_CLIKE,
    FRONTEND_KIND_SHELL
} FrontendKind;

FrontendKind frontendPushKind(FrontendKind kind);
void frontendPopKind(FrontendKind previous);
FrontendKind frontendGetKind(void);

static inline bool frontendIsPascal(void) {
    FrontendKind kind = frontendGetKind();
    return kind == FRONTEND_KIND_PASCAL || kind == FRONTEND_KIND_UNKNOWN;
}

static inline bool frontendIsRea(void) {
    return frontendGetKind() == FRONTEND_KIND_REA;
}

static inline bool frontendIsClike(void) {
    return frontendGetKind() == FRONTEND_KIND_CLIKE;
}

static inline bool frontendIsShell(void) {
    return frontendGetKind() == FRONTEND_KIND_SHELL;
}

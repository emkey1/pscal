#include "aether/diagnostics.h"

#include <string.h>

const char *aetherInferDiagnosticCode(const char *kind, const char *detail) {
    if (kind) {
        if (strcmp(kind, "parser") == 0) {
            return "SYN-001";
        }
        if (strcmp(kind, "compatibility") == 0 && detail) {
            if (strstr(detail, "let mut") || strstr(detail, "`mut` is ignored")) {
                return "MUT-001";
            }
            if (strstr(detail, "missing import") || strstr(detail, "module")) {
                return "IMP-001";
            }
        }
        if (strcmp(kind, "effect") == 0) {
            return "FX-001";
        }
        if (strcmp(kind, "purity") == 0 || strcmp(kind, "contract") == 0) {
            return "ANN-001";
        }
        if (strcmp(kind, "import") == 0) {
            return "IMP-001";
        }
        if (strcmp(kind, "scope") == 0) {
            return "SCOPE-001";
        }
    }

    if (!detail) {
        return NULL;
    }

    if (strstr(detail, "Unexpected token")) {
        return "SYN-001";
    }
    if (strstr(detail, "explicit return type")) {
        return "SYN-001";
    }
    if (strstr(detail, "type fields must end with ';', not ','")) {
        return "SYN-001";
    }
    if (strstr(detail, "cannot infer the type of")) {
        return "TYPE-001";
    }
    if (strstr(detail, "@pre must annotate the next function declaration") ||
        strstr(detail, "@post must annotate the next function declaration") ||
        strstr(detail, "cannot call effectful builtin") ||
        strstr(detail, "cannot call non-pure function")) {
        return "ANN-001";
    }
    if (strstr(detail, "tuple return types are not supported yet")) {
        return "TUP-001";
    }
    if (strstr(detail, "opaque TOON handle") ||
        strstr(detail, "expects a ToonDoc handle") ||
        strstr(detail, "expects a ToonNode handle") ||
        strstr(detail, " first argument") ||
        strstr(detail, " second argument") ||
        strstr(detail, " third argument")) {
        return "TOON-001";
    }
    if (strstr(detail, "requires an fx block")) {
        return "FX-001";
    }
    if (strstr(detail, "unable to open module")) {
        return "IMP-001";
    }
    if (strstr(detail, "not in scope")) {
        return "SCOPE-001";
    }
    if (strstr(detail, "Unknown field")) {
        return "FIELD-002";
    }
    if (strstr(detail, "fallthrough path with no return value")) {
        return "FLOW-001";
    }
    if (strstr(detail, "let mut") || strstr(detail, "`mut` is ignored")) {
        return "MUT-001";
    }

    return NULL;
}

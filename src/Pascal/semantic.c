#include "Pascal/semantic.h"

#include "Pascal/globals.h"
#include "Pascal/type_registry.h"
#include "ast/closure_registry.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static AST *gProgramRoot = NULL;
static ClosureCaptureRegistry gClosureRegistry;
static bool gRegistryInitialized = false;

static AST *resolveTypeAliasLocal(AST *type_node) {
    if (!type_node) {
        return NULL;
    }

    while (type_node) {
        if ((type_node->type == AST_TYPE_REFERENCE || type_node->type == AST_VARIABLE) &&
            type_node->token && type_node->token->value) {
            AST *resolved = lookupType(type_node->token->value);
            if (!resolved || resolved == type_node) {
                break;
            }
            type_node = resolved;
            continue;
        }

        if (type_node->type == AST_TYPE_DECL && type_node->left) {
            type_node = type_node->left;
            continue;
        }

        break;
    }

    return type_node;
}

static bool astIsInterface(AST *node) {
    AST *resolved = resolveTypeAliasLocal(node);
    return resolved && resolved->type == AST_INTERFACE;
}

static bool astIsRecord(AST *node) {
    AST *resolved = resolveTypeAliasLocal(node);
    return resolved && resolved->type == AST_RECORD_TYPE;
}

static AST *findRecordMethod(AST *recordType, const char *name) {
    if (!recordType || recordType->type != AST_RECORD_TYPE || !name) {
        return NULL;
    }

    for (int i = 0; i < recordType->child_count; i++) {
        AST *child = recordType->children[i];
        if (!child) {
            continue;
        }
        if ((child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) &&
            child->token && child->token->value &&
            strcasecmp(child->token->value, name) == 0) {
            return child;
        }
    }

    return NULL;
}

static void markRecordAgainstInterface(AST *recordType, AST *interfaceType);

static void markRecordAgainstInterfaceList(AST *recordType, AST *baseList) {
    if (!baseList) {
        return;
    }

    if (baseList->type == AST_LIST) {
        for (int i = 0; i < baseList->child_count; i++) {
            markRecordAgainstInterface(recordType, baseList->children[i]);
        }
    } else {
        markRecordAgainstInterface(recordType, baseList);
    }
}

static void markRecordAgainstInterface(AST *recordType, AST *interfaceType) {
    AST *resolvedInterface = resolveTypeAliasLocal(interfaceType);
    if (!resolvedInterface || resolvedInterface->type != AST_INTERFACE) {
        return;
    }

    if (resolvedInterface->extra) {
        markRecordAgainstInterfaceList(recordType, resolvedInterface->extra);
    }

    for (int i = 0; i < resolvedInterface->child_count; i++) {
        AST *method = resolvedInterface->children[i];
        if (!method || (method->type != AST_PROCEDURE_DECL && method->type != AST_FUNCTION_DECL)) {
            continue;
        }
        if (!method->token || !method->token->value) {
            continue;
        }
        AST *recordMethod = findRecordMethod(recordType, method->token->value);
        if (recordMethod) {
            recordMethod->is_virtual = true;
        }
    }
}

static void markVirtualMethodsForInterfaces(void) {
    for (TypeEntry *recordEntry = type_table; recordEntry; recordEntry = recordEntry->next) {
        AST *recordNode = resolveTypeAliasLocal(recordEntry->typeAST);
        if (!astIsRecord(recordNode)) {
            continue;
        }

        for (TypeEntry *ifaceEntry = type_table; ifaceEntry; ifaceEntry = ifaceEntry->next) {
            AST *ifaceNode = ifaceEntry->typeAST;
            if (!astIsInterface(ifaceNode)) {
                continue;
            }
            markRecordAgainstInterface(recordNode, ifaceNode);
        }
    }
}

typedef struct {
    AST *declaration;
    bool is_by_ref;
} CaptureInfo;

typedef struct {
    CaptureInfo *items;
    size_t count;
    size_t capacity;
} CaptureCollection;

#define MAX_CLOSURE_CAPTURES (sizeof(((Symbol *)0)->upvalues) / sizeof(((Symbol *)0)->upvalues[0]))

static void captureCollectionInit(CaptureCollection *collection) {
    if (!collection) {
        return;
    }
    collection->items = NULL;
    collection->count = 0;
    collection->capacity = 0;
}

static void captureCollectionFree(CaptureCollection *collection) {
    if (!collection) {
        return;
    }
    free(collection->items);
    collection->items = NULL;
    collection->count = 0;
    collection->capacity = 0;
}

static bool captureCollectionAdd(CaptureCollection *collection, AST *decl, bool is_by_ref) {
    if (!collection || !decl) {
        return false;
    }

    for (size_t i = 0; i < collection->count; i++) {
        if (collection->items[i].declaration == decl) {
            if (is_by_ref) {
                collection->items[i].is_by_ref = true;
            }
            return true;
        }
    }

    if (collection->count >= MAX_CLOSURE_CAPTURES) {
        return false;
    }

    if (collection->count == collection->capacity) {
        size_t new_capacity = collection->capacity ? collection->capacity * 2 : 8;
        if (new_capacity > MAX_CLOSURE_CAPTURES) {
            new_capacity = MAX_CLOSURE_CAPTURES;
        }
        CaptureInfo *resized = (CaptureInfo *)realloc(collection->items, new_capacity * sizeof(CaptureInfo));
        if (!resized) {
            return false;
        }
        collection->items = resized;
        collection->capacity = new_capacity;
    }

    collection->items[collection->count].declaration = decl;
    collection->items[collection->count].is_by_ref = is_by_ref;
    collection->count++;
    return true;
}

static void ensureRegistry(void) {
    if (!gRegistryInitialized) {
        closureRegistryInit(&gClosureRegistry);
        gRegistryInitialized = true;
    }
    closureRegistryReset(&gClosureRegistry);
}

static AST *findEnclosingFunction(AST *node) {
    if (!node) {
        return NULL;
    }
    AST *curr = node->parent;
    while (curr) {
        if (curr->type == AST_FUNCTION_DECL || curr->type == AST_PROCEDURE_DECL) {
            return curr;
        }
        curr = curr->parent;
    }
    return NULL;
}

static AST *getRoutineBody(AST *routine) {
    if (!routine) {
        return NULL;
    }
    if (routine->type == AST_FUNCTION_DECL) {
        return routine->extra;
    }
    if (routine->type == AST_PROCEDURE_DECL) {
        return routine->right;
    }
    return NULL;
}

static void collectCapturesVisitor(AST *node, AST *func, CaptureCollection *captures) {
    if (!node || !func || !captures) {
        return;
    }
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        return;
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        const char *name = node->token->value;
        AST *decl = findStaticDeclarationInAST(name, node, gProgramRoot);
        if (decl && decl->type == AST_VAR_DECL) {
            AST *owner = findEnclosingFunction(decl);
            if (owner && owner != func) {
                bool is_by_ref = decl->by_ref != 0;
                captureCollectionAdd(captures, decl, is_by_ref);
            }
        }
    }

    if (node->left) {
        collectCapturesVisitor(node->left, func, captures);
    }
    if (node->right) {
        collectCapturesVisitor(node->right, func, captures);
    }
    if (node->extra) {
        collectCapturesVisitor(node->extra, func, captures);
    }
    for (int i = 0; i < node->child_count; i++) {
        collectCapturesVisitor(node->children[i], func, captures);
    }
}

static bool collectFunctionCaptureDescriptors(AST *func,
                                              ClosureCaptureDescriptor **out_descriptors,
                                              size_t *out_count) {
    if (out_descriptors) {
        *out_descriptors = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!func) {
        return false;
    }

    AST *body = getRoutineBody(func);
    if (!body) {
        return true;
    }

    CaptureCollection captures;
    captureCollectionInit(&captures);
    collectCapturesVisitor(body, func, &captures);

    if (captures.count == 0) {
        captureCollectionFree(&captures);
        return true;
    }

    size_t count = captures.count;
    ClosureCaptureDescriptor *descriptors = (ClosureCaptureDescriptor *)calloc(count, sizeof(ClosureCaptureDescriptor));
    if (!descriptors) {
        captureCollectionFree(&captures);
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        descriptors[i].slot_index = (uint8_t)i;
        descriptors[i].is_by_ref = captures.items[i].is_by_ref;
    }

    captureCollectionFree(&captures);

    if (out_descriptors) {
        *out_descriptors = descriptors;
    } else {
        free(descriptors);
    }
    if (out_count) {
        *out_count = count;
    }
    return true;
}

static void applyCaptureLayoutToSymbol(Symbol *sym,
                                       const ClosureCaptureDescriptor *descriptors,
                                       size_t descriptor_count) {
    if (!sym) {
        return;
    }
    size_t limit = descriptor_count;
    if (limit > MAX_CLOSURE_CAPTURES) {
        limit = MAX_CLOSURE_CAPTURES;
    }
    sym->upvalue_count = (uint8_t)limit;
    for (size_t i = 0; i < limit; i++) {
        uint8_t slot = descriptors ? descriptors[i].slot_index : (uint8_t)i;
        sym->upvalues[i].index = slot;
        sym->upvalues[i].isLocal = false;
        sym->upvalues[i].is_ref = descriptors ? descriptors[i].is_by_ref : false;
    }
    sym->closure_captures = limit > 0;
}

static Symbol *symbolForRoutine(AST *routine) {
    if (!routine || !routine->token || !routine->token->value) {
        return NULL;
    }
    char lowered[MAX_SYMBOL_LENGTH];
    strncpy(lowered, routine->token->value, MAX_SYMBOL_LENGTH - 1);
    lowered[MAX_SYMBOL_LENGTH - 1] = '\0';
    toLowerString(lowered);
    Symbol *sym = lookupProcedure(lowered);
    if (!sym) {
        sym = lookupGlobalSymbol(lowered);
    }
    return sym;
}

static void analyzeClosureCaptures(AST *node) {
    if (!node) {
        return;
    }
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        ClosureCaptureDescriptor *descriptors = NULL;
        size_t descriptor_count = 0;
        collectFunctionCaptureDescriptors(node, &descriptors, &descriptor_count);
        bool captures = descriptor_count > 0;
        closureRegistryRecord(&gClosureRegistry, node, captures, descriptors, descriptor_count, false);
        Symbol *sym = symbolForRoutine(node);
        if (sym) {
            if (captures) {
                applyCaptureLayoutToSymbol(sym, descriptors, descriptor_count);
            } else {
                sym->closure_captures = false;
                sym->upvalue_count = 0;
            }
            sym->closure_escapes = false;
        }
        free(descriptors);
    }
    if (node->left) {
        analyzeClosureCaptures(node->left);
    }
    if (node->right) {
        analyzeClosureCaptures(node->right);
    }
    if (node->extra) {
        analyzeClosureCaptures(node->extra);
    }
    for (int i = 0; i < node->child_count; i++) {
        analyzeClosureCaptures(node->children[i]);
    }
}

static void noteClosureEscape(AST *decl) {
    if (!decl) {
        return;
    }
    bool captures = closureRegistryCaptures(&gClosureRegistry, decl);
    closureRegistryRecord(&gClosureRegistry, decl, captures, NULL, 0, true);
    Symbol *sym = symbolForRoutine(decl);
    if (!sym) {
        return;
    }
    sym->closure_escapes = true;
    if (captures && sym->upvalue_count == 0) {
        size_t descriptor_count = 0;
        const ClosureCaptureDescriptor *descriptors =
            closureRegistryGetDescriptors(&gClosureRegistry, decl, &descriptor_count);
        if (descriptors && descriptor_count > 0) {
            applyCaptureLayoutToSymbol(sym, descriptors, descriptor_count);
        }
    }
}

static void checkClosureEscapes(AST *node) {
    if (!node) {
        return;
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        const char *name = node->token->value;
        AST *decl = findStaticDeclarationInAST(name, node, gProgramRoot);
        if (decl && (decl->type == AST_FUNCTION_DECL || decl->type == AST_PROCEDURE_DECL)) {
            if (closureRegistryCaptures(&gClosureRegistry, decl)) {
                AST *parent = node->parent;
                bool partOfCall = false;
                bool assigningFunctionResult = false;
                if (parent && parent->type == AST_PROCEDURE_CALL && parent->token && parent->token->value &&
                    node->token && node->token->value &&
                    strcasecmp(parent->token->value, node->token->value) == 0) {
                    partOfCall = true;
                }
                if (!partOfCall && parent && parent->type == AST_ASSIGN && parent->left == node) {
                    AST *enclosing = findEnclosingFunction(node);
                    if (enclosing == decl) {
                        assigningFunctionResult = true;
                    }
                }
                if (!partOfCall && !assigningFunctionResult) {
                    noteClosureEscape(decl);
                }
            }
        }
    }

    if (node->left) {
        checkClosureEscapes(node->left);
    }
    if (node->right) {
        checkClosureEscapes(node->right);
    }
    if (node->extra) {
        checkClosureEscapes(node->extra);
    }
    for (int i = 0; i < node->child_count; i++) {
        checkClosureEscapes(node->children[i]);
    }
}

void pascalPerformSemanticAnalysis(AST *root) {
    if (!root) {
        return;
    }

    ensureRegistry();
    gProgramRoot = root;

    markVirtualMethodsForInterfaces();

    analyzeClosureCaptures(root);
    checkClosureEscapes(root);

    if (gRegistryInitialized) {
        closureRegistryDestroy(&gClosureRegistry);
        gRegistryInitialized = false;
    }
    gProgramRoot = NULL;
}

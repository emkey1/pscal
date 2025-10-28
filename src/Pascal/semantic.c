#include "Pascal/semantic.h"

#include "Pascal/globals.h"
#include "ast/closure_registry.h"
#include <stdbool.h>
#include <stdio.h>
#include <strings.h>

static AST *gProgramRoot = NULL;
static ClosureCaptureRegistry gClosureRegistry;
static bool gRegistryInitialized = false;

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

static bool functionCapturesOuterVisitor(AST *node, AST *func) {
    if (!node || !func) {
        return false;
    }
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        return false;
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        const char *name = node->token->value;
        AST *decl = findStaticDeclarationInAST(name, node, gProgramRoot);
        if (decl && (decl->type == AST_VAR_DECL || decl->type == AST_CONST_DECL)) {
            AST *owner = findEnclosingFunction(decl);
            if (owner && owner != func) {
                return true;
            }
        }
    }

    if (node->left && functionCapturesOuterVisitor(node->left, func)) {
        return true;
    }
    if (node->right && functionCapturesOuterVisitor(node->right, func)) {
        return true;
    }
    if (node->extra && functionCapturesOuterVisitor(node->extra, func)) {
        return true;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (functionCapturesOuterVisitor(node->children[i], func)) {
            return true;
        }
    }
    return false;
}

static bool functionCapturesOuter(AST *func) {
    AST *body = getRoutineBody(func);
    if (!body) {
        return false;
    }
    return functionCapturesOuterVisitor(body, func);
}

static void analyzeClosureCaptures(AST *node) {
    if (!node) {
        return;
    }
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        bool captures = functionCapturesOuter(node);
        closureRegistryRecord(&gClosureRegistry, node, captures);
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

static void reportIllegalEscape(AST *node) {
    if (!node || !node->token) {
        return;
    }
    fprintf(stderr, "L%d: closure captures a local value that would escape its lifetime.\n",
            node->token->line);
    pascal_semantic_error_count++;
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
                    reportIllegalEscape(node);
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

    analyzeClosureCaptures(root);
    checkClosureEscapes(root);

    if (gRegistryInitialized) {
        closureRegistryDestroy(&gClosureRegistry);
        gRegistryInitialized = false;
    }
    gProgramRoot = NULL;
}

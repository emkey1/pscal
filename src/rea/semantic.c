#include "rea/semantic.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "core/types.h"
#include "core/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Forward declaration from core/utils.c
Token *newToken(TokenType type, const char *value, int line, int column);
// Needed to evaluate class constant expressions at compile time
Value evaluateCompileTimeValue(AST *node);

/* ------------------------------------------------------------------------- */
/*  Internal helpers                                                         */
/* ------------------------------------------------------------------------- */

typedef struct ClassInfo {
    char *name;               /* Class name */
    char *parent_name;        /* Parent class name (if any) */
    struct ClassInfo *parent; /* Resolved parent pointer */
    HashTable *fields;        /* Field symbol table */
    HashTable *methods;       /* Method symbol table */
} ClassInfo;

static HashTable *class_table = NULL;    /* Maps class name -> ClassInfo */
static AST *gProgramRoot = NULL;         /* Needed for declaration lookups */

static char *lowerDup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *r = (char *)malloc(len + 1);
    if (!r) return NULL;
    for (size_t i = 0; i < len; i++) r[i] = (char)tolower((unsigned char)s[i]);
    r[len] = '\0';
    return r;
}

static void lowerCopy(const char *s, char *buf) {
    size_t i = 0;
    if (!s) { buf[0] = '\0'; return; }
    for (; s[i] && i < MAX_SYMBOL_LENGTH - 1; i++) {
        buf[i] = (char)tolower((unsigned char)s[i]);
    }
    buf[i] = '\0';
}

static ClassInfo *lookupClass(const char *name) {
    if (!class_table || !name) return NULL;
    char lower[MAX_SYMBOL_LENGTH];
    lowerCopy(name, lower);
    Symbol *sym = hashTableLookup(class_table, lower);
    if (!sym || !sym->value) return NULL;
    return (ClassInfo *)sym->value->ptr_val;
}

static void insertClassInfo(ClassInfo *ci) {
    if (!class_table) class_table = createHashTable();
    if (!ci || !class_table || !ci->name) return;
    Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
    Value *v = (Value *)calloc(1, sizeof(Value));
    if (!sym || !v) { free(sym); free(v); return; }
    sym->name = lowerDup(ci->name);
    v->ptr_val = (Value *)ci;  /* store as pointer */
    sym->value = v;
    hashTableInsert(class_table, sym);
}

static void freeSymbolTable(HashTable *table, bool freeTypeDefs) {
    if (!table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = table->buckets[i];
        while (s) {
            Symbol *next = s->next;
            if (s->name) free(s->name);
            if (freeTypeDefs && s->type_def) freeAST(s->type_def);
            if (s->value) free(s->value);
            free(s);
            s = next;
        }
    }
    free(table);
}

static void freeClassTable(void) {
    if (!class_table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            Symbol *next = s->next;
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci) {
                if (ci->fields) {
                    /* Field type_defs reference the original AST; do not free them here */
                    freeSymbolTable(ci->fields, false);
                }
                if (ci->methods) freeSymbolTable(ci->methods, false);
                free(ci->parent_name);
                free(ci->name);
                free(ci);
            }
            if (s->value) free(s->value);
            if (s->name) free(s->name);
            free(s);
            s = next;
        }
    }
    free(class_table);
    class_table = NULL;
}

/* ------------------------------------------------------------------------- */
/*  Class and method collection                                             */
/* ------------------------------------------------------------------------- */

static void collectClasses(AST *node) {
    if (!node) return;
    if (node->type == AST_TYPE_DECL && node->left && node->left->type == AST_RECORD_TYPE && node->token && node->token->value) {
        ClassInfo *ci = (ClassInfo *)calloc(1, sizeof(ClassInfo));
        if (!ci) return;
        ci->name = strdup(node->token->value);
        if (node->left->extra && node->left->extra->token && node->left->extra->token->value) {
            ci->parent_name = strdup(node->left->extra->token->value);
        }
        ci->fields = createHashTable();
        ci->methods = createHashTable();
        /* Gather fields and constants */
        for (int i = 0; i < node->left->child_count; i++) {
            AST *field = node->left->children[i];
            if (!field) continue;
            const char *fname = NULL;
            AST *ftype = NULL;
            if (field->type == AST_VAR_DECL) {
                AST *var = field->child_count > 0 ? field->children[0] : NULL;
                if (!var || !var->token || !var->token->value) continue;
                fname = var->token->value;
                ftype = field->right;
            } else if (field->type == AST_CONST_DECL) {
                if (!field->token || !field->token->value) continue;
                fname = field->token->value;
                /* For const declarations, ftype may appear on the right or, if omitted,
                   the value's node carries the type information. */
                ftype = field->right ? field->right : (field->left ? field->left : NULL);
            } else {
                continue;
            }
            char *lname = lowerDup(fname);
            if (!lname) continue;
            if (hashTableLookup(ci->fields, lname)) {
                fprintf(stderr, "Duplicate field '%s' in class '%s'\n", fname, ci->name);
                pascal_semantic_error_count++;
                free(lname);
                continue;
            }
            Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
            if (!sym) { free(lname); continue; }
            sym->name = lname;
            /*
             * Store a reference to the original AST node rather than a deep copy.
             * copyAST would duplicate pointers to tokens, and freeing the copied
             * tree would free those tokens while the original AST still needs
             * them, leading to double-free or use-after-free errors.
             */
            sym->type_def = ftype;
            if (field->type == AST_CONST_DECL) {
                sym->is_const = true;
                if (field->left) {
                    Value v = evaluateCompileTimeValue(field->left);
                    if (v.type != TYPE_VOID && v.type != TYPE_UNKNOWN) {
                        Value *vp = (Value *)calloc(1, sizeof(Value));
                        if (vp) {
                            *vp = v; /* shallow copy; safe for numeric types */
                            sym->value = vp;
                        }
                    }
                }
            }
            hashTableInsert(ci->fields, sym);
        }
        insertClassInfo(ci);
    }
    if (node->left) collectClasses(node->left);
    if (node->right) collectClasses(node->right);
    if (node->extra) collectClasses(node->extra);
    for (int i = 0; i < node->child_count; i++) {
        collectClasses(node->children[i]);
    }
}

static void ensureSelfParam(AST *node, const char *cls) {
    if (!node || !cls) return;
    bool hasSelf = false;
    if (node->child_count > 0) {
        AST *param = node->children[0];
        if (param && param->type == AST_VAR_DECL) {
            AST *ptype = param->right;
            while (ptype && (ptype->type == AST_POINTER_TYPE || ptype->type == AST_ARRAY_TYPE)) {
                ptype = ptype->right;
            }
            if (ptype && ptype->type == AST_TYPE_REFERENCE && ptype->token && ptype->token->value &&
                strcasecmp(ptype->token->value, cls) == 0) {
                hasSelf = true;
            }
        }
    }
    if (hasSelf) return;

    Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", node->token ? node->token->line : 0, 0);
    Token *clsTok = newToken(TOKEN_IDENTIFIER, cls, node->token ? node->token->line : 0, 0);
    AST *typeRef = newASTNode(AST_TYPE_REFERENCE, clsTok);
    setTypeAST(typeRef, TYPE_RECORD);
    AST *ptrType = newASTNode(AST_POINTER_TYPE, NULL);
    setRight(ptrType, typeRef);
    setTypeAST(ptrType, TYPE_POINTER);
    AST *varDecl = newASTNode(AST_VAR_DECL, selfTok);
    setRight(varDecl, ptrType);
    setTypeAST(varDecl, TYPE_POINTER);

    addChild(node, NULL);
    for (int i = node->child_count - 1; i > 0; i--) {
        node->children[i] = node->children[i - 1];
        if (node->children[i]) node->children[i]->parent = node;
    }
    node->children[0] = varDecl;
    varDecl->parent = node;
}

static void collectMethods(AST *node) {
    if (!node) return;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) && node->token && node->token->value) {
        const char *fullname = node->token->value;
        const char *us = strchr(fullname, '_');
        if (us) {
            size_t cls_len = (size_t)(us - fullname);
            char *cls = (char *)malloc(cls_len + 1);
            if (cls) {
                memcpy(cls, fullname, cls_len);
                cls[cls_len] = '\0';
                const char *mname = us + 1;
                ClassInfo *ci = lookupClass(cls);
                if (!ci) {
                    fprintf(stderr, "Method '%s' defined for unknown class '%s'\n", mname, cls);
                    pascal_semantic_error_count++;
                } else {
                    ensureSelfParam(node, cls);
                    char *lname = lowerDup(mname);
                    if (!lname) {
                        free(cls);
                        goto recurse; /* continue traversal */
                    }
                    if (hashTableLookup(ci->methods, lname)) {
                        fprintf(stderr, "Duplicate method '%s' in class '%s'\n", mname, cls);
                        pascal_semantic_error_count++;
                        free(lname);
                    } else {
                        Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
                        Value *v = (Value *)calloc(1, sizeof(Value));
                        if (sym && v) {
                            sym->name = lname;
                            v->ptr_val = (Value *)node;
                            sym->value = v;
                            sym->type_def = node; /* reference for signature */
                            hashTableInsert(ci->methods, sym);
                            char lowerName[MAX_SYMBOL_LENGTH];
                            lowerCopy(fullname, lowerName);
                            if (!lookupProcedure(lowerName)) {
                                Symbol *ps = (Symbol *)calloc(1, sizeof(Symbol));
                                if (ps) {
                                    ps->name = strdup(lowerName);
                                    /*
                                     * Store a deep copy of the AST node in the global procedure
                                     * table.  freeProcedureTable() assumes ownership of
                                     * type_def entries and will call freeAST on them during
                                     * teardown; using the original node would result in a
                                     * double free when the program AST is cleaned up separately.
                                     */
                                    ps->type_def = copyAST(node);
                                    hashTableInsert(procedure_table, ps);
                                }
                            }
                        } else {
                            free(sym); free(v); free(lname);
                        }
                    }
                }
                free(cls);
            }
        } else if (node->parent && node->parent->type == AST_COMPOUND) {
            /* Handle un-mangled methods; examine first parameter for class type */
            AST *param = (node->child_count > 0) ? node->children[0] : NULL;
            if (!param) {
                /* Some parsers emit an explicit 'myself' VAR_DECL as a sibling
                 * preceding the procedure declaration instead of a child.  If we
                 * detect that pattern, adopt the VAR_DECL as the method's first
                 * parameter so method collection and later argument checks work
                 * as expected.
                 */
                AST *parent = node->parent;
                for (int i = 0; i < parent->child_count; i++) {
                    if (parent->children[i] == node && i > 0) {
                        AST *prev = parent->children[i - 1];
                        if (prev && prev->type == AST_VAR_DECL) {
                            addChild(node, prev);
                            for (int j = i - 1; j < parent->child_count - 1; j++) {
                                parent->children[j] = parent->children[j + 1];
                            }
                            parent->child_count--;
                            param = node->children[0];
                        }
                        break;
                    }
                }
            }
            if (param && param->type == AST_VAR_DECL) {
                AST *ptype = param->right;
                while (ptype && (ptype->type == AST_POINTER_TYPE || ptype->type == AST_ARRAY_TYPE)) {
                    ptype = ptype->right;
                }
                if (ptype && ptype->type == AST_TYPE_REFERENCE && ptype->token && ptype->token->value) {
                    const char *cls = ptype->token->value;
                    ClassInfo *ci = lookupClass(cls);
                    if (ci) {
                        size_t ln = strlen(cls) + 1 + strlen(fullname) + 1;
                        char *mangled = (char *)malloc(ln);
                        if (mangled) {
                            snprintf(mangled, ln, "%s_%s", cls, fullname);
                            free(node->token->value);
                            node->token->value = mangled;
                            node->token->length = strlen(mangled);
                            fullname = node->token->value;
                        }
                        ensureSelfParam(node, cls);
                        // Assign method index for implicitly declared methods
                        int method_index = 0;
                        if (ci->methods) {
                            for (int mb = 0; mb < HASHTABLE_SIZE; mb++) {
                                for (Symbol *ms = ci->methods->buckets[mb]; ms; ms = ms->next) {
                                    method_index++;
                                }
                            }
                        }
                        node->is_virtual = true;
                        node->i_val = method_index;
                        char *lname = lowerDup(fullname + strlen(cls) + 1);
                        if (lname) {
                            if (hashTableLookup(ci->methods, lname)) {
                                fprintf(stderr, "Duplicate method '%s' in class '%s'\n", fullname + strlen(cls) + 1, cls);
                                pascal_semantic_error_count++;
                                free(lname);
                            } else {
                                Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
                                Value *v = (Value *)calloc(1, sizeof(Value));
                                if (sym && v) {
                                    sym->name = lname;
                                    v->ptr_val = (Value *)node;
                                    sym->value = v;
                                    sym->type_def = node;
                                    hashTableInsert(ci->methods, sym);
                                    char lowerName[MAX_SYMBOL_LENGTH];
                                    lowerCopy(fullname, lowerName);
                                    Symbol *existing = lookupProcedure(lowerName);
                                    if (!existing) {
                                        Symbol *ps = (Symbol *)calloc(1, sizeof(Symbol));
                                        if (ps) {
                                            ps->name = strdup(lowerName);
                                            ps->type_def = node;
                                            hashTableInsert(procedure_table, ps);
                                            existing = ps;
                                        }
                                    } else {
                                        existing->type_def = node;
                                    }
                                    // Ensure bare method name aliases to the mangled symbol
                                    if (existing) {
                                        Symbol *alias = lookupProcedure(lname);
                                        if (alias) {
                                            alias->is_alias = true;
                                            alias->real_symbol = existing;
                                        }
                                    }
                                } else {
                                    free(sym); free(v); free(lname);
                                }
                            }
                        }
                    }
                }
            }
        } else if (node->parent && node->parent->type == AST_RECORD_TYPE) {
            AST *typeDecl = node->parent;
            while (typeDecl && typeDecl->type != AST_TYPE_DECL) typeDecl = typeDecl->parent;
            const char *cls = (typeDecl && typeDecl->token) ? typeDecl->token->value : NULL;
            if (cls) {
                ClassInfo *ci = lookupClass(cls);
                if (ci) {
                    size_t ln = strlen(cls) + 1 + strlen(fullname) + 1;
                    char *mangled = (char *)malloc(ln);
                    if (mangled) {
                        snprintf(mangled, ln, "%s_%s", cls, fullname);
                        free(node->token->value);
                        node->token->value = mangled;
                        node->token->length = strlen(mangled);
                        fullname = node->token->value;
                    }
                    ensureSelfParam(node, cls);
                    char *lname = lowerDup(fullname + strlen(cls) + 1);
                    if (lname) {
                        if (hashTableLookup(ci->methods, lname)) {
                            fprintf(stderr, "Duplicate method '%s' in class '%s'\n", fullname + strlen(cls) + 1, cls);
                            pascal_semantic_error_count++;
                            free(lname);
                        } else {
                            Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
                            Value *v = (Value *)calloc(1, sizeof(Value));
                            if (sym && v) {
                                sym->name = lname;
                                v->ptr_val = (Value *)node;
                                sym->value = v;
                                sym->type_def = node;
                                hashTableInsert(ci->methods, sym);
                                char lowerName[MAX_SYMBOL_LENGTH];
                                lowerCopy(fullname, lowerName);
                                if (!lookupProcedure(lowerName)) {
                                    Symbol *ps = (Symbol *)calloc(1, sizeof(Symbol));
                                    if (ps) {
                                        ps->name = strdup(lowerName);
                                        ps->type_def = node;
                                        hashTableInsert(procedure_table, ps);
                                    }
                                }
                            } else {
                                free(sym); free(v); free(lname);
                            }
                        }
                    }
                }
            }
        }
    }
recurse:
    if (node->left) collectMethods(node->left);
    if (node->right) collectMethods(node->right);
    if (node->extra) collectMethods(node->extra);
    for (int i = 0; i < node->child_count; i++) collectMethods(node->children[i]);
}

static void linkParents(void) {
    if (!class_table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci && ci->parent_name && !ci->parent) {
                ci->parent = lookupClass(ci->parent_name);
                if (!ci->parent) {
                    fprintf(stderr, "Unknown parent class '%s' for class '%s'\n", ci->parent_name, ci->name);
                    pascal_semantic_error_count++;
                }
            }
            s = s->next;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Override checking                                                        */
/* ------------------------------------------------------------------------- */

static bool paramTypeEquals(AST *a, AST *b) {
    if (!a || !b) return a == b;
    if (a->var_type != b->var_type) return false;
    AST *at = a->right;  /* type node */
    AST *bt = b->right;
    if (at && bt && at->token && bt->token) {
        if (strcasecmp(at->token->value, bt->token->value) != 0) return false;
    }
    return true;
}

static bool signaturesMatch(AST *a, AST *b) {
    if (!a || !b) return a == b;
    if (a->var_type != b->var_type) return false;
    if (a->child_count != b->child_count) return false;
    for (int i = 0; i < a->child_count; i++) {
        if (!paramTypeEquals(a->children[i], b->children[i])) return false;
    }
    return true;
}

static void checkOverrides(void) {
    if (!class_table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci && ci->parent) {
                for (int j = 0; j < HASHTABLE_SIZE; j++) {
                    Symbol *m = ci->methods->buckets[j];
                    while (m) {
                        ClassInfo *p = ci->parent;
                        Symbol *pm = NULL;
                        while (p && !pm) {
                            pm = hashTableLookup(p->methods, m->name);
                            p = p->parent;
                        }
                        if (pm) {
                            AST *childDecl = (AST *)m->type_def;
                            AST *parentDecl = (AST *)pm->type_def;
                            if (!signaturesMatch(childDecl, parentDecl)) {
                                fprintf(stderr, "Method '%s' in class '%s' does not properly override parent method\n", m->name, ci->name);
                                pascal_semantic_error_count++;
                            }
                        }
                        m = m->next;
                    }
                }
            }
            s = s->next;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Inherited method alias insertion                                        */
/* ------------------------------------------------------------------------- */

static void addInheritedMethodAliases(void) {
    if (!class_table || !procedure_table) return;

    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci && ci->parent) {
                ClassInfo *p = ci->parent;
                while (p) {
                    for (int j = 0; j < HASHTABLE_SIZE; j++) {
                        Symbol *m = p->methods ? p->methods->buckets[j] : NULL;
                        while (m) {
                            /* Skip if subclass defines/overrides this method */
                            if (!hashTableLookup(ci->methods, m->name)) {
                                char classLower[MAX_SYMBOL_LENGTH];
                                lowerCopy(ci->name, classLower);
                                char aliasName[MAX_SYMBOL_LENGTH * 2];
                                snprintf(aliasName, sizeof(aliasName), "%s_%s", classLower, m->name);
                                if (!hashTableLookup(procedure_table, aliasName)) {
                                    /* Find parent's fully qualified symbol */
                                    char parentLower[MAX_SYMBOL_LENGTH];
                                    lowerCopy(p->name, parentLower);
                                    char targetName[MAX_SYMBOL_LENGTH * 2];
                                    snprintf(targetName, sizeof(targetName), "%s_%s", parentLower, m->name);
                                    Symbol *target = hashTableLookup(procedure_table, targetName);
                                    if (target) {
                                        Symbol *alias = (Symbol *)calloc(1, sizeof(Symbol));
                                        if (alias) {
                                            alias->name = strdup(aliasName);
                                            alias->is_alias = true;
                                            alias->real_symbol = target->is_alias ? target->real_symbol : target;
                                            alias->type = target->type;
                                            alias->type_def = target->type_def ? copyAST(target->type_def) : NULL;
                                            if (alias->type_def && alias->type_def->token) {
                                                size_t ln = strlen(ci->name) + 1 + strlen(m->name) + 1;
                                                char *full = (char *)malloc(ln);
                                                if (full) {
                                                    snprintf(full, ln, "%s_%s", ci->name, m->name);
                                                    free(alias->type_def->token->value);
                                                    alias->type_def->token->value = full;
                                                    alias->type_def->token->length = (int)strlen(full);
                                                }
                                            }
                                            hashTableInsert(procedure_table, alias);
                                        }
                                    }
                                }
                            }
                            m = m->next;
                        }
                    }
                    p = p->parent;
                }
            }
            s = s->next;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Field/method usage checks                                                */
/* ------------------------------------------------------------------------- */

static Symbol *lookupField(ClassInfo *ci, const char *name) {
    if (!ci || !name) return NULL;
    char lower[MAX_SYMBOL_LENGTH];
    lowerCopy(name, lower);
    ClassInfo *curr = ci;
    while (curr) {
        Symbol *s = hashTableLookup(curr->fields, lower);
        if (s) return s;
        curr = curr->parent;
    }
    return NULL;
}

static Symbol *lookupMethod(ClassInfo *ci, const char *name) {
    if (!ci || !name) return NULL;
    char lower[MAX_SYMBOL_LENGTH];
    lowerCopy(name, lower);
    ClassInfo *curr = ci;
    while (curr) {
        Symbol *s = hashTableLookup(curr->methods, lower);
        if (s) return s;
        curr = curr->parent;
    }
    return NULL;
}

static const char *resolveExprClass(AST *expr, ClassInfo *currentClass) {
    if (!expr) return NULL;
    switch (expr->type) {
    case AST_VARIABLE: {
        if (!expr->token || !expr->token->value) return NULL;
        /*
         * The current object reference can appear as the implicit
         * parameter "myself".  If semantic lookup fails, fall back to
         * the class currently being validated so that expressions like
         * `my.field` or `my.method()` resolve correctly.
         */
        if (currentClass && expr->token && expr->token->value &&
            (strcasecmp(expr->token->value, "myself") == 0 ||
             strcasecmp(expr->token->value, "my") == 0)) {
            return currentClass->name;
        }
        AST *decl = findStaticDeclarationInAST(expr->token->value, expr, gProgramRoot);
        if (!decl && currentClass) {
            Symbol *fs = lookupField(currentClass, expr->token->value);
            if (fs && fs->type_def) decl = fs->type_def;
        }
        if (decl && decl->right) {
            AST *type = decl->right;
            /* Drill through array and pointer wrappers */
            while (type && (type->type == AST_ARRAY_TYPE || type->type == AST_POINTER_TYPE)) {
                type = type->right;
            }
            if (type && type->type == AST_TYPE_REFERENCE && type->token) {
                return type->token->value;
            }
            if (type && type->token) {
                return type->token->value;
            }
        }
        return NULL;
    }
    case AST_ARRAY_ACCESS:
        return resolveExprClass(expr->left, currentClass);
    case AST_FIELD_ACCESS: {
        const char *base = resolveExprClass(expr->left, currentClass);
        if (!base) return NULL;
        ClassInfo *ci = lookupClass(base);
        if (!ci) return NULL;
        const char *fname = expr->right && expr->right->token ? expr->right->token->value : NULL;
        Symbol *fs = lookupField(ci, fname);
        if (!fs) {
            fprintf(stderr, "Unknown field '%s' on class '%s'\n", fname ? fname : "(null)", base);
            pascal_semantic_error_count++;
            return NULL;
        }
        if (fs->type_def) {
            AST *type = fs->type_def;
            while (type && (type->type == AST_ARRAY_TYPE || type->type == AST_POINTER_TYPE)) {
                type = type->right;
            }
            if (type && type->token) {
                return type->token->value;
            }
        }
        return NULL;
    }
    case AST_NEW:
        return expr->token ? expr->token->value : NULL;
    default:
        return NULL;
    }
}

static void validateNodeInternal(AST *node, ClassInfo *currentClass) {
    if (!node) return;

    ClassInfo *clsContext = currentClass;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        const char *fullname = node->token ? node->token->value : NULL;
        const char *us = fullname ? strchr(fullname, '_') : NULL;
        if (us) {
            size_t len = (size_t)(us - fullname);
            char buf[MAX_SYMBOL_LENGTH];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, fullname, len);
            buf[len] = '\0';
            clsContext = lookupClass(buf);
        } else {
            clsContext = NULL;
        }
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        /* Preserve explicit syntax while still annotating variables with their
         * declared types so later analyses (e.g. array element access) can
         * determine the base type.
         */
        AST *decl = findStaticDeclarationInAST(node->token->value, node, gProgramRoot);
        if (decl && decl->right) {
            node->type_def = decl->right;
            node->var_type = decl->right->var_type;
        }
    }

    if (node->type == AST_FIELD_ACCESS) {
        const char *cls = resolveExprClass(node->left, clsContext);
        if (cls) {
            ClassInfo *ci = lookupClass(cls);
            const char *fname = node->right && node->right->token ? node->right->token->value : NULL;
            if (ci) {
                Symbol *fs = lookupField(ci, fname);
                if (!fs) {
                    fprintf(stderr, "Unknown field '%s' on class '%s'\n", fname ? fname : "(null)", cls);
                    pascal_semantic_error_count++;
                } else if (fs->is_const && fs->value) {
                    /* Replace field access with constant literal */
                    Value *v = fs->value;
                    char buf[64];
                    Token *tok = NULL;
                    ASTNodeType newType = AST_NUMBER;
                    if (v->type == TYPE_DOUBLE || v->type == TYPE_REAL ||
                        v->type == TYPE_LONG_DOUBLE || v->type == TYPE_FLOAT) {
                        snprintf(buf, sizeof(buf), "%Lf", v->real.r_val);
                        tok = newToken(TOKEN_REAL_CONST, strdup(buf), 0, 0);
                        newType = AST_NUMBER;
                    } else if (v->type == TYPE_BOOLEAN) {
                        const char *lex = v->i_val ? "true" : "false";
                        tok = newToken(v->i_val ? TOKEN_TRUE : TOKEN_FALSE,
                                       strdup(lex), 0, 0);
                        newType = AST_BOOLEAN;
                    } else if (v->type == TYPE_STRING) {
                        tok = newToken(TOKEN_STRING_CONST,
                                       v->s_val ? strdup(v->s_val) : strdup(""),
                                       0, 0);
                        newType = AST_STRING;
                    } else if (v->type == TYPE_CHAR) {
                        char chbuf[2] = {(char)v->c_val, '\0'};
                        tok = newToken(TOKEN_STRING_CONST, strdup(chbuf), 0, 0);
                        newType = AST_STRING;
                    } else if (v->type == TYPE_ENUM && v->enum_val.enum_name) {
                        tok = newToken(TOKEN_IDENTIFIER,
                                       strdup(v->enum_val.enum_name), 0, 0);
                        newType = AST_ENUM_VALUE;
                    } else if (isIntlikeType(v->type)) {
                        snprintf(buf, sizeof(buf), "%lld", (long long)v->i_val);
                        tok = newToken(TOKEN_INTEGER_CONST, strdup(buf), 0, 0);
                        newType = AST_NUMBER;
                    }
                    if (tok) {
                        if (node->left) freeAST(node->left);
                        if (node->right) freeAST(node->right);
                        node->left = node->right = node->extra = NULL;
                        node->child_count = 0;
                        node->children = NULL;
                        node->token = tok;
                        node->type = newType;
                        setTypeAST(node, v->type);
                        switch (v->type) {
                            case TYPE_STRING:
                                node->i_val = v->s_val ? (int)strlen(v->s_val) : 0;
                                break;
                            case TYPE_CHAR:
                                node->i_val = 1;
                                break;
                            case TYPE_BOOLEAN:
                                node->i_val = v->i_val;
                                break;
                            case TYPE_ENUM:
                                node->i_val = v->enum_val.ordinal;
                                break;
                            default:
                                break;
                        }
                    }
                } else if (fs->type_def) {
                    node->var_type = fs->type_def->var_type;
                    node->type_def = copyAST(fs->type_def);
                }
            }
        }
    } else if (node->type == AST_PROCEDURE_CALL) {
        if (!node->left && node->token && node->token->value && node->i_val == 0) {
            const char *us = strchr(node->token->value, '_');
            if (us) {
                size_t cls_len = (size_t)(us - node->token->value);
                char cls[MAX_SYMBOL_LENGTH];
                if (cls_len < sizeof(cls)) {
                    memcpy(cls, node->token->value, cls_len);
                    cls[cls_len] = '\0';
                    if (lookupClass(cls)) {
                        fprintf(stderr,
                                "Legacy method call '%s' is no longer supported; use instance.%s() instead\n",
                                node->token->value, us + 1);
                        pascal_semantic_error_count++;
                    }
                }
            }
        }
        if (node->i_val == 1) {
            /* super constructor/method call already has implicit 'myself' */
            if (node->token && node->token->value && !strchr(node->token->value, '_')) {
                const char *pname = node->token->value;
                size_t ln = strlen(pname) + 1 + strlen(pname) + 1;
                char *m = (char*)malloc(ln);
                if (m) {
                    snprintf(m, ln, "%s_%s", pname, pname);
                    free(node->token->value);
                    node->token->value = m;
                    node->token->length = strlen(m);
                }
            }
        } else if (node->left) {
            const char *cls = resolveExprClass(node->left, clsContext);
            const char *name = node->token ? node->token->value : NULL;
            if (cls && name) {
                const char *method = name;
                const char *us = strchr(name, '_');
                bool already = false;
                if (us && strncasecmp(name, cls, (size_t)(us - name)) == 0) {
                    method = us + 1;
                    already = true;
                }
                ClassInfo *ci = lookupClass(cls);
                if (ci) {
                    Symbol *ms = lookupMethod(ci, method);
                    if (!already && (ms || cls)) {
                        size_t ln = strlen(cls) + 1 + strlen(name) + 1;
                        char *m = (char*)malloc(ln);
                        if (m) {
                            snprintf(m, ln, "%s_%s", cls, name);
                            free(node->token->value);
                            node->token->value = m;
                            node->token->length = strlen(m);
                        }
                    }
                }
            }
            if (node->child_count > 0 && node->children[0] &&
                node->children[0]->type == AST_VARIABLE &&
                node->children[0]->token && node->children[0]->token->value &&
                (strcasecmp(node->children[0]->token->value, "myself") == 0 ||
                 strcasecmp(node->children[0]->token->value, "my") == 0)) {
                /*
                 * The parser places the receiver both as `left` and as the first
                 * child argument.  Using the same node in both locations leads to
                 * double-free errors when the AST is destroyed.  Instead of
                 * discarding the child (which would make the argument list empty),
                 * keep a separate copy so the call still receives the receiver as
                 * its first argument.
                 */
                AST *recv_copy = copyAST(node->left);
                if (recv_copy) {
                    node->children[0] = recv_copy;
                    recv_copy->parent = node;
                }
            }
        } else if (currentClass && node->token) {
            Symbol *sym = lookupMethod(currentClass, node->token->value);
            if (sym && sym->type_def && sym->type_def->token && sym->type_def->token->value) {
                const char *fullname = sym->type_def->token->value;
                size_t ln = strlen(fullname) + 1;
                char *m = (char*)malloc(ln);
                if (m) {
                    memcpy(m, fullname, ln);
                    free(node->token->value);
                    node->token->value = m;
                    node->token->length = ln - 1;
                }

                bool firstIsMyself = false;
                if (node->child_count > 0 && node->children[0] &&
                    node->children[0]->type == AST_VARIABLE &&
                    node->children[0]->token && node->children[0]->token->value &&
                    (strcasecmp(node->children[0]->token->value, "myself") == 0 ||
                     strcasecmp(node->children[0]->token->value, "my") == 0)) {
                    firstIsMyself = true;
                }

                if (!firstIsMyself && node->i_val == 0) {
                    Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", node->token ? node->token->line : 0, 0);
                    AST *selfVar = newASTNode(AST_VARIABLE, selfTok);
                    selfVar->var_type = TYPE_POINTER;
                    addChild(node, NULL);
                    for (int i = node->child_count - 1; i > 0; i--) {
                        node->children[i] = node->children[i - 1];
                        if (node->children[i]) node->children[i]->parent = node;
                    }
                    node->children[0] = selfVar;
                    selfVar->parent = node;
                    setLeft(node, selfVar);
                } else if (firstIsMyself && node->i_val == 0) {
                    setLeft(node, node->children[0]);
                }
            }
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        if (node->left) validateNodeInternal(node->left, clsContext);
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) validateNodeInternal(node->children[i], clsContext);
        }
        if (node->right) validateNodeInternal(node->right, clsContext);
        if (node->extra) validateNodeInternal(node->extra, clsContext);

        AST *baseType = node->left ? node->left->type_def : NULL;
        for (int i = 0; i < node->child_count && baseType; i++) {
            if (baseType->type == AST_ARRAY_TYPE) {
                baseType = baseType->right;
            } else {
                baseType = NULL;
            }
        }
        if (!baseType) {
            const char *cls = resolveExprClass(node->left, clsContext);
            if (cls) {
                Token *tok = newToken(TOKEN_IDENTIFIER, cls,
                                       node->token ? node->token->line : 0, 0);
                AST *typeRef = newASTNode(AST_TYPE_REFERENCE, tok);
                setTypeAST(typeRef, TYPE_RECORD);
                AST *ptrType = newASTNode(AST_POINTER_TYPE, NULL);
                setRight(ptrType, typeRef);
                setTypeAST(ptrType, TYPE_POINTER);
                node->type_def = ptrType;
            }
            setTypeAST(node, TYPE_POINTER);
            return;
        }
        AST *elemType = copyAST(baseType);
        setTypeAST(node, baseType->var_type);
        if (node->var_type == TYPE_RECORD || node->var_type == TYPE_VOID ||
            node->var_type == TYPE_UNKNOWN || baseType->type == AST_TYPE_REFERENCE ||
            baseType->type == AST_RECORD_TYPE) {
            AST *ptrType = newASTNode(AST_POINTER_TYPE, NULL);
            setRight(ptrType, elemType);
            setTypeAST(ptrType, TYPE_POINTER);
            node->type_def = ptrType;
            setTypeAST(node, TYPE_POINTER);
        } else {
            node->type_def = elemType;
        }
        return;
    }

    if (node->left) validateNodeInternal(node->left, clsContext);
    if (node->right) validateNodeInternal(node->right, clsContext);
    if (node->extra) validateNodeInternal(node->extra, clsContext);
    for (int i = 0; i < node->child_count; i++) validateNodeInternal(node->children[i], clsContext);
}

/* ------------------------------------------------------------------------- */
/*  Public entry                                                             */
/* ------------------------------------------------------------------------- */

void reaPerformSemanticAnalysis(AST *root) {
    if (!root) return;
    gProgramRoot = root;
    collectClasses(root);
    collectMethods(root);
    linkParents();
    checkOverrides();
    addInheritedMethodAliases();
    validateNodeInternal(root, NULL);
    freeClassTable();
}


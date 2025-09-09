#include "rea/semantic.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "core/types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Forward declaration from core/utils.c
Token *newToken(TokenType type, const char *value, int line, int column);

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
                if (ci->fields) freeSymbolTable(ci->fields, true);
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
            sym->type_def = ftype ? copyAST(ftype) : NULL;
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
                        } else {
                            free(sym); free(v); free(lname);
                        }
                    }
                }
                free(cls);
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
        if (fs->type_def && fs->type_def->token) {
            return fs->type_def->token->value;
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
        bool resolved = false;
        if (clsContext) {
            const char *vname = node->token->value;
            Symbol *fs = lookupField(clsContext, vname);
            if (fs && fs->type_def) {
                node->var_type = fs->type_def->var_type;
                node->type_def = copyAST(fs->type_def);
                resolved = true;
            }
        }
        if (!resolved && gProgramRoot) {
            AST *decl = findStaticDeclarationInAST(node->token->value, node, gProgramRoot);
            if (decl && decl->right) {
                node->var_type = decl->right->var_type;
                node->type_def = copyAST(decl->right);
            }
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
                } else if (fs->type_def) {
                    node->var_type = fs->type_def->var_type;
                    node->type_def = copyAST(fs->type_def);
                }
            }
        }
    } else if (node->type == AST_PROCEDURE_CALL) {
        if (node->left) {
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
                if (ci && lookupMethod(ci, method)) {
                    if (!already) {
                        size_t ln = strlen(cls) + 1 + strlen(name) + 1;
                        char *m = (char*)malloc(ln);
                        if (m) {
                            snprintf(m, ln, "%s_%s", cls, name);
                            free(node->token->value);
                            node->token->value = m;
                            node->token->length = strlen(m);
                        }
                    }
                } else if (ci && !lookupMethod(ci, method)) {
                    fprintf(stderr, "Unknown method '%s' for class '%s'\n", method, cls);
                    pascal_semantic_error_count++;
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

                Token *thisTok = newToken(TOKEN_IDENTIFIER, "this", node->token ? node->token->line : 0, 0);
                AST *thisVar = newASTNode(AST_VARIABLE, thisTok);
                thisVar->var_type = TYPE_POINTER;
                addChild(node, NULL);
                for (int i = node->child_count - 1; i > 0; i--) {
                    node->children[i] = node->children[i - 1];
                    if (node->children[i]) node->children[i]->parent = node;
                }
                node->children[0] = thisVar;
                thisVar->parent = node;
                setLeft(node, thisVar);
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
        if (baseType) {
            node->var_type = baseType->var_type;
            node->type_def = copyAST(baseType);
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
    validateNodeInternal(root, NULL);
    freeClassTable();
}


// A lightweight JSON parser sufficient to consume the AST JSON produced
// by dumpASTJSON() in src/ast/ast.c and reconstruct the AST.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "tools/ast_json_loader.h"
#include "core/utils.h"    // newToken, varTypeToString etc.
#include "core/list.h"

typedef struct {
    const char* s;
    size_t i;
    size_t n;
} J;

static void skip_ws(J* j) {
    while (j->i < j->n) {
        char c = j->s[j->i];
        if (c==' '||c=='\t'||c=='\r'||c=='\n') { j->i++; continue; }
        break;
    }
}

static int peek(J* j) { return j->i < j->n ? (unsigned char)j->s[j->i] : -1; }
static int nextc(J* j) { return j->i < j->n ? (unsigned char)j->s[j->i++] : -1; }

static int expect(J* j, char c) {
    skip_ws(j);
    if (peek(j) == c) { j->i++; return 1; }
    return 0;
}

static int hexval(int c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static char* parse_string(J* j) {
    skip_ws(j);
    if (nextc(j) != '"') return NULL;
    size_t cap = 64, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    while (j->i < j->n) {
        int c = nextc(j);
        if (c < 0) { free(out); return NULL; }
        if (c == '"') break;
        if (c == '\\') {
            int e = nextc(j);
            if (e < 0) { free(out); return NULL; }
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    // Parse \uXXXX. For simplicity, store as UTF-8 for BMP or replace with '?'.
                    int h1 = hexval(nextc(j));
                    int h2 = hexval(nextc(j));
                    int h3 = hexval(nextc(j));
                    int h4 = hexval(nextc(j));
                    if (h1<0||h2<0||h3<0||h4<0) { free(out); return NULL; }
                    unsigned code = (h1<<12)|(h2<<8)|(h3<<4)|h4;
                    char buf[4]; size_t bl = 0;
                    if (code < 0x80) { buf[0] = (char)code; bl = 1; }
                    else if (code < 0x800) { buf[0] = 0xC0 | (code>>6); buf[1] = 0x80 | (code&0x3F); bl=2; }
                    else { buf[0]=0xE0|(code>>12); buf[1]=0x80|((code>>6)&0x3F); buf[2]=0x80|(code&0x3F); bl=3; }
                    for (size_t k=0;k<bl;k++) {
                        if (len+1 >= cap) { cap*=2; out=(char*)realloc(out,cap); if(!out) return NULL; }
                        out[len++] = buf[k];
                    }
                    continue;
                }
                default: c = '?'; break;
            }
        }
        if (len+1 >= cap) { cap*=2; out=(char*)realloc(out,cap); if(!out) return NULL; }
        out[len++] = (char)c;
    }
    if (len+1 >= cap) { cap*=2; out=(char*)realloc(out,cap); if(!out) return NULL; }
    out[len] = '\0';
    return out;
}

static long long parse_integer(J* j) {
    skip_ws(j);
    int sign = 1; if (peek(j) == '-') { sign = -1; j->i++; }
    long long v = 0; int any=0;
    while (j->i < j->n && isdigit((unsigned char)j->s[j->i])) { v = v*10 + (j->s[j->i]-'0'); j->i++; any=1; }
    return any ? sign*v : 0;
}

static int parse_bool(J* j, int* out) {
    skip_ws(j);
    if (j->i+4 <= j->n && strncmp(j->s+j->i, "true", 4)==0) { *out=1; j->i+=4; return 1; }
    if (j->i+5 <= j->n && strncmp(j->s+j->i, "false", 5)==0) { *out=0; j->i+=5; return 1; }
    return 0;
}

static int parse_null(J* j) {
    skip_ws(j);
    if (j->i+4 <= j->n && strncmp(j->s+j->i, "null", 4)==0) { j->i+=4; return 1; }
    return 0;
}

static TokenType tokenTypeFromString(const char* s) {
    for (int t = TOKEN_PROGRAM; t <= TOKEN_AT; t++) {
        if (strcmp(tokenTypeToString((TokenType)t), s) == 0) return (TokenType)t;
    }
    // Fallback
    return TOKEN_IDENTIFIER;
}

static ASTNodeType astTypeFromString(const char* s) {
    for (int t = AST_NOOP; t <= AST_NEW; t++) {
        if (strcmp(astTypeToString((ASTNodeType)t), s) == 0) return (ASTNodeType)t;
    }
    return AST_NOOP;
}

static VarType varTypeFromString(const char* s) {
    for (int t = TYPE_UNKNOWN; t <= TYPE_THREAD; t++) {
        if (strcmp(varTypeToString((VarType)t), s) == 0) return (VarType)t;
    }
    return TYPE_UNKNOWN;
}

static AST* parse_ast_node(J* j);

static void set_child_index(AST* parent, int index, AST* child) {
    // Ensure capacity and assign by index (used for BLOCK declarations/body)
    while (parent->child_count < index) {
        addChild(parent, NULL);
    }
    if (parent->child_count == index) {
        addChild(parent, child);
    } else {
        parent->children[index] = child;
        if (child) child->parent = parent;
    }
}

static AST* parse_ast_object(J* j) {
    if (!expect(j, '{')) return NULL;
    ASTNodeType node_type = AST_NOOP;
    Token* tok = NULL;
    VarType vtype = TYPE_UNKNOWN;
    int by_ref = 0, is_inline = 0, is_global_scope = 0;
    int i_val = 0;
    // Children to set after creating node
    AST* left = NULL; AST* right = NULL; AST* extra = NULL;
    AST* program_name_node = NULL; AST* main_block = NULL;
    List* unit_list = NULL;
    // temp vectors for children
    AST** kids = NULL; int kids_count = 0, kids_cap = 0;
    AST* decls = NULL; AST* body = NULL;

    while (1) {
        skip_ws(j);
        if (expect(j, '}')) break;
        char* key = parse_string(j);
        if (!key) { free(kids); return NULL; }
        if (!expect(j, ':')) { free(key); free(kids); return NULL; }
        if (strcmp(key, "node_type") == 0) {
            char* v = parse_string(j);
            if (!v) { free(key); free(kids); return NULL; }
            node_type = astTypeFromString(v);
            free(v);
        } else if (strcmp(key, "token") == 0) {
            if (!expect(j, '{')) { free(key); free(kids); return NULL; }
            char* ttype = NULL; char* tvalue = NULL;
            while (1) {
                skip_ws(j);
                if (expect(j, '}')) break;
                char* k2 = parse_string(j);
                if (!k2) { free(key); free(kids); return NULL; }
                if (!expect(j, ':')) { free(k2); free(key); free(kids); return NULL; }
                if (strcmp(k2, "type") == 0) {
                    ttype = parse_string(j);
                } else if (strcmp(k2, "value") == 0) {
                    tvalue = parse_string(j);
                } else {
                    // unknown field; skip value generically
                    if (peek(j) == '"') {
                        char* tmp = parse_string(j); free(tmp);
                    } else if (peek(j) == '{') {
                        int depth=1; nextc(j);
                        while (j->i<j->n && depth) {
                            int c=nextc(j);
                            if(c=='{') depth++;
                            else if(c=='}') depth--;
                        }
                    } else if (peek(j) == '[') {
                        int depth=1; nextc(j);
                        while (j->i<j->n && depth) {
                            int c=nextc(j);
                            if(c=='[') depth++;
                            else if(c==']') depth--;
                        }
                    } else if (parse_null(j)) {
                        /* nothing */
                    } else {
                        (void)parse_integer(j);
                    }
                }
                skip_ws(j); (void)expect(j, ',');
                skip_ws(j);
            }
            if (ttype || tvalue) {
                TokenType tt = ttype ? tokenTypeFromString(ttype) : TOKEN_IDENTIFIER;
                tok = newToken(tt, tvalue ? tvalue : NULL, 0, 0);
            }
            free(ttype); free(tvalue);
        } else if (strcmp(key, "var_type_annotated") == 0) {
            char* v = parse_string(j); vtype = v ? varTypeFromString(v) : TYPE_UNKNOWN; free(v);
        } else if (strcmp(key, "by_ref") == 0) {
            int b; if (parse_bool(j,&b)) by_ref = b; else (void)parse_null(j);
        } else if (strcmp(key, "is_inline") == 0) {
            int b; if (parse_bool(j,&b)) is_inline = b; else (void)parse_null(j);
        } else if (strcmp(key, "i_val") == 0) {
            i_val = (int)parse_integer(j);
        } else if (strcmp(key, "is_global_scope") == 0) {
            int b; if (parse_bool(j,&b)) is_global_scope = b; else (void)parse_null(j);
        } else if (strcmp(key, "left") == 0) {
            if (parse_null(j)) left = NULL; else left = parse_ast_node(j);
        } else if (strcmp(key, "right") == 0) {
            if (parse_null(j)) right = NULL; else right = parse_ast_node(j);
        } else if (strcmp(key, "extra") == 0) {
            if (parse_null(j)) extra = NULL; else extra = parse_ast_node(j);
        } else if (strcmp(key, "children") == 0) {
            if (!expect(j, '[')) { free(key); free(kids); return NULL; }
            while (1) {
                skip_ws(j);
                if (expect(j, ']')) break;
                AST* c = parse_ast_node(j);
                if (kids_count+1 >= kids_cap) {
                    kids_cap = kids_cap ? kids_cap*2 : 4;
                    kids = (AST**)realloc(kids, kids_cap*sizeof(AST*));
                    if (!kids) return NULL;
                }
                kids[kids_count++] = c;
                skip_ws(j); (void)expect(j, ',');
            }
        } else if (strcmp(key, "program_name_node") == 0) {
            program_name_node = parse_ast_node(j);
        } else if (strcmp(key, "main_block") == 0) {
            main_block = parse_ast_node(j);
        } else if (strcmp(key, "uses_clauses") == 0) {
            if (!expect(j, '[')) { free(key); free(kids); return NULL; }
            while (1) {
                skip_ws(j);
                if (expect(j, ']')) break;
                AST* c = parse_ast_node(j);
                if (kids_count+1 >= kids_cap) {
                    kids_cap = kids_cap ? kids_cap*2 : 4;
                    kids = (AST**)realloc(kids, kids_cap*sizeof(AST*));
                    if (!kids) return NULL;
                }
                kids[kids_count++] = c;
                skip_ws(j); (void)expect(j, ',');
            }
        } else if (strcmp(key, "declarations") == 0) {
            if (parse_null(j)) decls = NULL; else decls = parse_ast_node(j);
        } else if (strcmp(key, "body") == 0) {
            if (parse_null(j)) body = NULL; else body = parse_ast_node(j);
        } else if (strcmp(key, "unit_list") == 0) {
            if (!expect(j, '[')) { free(key); free(kids); return NULL; }
            unit_list = createList();
            while (1) {
                skip_ws(j);
                if (expect(j, ']')) break;
                char* s = parse_string(j);
                if (s) { listAppend(unit_list, s); free(s); }
                skip_ws(j); (void)expect(j, ',');
            }
        } else {
            // Unknown field: skip a generic JSON value
            skip_ws(j);
            int p = peek(j);
            if (p == '"') { char* tmp = parse_string(j); free(tmp); }
            else if (p == '{') {
                int depth=1; nextc(j); while (j->i<j->n && depth){ int c=nextc(j); if(c=='{')depth++; else if(c=='}')depth--; }
            } else if (p == '[') {
                int depth=1; nextc(j); while (j->i<j->n && depth){ int c=nextc(j); if(c=='[')depth++; else if(c==']')depth--; }
            } else if (!parse_null(j)) {
                (void)parse_integer(j);
            }
        }
        free(key);
        skip_ws(j);
        (void)expect(j, ',');
    }

    AST* node = newASTNode(node_type, tok);
    if (tok) { freeToken(tok); tok = NULL; }
    if (vtype != TYPE_UNKNOWN) setTypeAST(node, vtype);
    node->by_ref = by_ref;
    node->is_inline = is_inline;
    node->is_global_scope = is_global_scope;
    node->i_val = i_val;
    if (unit_list && node->type == AST_USES_CLAUSE) node->unit_list = unit_list; else if (unit_list) freeList(unit_list);

    if (program_name_node) setLeft(node, program_name_node);
    if (main_block) setRight(node, main_block);
    if (left) setLeft(node, left);
    if (right) setRight(node, right);
    if (extra) setExtra(node, extra);
    if (node->type == AST_BLOCK) {
        if (decls) set_child_index(node, 0, decls);
        if (body) set_child_index(node, 1, body);
    }
    for (int i = 0; i < kids_count; i++) {
        addChild(node, kids[i]);
    }
    free(kids);
    return node;
}

static AST* parse_ast_node(J* j) {
    skip_ws(j);
    if (peek(j) == '{') return parse_ast_object(j);
    if (parse_null(j)) return NULL;
    // Unexpected
    return NULL;
}

AST* loadASTFromJSON(const char* json_text) {
    if (!json_text) return NULL;
    J j = { json_text, 0, strlen(json_text) };
    skip_ws(&j);
    AST* root = parse_ast_node(&j);
    return root;
}

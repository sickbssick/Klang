
#include "kcc.h"

typedef struct Parser {
    TokenList *tokens;
    long long pos;
    int opt_default;
    int opt_ff;
    int opt_kernel;
    char *file_path;
} Parser;

static Token *p_peek(Parser *p) {
    return p->tokens->items[p->pos];
}

static Token *p_peek_at(Parser *p, long long offset) {
    long long idx = p->pos + offset;
    if (idx >= p->tokens->count) idx = p->tokens->count - 1;
    return p->tokens->items[idx];
}

static Token *p_advance(Parser *p) {
    Token *t = p->tokens->items[p->pos];
    if (t->kind != TOK_EOF) p->pos++;
    return t;
}

static int p_match(Parser *p, int k) {
    Token *t = p_peek(p);
    if (t->kind == k) { p_advance(p); return 1; }
    return 0;
}

static Token *p_expect(Parser *p, int k, const char *msg) {
    Token *t = p_peek(p);
    if (t->kind != k) {
        fprintf(k_stderr, "error (%s:%lld:%lld): expected %s\n", p->file_path, t->line, t->col, msg);
        fprintf(k_stderr, "  help: got '%s' instead of '%s'\n", t->text, msg);
        exit(1);
    }
    return p_advance(p);
}

/* Forward declarations */
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_var_decl(Parser *p, int expect_semi);

static KType *parse_base_type(Parser *p, long long depth, long long arr_len) {
    if (p_match(p, TOK_SAFE)) {
        p_expect(p, TOK_DCOLON, "::");
        Token *t = p_expect(p, TOK_IDENT, "type after safe::");
        if (strcmp(t->text, "str") == 0) return make_type(TY_STR, depth, arr_len, NULL);
        fprintf(k_stderr, "error (%s:%lld:%lld): unknown safe type 'safe::%s'\n", p->file_path, t->line, t->col, t->text);
        exit(1);
    }
    Token *t = p_expect(p, TOK_IDENT, "type name");
    char *s = t->text;
    for (long long i = 0; i < alias_count; i++) {
        TypeAlias *al = aliases[i];
        if (strcmp(al->alias, s) == 0) {
            KType *res = clone_type(al->target);
            res->ptr_depth += depth;
            if (arr_len > 0) res->array_len = arr_len;
            return res;
        }
    }
    ClassDef *cls = find_class(s);
    if (cls != NULL) return make_type(TY_CLASS, depth, arr_len, cls->name);
    EnumDef *en = find_enum(s);
    if (en != NULL)  return make_type(TY_ENUM,  depth, arr_len, en->name);

    if (strcmp(s,"i8")==0)  return make_type(TY_I8,  depth, arr_len, NULL);
    if (strcmp(s,"i16")==0) return make_type(TY_I16, depth, arr_len, NULL);
    if (strcmp(s,"i32")==0) return make_type(TY_I32, depth, arr_len, NULL);
    if (strcmp(s,"i64")==0) return make_type(TY_I64, depth, arr_len, NULL);
    if (strcmp(s,"u8")==0)  return make_type(TY_U8,  depth, arr_len, NULL);
    if (strcmp(s,"u16")==0) return make_type(TY_U16, depth, arr_len, NULL);
    if (strcmp(s,"u32")==0) return make_type(TY_U32, depth, arr_len, NULL);
    if (strcmp(s,"u64")==0) return make_type(TY_U64, depth, arr_len, NULL);
    if (strcmp(s,"f8")==0)  return make_type(TY_F8,  depth, arr_len, NULL);
    if (strcmp(s,"f16")==0) return make_type(TY_F16, depth, arr_len, NULL);
    if (strcmp(s,"f32")==0) return make_type(TY_F32, depth, arr_len, NULL);
    if (strcmp(s,"f64")==0 || strcmp(s,"double")==0) return make_type(TY_F64, depth, arr_len, NULL);
    if (strcmp(s,"bool")==0) return make_type(TY_BOOL, depth, arr_len, NULL);
    if (strcmp(s,"byte")==0 || strcmp(s,"char")==0) return make_type(TY_CHAR, depth, arr_len, NULL);
    if (strcmp(s,"void")==0) return make_type(TY_VOID, depth, arr_len, NULL);

    fprintf(k_stderr, "error (%s:%lld:%lld): unknown type '%s'\n", p->file_path, t->line, t->col, s);
    exit(1);
    return NULL;
}

static KType *parse_type(Parser *p) {
    long long arr_len = 0;
    if (p_match(p, TOK_LBRACKET)) {
        Token *len_tok = p_expect(p, TOK_INT, "array length");
        if (len_tok->int_val <= 0) {
            fprintf(k_stderr, "error (%s:%lld:%lld): array length must be > 0\n", p->file_path, len_tok->line, len_tok->col);
            exit(1);
        }
        arr_len = len_tok->int_val;
        p_expect(p, TOK_RBRACKET, "]");
    }
    long long depth = 0;
    while (p_peek(p)->kind == TOK_STAR || p_peek(p)->kind == TOK_POW) {
        if (p_match(p, TOK_POW)) depth += 2;
        else { p_advance(p); depth += 1; }
    }
    KType *ty = parse_base_type(p, depth, arr_len);

    /* optional trailing ", ar[N]" or "ar[N]" syntax */
    Token *t0 = p_peek(p);
    Token *t1 = p_peek_at(p, 1);
    Token *t2 = p_peek_at(p, 2);
    int has_ar = 0;
    if (t0->kind == TOK_COMMA && t1->kind == TOK_IDENT && strcmp(t1->text,"ar")==0 && t2->kind == TOK_LBRACKET) {
        p_advance(p); p_advance(p); has_ar = 1;
    } else if (t0->kind == TOK_IDENT && strcmp(t0->text,"ar")==0 && t1->kind == TOK_LBRACKET) {
        p_advance(p); has_ar = 1;
    }
    if (has_ar) {
        p_expect(p, TOK_LBRACKET, "[");
        Token *len_tok = p_expect(p, TOK_INT, "array length");
        if (len_tok->int_val <= 0) {
            fprintf(k_stderr, "error (%s:%lld:%lld): array length must be > 0\n", p->file_path, len_tok->line, len_tok->col);
            exit(1);
        }
        ty->array_len = len_tok->int_val;
        p_expect(p, TOK_RBRACKET, "]");
    }
    return ty;
}

static Node *parse_primary(Parser *p) {
    Token *t = p_peek(p);
    if (p_match(p, TOK_INT)) {
        Node *n = new_node(NODE_INT, t->line, t->col);
        n->int_val = t->int_val;
        n->val_type = make_type(TY_I64, 0, 0, NULL);
        return n;
    }
    if (p_match(p, TOK_FLOAT)) {
        Node *n = new_node(NODE_FLOAT, t->line, t->col);
        n->float_val = t->float_val;
        n->val_type = make_type(TY_F64, 0, 0, NULL);
        return n;
    }
    if (p_match(p, TOK_STR)) {
        Node *n = new_node(NODE_STR, t->line, t->col);
        n->str_val = strdup(t->text);
        n->val_type = make_type(TY_STR, 0, 0, NULL);
        return n;
    }
    if (p_match(p, TOK_CHAR)) {
        Node *n = new_node(NODE_CHAR, t->line, t->col);
        n->int_val = t->int_val;
        n->val_type = make_type(TY_CHAR, 0, 0, NULL);
        return n;
    }
    if (p_match(p, TOK_TRUE)) {
        Node *n = new_node(NODE_BOOL, t->line, t->col);
        n->int_val = 1;
        n->val_type = make_type(TY_BOOL, 0, 0, NULL);
        return n;
    }
    if (p_match(p, TOK_FALSE)) {
        Node *n = new_node(NODE_BOOL, t->line, t->col);
        n->int_val = 0;
        n->val_type = make_type(TY_BOOL, 0, 0, NULL);
        return n;
    }
    if (p_match(p, TOK_SIZEOF)) {
        p_expect(p, TOK_LBRACKET, "[");
        Node *n = new_node(NODE_SIZEOF, t->line, t->col);
        n->sizeof_type = parse_type(p);
        p_expect(p, TOK_RBRACKET, "]");
        n->val_type = make_type(TY_U64, 0, 0, NULL);
        return n;
    }
    if (p_match(p, TOK_LBRACE)) {
        Node *n = new_node(NODE_ARRAY_LIT, t->line, t->col);
        if (p_peek(p)->kind != TOK_RBRACE) {
            int more = 1;
            while (more) {
                n->args = realloc(n->args, (n->arg_count+1)*sizeof(void*));
                n->args[n->arg_count++] = parse_expr(p);
                more = p_match(p, TOK_COMMA);
            }
        }
        p_expect(p, TOK_RBRACE, "}");
        return n;
    }
    if (p_match(p, TOK_RAW)) {
        p_expect(p, TOK_DCOLON, "::");
        if (p_match(p, TOK_INB)) {
            Node *n = new_node(NODE_INB, t->line, t->col);
            p_expect(p, TOK_LBRACKET, "[");
            n->left = parse_expr(p);
            p_expect(p, TOK_RBRACKET, "]");
            n->val_type = make_type(TY_U8, 0, 0, NULL);
            return n;
        }
        if (p_match(p, TOK_INW)) {
            Node *n = new_node(NODE_INW, t->line, t->col);
            p_expect(p, TOK_LBRACKET, "[");
            n->left = parse_expr(p);
            p_expect(p, TOK_RBRACKET, "]");
            n->val_type = make_type(TY_U16, 0, 0, NULL);
            return n;
        }
        fprintf(k_stderr, "error (%s:%lld:%lld): unknown raw:: expression\n", p->file_path, t->line, t->col);
        exit(1);
    }
    if (p_match(p, TOK_IDENT)) {
        if (p_match(p, TOK_DCOLON) || p_match(p, TOK_AT)) {
            Token *vtok = p_expect(p, TOK_IDENT, "enum variant");
            EnumDef *en = find_enum(t->text);
            if (en == NULL) {
                fprintf(k_stderr, "error (%s:%lld:%lld): unknown enum '%s'\n", p->file_path, t->line, t->col, t->text);
                exit(1);
            }
            long long ev = 0;
            if (find_enum_value(t->text, vtok->text, &ev) == 0) {
                fprintf(k_stderr, "error (%s:%lld:%lld): enum '%s' has no variant '%s'\n", p->file_path, vtok->line, vtok->col, t->text, vtok->text);
                exit(1);
            }
            Node *n = new_node(NODE_INT, t->line, t->col);
            n->int_val = ev;
            n->val_type = make_type(TY_ENUM, 0, 0, en->name);
            return n;
        }
        if (p_match(p, TOK_LBRACKET)) {
            Node *n = new_node(NODE_CALL, t->line, t->col);
            n->call_name = strdup(t->text);
            if (p_match(p, TOK_SAFE)) {
                p_expect(p, TOK_DCOLON, "::");
                p_expect(p, TOK_NUL, "nul");
            } else if (p_peek(p)->kind != TOK_RBRACKET && p_peek(p)->kind != TOK_NUL) {
                int more = 1;
                while (more) {
                    n->args = realloc(n->args, (n->arg_count+1)*sizeof(void*));
                    n->args[n->arg_count++] = parse_expr(p);
                    more = p_match(p, TOK_COMMA);
                }
            } else {
                p_match(p, TOK_NUL);
            }
            p_expect(p, TOK_RBRACKET, "]");
            return n;
        }
        /* bare enum value? */
        EnumDef *bare_enum = NULL;
        long long eval = 0;
        if (find_bare_enum_value(t->text, &bare_enum, &eval)) {
            Node *n = new_node(NODE_INT, t->line, t->col);
            n->int_val = eval;
            n->val_type = make_type(TY_ENUM, 0, 0, bare_enum->name);
            return n;
        }
        Node *n = new_node(NODE_VAR, t->line, t->col);
        n->var_name = strdup(t->text);
        return n;
    }
    if (p_match(p, TOK_LPAREN)) {
        Node *n = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        return n;
    }
    fprintf(k_stderr, "error (%s:%lld:%lld): unexpected token '%s' in expression\n", p->file_path, t->line, t->col, t->text);
    exit(1);
    return NULL;
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    while (p_peek(p)->kind == TOK_INC || p_peek(p)->kind == TOK_DEC ||
           p_peek(p)->kind == TOK_LBRACKET || p_peek(p)->kind == TOK_ARROW ||
           p_peek(p)->kind == TOK_DOT) {
        if (p_match(p, TOK_LBRACKET)) {
            Node *idx = new_node(NODE_INDEX, n->line, n->col);
            idx->left  = n;
            idx->right = parse_expr(p);
            p_expect(p, TOK_RBRACKET, "]");
            n = idx;
        } else if (p_peek(p)->kind == TOK_ARROW || p_peek(p)->kind == TOK_DOT) {
            int arrow = 0;
            if (p_advance(p)->kind == TOK_ARROW) arrow = 1;
            Token *fld = p_expect(p, TOK_IDENT, "field name");
            Node *m = new_node(NODE_MEMBER, fld->line, fld->col);
            m->left = n;
            m->member_name = strdup(fld->text);
            m->via_arrow = arrow;
            n = m;
        } else {
            Token *op = p_advance(p);
            Node *post = new_node(NODE_POSTOP, op->line, op->col);
            post->op   = op->kind;
            post->left = n;
            n = post;
        }
    }
    return n;
}

static Node *parse_unary(Parser *p) {
    Token *t = p_peek(p);
    if (p_match(p, TOK_AMP)) {
        Node *n = new_node(NODE_ADDR, t->line, t->col);
        n->left = parse_unary(p);
        return n;
    }
    if (p_match(p, TOK_STAR)) {
        Node *n = new_node(NODE_DEREF, t->line, t->col);
        n->left = parse_unary(p);
        return n;
    }
    if (p_match(p, TOK_MINUS) || p_match(p, TOK_NOT) || p_match(p, TOK_TILDE)) {
        Node *n = new_node(NODE_UNOP, t->line, t->col);
        n->op   = t->kind;
        n->left = parse_unary(p);
        return n;
    }
    return parse_postfix(p);
}

static Node *parse_power(Parser *p) {
    Node *left = parse_unary(p);
    if (p_match(p, TOK_POW)) {
        Node *n = new_node(NODE_BINOP, left->line, left->col);
        n->op    = TOK_POW;
        n->left  = left;
        n->right = parse_power(p);
        return n;
    }
    return left;
}

static Node *parse_mul(Parser *p) {
    Node *left = parse_power(p);
    while (p_peek(p)->kind==TOK_STAR || p_peek(p)->kind==TOK_SLASH || p_peek(p)->kind==TOK_PERCENT) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_power(p);
        left = n;
    }
    return left;
}

static Node *parse_add(Parser *p) {
    Node *left = parse_mul(p);
    while (p_peek(p)->kind==TOK_PLUS || p_peek(p)->kind==TOK_MINUS) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_mul(p);
        left = n;
    }
    return left;
}

static Node *parse_shift(Parser *p) {
    Node *left = parse_add(p);
    while (p_peek(p)->kind==TOK_SHL || p_peek(p)->kind==TOK_SHR) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_add(p);
        left = n;
    }
    return left;
}

static Node *parse_rel(Parser *p) {
    Node *left = parse_shift(p);
    while (p_peek(p)->kind==TOK_LT || p_peek(p)->kind==TOK_GT ||
           p_peek(p)->kind==TOK_LE || p_peek(p)->kind==TOK_GE) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_shift(p);
        left = n;
    }
    return left;
}

static Node *parse_eq(Parser *p) {
    Node *left = parse_rel(p);
    while (p_peek(p)->kind==TOK_EQ || p_peek(p)->kind==TOK_NE) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_rel(p);
        left = n;
    }
    return left;
}

static Node *parse_bitand(Parser *p) {
    Node *left = parse_eq(p);
    while (p_peek(p)->kind==TOK_AMP) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_eq(p);
        left = n;
    }
    return left;
}

static Node *parse_bitxor(Parser *p) {
    Node *left = parse_bitand(p);
    while (p_peek(p)->kind==TOK_CARET) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_bitand(p);
        left = n;
    }
    return left;
}

static Node *parse_bitor(Parser *p) {
    Node *left = parse_bitxor(p);
    while (p_peek(p)->kind==TOK_PIPE) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_bitxor(p);
        left = n;
    }
    return left;
}

static Node *parse_and(Parser *p) {
    Node *left = parse_bitor(p);
    while (p_peek(p)->kind==TOK_AND) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_bitor(p);
        left = n;
    }
    return left;
}

static Node *parse_or(Parser *p) {
    Node *left = parse_and(p);
    while (p_peek(p)->kind==TOK_OR) {
        Token *op = p_advance(p);
        Node *n = new_node(NODE_BINOP, op->line, op->col);
        n->op = op->kind; n->left = left; n->right = parse_and(p);
        left = n;
    }
    return left;
}

static Node *parse_expr(Parser *p) {
    Node *left = parse_or(p);
    if (p_match(p, TOK_ASSIGN)) {
        Node *n = new_node(NODE_ASSIGN, left->line, left->col);
        n->left  = left;
        n->right = parse_expr(p);
        return n;
    }
    return left;
}

static Node *parse_var_decl(Parser *p, int expect_semi) {
    long long line = p_peek(p)->line;
    long long col  = p_peek(p)->col;
    p_expect(p, TOK_DEF, "def");
    Token *name = p_expect(p, TOK_IDENT, "variable name");
    p_expect(p, TOK_COLON, ":");

    int is_pub = 0, is_mut = 0;
    while (p_peek(p)->kind == TOK_PUB || p_peek(p)->kind == TOK_MUT) {
        if (p_match(p, TOK_PUB)) is_pub = 1;
        if (p_match(p, TOK_MUT)) is_mut = 1;
        p_expect(p, TOK_COMMA, ",");
    }
    KType *ty = parse_type(p);
    while (p_peek(p)->kind == TOK_COMMA &&
           (p_peek_at(p,1)->kind == TOK_PUB || p_peek_at(p,1)->kind == TOK_MUT)) {
        p_advance(p);
        if (p_match(p, TOK_PUB)) is_pub = 1;
        if (p_match(p, TOK_MUT)) is_mut = 1;
    }

    Node *init_e = NULL;
    if (p_match(p, TOK_ASSIGN)) init_e = parse_expr(p);
    if (expect_semi) p_expect(p, TOK_SEMI, ";");

    Node *n = new_node(NODE_VAR_DECL, line, col);
    n->var_name  = strdup(name->text);
    n->var_type  = ty;
    n->is_pub    = is_pub;
    n->is_mut    = is_mut;
    n->init_expr = init_e;
    return n;
}

static Node *parse_block(Parser *p) {
    long long line = p_peek(p)->line;
    long long col  = p_peek(p)->col;
    p_expect(p, TOK_LBRACE, "{");
    Node *blk = new_node(NODE_BLOCK, line, col);
    while (p_peek(p)->kind != TOK_RBRACE && p_peek(p)->kind != TOK_EOF) {
        Node *s = parse_stmt(p);
        if (s != NULL) {
            blk->stmts = realloc(blk->stmts, (blk->stmt_count+1)*sizeof(void*));
            blk->stmts[blk->stmt_count++] = s;
        }
    }
    p_expect(p, TOK_RBRACE, "}");
    return blk;
}

static void parse_class_def(Parser *p) {
    p_expect(p, TOK_CLASS, "class");
    Token *cname = p_expect(p, TOK_IDENT, "class name");
    ClassDef *cls = calloc(1, sizeof(ClassDef));
    cls->name = strdup(cname->text);
    classes[class_count++] = cls;

    p_expect(p, TOK_LBRACE, "{");
    long long offset = 0;
    while (p_peek(p)->kind != TOK_RBRACE && p_peek(p)->kind != TOK_EOF) {
        Token *fname = p_expect(p, TOK_IDENT, "field name");
        p_expect(p, TOK_COLON, ":");
        while (p_peek(p)->kind == TOK_PUB || p_peek(p)->kind == TOK_MUT) {
            p_advance(p); p_expect(p, TOK_COMMA, ",");
        }
        KType *fty = parse_type(p);
        p_expect(p, TOK_SEMI, ";");

        long long sz = ktype_size(fty);
        long long align = sz > 8 ? 8 : sz;
        if (align < 1) align = 1;
        if ((offset % align) != 0) offset += align - (offset % align);

        ClassField *cf = calloc(1, sizeof(ClassField));
        cf->name   = strdup(fname->text);
        cf->ftype  = fty;
        cf->offset = offset;
        cf->size   = sz;
        cls->fields = realloc(cls->fields, (cls->field_count+1)*sizeof(void*));
        cls->fields[cls->field_count++] = cf;
        offset += sz;
    }
    p_expect(p, TOK_RBRACE, "}");
    p_match(p, TOK_SEMI);
    if (offset == 0) offset = 8;
    else if ((offset % 8) != 0) offset += 8 - (offset % 8);
    cls->total_size = offset;
}

static void parse_enum_def(Parser *p) {
    p_expect(p, TOK_ENUM, "enum");
    Token *ename = p_expect(p, TOK_IDENT, "enum name");
    EnumDef *en = calloc(1, sizeof(EnumDef));
    en->name = strdup(ename->text);
    enums[enum_count++] = en;

    p_expect(p, TOK_LBRACE, "{");
    long long next_val = 0;
    while (p_peek(p)->kind != TOK_RBRACE && p_peek(p)->kind != TOK_EOF) {
        Token *vname = p_expect(p, TOK_IDENT, "enum variant name");
        if (p_match(p, TOK_ASSIGN)) {
            int neg = p_match(p, TOK_MINUS);
            Token *vt = p_expect(p, TOK_INT, "integer value");
            next_val = neg ? -vt->int_val : vt->int_val;
        }
        EnumVariant *ev = calloc(1, sizeof(EnumVariant));
        ev->name  = strdup(vname->text);
        ev->value = next_val++;
        en->variants = realloc(en->variants, (en->variant_count+1)*sizeof(void*));
        en->variants[en->variant_count++] = ev;
        if (!p_match(p, TOK_COMMA)) p_match(p, TOK_SEMI);
    }
    p_expect(p, TOK_RBRACE, "}");
    p_match(p, TOK_SEMI);
}

static Node *parse_stmt(Parser *p) {
    long long line = p_peek(p)->line;
    long long col  = p_peek(p)->col;

    if (p_peek(p)->kind == TOK_DEF) return parse_var_decl(p, 1);

    if (p_match(p, TOK_SET)) {
        KType *target = parse_type(p);
        p_expect(p, TOK_COMMA, ",");
        Token *alias_tok = p_expect(p, TOK_IDENT, "alias name");
        p_expect(p, TOK_SEMI, ";");
        TypeAlias *al = calloc(1, sizeof(TypeAlias));
        al->alias  = strdup(alias_tok->text);
        al->target = target;
        aliases[alias_count++] = al;
        return NULL;
    }
    if (p_match(p, TOK_RET)) {
        Node *n = new_node(NODE_RET, line, col);
        if (p_peek(p)->kind != TOK_SEMI) n->ret_expr = parse_expr(p);
        p_expect(p, TOK_SEMI, ";");
        return n;
    }
    if (p_match(p, TOK_BREAK))    { p_expect(p, TOK_SEMI, ";"); return new_node(NODE_BREAK, line, col); }
    if (p_match(p, TOK_CONTINUE)) { p_expect(p, TOK_SEMI, ";"); return new_node(NODE_CONTINUE, line, col); }

    if (p_match(p, TOK_IFD)) {
        Node *n = new_node(NODE_IFD, line, col);
        p_expect(p, TOK_LPAREN, "(");
        n->cond = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        p_expect(p, TOK_DO, ">>do");
        n->then_branch = parse_block(p);
        if (p_match(p, TOK_ELSE)) {
            if (p_peek(p)->kind == TOK_IFD) {
                n->else_branch = parse_stmt(p);
            } else {
                p_expect(p, TOK_DO, ">>do");
                n->else_branch = parse_block(p);
            }
        }
        return n;
    }
    if (p_match(p, TOK_WHILE)) {
        Node *n = new_node(NODE_WHILE, line, col);
        p_expect(p, TOK_LPAREN, "(");
        n->cond = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        p_expect(p, TOK_DO, ">>do");
        n->then_branch = parse_block(p);
        return n;
    }
    if (p_match(p, TOK_FOR)) {
        Node *n = new_node(NODE_FOR, line, col);
        p_expect(p, TOK_LPAREN, "(");
        if (p_peek(p)->kind == TOK_DEF) {
            n->for_init = parse_var_decl(p, 1);
        } else {
            if (p_peek(p)->kind != TOK_SEMI) n->for_init = parse_expr(p);
            p_expect(p, TOK_SEMI, ";");
        }
        if (p_peek(p)->kind != TOK_SEMI) n->cond = parse_expr(p);
        p_expect(p, TOK_SEMI, ";");
        if (p_peek(p)->kind != TOK_RPAREN) n->for_step = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        p_expect(p, TOK_DO, ">>do");
        n->then_branch = parse_block(p);
        return n;
    }
    if (p_match(p, TOK_SAFE)) {
        p_expect(p, TOK_DCOLON, "::");
        if (p_match(p, TOK_PRINT)) {
            p_expect(p, TOK_LBRACKET, "[");
            Token *fmt_tok = p_expect(p, TOK_STR, "format string");
            Node *n = new_node(NODE_PRINT, line, col);
            n->fmt = strdup(fmt_tok->text);
            while (p_match(p, TOK_COMMA)) {
                n->args = realloc(n->args, (n->arg_count+1)*sizeof(void*));
                n->args[n->arg_count++] = parse_expr(p);
            }
            p_expect(p, TOK_RBRACKET, "]");
            p_expect(p, TOK_SEMI, ";");
            return n;
        }
        if (p_match(p, TOK_GETIN)) {
            p_expect(p, TOK_LBRACKET, "[");
            Token *fmt_tok = p_expect(p, TOK_STR, "format string");
            Node *n = new_node(NODE_GETIN, line, col);
            n->fmt = strdup(fmt_tok->text);
            while (p_match(p, TOK_COMMA)) {
                n->args = realloc(n->args, (n->arg_count+1)*sizeof(void*));
                n->args[n->arg_count++] = parse_expr(p);
            }
            p_expect(p, TOK_RBRACKET, "]");
            p_expect(p, TOK_SEMI, ";");
            return n;
        }
        fprintf(k_stderr, "error (%s:%lld:%lld): unknown safe:: statement\n", p->file_path, line, col);
        exit(1);
    }
    if (p_match(p, TOK_RAW)) {
        p_expect(p, TOK_DCOLON, "::");
        if (p_match(p, TOK_ASM)) {
            p_expect(p, TOK_LBRACKET, "[");
            Token *asm_tok = p_expect(p, TOK_STR, "assembly string");
            p_expect(p, TOK_RBRACKET, "]");
            p_expect(p, TOK_SEMI, ";");
            Node *n = new_node(NODE_ASM, line, col);
            n->str_val = strdup(asm_tok->text);
            return n;
        }
        if (p_match(p, TOK_OUTB)) {
            p_expect(p, TOK_LBRACKET, "[");
            Node *n = new_node(NODE_OUTB, line, col);
            n->left = parse_expr(p);
            p_expect(p, TOK_COMMA, ",");
            n->right = parse_expr(p);
            p_expect(p, TOK_RBRACKET, "]");
            p_expect(p, TOK_SEMI, ";");
            return n;
        }
        if (p_match(p, TOK_OUTW)) {
            p_expect(p, TOK_LBRACKET, "[");
            Node *n = new_node(NODE_OUTW, line, col);
            n->left = parse_expr(p);
            p_expect(p, TOK_COMMA, ",");
            n->right = parse_expr(p);
            p_expect(p, TOK_RBRACKET, "]");
            p_expect(p, TOK_SEMI, ";");
            return n;
        }
        fprintf(k_stderr, "error (%s:%lld:%lld): unknown raw:: statement\n", p->file_path, line, col);
        exit(1);
    }
    Node *n = new_node(NODE_EXPR_STMT, line, col);
    n->left = parse_expr(p);
    p_expect(p, TOK_SEMI, ";");
    return n;
}

static Node *parse_func(Parser *p) {
    long long line = p_peek(p)->line;
    long long col  = p_peek(p)->col;
    p_expect(p, TOK_INIT, "init");
    p_expect(p, TOK_DCOLON, "::");
    int is_isr = 0;
    if (p_match(p, TOK_ISR)) is_isr = 1;
    else p_expect(p, TOK_EXEC, "exec");
    Token *name = p_expect(p, TOK_IDENT, "function name");
    p_expect(p, TOK_LBRACKET, "[");

    Node *fn = new_node(NODE_FUNC, line, col);
    fn->func_name = strdup(name->text);
    fn->is_isr = is_isr;

    if (p_match(p, TOK_SAFE)) {
        p_expect(p, TOK_DCOLON, "::");
        p_expect(p, TOK_NUL, "nul");
    } else if (p_match(p, TOK_NUL)) {
        /* no params */
    } else if (p_peek(p)->kind != TOK_RBRACKET) {
        int more = 1;
        while (more) {
            Token *pname = p_expect(p, TOK_IDENT, "param name");
            p_expect(p, TOK_COLON, ":");
            if (p_match(p, TOK_MUT)) p_expect(p, TOK_COMMA, ",");
            KType *pty = parse_type(p);
            Node *param = new_node(NODE_VAR_DECL, pname->line, pname->col);
            param->var_name = strdup(pname->text);
            param->var_type = pty;
            param->is_mut = 1;
            fn->params = realloc(fn->params, (fn->param_count+1)*sizeof(void*));
            fn->params[fn->param_count++] = param;
            more = p_match(p, TOK_COMMA);
        }
    }
    p_expect(p, TOK_RBRACKET, "]");
    p_expect(p, TOK_ARROW, "->");
    fn->ret_type = parse_type(p);
    fn->body = parse_block(p);
    return fn;
}

static char *read_header_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz+1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static void handle_unpack(Parser *p, Node *prog) {
    long long line = p_peek(p)->line;
    p_expect(p, TOK_LT, "<");
    char *mod = calloc(520, 1);
    while (p_peek(p)->kind != TOK_GT && p_peek(p)->kind != TOK_EOF) {
        Token *tok = p_advance(p);
        long long cur_len = strlen(mod);
        snprintf(mod + cur_len, 520 - cur_len, "%s", tok->text);
    }
    p_expect(p, TOK_GT, ">");
    p_expect(p, TOK_SEMI, ";");

    if (strcmp(mod,"Safe")==0 || strcmp(mod,"safe")==0) {
        if (!p->opt_default && !p->opt_ff && !p->opt_kernel) prog->has_safe = 1;
        free(mod); return;
    }
    if (k_ends_with(mod, ".k")) {
        fprintf(k_stderr, "error (%s:%lld): cannot unpack '.k' file '%s'\n", p->file_path, line, mod);
        exit(1);
    }
    char *fname = calloc(528, 1);
    if (k_ends_with(mod, ".kl")) snprintf(fname, 528, "%s", mod);
    else snprintf(fname, 528, "%s.kl", mod);

    char *resolved = calloc(1024, 1);
    char *slash = strrchr(p->file_path, '/');
    if (slash != NULL && fname[0] != '/') {
        long long dlen = slash - p->file_path;
        char *dir = strndup(p->file_path, dlen);
        snprintf(resolved, 1024, "%s/%s", dir, fname);
        free(dir);
    } else {
        snprintf(resolved, 1024, "%s", fname);
    }

    char *hsrc = read_header_file(resolved);
    if (hsrc == NULL) {
        snprintf(resolved, 1024, "%s", fname);
        hsrc = read_header_file(resolved);
    }
    if (hsrc == NULL && fname[0] != '/') {
        for (long long ii = 0; ii < inc_count && hsrc == NULL; ii++) {
            snprintf(resolved, 1024, "%s/%s", inc_paths[ii], fname);
            hsrc = read_header_file(resolved);
        }
    }
    if (hsrc == NULL) {
        fprintf(k_stderr, "error (%s:%lld): cannot open unpacked header '%s'\n", p->file_path, line, fname);
        exit(1);
    }

    for (long long i = 0; i < unpacked_count; i++) {
        if (strcmp(unpacked_paths[i], resolved) == 0) {
            free(mod); free(fname); free(resolved); free(hsrc); return;
        }
    }
    unpacked_paths[unpacked_count++] = strdup(resolved);

    CompilerOpts sub_opts = {0};
    sub_opts.flag_default = p->opt_default;
    sub_opts.flag_ff      = p->opt_ff;
    sub_opts.flag_kernel  = p->opt_kernel;
    parse_into_program(prog, resolved, hsrc, &sub_opts, 1);
    free(mod); free(fname); free(resolved);
}

void parse_into_program(Node *prog, char *file_path, char *src, CompilerOpts *opts, int is_header) {
    TokenList *tokens = lex_source(src);
    Parser *p = calloc(1, sizeof(Parser));
    p->tokens     = tokens;
    p->pos        = 0;
    p->opt_default = opts->flag_default;
    p->opt_ff      = opts->flag_ff;
    p->opt_kernel  = opts->flag_kernel;
    p->file_path   = file_path;

    while (p_peek(p)->kind != TOK_EOF) {
        if (p_match(p, TOK_UNPACK)) {
            handle_unpack(p, prog);
        } else if (p_match(p, TOK_ENTRY)) {
            p_expect(p, TOK_LT, "<");
            Token *e = p_expect(p, TOK_IDENT, "entry function");
            if (!is_header) {
                free(prog->entry_name);
                prog->entry_name = strdup(e->text);
            }
            p_expect(p, TOK_GT, ">");
            p_expect(p, TOK_SEMI, ";");
        } else if (p_peek(p)->kind == TOK_CLASS) {
            parse_class_def(p);
        } else if (p_peek(p)->kind == TOK_ENUM) {
            parse_enum_def(p);
        } else if (p_peek(p)->kind == TOK_IDENT && strcmp(p_peek(p)->text,"list")==0 &&
                   p_peek_at(p,1)->kind == TOK_DCOLON && p_peek_at(p,2)->kind == TOK_ENUM) {
            p_advance(p); p_advance(p);
            parse_enum_def(p);
        } else if (p_peek(p)->kind == TOK_SET) {
            parse_stmt(p);
        } else if (p_peek(p)->kind == TOK_DEF) {
            Node *g = parse_var_decl(p, 1);
            prog->globals = realloc(prog->globals, (prog->global_count+1)*sizeof(void*));
            prog->globals[prog->global_count++] = g;
        } else if (p_peek(p)->kind == TOK_INIT) {
            Node *fn = parse_func(p);
            prog->funcs = realloc(prog->funcs, (prog->func_count+1)*sizeof(void*));
            prog->funcs[prog->func_count++] = fn;
        } else {
            Token *tok = p_peek(p);
            fprintf(k_stderr, "error (%s:%lld:%lld): unexpected top-level token '%s'\n",
                    file_path, tok->line, tok->col, tok->text);
            exit(1);
        }
    }
    free(p);
}

Node *parse_program_file(char *path, char *src, CompilerOpts *opts) {
    alias_count    = 0;
    class_count    = 0;
    enum_count     = 0;
    unpacked_count = 0;

    Node *prog = new_node(NODE_PROGRAM, 1, 1);
    prog->entry_name = strdup("main");
    prog->has_safe   = 0;
    parse_into_program(prog, path, src, opts, 0);
    return prog;
}

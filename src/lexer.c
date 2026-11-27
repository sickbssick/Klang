/* lexer.c — tokenizer for the K language bootstrap compiler */
#include "kcc.h"

/* ---- Global compiler state definitions ---- */
FILE *k_stderr = NULL;
int cg_win = 0;
char **inc_paths = NULL;
long long inc_count = 0;
TypeAlias **aliases = NULL;
long long alias_count = 0;
ClassDef **classes = NULL;
long long class_count = 0;
EnumDef **enums = NULL;
long long enum_count = 0;
char **unpacked_paths = NULL;
long long unpacked_count = 0;

/* ---- AST helpers (from ast.kl) ---- */

void k_init_globals(void) {
    if (k_stderr == NULL) {
        k_stderr = fdopen(2, "w");
    }
    aliases = calloc(256, sizeof(void*));
    classes = calloc(256, sizeof(void*));
    enums = calloc(256, sizeof(void*));
    unpacked_paths = calloc(256, sizeof(void*));
    inc_paths = calloc(64, sizeof(void*));
}

KType *make_type(int kind, long long ptr_depth, long long array_len, char *cname) {
    KType *t = calloc(1, sizeof(KType));
    t->kind = kind;
    t->ptr_depth = ptr_depth;
    t->array_len = array_len;
    t->class_name = cname;
    return t;
}

KType *clone_type(KType *src) {
    if (src == NULL) return make_type(TY_UNKNOWN, 0, 0, NULL);
    return make_type(src->kind, src->ptr_depth, src->array_len, src->class_name);
}

Node *new_node(int kind, long long line, long long col) {
    Node *n = calloc(1, sizeof(Node));
    n->kind = kind;
    n->line = line;
    n->col = col;
    n->val_type = make_type(TY_UNKNOWN, 0, 0, NULL);
    return n;
}

int is_float_type(KType *t) {
    if (t == NULL) return 0;
    if (t->ptr_depth > 0 || t->array_len > 0) return 0;
    int k = t->kind;
    if (k == TY_F8 || k == TY_F16 || k == TY_F32 || k == TY_F64) return 1;
    return 0;
}

int is_int_like_type(KType *t) {
    if (t == NULL) return 0;
    if (t->ptr_depth > 0 || t->array_len > 0) return 0;
    int k = t->kind;
    if (k >= TY_I8 && k <= TY_U64) return 1;
    if (k == TY_BOOL || k == TY_CHAR || k == TY_ENUM) return 1;
    return 0;
}

int is_unsigned_type(KType *t) {
    if (t == NULL) return 0;
    if (t->ptr_depth > 0) return 1;
    int k = t->kind;
    if (k >= TY_U8 && k <= TY_U64) return 1;
    if (k == TY_BOOL || k == TY_CHAR) return 1;
    return 0;
}

ClassDef *find_class(char *name) {
    if (name == NULL) return NULL;
    for (long long i = 0; i < class_count; i++) {
        ClassDef *c = classes[i];
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

ClassField *find_class_field(ClassDef *cls, char *fname) {
    if (cls == NULL || fname == NULL) return NULL;
    for (long long i = 0; i < cls->field_count; i++) {
        ClassField *f = cls->fields[i];
        if (strcmp(f->name, fname) == 0) return f;
    }
    return NULL;
}

EnumDef *find_enum(char *name) {
    if (name == NULL) return NULL;
    for (long long i = 0; i < enum_count; i++) {
        EnumDef *e = enums[i];
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

int find_enum_value(char *ename, char *vname, long long *out_val) {
    EnumDef *e = find_enum(ename);
    if (e == NULL) return 0;
    for (long long i = 0; i < e->variant_count; i++) {
        EnumVariant *v = e->variants[i];
        if (strcmp(v->name, vname) == 0) {
            *out_val = v->value;
            return 1;
        }
    }
    return 0;
}

int find_bare_enum_value(char *vname, EnumDef **out_enum, long long *out_val) {
    for (long long i = 0; i < enum_count; i++) {
        EnumDef *e = enums[i];
        for (long long j = 0; j < e->variant_count; j++) {
            EnumVariant *v = e->variants[j];
            if (strcmp(v->name, vname) == 0) {
                if (out_enum != NULL) *out_enum = e;
                if (out_val != NULL) *out_val = v->value;
                return 1;
            }
        }
    }
    return 0;
}

long long ktype_size(KType *t) {
    if (t == NULL) return 8;
    if (t->array_len > 0) {
        KType elem = { t->kind, t->ptr_depth, 0, t->class_name };
        long long esz = ktype_size(&elem);
        return t->array_len * esz;
    }
    if (t->ptr_depth > 0) return 8;
    if (t->kind == TY_CLASS) {
        ClassDef *c = find_class(t->class_name);
        if (c != NULL) return c->total_size;
        return 8;
    }
    if (t->kind == TY_ENUM) return 4;
    int k = t->kind;
    if (k == TY_I8 || k == TY_U8 || k == TY_BOOL || k == TY_CHAR || k == TY_F8) return 1;
    if (k == TY_I16 || k == TY_U16 || k == TY_F16) return 2;
    if (k == TY_I32 || k == TY_U32 || k == TY_F32) return 4;
    return 8;
}

long long type_byte_width(KType *t) {
    if (t == NULL) return 8;
    if (t->ptr_depth > 0) return 8;
    if (t->kind == TY_ENUM) return 4;
    int k = t->kind;
    if (k == TY_I8 || k == TY_U8 || k == TY_BOOL || k == TY_CHAR || k == TY_F8) return 1;
    if (k == TY_I16 || k == TY_U16 || k == TY_F16) return 2;
    if (k == TY_I32 || k == TY_U32 || k == TY_F32) return 4;
    return 8;
}

char *ktype_str(KType *t, char *buf, long long sz) {
    if (t == NULL) { snprintf(buf, sz, "unknown"); return buf; }
    if (t->array_len > 0) {
        KType elem = { t->kind, t->ptr_depth, 0, t->class_name };
        char *ebuf = calloc(128, 1);
        ktype_str(&elem, ebuf, 128);
        snprintf(buf, sz, "[%lld]%s", t->array_len, ebuf);
        free(ebuf);
        return buf;
    }
    const char *base = "unknown";
    int k = t->kind;
    if (k == TY_VOID)        base = "void";
    else if (k == TY_I8)     base = "i8";
    else if (k == TY_I16)    base = "i16";
    else if (k == TY_I32)    base = "i32";
    else if (k == TY_I64)    base = "i64";
    else if (k == TY_U8)     base = "u8";
    else if (k == TY_U16)    base = "u16";
    else if (k == TY_U32)    base = "u32";
    else if (k == TY_U64)    base = "u64";
    else if (k == TY_F8)     base = "f8";
    else if (k == TY_F16)    base = "f16";
    else if (k == TY_F32)    base = "f32";
    else if (k == TY_F64)    base = "f64";
    else if (k == TY_BOOL)   base = "bool";
    else if (k == TY_CHAR)   base = "char";
    else if (k == TY_STR)    base = "safe::str";
    else if (k == TY_CLASS || k == TY_ENUM) {
        base = (t->class_name != NULL) ? t->class_name : "custom";
    }
    char stars[32];
    long long pos = 0;
    for (long long i = 0; i < t->ptr_depth && pos < 30; i++) {
        stars[pos++] = '*';
    }
    stars[pos] = 0;
    snprintf(buf, sz, "%s%s", stars, base);
    return buf;
}

/* ---- Lexer (from lexer.kl) ---- */

static void push_tok(TokenList *list, int kind, char *text, long long line, long long col, long long ival, double fval) {
    if (list->count >= list->cap) {
        if (list->cap == 0) list->cap = 1024;
        else list->cap *= 2;
        list->items = realloc(list->items, list->cap * sizeof(void*));
    }
    Token *t = calloc(1, sizeof(Token));
    t->kind = kind;
    t->text = text;
    t->line = line;
    t->col = col;
    t->int_val = ival;
    t->float_val = fval;
    list->items[list->count++] = t;
}

static int keyword_kind(char *s) {
    char c0 = s[0];
    if (c0 == 'd') { if (strcmp(s,"def")==0) return TOK_DEF; return TOK_IDENT; }
    if (c0 == 'i') {
        if (strcmp(s,"ifd")==0)  return TOK_IFD;
        if (strcmp(s,"init")==0) return TOK_INIT;
        if (strcmp(s,"isr")==0)  return TOK_ISR;
        if (strcmp(s,"inb")==0)  return TOK_INB;
        if (strcmp(s,"inw")==0)  return TOK_INW;
        return TOK_IDENT;
    }
    if (c0 == 'r') {
        if (strcmp(s,"ret")==0) return TOK_RET;
        if (strcmp(s,"raw")==0) return TOK_RAW;
        return TOK_IDENT;
    }
    if (c0 == 'e') {
        if (strcmp(s,"else")==0) return TOK_ELSE;
        if (strcmp(s,"enum")==0) return TOK_ENUM;
        if (strcmp(s,"exec")==0) return TOK_EXEC;
        return TOK_IDENT;
    }
    if (c0 == 'f') {
        if (strcmp(s,"for")==0)   return TOK_FOR;
        if (strcmp(s,"false")==0) return TOK_FALSE;
        return TOK_IDENT;
    }
    if (c0 == 's') {
        if (strcmp(s,"set")==0)    return TOK_SET;
        if (strcmp(s,"sizeof")==0) return TOK_SIZEOF;
        if (strcmp(s,"safe")==0)   return TOK_SAFE;
        return TOK_IDENT;
    }
    if (c0 == 'p') {
        if (strcmp(s,"pub")==0)   return TOK_PUB;
        if (strcmp(s,"print")==0) return TOK_PRINT;
        return TOK_IDENT;
    }
    if (c0 == 'c') {
        if (strcmp(s,"class")==0)    return TOK_CLASS;
        if (strcmp(s,"continue")==0) return TOK_CONTINUE;
        return TOK_IDENT;
    }
    if (c0 == 'b') { if (strcmp(s,"break")==0) return TOK_BREAK; return TOK_IDENT; }
    if (c0 == 'm') { if (strcmp(s,"mut")==0)   return TOK_MUT;   return TOK_IDENT; }
    if (c0 == 'n') { if (strcmp(s,"nul")==0)   return TOK_NUL;   return TOK_IDENT; }
    if (c0 == 'o') {
        if (strcmp(s,"outb")==0) return TOK_OUTB;
        if (strcmp(s,"outw")==0) return TOK_OUTW;
        return TOK_IDENT;
    }
    if (c0 == 'g') { if (strcmp(s,"getin")==0) return TOK_GETIN; return TOK_IDENT; }
    if (c0 == 'a') { if (strcmp(s,"asm")==0)   return TOK_ASM;   return TOK_IDENT; }
    if (c0 == 't') { if (strcmp(s,"true")==0)  return TOK_TRUE;  return TOK_IDENT; }
    if (c0 == 'w') { if (strcmp(s,"while")==0) return TOK_WHILE; return TOK_IDENT; }
    return TOK_IDENT;
}

static int is_hex_char(unsigned char c) {
    if (isdigit(c)) return 1;
    if (c >= 'a' && c <= 'f') return 1;
    if (c >= 'A' && c <= 'F') return 1;
    return 0;
}

int k_ends_with(char *s, char *suf) {
    long long ls = strlen(s);
    long long l  = strlen(suf);
    if (ls >= l && strcmp(s + ls - l, suf) == 0) return 1;
    return 0;
}

TokenList *lex_source(char *src) {
    TokenList *list = calloc(1, sizeof(TokenList));
    long long p = 0;
    long long line = 1;
    long long col  = 1;

    while (src[p] != 0) {
        unsigned char ch = src[p];
        if (ch == ' ' || ch == '\t' || ch == '\r') { p++; col++; continue; }
        if (ch == '\n') { p++; line++; col = 1; continue; }

        /* Line comment ;; */
        if (ch == ';' && src[p+1] == ';') {
            while (src[p] != 0 && src[p] != '\n') p++;
            continue;
        }
        /* Block comment ;* ... *; */
        if (ch == ';' && src[p+1] == '*') {
            p += 2; col += 2;
            while (src[p] != 0 && !(src[p] == '*' && src[p+1] == ';')) {
                if (src[p] == '\n') { line++; col = 1; } else col++;
                p++;
            }
            if (src[p] == '*') { p += 2; col += 2; }
            continue;
        }

        /* Directives */
        if (ch == '$') {
            if (strncmp(src+p, "$unpack->", 9) == 0) {
                push_tok(list, TOK_UNPACK, "$unpack->", line, col, 0, 0.0);
                p += 9; col += 9; continue;
            }
            if (strncmp(src+p, "$entry->", 8) == 0) {
                push_tok(list, TOK_ENTRY, "$entry->", line, col, 0, 0.0);
                p += 8; col += 8; continue;
            }
        }

        /* >>do token */
        if (ch == '>' && src[p+1] == '>') {
            if (src[p+2] == 'd' && src[p+3] == 'o' &&
                !(isalnum((unsigned char)src[p+4]) || src[p+4] == '_')) {
                push_tok(list, TOK_DO, ">>do", line, col, 0, 0.0);
                p += 4; col += 4; continue;
            }
        }

        /* String literal */
        if (ch == '"') {
            long long start_col = col;
            p++; col++;
            long long cap = 64, len = 0;
            char *buf = malloc(cap);
            while (src[p] != 0 && src[p] != '"') {
                unsigned char sc = src[p];
                if (sc == '\\' && src[p+1] != 0) {
                    p++; col++;
                    unsigned char esc = src[p];
                    if      (esc == 'n')  sc = '\n';
                    else if (esc == 't')  sc = '\t';
                    else if (esc == 'r')  sc = '\r';
                    else if (esc == '\\') sc = '\\';
                    else if (esc == '"')  sc = '"';
                    else if (esc == '0')  sc = 0;
                    else sc = esc;
                }
                if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = sc;
                p++; col++;
            }
            buf[len] = 0;
            if (src[p] == '"') { p++; col++; }
            push_tok(list, TOK_STR, buf, line, start_col, 0, 0.0);
            continue;
        }

        /* Char literal */
        if (ch == '\'') {
            long long start_col = col;
            p++; col++;
            unsigned char cv = 0;
            if (src[p] == '\\' && src[p+1] != 0) {
                p++; col++;
                unsigned char esc = src[p];
                if      (esc == 'n')  cv = '\n';
                else if (esc == 't')  cv = '\t';
                else if (esc == 'r')  cv = '\r';
                else if (esc == '\\') cv = '\\';
                else if (esc == '\'') cv = '\'';
                else if (esc == '0')  cv = 0;
                else cv = esc;
                p++; col++;
            } else if (src[p] != 0) {
                cv = src[p]; p++; col++;
            }
            if (src[p] == '\'') { p++; col++; }
            char *ctxt = calloc(4, 1);
            ctxt[0] = cv;
            push_tok(list, TOK_CHAR, ctxt, line, start_col, cv, 0.0);
            continue;
        }

        /* Numbers */
        if (isdigit(ch)) {
            long long start_col = col;
            long long start = p;
            /* hex */
            if (ch == '0' && (src[p+1] == 'x' || src[p+1] == 'X')) {
                p += 2; col += 2;
                unsigned long long val = 0;
                while (is_hex_char(src[p]) || src[p] == '_') {
                    if (src[p] != '_') {
                        unsigned char hc = src[p];
                        unsigned long long d = 0;
                        if      (hc >= '0' && hc <= '9') d = hc - '0';
                        else if (hc >= 'a' && hc <= 'f') d = hc - 'a' + 10;
                        else d = hc - 'A' + 10;
                        val = (val << 4) | d;
                    }
                    p++; col++;
                }
                push_tok(list, TOK_INT, strndup(src+start, p-start), line, start_col, (long long)val, 0.0);
                continue;
            }
            /* binary */
            if (ch == '0' && (src[p+1] == 'b' || src[p+1] == 'B')) {
                p += 2; col += 2;
                unsigned long long val = 0;
                while (src[p] == '0' || src[p] == '1' || src[p] == '_') {
                    if (src[p] != '_') val = (val << 1) | (src[p] - '0');
                    p++; col++;
                }
                push_tok(list, TOK_INT, strndup(src+start, p-start), line, start_col, (long long)val, 0.0);
                continue;
            }
            /* decimal / float */
            int is_flt = 0, has_us = 0;
            while (isdigit(src[p]) || src[p] == '_') {
                if (src[p] == '_') has_us = 1;
                p++; col++;
            }
            if (src[p] == '.' && isdigit((unsigned char)src[p+1])) {
                is_flt = 1; p++; col++;
                while (isdigit((unsigned char)src[p])) { p++; col++; }
            }
            char *txt = strndup(src+start, p-start);
            char *num_str = txt;
            char *clean = NULL;
            if (has_us) {
                clean = calloc(128, 1);
                long long ci = 0;
                for (long long i2 = 0; txt[i2] != 0 && ci < 127; i2++) {
                    if (txt[i2] != '_') clean[ci++] = txt[i2];
                }
                num_str = clean;
            }
            if (is_flt) {
                double fv = strtod(num_str, NULL);
                push_tok(list, TOK_FLOAT, txt, line, start_col, 0, fv);
            } else {
                long long iv = strtoll(num_str, NULL, 10);
                push_tok(list, TOK_INT, txt, line, start_col, iv, 0.0);
            }
            if (clean) free(clean);
            continue;
        }

        /* Identifiers / Keywords */
        if (isalpha(ch) || ch == '_') {
            long long start_col = col;
            long long start = p;
            while (isalnum((unsigned char)src[p]) || src[p] == '_') { p++; col++; }
            char *txt = strndup(src+start, p-start);
            int kk = keyword_kind(txt);
            push_tok(list, kk, txt, line, start_col, 0, 0.0);
            continue;
        }

        /* Two-character operators */
        unsigned char c2 = src[p+1];
        if (ch == ':' && c2 == ':') { push_tok(list, TOK_DCOLON, "::",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '-' && c2 == '>') { push_tok(list, TOK_ARROW,  "->",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '*' && c2 == '*') { push_tok(list, TOK_POW,    "**",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '+' && c2 == '+') { push_tok(list, TOK_INC,    "++",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '-' && c2 == '-') { push_tok(list, TOK_DEC,    "--",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '=' && c2 == '=') { push_tok(list, TOK_EQ,     "==",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '!' && c2 == '=') { push_tok(list, TOK_NE,     "!=",  line,col,0,0.0); p+=2; col+=2; continue; }
        if ((ch == '<' && c2 == '=') || (ch == '=' && c2 == '<')) { push_tok(list, TOK_LE, "<=", line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '>' && c2 == '=') { push_tok(list, TOK_GE,     ">=",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '<' && c2 == '<') { push_tok(list, TOK_SHL,    "<<",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '>' && c2 == '>') { push_tok(list, TOK_SHR,    ">>",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '&' && c2 == '&') { push_tok(list, TOK_AND,    "&&",  line,col,0,0.0); p+=2; col+=2; continue; }
        if (ch == '|' && c2 == '|') { push_tok(list, TOK_OR,     "||",  line,col,0,0.0); p+=2; col+=2; continue; }

        /* Single-character tokens */
        int sk = TOK_EOF;
        char *st = "";
        if      (ch == '(') { sk = TOK_LPAREN;   st = "("; }
        else if (ch == ')') { sk = TOK_RPAREN;   st = ")"; }
        else if (ch == '{') { sk = TOK_LBRACE;   st = "{"; }
        else if (ch == '}') { sk = TOK_RBRACE;   st = "}"; }
        else if (ch == '[') { sk = TOK_LBRACKET; st = "["; }
        else if (ch == ']') { sk = TOK_RBRACKET; st = "]"; }
        else if (ch == ':') { sk = TOK_COLON;    st = ":"; }
        else if (ch == ';') { sk = TOK_SEMI;     st = ";"; }
        else if (ch == ',') { sk = TOK_COMMA;    st = ","; }
        else if (ch == '.') { sk = TOK_DOT;      st = "."; }
        else if (ch == '=') { sk = TOK_ASSIGN;   st = "="; }
        else if (ch == '+') { sk = TOK_PLUS;     st = "+"; }
        else if (ch == '-') { sk = TOK_MINUS;    st = "-"; }
        else if (ch == '*') { sk = TOK_STAR;     st = "*"; }
        else if (ch == '/') { sk = TOK_SLASH;    st = "/"; }
        else if (ch == '%') { sk = TOK_PERCENT;  st = "%"; }
        else if (ch == '<') { sk = TOK_LT;       st = "<"; }
        else if (ch == '>') { sk = TOK_GT;       st = ">"; }
        else if (ch == '!') { sk = TOK_NOT;      st = "!"; }
        else if (ch == '&') { sk = TOK_AMP;      st = "&"; }
        else if (ch == '|') { sk = TOK_PIPE;     st = "|"; }
        else if (ch == '^') { sk = TOK_CARET;    st = "^"; }
        else if (ch == '~') { sk = TOK_TILDE;    st = "~"; }
        else if (ch == '@') { sk = TOK_AT;       st = "@"; }
        else {
            fprintf(k_stderr, "kcc: lexer error at line %lld, col %lld: unexpected char '%c'\n", line, col, ch);
            exit(1);
        }
        push_tok(list, sk, st, line, col, 0, 0.0);
        p++; col++;
    }

    push_tok(list, TOK_EOF, "EOF", line, col, 0, 0.0);
    return list;
}

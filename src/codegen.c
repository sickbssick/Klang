#define _POSIX_C_SOURCE 200809L
#include "kcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    KType type;
    bool is_mut;
    bool is_global;
    int offset;    
   int reg_idx;   
} VarSym;

typedef struct {
    char *label;
    char *str;
} StrLit;

typedef struct {
    char *label;
    double val;
} FloatLit;

static const char *callee_regs[5] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};

static FILE *out;
static int label_seq = 0;
static VarSym vars[2048];
static size_t var_count = 0;
static int stack_offset = 40;
static int next_reg = 0;
static const char *addr_taken_names[512];
static size_t addr_taken_count = 0;
static bool current_has_safe = false;

static int break_labels[64];
static int cont_labels[64];
static int loop_depth = 0;

static StrLit str_lits[4096];
static size_t str_count = 0;
static FloatLit float_lits[512];
static size_t float_count = 0;

static int new_label(void) { return label_seq++; }

static VarSym *find_var(const char *name) {
    for (int i = (int)var_count - 1; i >= 0; i--) {
        if (strcmp(vars[i].name, name) == 0) return &vars[i];
    }
    return NULL;
}

static bool is_addr_taken(const char *name) {
    for (size_t i = 0; i < addr_taken_count; i++) {
        if (strcmp(addr_taken_names[i], name) == 0) return true;
    }
    return false;
}

static void collect_addr_taken(Node *n) {
    if (!n) return;
    if (n->kind == NODE_GETIN) {
        for (size_t i = 0; i < n->arg_count; i++) {
            if (n->args[i]->kind == NODE_VAR && addr_taken_count < 256) {
                addr_taken_names[addr_taken_count++] = n->args[i]->var_name;
            }
        }
    } else if (n->kind == NODE_ADDR && n->left && n->left->kind == NODE_VAR) {
        if (addr_taken_count < 256) {
            addr_taken_names[addr_taken_count++] = n->left->var_name;
        }
    }
    collect_addr_taken(n->init);
    collect_addr_taken(n->ret_expr);
    collect_addr_taken(n->cond);
    collect_addr_taken(n->then_branch);
    collect_addr_taken(n->else_branch);
    collect_addr_taken(n->for_init);
    collect_addr_taken(n->for_step);
    collect_addr_taken(n->left);
    collect_addr_taken(n->right);
    for (size_t i = 0; i < n->stmt_count; i++) collect_addr_taken(n->stmts[i]);
    for (size_t i = 0; i < n->arg_count; i++) collect_addr_taken(n->args[i]);
}

static const char *add_str_lit(const char *s) {
    char buf[32];
    snprintf(buf, sizeof(buf), ".LC_str_%zu", str_count);
    str_lits[str_count].label = strdup(buf);
    str_lits[str_count].str = strdup(s);
    return str_lits[str_count++].label;
}

static const char *add_float_lit(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), ".LC_flt_%zu", float_count);
    float_lits[float_count].label = strdup(buf);
    float_lits[float_count].val = v;
    return float_lits[float_count++].label;
}

static void var_loc(const VarSym *v, char *buf, size_t sz) {
    if (v->reg_idx >= 0) snprintf(buf, sz, "%s", callee_regs[v->reg_idx]);
    else if (v->is_global) snprintf(buf, sz, "g_%s(%%rip)", v->name);
    else snprintf(buf, sz, "-%d(%%rbp)", v->offset);
}

static bool fits_imm32(long long v) {
    return v >= -2147483648LL && v <= 2147483647LL;
}

static bool is_const_int(Node *n, long long *val) {
    if (!n) return false;
    if (n->kind == NODE_INT || n->kind == NODE_CHAR || n->kind == NODE_BOOL) {
        *val = n->int_val;
        return true;
    }
    if (n->kind == NODE_SIZEOF) {
        *val = ktype_size(n->sizeof_type);
        return true;
    }
    if (n->kind == NODE_UNOP && is_const_int(n->left, val)) {
        if (n->op == TOK_MINUS) { *val = -(*val); return true; }
        if (n->op == TOK_TILDE) { *val = ~(*val); return true; }
    }
    if (n->kind == NODE_BINOP) {
        long long l, r;
        if (is_const_int(n->left, &l) && is_const_int(n->right, &r)) {
            switch (n->op) {
                case TOK_PLUS: *val = l + r; return true;
                case TOK_MINUS: *val = l - r; return true;
                case TOK_STAR: *val = l * r; return true;
                case TOK_SLASH: if (r != 0) { *val = l / r; return true; } break;
                case TOK_PERCENT: if (r != 0) { *val = l % r; return true; } break;
                case TOK_SHL: *val = l << r; return true;
                case TOK_SHR: *val = (long long)((unsigned long long)l >> r); return true;
                case TOK_AMP: *val = l & r; return true;
                case TOK_PIPE: *val = l | r; return true;
                case TOK_CARET: *val = l ^ r; return true;
                case TOK_POW: {
                    long long res = 1;
                    for (long long k = 0; k < r; k++) res *= l;
                    *val = res;
                    return true;
                }
                default: break;
            }
        }
    }
    return false;
}

static bool is_simple_leaf(Node *n) {
    return n->kind == NODE_INT || n->kind == NODE_CHAR ||
           n->kind == NODE_BOOL || n->kind == NODE_SIZEOF || n->kind == NODE_VAR;
}

static KType infer_expr_type(Node *n) {
    if (!n) return (KType){TY_I64, 0, 0, NULL};
    if (n->val_type.kind != TY_VOID && n->val_type.kind != TY_UNKNOWN) return n->val_type;
    if (n->kind == NODE_VAR) {
        VarSym *v = find_var(n->var_name);
        if (v) return v->type;
    }
    if (n->kind == NODE_DEREF) {
        KType t = infer_expr_type(n->left);
        if (t.ptr_depth > 0) { t.ptr_depth--; return t; }
        if (t.kind == TY_STR) return (KType){TY_U8, 0, 0, NULL};
    }
    if (n->kind == NODE_INDEX) {
        KType t = infer_expr_type(n->left);
        if (t.array_len > 0) { t.array_len = 0; return t; }
        if (t.ptr_depth > 0) { t.ptr_depth--; return t; }
        if (t.kind == TY_STR) return (KType){TY_U8, 0, 0, NULL};
    }
    if (n->kind == NODE_CALL && n->arg_count == 1) {
        VarSym *pv = find_var(n->call_name);
        if (pv) {
            if (pv->type.array_len > 0) { KType t = pv->type; t.array_len = 0; return t; }
            if (pv->type.ptr_depth > 0) { KType t = pv->type; t.ptr_depth--; return t; }
            if (pv->type.kind == TY_STR) return (KType){TY_U8, 0, 0, NULL};
        }
    }
    if (n->kind == NODE_MEMBER) {
        KType base = infer_expr_type(n->left);
        if (base.kind == TY_CLASS) {
            ClassDef *cls = find_class(base.class_name);
            ClassField *f = find_class_field(cls, n->member_name);
            if (f) return f->type;
        }
    }
    return n->val_type.kind ? n->val_type : (KType){TY_I64, 0, 0, NULL};
}

static void emit_expr(Node *n);

static void emit_as_double(Node *n) {
    emit_expr(n);
    if (!is_float_type(infer_expr_type(n))) {
        fprintf(out, "    cvtsi2sdq %%rax, %%xmm0\n");
    }
}

static void emit_load_into(Node *n, const char *reg) {
    long long cv;
    if (is_const_int(n, &cv)) {
        if (cv == 0) fprintf(out, "    xorl %s, %s\n",
                             strcmp(reg, "%rax") == 0 ? "%eax" : "%ecx",
                             strcmp(reg, "%rax") == 0 ? "%eax" : "%ecx");
        else fprintf(out, "    movq $%lld, %s\n", cv, reg);
        return;
    }
    if (n->kind == NODE_VAR) {
        VarSym *v = find_var(n->var_name);
        if (v && v->type.array_len == 0 && !(v->type.kind == TY_CLASS && v->type.ptr_depth == 0)) {
            char loc[64];
            var_loc(v, loc, sizeof(loc));
            fprintf(out, "    movq %s, %s\n", loc, reg);
            return;
        }
    }
    emit_expr(n);
    if (strcmp(reg, "%rax") != 0) {
        fprintf(out, "    movq %%rax, %s\n", reg);
    }
}

static void emit_addr(Node *n) {
    if (n->kind == NODE_VAR) {
        VarSym *v = find_var(n->var_name);
        if (!v) {
            fprintf(stderr, "kcc: undefined variable '%s' at line %d\n", n->var_name, n->line);
            exit(1);
        }
        if (v->is_global) fprintf(out, "    lea g_%s(%%rip), %%rax\n", v->name);
        else fprintf(out, "    lea -%d(%%rbp), %%rax\n", v->offset);
        return;
    }
    if (n->kind == NODE_MEMBER) {
        KType base = infer_expr_type(n->left);
        ClassDef *cls = find_class(base.class_name);
        ClassField *f = find_class_field(cls, n->member_name);
        if (base.ptr_depth > 0) emit_expr(n->left);
        else emit_addr(n->left);
        if (f->offset != 0) fprintf(out, "    addq $%d, %%rax\n", f->offset);
        return;
    }
    if (n->kind == NODE_INDEX) {
        KType pt = infer_expr_type(n);
        int w = is_float_type(pt) ? 8 : (pt.kind == TY_CLASS && pt.ptr_depth == 0 ? ktype_size(pt) : type_byte_width(pt));
        emit_expr(n->left);
        fprintf(out, "    pushq %%rax\n");
        emit_expr(n->right);
        if (w > 1) fprintf(out, "    imulq $%d, %%rax\n", w);
        fprintf(out, "    popq %%rcx\n    addq %%rcx, %%rax\n");
        return;
    }
    if (n->kind == NODE_CALL && n->arg_count == 1) {
        VarSym *pv = find_var(n->call_name);
        if (pv && (pv->type.array_len > 0 || pv->type.ptr_depth > 0 || pv->type.kind == TY_STR)) {
            KType elem = pv->type;
            if (elem.array_len > 0) elem.array_len = 0;
            else if (elem.ptr_depth > 0) elem.ptr_depth--;
            else elem = (KType){TY_U8, 0, 0, NULL};
            int w = is_float_type(elem) ? 8 : (elem.kind == TY_CLASS && elem.ptr_depth == 0 ? ktype_size(elem) : type_byte_width(elem));
            emit_expr(n->args[0]);
            if (w > 1) fprintf(out, "    imulq $%d, %%rax\n", w);
            if (pv->type.array_len > 0) {
                fprintf(out, "    pushq %%rax\n");
                if (pv->is_global) fprintf(out, "    lea g_%s(%%rip), %%rax\n", pv->name);
                else fprintf(out, "    lea -%d(%%rbp), %%rax\n", pv->offset);
                fprintf(out, "    popq %%rcx\n    addq %%rcx, %%rax\n");
            } else {
                char loc[64];
                var_loc(pv, loc, sizeof(loc));
                fprintf(out, "    addq %s, %%rax\n", loc);
            }
            return;
        }
    }
    fprintf(stderr, "kcc: expected addressable lvalue at line %d\n", n->line);
    exit(1);
}

static void emit_mem_read(KType t) {
    if (is_float_type(t)) {
        fprintf(out, "    movsd (%%rax), %%xmm0\n");
        return;
    }
    int width = type_byte_width(t);
    if (width == 1) fprintf(out, t.kind == TY_I8 && t.ptr_depth == 0 ? "    movsbq (%%rax), %%rax\n" : "    movzbq (%%rax), %%rax\n");
    else if (width == 2) fprintf(out, t.kind == TY_I16 && t.ptr_depth == 0 ? "    movswq (%%rax), %%rax\n" : "    movzwq (%%rax), %%rax\n");
    else if (width == 4) fprintf(out, (t.kind == TY_I32 || t.kind == TY_ENUM) && t.ptr_depth == 0 ? "    movslq (%%rax), %%rax\n" : "    movl (%%rax), %%eax\n");
    else fprintf(out, "    movq (%%rax), %%rax\n");
}

static void emit_mem_write(KType t, const char *addr_reg) {
    if (is_float_type(t)) {
        fprintf(out, "    movsd %%xmm0, (%s)\n", addr_reg);
        return;
    }
    int width = type_byte_width(t);
    if (width == 1) fprintf(out, "    movb %%al, (%s)\n", addr_reg);
    else if (width == 2) fprintf(out, "    movw %%ax, (%s)\n", addr_reg);
    else if (width == 4) fprintf(out, "    movl %%eax, (%s)\n", addr_reg);
    else fprintf(out, "    movq %%rax, (%s)\n", addr_reg);
}

static void emit_expr(Node *n) {
    long long cv;
    if (is_const_int(n, &cv)) {
        if (cv == 0) fprintf(out, "    xorl %%eax, %%eax\n");
        else fprintf(out, "    movq $%lld, %%rax\n", cv);
        return;
    }

    switch (n->kind) {
        case NODE_FLOAT: {
            const char *lbl = add_float_lit(n->float_val);
            fprintf(out, "    movsd %s(%%rip), %%xmm0\n", lbl);
            break;
        }
        case NODE_STR: {
            const char *lbl = add_str_lit(n->str_val);
            fprintf(out, "    lea %s(%%rip), %%rax\n", lbl);
            break;
        }
        case NODE_VAR: {
            VarSym *v = find_var(n->var_name);
            if (!v) {
                fprintf(stderr, "kcc: undefined variable '%s' at line %d\n", n->var_name, n->line);
                exit(1);
            }
            n->val_type = v->type;
            if (v->type.array_len > 0 || (v->type.kind == TY_CLASS && v->type.ptr_depth == 0)) {
                emit_addr(n);
                break;
            }
            char loc[64];
            var_loc(v, loc, sizeof(loc));
            if (is_float_type(v->type)) fprintf(out, "    movsd %s, %%xmm0\n", loc);
            else fprintf(out, "    movq %s, %%rax\n", loc);
            break;
        }
        case NODE_ADDR:
            emit_addr(n->left);
            break;
        case NODE_DEREF: {
            KType pt = infer_expr_type(n);
            emit_expr(n->left);
            emit_mem_read(pt);
            break;
        }
        case NODE_INDEX: {
            KType pt = infer_expr_type(n);
            int w = is_float_type(pt) ? 8 : (pt.kind == TY_CLASS && pt.ptr_depth == 0 ? ktype_size(pt) : type_byte_width(pt));
            emit_expr(n->left);
            fprintf(out, "    pushq %%rax\n");
            emit_expr(n->right);
            if (w > 1) fprintf(out, "    imulq $%d, %%rax\n", w);
            fprintf(out, "    popq %%rcx\n    addq %%rcx, %%rax\n");
            if (!(pt.kind == TY_CLASS && pt.ptr_depth == 0)) emit_mem_read(pt);
            break;
        }
        case NODE_MEMBER: {
            KType ft = infer_expr_type(n);
            emit_addr(n);
            emit_mem_read(ft);
            break;
        }
        case NODE_INB:
            emit_expr(n->left);
            fprintf(out, "    movq %%rax, %%rdx\n    xorl %%eax, %%eax\n    inb %%dx, %%al\n");
            break;
        case NODE_INW:
            emit_expr(n->left);
            fprintf(out, "    movq %%rax, %%rdx\n    xorl %%eax, %%eax\n    inw %%dx, %%ax\n");
            break;
        case NODE_ASSIGN: {
            if (n->left->kind == NODE_MEMBER) {
                KType ft = infer_expr_type(n->left);
                emit_addr(n->left);
                fprintf(out, "    pushq %%rax\n");
                if (is_float_type(ft)) emit_as_double(n->right);
                else emit_expr(n->right);
                fprintf(out, "    popq %%rdx\n");
                emit_mem_write(ft, "%rdx");
                break;
            }
            if (n->left->kind == NODE_DEREF) {
                KType pt = infer_expr_type(n->left);
                emit_expr(n->left->left);
                fprintf(out, "    pushq %%rax\n");
                if (is_float_type(pt)) emit_as_double(n->right);
                else emit_expr(n->right);
                fprintf(out, "    popq %%rdx\n");
                emit_mem_write(pt, "%rdx");
                break;
            }
            if (n->left->kind == NODE_INDEX) {
                KType pt = infer_expr_type(n->left);
                int w = is_float_type(pt) ? 8 : type_byte_width(pt);
                emit_expr(n->left->left);
                fprintf(out, "    pushq %%rax\n");
                emit_expr(n->left->right);
                if (w > 1) fprintf(out, "    imulq $%d, %%rax\n", w);
                fprintf(out, "    popq %%rcx\n    addq %%rcx, %%rax\n    pushq %%rax\n");
                if (is_float_type(pt)) emit_as_double(n->right);
                else emit_expr(n->right);
                fprintf(out, "    popq %%rdx\n");
                emit_mem_write(pt, "%rdx");
                break;
            }
            if (n->left->kind == NODE_CALL && n->left->arg_count == 1) {
                VarSym *pv = find_var(n->left->call_name);
                if (pv && (pv->type.array_len > 0 || pv->type.ptr_depth > 0 || pv->type.kind == TY_STR)) {
                    KType elem = pv->type;
                    if (elem.array_len > 0) elem.array_len = 0;
                    else if (elem.ptr_depth > 0) elem.ptr_depth--;
                    else elem = (KType){TY_U8, 0, 0, NULL};
                    int w = is_float_type(elem) ? 8 : type_byte_width(elem);
                    emit_expr(n->left->args[0]);
                    if (w > 1) fprintf(out, "    imulq $%d, %%rax\n", w);
                    if (pv->type.array_len > 0) {
                        fprintf(out, "    pushq %%rax\n");
                        if (pv->is_global) fprintf(out, "    lea g_%s(%%rip), %%rax\n", pv->name);
                        else fprintf(out, "    lea -%d(%%rbp), %%rax\n", pv->offset);
                        fprintf(out, "    popq %%rcx\n    addq %%rcx, %%rax\n    pushq %%rax\n");
                    } else {
                        char loc[64];
                        var_loc(pv, loc, sizeof(loc));
                        fprintf(out, "    addq %s, %%rax\n    pushq %%rax\n", loc);
                    }
                    if (is_float_type(elem)) emit_as_double(n->right);
                    else emit_expr(n->right);
                    fprintf(out, "    popq %%rdx\n");
                    emit_mem_write(elem, "%rdx");
                    break;
                }
            }
            VarSym *v = find_var(n->left->var_name);
            char loc[64];
            var_loc(v, loc, sizeof(loc));
            if (is_float_type(v->type)) {
                emit_as_double(n->right);
                fprintf(out, "    movsd %%xmm0, %s\n", loc);
            } else {
                emit_expr(n->right);
                fprintf(out, "    movq %%rax, %s\n", loc);
            }
            break;
        }
        case NODE_POSTOP: {
            if (n->left->kind == NODE_VAR) {
                VarSym *v = find_var(n->left->var_name);
                char loc[64];
                var_loc(v, loc, sizeof(loc));
                fprintf(out, "    movq %s, %%rax\n", loc);
                fprintf(out, "    %s %s\n", n->op == TOK_INC ? "incq" : "decq", loc);
            } else {
                KType lt = infer_expr_type(n->left);
                emit_addr(n->left);
                fprintf(out, "    pushq %%rax\n");
                emit_mem_read(lt);
                fprintf(out, "    movq %%rax, %%rcx\n");
                fprintf(out, "    %s %%rax\n", n->op == TOK_INC ? "incq" : "decq");
                fprintf(out, "    popq %%rdx\n");
                emit_mem_write(lt, "%rdx");
                fprintf(out, "    movq %%rcx, %%rax\n");
            }
            break;
        }
        case NODE_UNOP:
            if (n->op == TOK_MINUS && is_float_type(infer_expr_type(n->left))) {
                emit_as_double(n->left);
                fprintf(out, "    movabsq $-9223372036854775808, %%rax\n");
                fprintf(out, "    movq %%rax, %%xmm1\n    xorpd %%xmm1, %%xmm0\n");
                break;
            }
            emit_expr(n->left);
            if (n->op == TOK_MINUS) fprintf(out, "    negq %%rax\n");
            else if (n->op == TOK_TILDE) fprintf(out, "    notq %%rax\n");
            else if (n->op == TOK_NOT) {
                fprintf(out, "    testq %%rax, %%rax\n    sete %%al\n    movzbq %%al, %%rax\n");
            }
            break;
        case NODE_BINOP: {
            if (n->op == TOK_AND) {
                int l_false = new_label(), l_end = new_label();
                emit_expr(n->left);
                fprintf(out, "    testq %%rax, %%rax\n    je .L%d\n", l_false);
                emit_expr(n->right);
                fprintf(out, "    testq %%rax, %%rax\n    je .L%d\n", l_false);
                fprintf(out, "    movl $1, %%eax\n    jmp .L%d\n", l_end);
                fprintf(out, ".L%d:\n    xorl %%eax, %%eax\n.L%d:\n", l_false, l_end);
                break;
            }
            if (n->op == TOK_OR) {
                int l_true = new_label(), l_end = new_label();
                emit_expr(n->left);
                fprintf(out, "    testq %%rax, %%rax\n    jne .L%d\n", l_true);
                emit_expr(n->right);
                fprintf(out, "    testq %%rax, %%rax\n    jne .L%d\n", l_true);
                fprintf(out, "    xorl %%eax, %%eax\n    jmp .L%d\n", l_end);
                fprintf(out, ".L%d:\n    movl $1, %%eax\n.L%d:\n", l_true, l_end);
                break;
            }

            if (is_float_type(infer_expr_type(n->left)) || is_float_type(infer_expr_type(n->right))) {
                emit_as_double(n->left);
                fprintf(out, "    subq $8, %%rsp\n    movsd %%xmm0, (%%rsp)\n");
                emit_as_double(n->right);
                fprintf(out, "    movsd %%xmm0, %%xmm1\n    movsd (%%rsp), %%xmm0\n    addq $8, %%rsp\n");
                switch (n->op) {
                    case TOK_PLUS: fprintf(out, "    addsd %%xmm1, %%xmm0\n"); break;
                    case TOK_MINUS: fprintf(out, "    subsd %%xmm1, %%xmm0\n"); break;
                    case TOK_STAR: fprintf(out, "    mulsd %%xmm1, %%xmm0\n"); break;
                    case TOK_SLASH: fprintf(out, "    divsd %%xmm1, %%xmm0\n"); break;
                    case TOK_EQ: fprintf(out, "    ucomisd %%xmm1, %%xmm0\n    sete %%al\n    movzbq %%al, %%rax\n"); break;
                    case TOK_NE: fprintf(out, "    ucomisd %%xmm1, %%xmm0\n    setne %%al\n    movzbq %%al, %%rax\n"); break;
                    case TOK_LT: fprintf(out, "    ucomisd %%xmm1, %%xmm0\n    setb %%al\n    movzbq %%al, %%rax\n"); break;
                    case TOK_GT: fprintf(out, "    ucomisd %%xmm1, %%xmm0\n    seta %%al\n    movzbq %%al, %%rax\n"); break;
                    case TOK_LE: fprintf(out, "    ucomisd %%xmm1, %%xmm0\n    setbe %%al\n    movzbq %%al, %%rax\n"); break;
                    case TOK_GE: fprintf(out, "    ucomisd %%xmm1, %%xmm0\n    setae %%al\n    movzbq %%al, %%rax\n"); break;
                    default: break;
                }
                break;
            }

            long long imm_r;
            if (is_const_int(n->right, &imm_r) && fits_imm32(imm_r) &&
                (n->op == TOK_PLUS || n->op == TOK_MINUS || n->op == TOK_AMP ||
                 n->op == TOK_PIPE || n->op == TOK_CARET || n->op == TOK_SHL ||
                 n->op == TOK_SHR || n->op == TOK_EQ || n->op == TOK_NE ||
                 n->op == TOK_LT || n->op == TOK_GT || n->op == TOK_LE || n->op == TOK_GE)) {
                emit_expr(n->left);
                switch (n->op) {
                    case TOK_PLUS: fprintf(out, "    addq $%lld, %%rax\n", imm_r); break;
                    case TOK_MINUS: fprintf(out, "    subq $%lld, %%rax\n", imm_r); break;
                    case TOK_AMP: fprintf(out, "    andq $%lld, %%rax\n", imm_r); break;
                    case TOK_PIPE: fprintf(out, "    orq $%lld, %%rax\n", imm_r); break;
                    case TOK_CARET: fprintf(out, "    xorq $%lld, %%rax\n", imm_r); break;
                    case TOK_SHL: fprintf(out, "    shlq $%lld, %%rax\n", imm_r); break;
                    case TOK_SHR: fprintf(out, "    shrq $%lld, %%rax\n", imm_r); break;
                    case TOK_EQ: fprintf(out, "    cmpq $%lld, %%rax\n    sete %%al\n    movzbq %%al, %%rax\n", imm_r); break;
                    case TOK_NE: fprintf(out, "    cmpq $%lld, %%rax\n    setne %%al\n    movzbq %%al, %%rax\n", imm_r); break;
                    case TOK_LT: fprintf(out, "    cmpq $%lld, %%rax\n    setl %%al\n    movzbq %%al, %%rax\n", imm_r); break;
                    case TOK_GT: fprintf(out, "    cmpq $%lld, %%rax\n    setg %%al\n    movzbq %%al, %%rax\n", imm_r); break;
                    case TOK_LE: fprintf(out, "    cmpq $%lld, %%rax\n    setle %%al\n    movzbq %%al, %%rax\n", imm_r); break;
                    case TOK_GE: fprintf(out, "    cmpq $%lld, %%rax\n    setge %%al\n    movzbq %%al, %%rax\n", imm_r); break;
                    default: break;
                }
                break;
            }

            if (is_simple_leaf(n->right)) {
                emit_expr(n->left);
                emit_load_into(n->right, "%rcx");
            } else {
                emit_expr(n->left);
                fprintf(out, "    pushq %%rax\n");
                emit_expr(n->right);
                fprintf(out, "    movq %%rax, %%rcx\n    popq %%rax\n");
            }

            switch (n->op) {
                case TOK_PLUS: fprintf(out, "    addq %%rcx, %%rax\n"); break;
                case TOK_MINUS: fprintf(out, "    subq %%rcx, %%rax\n"); break;
                case TOK_STAR: fprintf(out, "    imulq %%rcx, %%rax\n"); break;
                case TOK_SLASH: fprintf(out, "    cqto\n    idivq %%rcx\n"); break;
                case TOK_PERCENT: fprintf(out, "    cqto\n    idivq %%rcx\n    movq %%rdx, %%rax\n"); break;
                case TOK_AMP: fprintf(out, "    andq %%rcx, %%rax\n"); break;
                case TOK_PIPE: fprintf(out, "    orq %%rcx, %%rax\n"); break;
                case TOK_CARET: fprintf(out, "    xorq %%rcx, %%rax\n"); break;
                case TOK_SHL: fprintf(out, "    shlq %%cl, %%rax\n"); break;
                case TOK_SHR: fprintf(out, "    shrq %%cl, %%rax\n"); break;
                case TOK_POW: {
                    int l_loop = new_label(), l_done = new_label();
                    fprintf(out, "    movq %%rax, %%rsi\n    movl $1, %%eax\n");
                    fprintf(out, ".L%d:\n    testq %%rcx, %%rcx\n    jle .L%d\n", l_loop, l_done);
                    fprintf(out, "    imulq %%rsi, %%rax\n    decq %%rcx\n    jmp .L%d\n.L%d:\n", l_loop, l_done);
                    break;
                }
                case TOK_EQ: fprintf(out, "    cmpq %%rcx, %%rax\n    sete %%al\n    movzbq %%al, %%rax\n"); break;
                case TOK_NE: fprintf(out, "    cmpq %%rcx, %%rax\n    setne %%al\n    movzbq %%al, %%rax\n"); break;
                case TOK_LT: fprintf(out, "    cmpq %%rcx, %%rax\n    setl %%al\n    movzbq %%al, %%rax\n"); break;
                case TOK_GT: fprintf(out, "    cmpq %%rcx, %%rax\n    setg %%al\n    movzbq %%al, %%rax\n"); break;
                case TOK_LE: fprintf(out, "    cmpq %%rcx, %%rax\n    setle %%al\n    movzbq %%al, %%rax\n"); break;
                case TOK_GE: fprintf(out, "    cmpq %%rcx, %%rax\n    setge %%al\n    movzbq %%al, %%rax\n"); break;
                default: break;
            }
            break;
        }
        case NODE_CALL: {
            if (n->arg_count == 1) {
                VarSym *pv = find_var(n->call_name);
                if (pv && (pv->type.array_len > 0 || pv->type.ptr_depth > 0 || pv->type.kind == TY_STR)) {
                    KType elem = pv->type;
                    if (elem.array_len > 0) elem.array_len = 0;
                    else if (elem.ptr_depth > 0) elem.ptr_depth--;
                    else elem = (KType){TY_U8, 0, 0, NULL};
                    int w = is_float_type(elem) ? 8 : (elem.kind == TY_CLASS && elem.ptr_depth == 0 ? ktype_size(elem) : type_byte_width(elem));
                    emit_expr(n->args[0]);
                    if (w > 1) fprintf(out, "    imulq $%d, %%rax\n", w);
                    if (pv->type.array_len > 0) {
                        fprintf(out, "    pushq %%rax\n");
                        if (pv->is_global) fprintf(out, "    lea g_%s(%%rip), %%rax\n", pv->name);
                        else fprintf(out, "    lea -%d(%%rbp), %%rax\n", pv->offset);
                        fprintf(out, "    popq %%rcx\n    addq %%rcx, %%rax\n");
                    } else {
                        char loc[64];
                        var_loc(pv, loc, sizeof(loc));
                        fprintf(out, "    addq %s, %%rax\n", loc);
                    }
                    if (!(elem.kind == TY_CLASS && elem.ptr_depth == 0)) emit_mem_read(elem);
                    break;
                }
            }
            const char *iregs[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
            const char *fregs[6] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5"};
            bool arg_is_flt[16] = {false};
            int ic = 0, fc = 0;
            for (size_t i = 0; i < n->arg_count && i < 12; i++) {
                KType at = infer_expr_type(n->args[i]);
                arg_is_flt[i] = is_float_type(at);
                emit_expr(n->args[i]);
                if (arg_is_flt[i]) {
                    fprintf(out, "    subq $8, %%rsp\n    movsd %%xmm0, (%%rsp)\n");
                    fc++;
                } else {
                    fprintf(out, "    pushq %%rax\n");
                    ic++;
                }
            }
            int ii = ic - 1, fi = fc - 1;
            for (int i = (int)(n->arg_count < 12 ? n->arg_count : 12) - 1; i >= 0; i--) {
                if (arg_is_flt[i]) {
                    if (fi < 6) fprintf(out, "    movsd (%%rsp), %s\n", fregs[fi]);
                    fprintf(out, "    addq $8, %%rsp\n");
                    fi--;
                } else {
                    if (ii < 6) fprintf(out, "    popq %s\n", iregs[ii]);
                    else fprintf(out, "    addq $8, %%rsp\n");
                    ii--;
                }
            }
            const char *target = n->call_name;
            if (current_has_safe) {
                if (strcmp(target, "malloc") == 0) target = "__k_gc_alloc";
                else if (strcmp(target, "calloc") == 0) target = "__k_gc_calloc";
                else if (strcmp(target, "realloc") == 0) target = "__k_gc_realloc";
                else if (strcmp(target, "free") == 0) target = "__k_gc_free";
            }
            if (fc == 0) fprintf(out, "    xorl %%eax, %%eax\n");
            else fprintf(out, "    movl $%d, %%eax\n", fc < 6 ? fc : 6);
            fprintf(out, "    call %s\n", target);
            break;
        }
        default:
            break;
    }
}

static void emit_jump_if_false(Node *cond, int false_label) {
    if (cond->kind == NODE_BINOP &&
        !is_float_type(infer_expr_type(cond->left)) && !is_float_type(infer_expr_type(cond->right)) &&
        (cond->op == TOK_EQ || cond->op == TOK_NE || cond->op == TOK_LT ||
         cond->op == TOK_GT || cond->op == TOK_LE || cond->op == TOK_GE)) {
        long long imm_r;
        if (is_const_int(cond->right, &imm_r) && fits_imm32(imm_r)) {
            if (cond->left->kind == NODE_VAR) {
                VarSym *v = find_var(cond->left->var_name);
                if (v && v->type.array_len == 0) {
                    char loc[64];
                    var_loc(v, loc, sizeof(loc));
                    fprintf(out, "    cmpq $%lld, %s\n", imm_r, loc);
                } else {
                    emit_expr(cond->left);
                    fprintf(out, "    cmpq $%lld, %%rax\n", imm_r);
                }
            } else {
                emit_expr(cond->left);
                fprintf(out, "    cmpq $%lld, %%rax\n", imm_r);
            }
        } else if (is_simple_leaf(cond->right)) {
            emit_expr(cond->left);
            emit_load_into(cond->right, "%rcx");
            fprintf(out, "    cmpq %%rcx, %%rax\n");
        } else {
            emit_expr(cond->left);
            fprintf(out, "    pushq %%rax\n");
            emit_expr(cond->right);
            fprintf(out, "    movq %%rax, %%rcx\n    popq %%rax\n    cmpq %%rcx, %%rax\n");
        }
        const char *jcc = "jne";
        switch (cond->op) {
            case TOK_EQ: jcc = "jne"; break;
            case TOK_NE: jcc = "je"; break;
            case TOK_LT: jcc = "jge"; break;
            case TOK_GT: jcc = "jle"; break;
            case TOK_LE: jcc = "jg"; break;
            case TOK_GE: jcc = "jl"; break;
            default: break;
        }
        fprintf(out, "    %s .L%d\n", jcc, false_label);
        return;
    }
    emit_expr(cond);
    fprintf(out, "    testq %%rax, %%rax\n    je .L%d\n", false_label);
}

static void emit_stmt_expr(Node *n) {
    if (n->kind == NODE_POSTOP && n->left->kind == NODE_VAR) {
        VarSym *v = find_var(n->left->var_name);
        if (v) {
            char loc[64];
            var_loc(v, loc, sizeof(loc));
            fprintf(out, "    %s %s\n", n->op == TOK_INC ? "incq" : "decq", loc);
            return;
        }
    }
    emit_expr(n);
}

static char *translate_print_fmt(const char *fmt) {
    size_t cap = strlen(fmt) * 2 + 16;
    char *res = malloc(cap);
    size_t j = 0;
    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] == '%') {
            if (fmt[i + 1] == 'd' && fmt[i + 2] == 'd') {
                res[j++] = '%'; res[j++] = 'f'; i += 2;
            } else if (fmt[i + 1] == 'd') {
                res[j++] = '%'; res[j++] = 'l'; res[j++] = 'l'; res[j++] = 'd'; i += 1;
            } else if (fmt[i + 1] == 'u') {
                res[j++] = '%'; res[j++] = 'l'; res[j++] = 'l'; res[j++] = 'u'; i += 1;
            } else if (fmt[i + 1] == 'b') {
                res[j++] = '%'; res[j++] = 's'; i += 1;
            } else {
                res[j++] = fmt[i];
            }
        } else {
            res[j++] = fmt[i];
        }
    }
    res[j] = '\0';
    return res;
}

static char *translate_getin_fmt(const char *fmt) {
    size_t cap = strlen(fmt) * 2 + 16;
    char *res = malloc(cap);
    size_t j = 0;
    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] == '%') {
            if (fmt[i + 1] == 'd' && fmt[i + 2] == 'd') {
                res[j++] = '%'; res[j++] = 'l'; res[j++] = 'f'; i += 2;
            } else if (fmt[i + 1] == 'd') {
                res[j++] = '%'; res[j++] = 'l'; res[j++] = 'l'; res[j++] = 'd'; i += 1;
            } else if (fmt[i + 1] == 'u') {
                res[j++] = '%'; res[j++] = 'l'; res[j++] = 'l'; res[j++] = 'u'; i += 1;
            } else {
                res[j++] = fmt[i];
            }
        } else {
            res[j++] = fmt[i];
        }
    }
    res[j] = '\0';
    return res;
}

static void emit_stmt(Node *n, const char *func_end_label) {
    switch (n->kind) {
        case NODE_BLOCK: {
            size_t saved = var_count;
            for (size_t i = 0; i < n->stmt_count; i++) {
                emit_stmt(n->stmts[i], func_end_label);
            }
            var_count = saved;
            break;
        }
        case NODE_VAR_DECL: {
            int reg = -1;
            int off = 0;
            bool is_agg = (n->var_type.array_len > 0 || (n->var_type.kind == TY_CLASS && n->var_type.ptr_depth == 0));
            if (!is_agg && !is_float_type(n->var_type) && !is_addr_taken(n->var_name) && next_reg < 5) {
                reg = next_reg++;
            } else {
                int sz = is_agg ? ktype_size(n->var_type) : 8;
                if (sz % 8 != 0) sz += 8 - (sz % 8);
                if (sz < 8) sz = 8;
                stack_offset += sz;
                off = stack_offset;
            }
            vars[var_count++] = (VarSym){n->var_name, n->var_type, n->is_mut, false, off, reg};

            if (n->var_type.array_len > 0) {
                KType elem = n->var_type;
                elem.array_len = 0;
                int w = is_float_type(elem) ? 8 : type_byte_width(elem);
                for (int k = 0; k < n->var_type.array_len; k++) {
                    if (n->init && n->init->kind == NODE_ARRAY_LIT && (size_t)k < n->init->arg_count) {
                        if (is_float_type(elem)) emit_as_double(n->init->args[k]);
                        else emit_expr(n->init->args[k]);
                        fprintf(out, "    lea -%d(%%rbp), %%rdx\n", off - k * w);
                        emit_mem_write(elem, "%rdx");
                    } else {
                        fprintf(out, "    lea -%d(%%rbp), %%rdx\n", off - k * w);
                        if (is_float_type(elem)) fprintf(out, "    xorpd %%xmm0, %%xmm0\n");
                        else fprintf(out, "    xorl %%eax, %%eax\n");
                        emit_mem_write(elem, "%rdx");
                    }
                }
                break;
            }
            if (is_agg) break;

            char loc[64];
            var_loc(&vars[var_count - 1], loc, sizeof(loc));

            if (n->init) {
                long long cv;
                if (reg >= 0 && is_const_int(n->init, &cv)) {
                    if (cv == 0) fprintf(out, "    xorq %s, %s\n", loc, loc);
                    else fprintf(out, "    movq $%lld, %s\n", cv, loc);
                } else if (is_float_type(n->var_type)) {
                    emit_as_double(n->init);
                    fprintf(out, "    movsd %%xmm0, %s\n", loc);
                } else {
                    emit_expr(n->init);
                    fprintf(out, "    movq %%rax, %s\n", loc);
                }
            } else {
                if (is_float_type(n->var_type)) fprintf(out, "    xorpd %%xmm0, %%xmm0\n    movsd %%xmm0, %s\n", loc);
                else fprintf(out, "    movq $0, %s\n", loc);
            }
            break;
        }
        case NODE_RET:
            if (n->ret_expr) emit_expr(n->ret_expr);
            fprintf(out, "    jmp %s\n", func_end_label);
            break;
        case NODE_BREAK:
            fprintf(out, "    jmp .L%d\n", break_labels[loop_depth - 1]);
            break;
        case NODE_CONTINUE:
            fprintf(out, "    jmp .L%d\n", cont_labels[loop_depth - 1]);
            break;
        case NODE_ASM:
            fprintf(out, "    %s\n", n->str_val);
            break;
        case NODE_OUTB:
        case NODE_OUTW: {
            emit_expr(n->left);
            fprintf(out, "    pushq %%rax\n");
            emit_expr(n->right);
            fprintf(out, "    popq %%rdx\n");
            if (n->kind == NODE_OUTB) fprintf(out, "    outb %%al, %%dx\n");
            else fprintf(out, "    outw %%ax, %%dx\n");
            break;
        }
        case NODE_EXPR_STMT:
            emit_stmt_expr(n->left);
            break;
        case NODE_IFD: {
            int l_else = new_label(), l_end = new_label();
            emit_jump_if_false(n->cond, l_else);
            emit_stmt(n->then_branch, func_end_label);
            if (n->else_branch) fprintf(out, "    jmp .L%d\n", l_end);
            fprintf(out, ".L%d:\n", l_else);
            if (n->else_branch) {
                emit_stmt(n->else_branch, func_end_label);
                fprintf(out, ".L%d:\n", l_end);
            }
            break;
        }
        case NODE_WHILE: {
            int l_cond = new_label(), l_end = new_label();
            break_labels[loop_depth] = l_end;
            cont_labels[loop_depth] = l_cond;
            loop_depth++;
            fprintf(out, ".L%d:\n", l_cond);
            emit_jump_if_false(n->cond, l_end);
            emit_stmt(n->then_branch, func_end_label);
            fprintf(out, "    jmp .L%d\n.L%d:\n", l_cond, l_end);
            loop_depth--;
            break;
        }
        case NODE_FOR: {
            size_t saved = var_count;
            int l_cond = new_label(), l_step = new_label(), l_end = new_label();
            if (n->for_init) {
                if (n->for_init->kind == NODE_VAR_DECL) emit_stmt(n->for_init, func_end_label);
                else emit_stmt_expr(n->for_init);
            }
            break_labels[loop_depth] = l_end;
            cont_labels[loop_depth] = l_step;
            loop_depth++;
            fprintf(out, ".L%d:\n", l_cond);
            emit_jump_if_false(n->cond, l_end);
            emit_stmt(n->then_branch, func_end_label);
            fprintf(out, ".L%d:\n", l_step);
            if (n->for_step) emit_stmt_expr(n->for_step);
            fprintf(out, "    jmp .L%d\n.L%d:\n", l_cond, l_end);
            loop_depth--;
            var_count = saved;
            break;
        }
        case NODE_PRINT: {
            char *cfmt = translate_print_fmt(n->fmt);
            const char *fmt_lbl = add_str_lit(cfmt);
            free(cfmt);

            const char *iregs[5] = {"%rsi", "%rdx", "%rcx", "%r8", "%r9"};
            const char *fregs[6] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5"};
            bool is_flt[16] = {false};
            int ic = 0, fc = 0;

            for (size_t i = 0; i < n->arg_count && i < 12; i++) {
                Node *arg = n->args[i];
                KType at = infer_expr_type(arg);
                is_flt[i] = is_float_type(at);
                emit_expr(arg);
                if (is_flt[i]) {
                    fprintf(out, "    subq $8, %%rsp\n    movsd %%xmm0, (%%rsp)\n");
                    fc++;
                } else if (at.kind == TY_BOOL && at.ptr_depth == 0) {
                    const char *t_lbl = add_str_lit("true");
                    const char *f_lbl = add_str_lit("false");
                    int l_f = new_label(), l_d = new_label();
                    fprintf(out, "    testq %%rax, %%rax\n    je .L%d\n", l_f);
                    fprintf(out, "    lea %s(%%rip), %%rax\n    jmp .L%d\n", t_lbl, l_d);
                    fprintf(out, ".L%d:\n    lea %s(%%rip), %%rax\n.L%d:\n", l_f, f_lbl, l_d);
                    fprintf(out, "    pushq %%rax\n");
                    ic++;
                } else {
                    fprintf(out, "    pushq %%rax\n");
                    ic++;
                }
            }
            int ii = ic - 1, fi = fc - 1;
            for (int i = (int)(n->arg_count < 12 ? n->arg_count : 12) - 1; i >= 0; i--) {
                if (is_flt[i]) {
                    if (fi < 6) fprintf(out, "    movsd (%%rsp), %s\n", fregs[fi]);
                    fprintf(out, "    addq $8, %%rsp\n");
                    fi--;
                } else {
                    if (ii < 5) fprintf(out, "    popq %s\n", iregs[ii]);
                    else fprintf(out, "    addq $8, %%rsp\n");
                    ii--;
                }
            }
            fprintf(out, "    lea %s(%%rip), %%rdi\n", fmt_lbl);
            if (fc == 0) fprintf(out, "    xorl %%eax, %%eax\n");
            else fprintf(out, "    movl $%d, %%eax\n", fc < 6 ? fc : 6);
            fprintf(out, "    call printf@PLT\n");
            break;
        }
        case NODE_GETIN: {
            char *cfmt = translate_getin_fmt(n->fmt);
            const char *fmt_lbl = add_str_lit(cfmt);
            free(cfmt);
            const char *int_regs[5] = {"%rsi", "%rdx", "%rcx", "%r8", "%r9"};
            for (size_t i = 0; i < n->arg_count && i < 5; i++) {
                emit_addr(n->args[i]);
                fprintf(out, "    movq %%rax, %s\n", int_regs[i]);
            }
            fprintf(out, "    lea %s(%%rip), %%rdi\n    xorl %%eax, %%eax\n    call scanf@PLT\n", fmt_lbl);
            break;
        }
        default:
            break;
    }
}

// If you're reading this just know i almost commited

static void emit_safe_gc_runtime(void) {
    fprintf(out,
        "\n.bss\n.align 8\n__k_gc_head: .quad 0\n"
        ".text\n"
        "__k_gc_alloc:\n"
        "    pushq %%rbx\n"
        "    leaq 16(%%rdi), %%rdi\n"
        "    call malloc@PLT\n"
        "    testq %%rax, %%rax\n"
        "    je .L_gc_alloc_null\n"
        "    movq __k_gc_head(%%rip), %%rbx\n"
        "    movq %%rbx, (%%rax)\n"
        "    movq $0, 8(%%rax)\n"
        "    testq %%rbx, %%rbx\n"
        "    je .L_gc_alloc_set\n"
        "    movq %%rax, 8(%%rbx)\n"
        ".L_gc_alloc_set:\n"
        "    movq %%rax, __k_gc_head(%%rip)\n"
        "    addq $16, %%rax\n"
        ".L_gc_alloc_null:\n"
        "    popq %%rbx\n"
        "    ret\n"
        "__k_gc_calloc:\n"
        "    imulq %%rsi, %%rdi\n"
        "    pushq %%rdi\n"
        "    call __k_gc_alloc\n"
        "    popq %%rcx\n"
        "    testq %%rax, %%rax\n"
        "    je .L_gc_calloc_done\n"
        "    movq %%rax, %%rdx\n"
        "    movq %%rax, %%rdi\n"
        "    xorl %%eax, %%eax\n"
        "    rep stosb\n"
        "    movq %%rdx, %%rax\n"
        ".L_gc_calloc_done:\n"
        "    ret\n"
        "__k_gc_free:\n"
        "    testq %%rdi, %%rdi\n"
        "    je .L_gc_free_done\n"
        "    subq $16, %%rdi\n"
        "    movq (%%rdi), %%rax\n"
        "    movq 8(%%rdi), %%rcx\n"
        "    testq %%rcx, %%rcx\n"
        "    je .L_gc_free_head\n"
        "    movq %%rax, (%%rcx)\n"
        "    jmp .L_gc_free_next\n"
        ".L_gc_free_head:\n"
        "    movq %%rax, __k_gc_head(%%rip)\n"
        ".L_gc_free_next:\n"
        "    testq %%rax, %%rax\n"
        "    je .L_gc_free_call\n"
        "    movq %%rcx, 8(%%rax)\n"
        ".L_gc_free_call:\n"
        "    call free@PLT\n"
        ".L_gc_free_done:\n"
        "    ret\n"
        "__k_gc_realloc:\n"
        "    testq %%rdi, %%rdi\n"
        "    jne .L_gc_realloc_existing\n"
        "    movq %%rsi, %%rdi\n"
        "    jmp __k_gc_alloc\n"
        ".L_gc_realloc_existing:\n"
        "    pushq %%rbx\n"
        "    subq $16, %%rdi\n"
        "    movq (%%rdi), %%rax\n"
        "    movq 8(%%rdi), %%rcx\n"
        "    testq %%rcx, %%rcx\n"
        "    je .L_gc_realloc_head\n"
        "    movq %%rax, (%%rcx)\n"
        "    jmp .L_gc_realloc_next\n"
        ".L_gc_realloc_head:\n"
        "    movq %%rax, __k_gc_head(%%rip)\n"
        ".L_gc_realloc_next:\n"
        "    testq %%rax, %%rax\n"
        "    je .L_gc_realloc_call\n"
        "    movq %%rcx, 8(%%rax)\n"
        ".L_gc_realloc_call:\n"
        "    leaq 16(%%rsi), %%rsi\n"
        "    call realloc@PLT\n"
        "    testq %%rax, %%rax\n"
        "    je .L_gc_realloc_done\n"
        "    movq __k_gc_head(%%rip), %%rbx\n"
        "    movq %%rbx, (%%rax)\n"
        "    movq $0, 8(%%rax)\n"
        "    testq %%rbx, %%rbx\n"
        "    je .L_gc_realloc_set\n"
        "    movq %%rax, 8(%%rbx)\n"
        ".L_gc_realloc_set:\n"
        "    movq %%rax, __k_gc_head(%%rip)\n"
        "    addq $16, %%rax\n"
        ".L_gc_realloc_done:\n"
        "    popq %%rbx\n"
        "    ret\n"
        "__k_gc_cleanup:\n"
        "    pushq %%rbx\n"
        "    pushq %%r12\n"
        "    subq $8, %%rsp\n"
        "    movq __k_gc_head(%%rip), %%rbx\n"
        ".L_gc_loop:\n"
        "    testq %%rbx, %%rbx\n"
        "    je .L_gc_done\n"
        "    movq (%%rbx), %%r12\n"
        "    movq %%rbx, %%rdi\n"
        "    call free@PLT\n"
        "    movq %%r12, %%rbx\n"
        "    jmp .L_gc_loop\n"
        ".L_gc_done:\n"
        "    movq $0, __k_gc_head(%%rip)\n"
        "    addq $8, %%rsp\n"
        "    popq %%r12\n"
        "    popq %%rbx\n"
        "    ret\n"
        ".section .fini_array,\"aw\",@fini_array\n"
        ".align 8\n"
        ".quad __k_gc_cleanup\n"
        ".text\n");
}

bool generate_x86_64_fp(Node *prog, FILE *out_fp, const CompilerOpts *opts) {
    out = out_fp;
    var_count = 0;
    str_count = 0;
    float_count = 0;
    loop_depth = 0;
    current_has_safe = prog->has_safe && !opts->flag_ff && !opts->flag_default && !opts->flag_kernel;

    fprintf(out, ".text\n");
    for (size_t i = 0; i < prog->global_count; i++) {
        Node *g = prog->globals[i];
        vars[var_count++] = (VarSym){g->var_name, g->var_type, g->is_mut, true, 0, -1};
    }

    for (size_t i = 0; i < prog->func_count; i++) {
        Node *fn = prog->funcs[i];
        size_t saved_vars = var_count;
        stack_offset = 40;
        next_reg = 0;
        addr_taken_count = 0;
        collect_addr_taken(fn->body);

        char end_lbl[64];
        snprintf(end_lbl, sizeof(end_lbl), ".L_fn_end_%zu", i);

        fprintf(out, ".globl %s\n%s:\n", fn->func_name, fn->func_name);
        if (fn->is_isr) {
            fprintf(out, "    pushq %%rax\n    pushq %%rcx\n    pushq %%rdx\n    pushq %%rsi\n    pushq %%rdi\n    pushq %%r8\n    pushq %%r9\n    pushq %%r10\n    pushq %%r11\n");
        }
        fprintf(out, "    pushq %%rbp\n    movq %%rsp, %%rbp\n");
        fprintf(out, "    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n    pushq %%r15\n");
        fprintf(out, "    subq $4104, %%rsp\n");

        const char *iregs[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        const char *fregs[6] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5"};
        size_t ip = 0, fp = 0;
        for (size_t p = 0; p < fn->param_count; p++) {
            stack_offset += 8;
            KType pt = fn->params[p]->var_type;
            vars[var_count++] = (VarSym){fn->params[p]->var_name, pt, true, false, stack_offset, -1};
            if (is_float_type(pt) && fp < 6) {
                fprintf(out, "    movsd %s, -%d(%%rbp)\n", fregs[fp++], stack_offset);
            } else if (!is_float_type(pt) && ip < 6) {
                fprintf(out, "    movq %s, -%d(%%rbp)\n", iregs[ip++], stack_offset);
            }
        }

        emit_stmt(fn->body, end_lbl);

        fprintf(out, "%s:\n", end_lbl);
        fprintf(out, "    addq $4104, %%rsp\n");
        fprintf(out, "    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%r12\n    popq %%rbx\n");
        fprintf(out, "    popq %%rbp\n");
        if (fn->is_isr) {
            fprintf(out, "    popq %%r11\n    popq %%r10\n    popq %%r9\n    popq %%r8\n    popq %%rdi\n    popq %%rsi\n    popq %%rdx\n    popq %%rcx\n    popq %%rax\n");
            fprintf(out, "    iretq\n\n");
        } else {
            fprintf(out, "    ret\n\n");
        }
        var_count = saved_vars;
    }

    if (current_has_safe) {
        emit_safe_gc_runtime();
    }

    if ((opts->flag_ff || opts->flag_kernel) && strcmp(prog->entry_name, "_start") != 0) {
        fprintf(out, ".section .text.start,\"ax\"\n.weak _start\n_start:\n    call %s\n", prog->entry_name);
        if (opts->flag_kernel) {
            fprintf(out, ".L_kern_halt:\n    cli\n    hlt\n    jmp .L_kern_halt\n");
        } else {
            fprintf(out, "    movq %%rax, %%rdi\n    movq $60, %%rax\n    syscall\n");
        }
    } else if (!opts->flag_ff && !opts->flag_kernel && strcmp(prog->entry_name, "main") != 0) {
        fprintf(out, ".globl main\nmain:\n    call %s\n    ret\n", prog->entry_name);
    }

    if (prog->global_count > 0) {
        fprintf(out, "\n.data\n");
        for (size_t i = 0; i < prog->global_count; i++) {
            Node *g = prog->globals[i];
            if (g->var_type.array_len > 0) {
                fprintf(out, "g_%s: .zero %d\n", g->var_name, ktype_size(g->var_type));
            } else if (g->init && g->init->kind == NODE_INT) {
                fprintf(out, "g_%s: .quad %lld\n", g->var_name, g->init->int_val);
            } else {
                fprintf(out, "g_%s: .quad 0\n", g->var_name);
            }
        }
    }

    if (str_count > 0 || float_count > 0) {
        fprintf(out, "\n.section .rodata\n");
        for (size_t i = 0; i < str_count; i++) {
            fprintf(out, "%s: .string \"", str_lits[i].label);
            for (const char *c = str_lits[i].str; *c; c++) {
                if (*c == '\n') fprintf(out, "\\n");
                else if (*c == '\t') fprintf(out, "\\t");
                else if (*c == '\r') fprintf(out, "\\r");
                else if (*c == '"') fprintf(out, "\\\"");
                else if (*c == '\\') fprintf(out, "\\\\");
                else fputc(*c, out);
            }
            fprintf(out, "\"\n");
        }
        for (size_t i = 0; i < float_count; i++) {
            fprintf(out, "%s: .double %.17g\n", float_lits[i].label, float_lits[i].val);
        }
    }

    fprintf(out, "\n.section .note.GNU-stack,\"\",@progbits\n");
    fflush(out);
    return true;
}

bool generate_x86_64(Node *prog, const char *asm_path, const CompilerOpts *opts) {
    FILE *fp = fopen(asm_path, "w");
    if (!fp) {
        perror("fopen asm");
        return false;
    }
    bool ok = generate_x86_64_fp(prog, fp, opts);
    fclose(fp);
    return ok;
}

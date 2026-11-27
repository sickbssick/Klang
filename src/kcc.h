#ifndef KCC_H
#define KCC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- Token kinds ---- */
typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STR,
    TOK_CHAR,
    /* Keywords */
    TOK_DEF,
    TOK_PUB,
    TOK_MUT,
    TOK_RET,
    TOK_SET,
    TOK_CLASS,
    TOK_ENUM,
    TOK_SIZEOF,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_IFD,
    TOK_ELSE,
    TOK_FOR,
    TOK_WHILE,
    TOK_TRUE,
    TOK_FALSE,
    TOK_INIT,
    TOK_EXEC,
    TOK_ISR,
    TOK_SAFE,
    TOK_RAW,
    TOK_PRINT,
    TOK_GETIN,
    TOK_ASM,
    TOK_OUTB,
    TOK_INB,
    TOK_OUTW,
    TOK_INW,
    TOK_NUL,
    /* Directives */
    TOK_UNPACK,
    TOK_ENTRY,
    /* Punctuation & Operators */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COLON,
    TOK_DCOLON,
    TOK_AT,
    TOK_SEMI,
    TOK_COMMA,
    TOK_DOT,
    TOK_ARROW,
    TOK_DO,
    TOK_ASSIGN,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_POW,
    TOK_INC,
    TOK_DEC,
    /* Bitwise */
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,
    TOK_SHL,
    TOK_SHR,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_AND,
    TOK_OR,
    TOK_NOT
} TokenKind;

/* ---- Type kinds ---- */
typedef enum {
    TY_UNKNOWN = 0,
    TY_VOID,
    TY_I8, TY_I16, TY_I32, TY_I64,
    TY_U8, TY_U16, TY_U32, TY_U64,
    TY_F8, TY_F16, TY_F32, TY_F64,
    TY_BOOL,
    TY_CHAR,
    TY_STR,
    TY_CLASS,
    TY_ENUM
} TypeKind;

/* ---- Node kinds ---- */
typedef enum {
    NODE_PROGRAM = 0,
    NODE_FUNC,
    NODE_BLOCK,
    NODE_VAR_DECL,
    NODE_RET,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_IFD,
    NODE_FOR,
    NODE_WHILE,
    NODE_PRINT,
    NODE_GETIN,
    NODE_ASM,
    NODE_OUTB,
    NODE_OUTW,
    NODE_INB,
    NODE_INW,
    NODE_EXPR_STMT,
    NODE_ASSIGN,
    NODE_BINOP,
    NODE_UNOP,
    NODE_POSTOP,
    NODE_CALL,
    NODE_ADDR,
    NODE_DEREF,
    NODE_INDEX,
    NODE_MEMBER,
    NODE_SIZEOF,
    NODE_ARRAY_LIT,
    NODE_VAR,
    NODE_INT,
    NODE_FLOAT,
    NODE_STR,
    NODE_CHAR,
    NODE_BOOL
} NodeKind;

/* ---- Structs ---- */
typedef struct Token {
    int kind;
    char *text;
    long long line;
    long long col;
    long long int_val;
    double float_val;
} Token;

typedef struct TokenList {
    Token **items;
    long long count;
    long long cap;
} TokenList;

typedef struct KType {
    int kind;
    long long ptr_depth;
    long long array_len;
    char *class_name;
} KType;

typedef struct ClassField {
    char *name;
    KType *ftype;
    long long offset;
    long long size;
} ClassField;

typedef struct ClassDef {
    char *name;
    ClassField **fields;
    long long field_count;
    long long total_size;
} ClassDef;

typedef struct EnumVariant {
    char *name;
    long long value;
} EnumVariant;

typedef struct EnumDef {
    char *name;
    EnumVariant **variants;
    long long variant_count;
} EnumDef;

typedef struct TypeAlias {
    char *alias;
    KType *target;
} TypeAlias;

typedef struct Node {
    int kind;
    long long line;
    long long col;
    KType *val_type;

    char *entry_name;
    int has_safe;
    struct Node **globals;
    long long global_cap;
    long long global_count;
    struct Node **funcs;
    long long func_cap;
    long long func_count;

    char *func_name;
    int is_isr;
    struct Node **params;
    long long param_count;
    KType *ret_type;
    struct Node *body;

    struct Node **stmts;
    long long stmt_cap;
    long long stmt_count;

    char *var_name;
    KType *var_type;
    int is_pub;
    int is_mut;
    struct Node *init_expr;

    struct Node *ret_expr;
    struct Node *cond;
    struct Node *then_branch;
    struct Node *else_branch;
    struct Node *for_init;
    struct Node *for_step;

    char *fmt;
    struct Node **args;
    long long arg_count;
    char *call_name;

    char *member_name;
    int via_arrow;

    KType *sizeof_type;

    long long int_val;
    double float_val;
    char *str_val;

    int op;
    struct Node *left;
    struct Node *right;
} Node;

typedef struct CompilerOpts {
    char **inputs;
    long long input_count;
    char *output_path;
    char *linker_script;
    char **lib_flags;
    long long lib_flag_count;
    int flag_bo;
    int flag_ffocus;
    int flag_default;
    int flag_derr;
    int flag_ff;
    int flag_kernel;
    int flag_run;
    int flag_c;
    int flag_S;
    int flag_werror;
    int flag_wall;
    int flag_time;
    int flag_quiet;
    int flag_win;
} CompilerOpts;

/* ---- Global compiler state ---- */
extern FILE *k_stderr;
extern int cg_win;
extern char **inc_paths;
extern long long inc_count;
extern TypeAlias **aliases;
extern long long alias_count;
extern ClassDef **classes;
extern long long class_count;
extern EnumDef **enums;
extern long long enum_count;
extern char **unpacked_paths;
extern long long unpacked_count;

/* ---- ast.c functions ---- */
void k_init_globals(void);
KType *make_type(int kind, long long ptr_depth, long long array_len, char *cname);
KType *clone_type(KType *src);
Node *new_node(int kind, long long line, long long col);
int is_float_type(KType *t);
int is_int_like_type(KType *t);
int is_unsigned_type(KType *t);
ClassDef *find_class(char *name);
ClassField *find_class_field(ClassDef *cls, char *fname);
EnumDef *find_enum(char *name);
int find_enum_value(char *ename, char *vname, long long *out_val);
int find_bare_enum_value(char *vname, EnumDef **out_enum, long long *out_val);
long long ktype_size(KType *t);
long long type_byte_width(KType *t);
char *ktype_str(KType *t, char *buf, long long sz);

/* ---- lexer.c functions ---- */
TokenList *lex_source(char *src);
int k_ends_with(char *s, char *suf);

/* ---- parser.c functions ---- */
Node *parse_program_file(char *path, char *src, CompilerOpts *opts);
void parse_into_program(Node *prog, char *file_path, char *src, CompilerOpts *opts, int is_header);

/* ---- typecheck.c functions ---- */
int typecheck_program(Node *prog, char *path, char *src, CompilerOpts *opts);

/* ---- codegen.c functions ---- */
int generate_x86_64_fp(Node *prog, FILE *out_fp, int opt_ff, int opt_kernel, int opt_default);

#endif /* KCC_H */

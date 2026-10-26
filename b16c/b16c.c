/*
 * b16c - a compiler of the B16 language for the 16-bit CPU16 processor (revision 6).
 *
 * Build:  cc -O2 -o b16c b16c.c   (or g++ -O2 -o b16c b16c.c: the code is compatible with C99 and C++)
 * Run:    b16c [-I dir] [-o out.s] prog.b16
 *          (for the full list of keys run b16c without arguments). The image parameters (target, code and
 *          data addresses, CX16 header fields) are set in the main file by option declarations, assembly
 *          modules by include asm lines; the keys --target, --code, --data, --min-free, --max-top,
 *          --asm do the same from the command line and take precedence.
 *
 * The std target: a CX16 executable (header, startup code for the monitor and for the bare machine).
 * The kernel target: no header and no startup code, entry points are export functions.
 *
 * The result is text for the cpu16asm assembler ("The Assembly Language of the 16-bit Processor.
 * Reference"): .word/.byte/.space, local labels @L<n>, Jxx jumps (the assembler chooses the JR form or
 * the long one), the PUSH/POP/RET/CLI/STI pseudo-instructions. Strings and character
 * literals are recoded into the machine encoding ("CPU16 Machine", sect. 5).
 * Names that coincide with mnemonics and registers get the prefix __ in the assembler.
 *
 * Calling convention:
 *   R0..R3   arguments 1..4 and results 1..4, clobbered by the called function;
 *   R4..R11  preserved by the called function;
 *   LR       return address (CALL), clobbered;
 *   arguments from the 5th on are pushed on the stack right to left, popped by the caller.
 */
#define _DEFAULT_SOURCE   /* realpath in POSIX */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

/* a 64-bit integer on any platform: on Windows (LLP64) long is 32 bits, while constant values, offsets and
   addresses up to $FFFFFFFF and computations with headroom up to 2^62 need 64 bits. */
typedef long long i64;
typedef char i64_is_64_bit[sizeof(i64) == 8 ? 1 : -1];     /* compile-time check */

/* ===================== Assembler syntax settings ===================== */

#define ASM_DW ".word"     /* words */
#define ASM_DB ".byte"     /* bytes */
#define ASM_DS ".space"    /* N zero bytes */
#define STACK_TOP "$FF00"
#define PORT_TIMER "$FF29"

/* Mnemonics of jumps by condition code. The short form (JR, +-64 words) or the long
   relative form is chosen by the assembler (reference, sect. 7.5 and 10). */
static const char *j_name[15] = {
    "JMP", "JZ", "JNZ", "JC", "JNC", "JA", "JBE", "JS", "JNS", "JO", "JNO", "JL", "JGE", "JG", "JLE"
};

/* ================================ Utilities ================================= */

static const char *srcname = "?";
static int errline;
static FILE *out;

/* A map of the lines of the combined text (after include) onto files. */
typedef struct { const char *file; int line; } LineInfo;
static LineInfo *linemap;
static int nlinemap;

static void where(int line, const char **file, int *fl)
{
    if (line >= 1 && line <= nlinemap) { *file = linemap[line - 1].file; *fl = linemap[line - 1].line; }
    else { *file = srcname; *fl = line; }
}

/* Diagnostics. An error is printed and, if there is a recovery point (a statement,
   declaration or function boundary), control goes there: several errors are
   reported in one run. Without a recovery point an error terminates translation. */
#define MAX_ERRORS 20
static int nerrors, nwarnings, opt_werror, opt_nowarn;
static jmp_buf *err_stack[256];
static int nerr_stack;
static int diag_silent;     /* pass 1 skips function bodies: their errors will be reported by pass 2 */

static void diag(const char *kind, int line, const char *f, va_list ap)
{
    const char *file;
    int fl;
    where(line, &file, &fl);
    if (line > 0) fprintf(stderr, "%s:%d: %s: ", file, fl, kind);
    else fprintf(stderr, "%s: %s: ", file, kind);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
}

static void too_many(void)
{
    if (nerrors >= MAX_ERRORS) {
        fprintf(stderr, "b16c: too many errors, stopping\n");
        exit(1);
    }
}

static void die(const char *f, ...)
{
    va_list ap;
    if (diag_silent && nerr_stack > 0) longjmp(*err_stack[nerr_stack - 1], 1);
    va_start(ap, f);
    diag("error", errline, f, ap);
    va_end(ap);
    nerrors++;
    too_many();
    if (nerr_stack > 0) longjmp(*err_stack[nerr_stack - 1], 1);
    exit(1);
}

/* An error without recovery (end of file inside a block, etc.). */
static void fatal(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    diag("error", errline, f, ap);
    va_end(ap);
    exit(1);
}

/* A warning at line line of the combined text; -Werror turns it into an error. */
static void warn_at(int line, const char *f, ...)
{
    if (opt_nowarn) return;
    va_list ap;
    va_start(ap, f);
    diag(opt_werror ? "error" : "warning", line, f, ap);
    va_end(ap);
    if (opt_werror) { nerrors++; too_many(); }
    else nwarnings++;
}

#define TRY_PUSH(jb) (err_stack[nerr_stack++] = &(jb))
/* Pass an already reported error on to the outer recovery point. */
static void rethrow(void)
{
    if (nerr_stack > 0) longjmp(*err_stack[nerr_stack - 1], 1);
    exit(1);
}
#define TRY_POP() (nerr_stack--)

static void *xcalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *r = (char*)xcalloc(n + 1);
    memcpy(r, s, n);
    return r;
}

static char *fmt(const char *f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return xstrndup(buf, strlen(buf));
}

static int popcount(int m) { int n = 0; while (m) { n += m & 1; m >>= 1; } return n; }

static int log2exact(i64 v)
{
    for (int k = 0; k < 16; k++) if ((1LL << k) == v) return k;
    return -1;
}

/* ================================= Lexer ================================= */

enum {
    TK_EOF = 256, TK_ID, TK_NUM, TK_STR,
    TK_FN, TK_VAR, TK_CONST, TK_STRUCT, TK_IF, TK_ELSE, TK_WHILE, TK_LOOP,
    TK_BREAK, TK_CONTINUE, TK_RETURN, TK_INTERRUPT, TK_AT, TK_ATOMIC, TK_ASM,
    TK_BYTE, TK_CHAR, TK_INT, TK_UINT, TK_LONG, TK_ULONG, TK_PTR, TK_SIZEOF, TK_GOTO, TK_INCLUDE,
    TK_EXPORT, TK_STATIC, TK_ASSERT, TK_OFFSETOF, TK_EXTERN,
    TK_EQ, TK_NE, TK_LE, TK_GE, TK_SHL, TK_SHR, TK_ANDAND, TK_OROR, TK_INC, TK_DEC,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ, TK_MODEQ, TK_ANDEQ, TK_OREQ, TK_XOREQ,
    TK_SHLEQ, TK_SHREQ
};

static const struct { const char *s; int t; } keywords[] = {
    {"fn", TK_FN}, {"var", TK_VAR}, {"const", TK_CONST}, {"struct", TK_STRUCT},
    {"if", TK_IF}, {"else", TK_ELSE}, {"while", TK_WHILE}, {"loop", TK_LOOP},
    {"break", TK_BREAK}, {"continue", TK_CONTINUE}, {"return", TK_RETURN},
    {"interrupt", TK_INTERRUPT}, {"at", TK_AT}, {"atomic", TK_ATOMIC}, {"asm", TK_ASM},
    {"byte", TK_BYTE}, {"char", TK_CHAR}, {"int", TK_INT}, {"uint", TK_UINT},
    {"long", TK_LONG}, {"ulong", TK_ULONG},
    {"ptr", TK_PTR}, {"sizeof", TK_SIZEOF}, {"goto", TK_GOTO}, {"include", TK_INCLUDE},
    {"export", TK_EXPORT}, {"static", TK_STATIC}, {"assert", TK_ASSERT}, {"offsetof", TK_OFFSETOF},
    {"extern", TK_EXTERN}, {NULL, 0}
};

static const struct { const char *s; int t; } puncts[] = {
    {"<<=", TK_SHLEQ}, {">>=", TK_SHREQ},
    {"==", TK_EQ}, {"!=", TK_NE}, {"<=", TK_LE}, {">=", TK_GE}, {"<<", TK_SHL}, {">>", TK_SHR},
    {"&&", TK_ANDAND}, {"||", TK_OROR}, {"++", TK_INC}, {"--", TK_DEC},
    {"+=", TK_ADDEQ}, {"-=", TK_SUBEQ}, {"*=", TK_MULEQ}, {"/=", TK_DIVEQ}, {"%=", TK_MODEQ},
    {"&=", TK_ANDEQ}, {"|=", TK_OREQ}, {"^=", TK_XOREQ}, {NULL, 0}
};

static char *src;
static const char *lp;
static int lline;
static int tok, lasttok, tokline;
static int lex_depth, tokdepth;   /* number of unclosed '{' before the current token */
static i64 tokval;
static char *tokstr;
static int tokslen;

/* After which tokens a newline means the end of a statement (as in Go). */
static int needs_semi(int t)
{
    switch (t) {
    case TK_ID: case TK_NUM: case TK_STR: case ')': case ']': case '}':
    case TK_INC: case TK_DEC: case TK_BREAK: case TK_CONTINUE: case TK_RETURN:
    case TK_BYTE: case TK_CHAR: case TK_INT: case TK_UINT: case TK_LONG: case TK_ULONG: case TK_PTR:
        return 1;
    }
    return 0;
}

/* Allows "}" and "else" to be written on different lines. */
static int next_is_else(const char *p)
{
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    return !strncmp(p, "else", 4) && !(isalnum((unsigned char)p[4]) || p[4] == '_');
}

/* The machine encoding ("CPU16 Machine", sect. 5; the table is from cpu16_machine.hpp):
   an element is a Unicode code point. Literals in the source (UTF-8) are recoded into it. */
static const unsigned charset[256] = {
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,   // $00
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,   // $08
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,   // $10
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,   // $18
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,   // $20
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,   // $28
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,   // $30
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,   // $38
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,   // $40
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,   // $48
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,   // $50
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,   // $58
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,   // $60
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,   // $68
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,   // $70
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,   // $78
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,   // $80
    0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,   // $88
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248,   // $90
    0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,   // $98
    0x2550, 0x2551, 0x2552, 0x0451, 0x2553, 0x2554, 0x2555, 0x2556,   // $A0
    0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x255C, 0x255D, 0x255E,   // $A8
    0x255F, 0x2560, 0x2561, 0x0401, 0x2562, 0x2563, 0x2564, 0x2565,   // $B0
    0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x256B, 0x256C, 0x00A9,   // $B8
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,   // $C0
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,   // $C8
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,   // $D0
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,   // $D8
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,   // $E0
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,   // $E8
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,   // $F0
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,   // $F8
};

static int charset_find(unsigned long cp)
{
    if (cp < 0x80) return (int)cp;
    for (int i = 0x80; i < 256; i++) if (charset[i] == cp) return i;
    for (int i = 1; i < 0x20; i++) if (charset[i] == cp) return i;
    return charset[0x7F] == cp ? 0x7F : -1;
}

/* A UTF-8 character starting at lp - 1 (the first byte has already been read) -> machine code. */
static int lex_utf8(int c)
{
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (n < 0) die("invalid UTF-8 sequence");
    unsigned long cp = c & (0x3F >> n);
    const char *start = lp - 1;
    for (int i = 0; i < n; i++) {
        int b = (unsigned char)*lp;
        if ((b & 0xC0) != 0x80) die("invalid UTF-8 sequence");
        cp = cp << 6 | (b & 0x3F);
        lp++;
    }
    int r = charset_find(cp);
    if (r < 0) die("character '%.*s' is not in the CPU16 character set", (int)(lp - start), start);
    return r;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10; }

static int lex_char(void)
{
    int c = (unsigned char)*lp++;
    if (c >= 0x80) return lex_utf8(c);
    if (c != '\\') return c;
    c = *lp++;
    switch (c) {
    case 'n': return 10;
    case 'r': return 13;
    case 't': return 9;
    case 'e': return 27;
    case '0': return 0;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'x': {
        int v = 0;
        for (int i = 0; i < 2 && isxdigit((unsigned char)*lp); i++) v = v * 16 + hexval(*lp++);
        return v;
    }
    }
    die("unknown escape sequence \\%c", c);
    return 0;
}

static void next(void)
{
    for (;;) {
        while (*lp == ' ' || *lp == '\t' || *lp == '\r') lp++;
        if (*lp == '\n') {
            lp++;
            lline++;
            if (needs_semi(lasttok) && !next_is_else(lp)) {
                tok = lasttok = ';';
                tokline = errline = lline - 1;
                tokdepth = lex_depth;
                return;
            }
            continue;
        }
        if (lp[0] == '/' && lp[1] == '/') { while (*lp && *lp != '\n') lp++; continue; }
        if (lp[0] == '/' && lp[1] == '*') {
            lp += 2;
            while (*lp && !(lp[0] == '*' && lp[1] == '/')) { if (*lp == '\n') lline++; lp++; }
            if (*lp) lp += 2;
            continue;
        }
        break;
    }
    tokline = errline = lline;
    if (!*lp) {
        tok = needs_semi(lasttok) ? (int)';' : (int)TK_EOF;
        lasttok = tok;
        tokdepth = lex_depth;
        return;
    }
    int c = (unsigned char)*lp;
    if (isalpha(c) || c == '_') {
        const char *s = lp;
        while (isalnum((unsigned char)*lp) || *lp == '_') lp++;
        tokstr = xstrndup(s, lp - s);
        tok = TK_ID;
        for (int i = 0; keywords[i].s; i++)
            if (!strcmp(keywords[i].s, tokstr)) { tok = keywords[i].t; break; }
    } else if (isdigit(c) || (c == '$' && isxdigit((unsigned char)lp[1]))) {
        i64 v = 0;
        if (c == '$' || (c == '0' && (lp[1] == 'x' || lp[1] == 'X'))) {
            lp += c == '$' ? 1 : 2;
            if (!isxdigit((unsigned char)*lp)) die("malformed hexadecimal number");
            while (isxdigit((unsigned char)*lp)) {
                v = v * 16 + hexval(*lp++);
                if (v > 0xFFFFFFFFLL) die("number does not fit in 32 bits");
            }
        } else if (c == '0' && (lp[1] == 'b' || lp[1] == 'B')) {
            lp += 2;
            while (*lp == '0' || *lp == '1') {
                v = v * 2 + (*lp++ - '0');
                if (v > 0xFFFFFFFFLL) die("number does not fit in 32 bits");
            }
        } else {
            while (isdigit((unsigned char)*lp)) {
                v = v * 10 + (*lp++ - '0');
                if (v > 0xFFFFFFFFLL) die("number does not fit in 32 bits");
            }
        }
        if (isalnum((unsigned char)*lp) || *lp == '_') die("malformed number");
        tokval = v;
        tok = TK_NUM;
    } else if (c == '\'') {
        lp++;
        tokval = lex_char();
        if (*lp != '\'') die("expected '");
        lp++;
        tok = TK_NUM;
    } else if (c == '"') {
        lp++;
        int cap = 64, n = 0;
        char *buf = (char*)xcalloc(cap);
        while (*lp != '"') {
            if (!*lp || *lp == '\n') die("unterminated string");
            if (n + 2 >= cap) { cap *= 2; buf = (char*)xrealloc(buf, cap); }
            buf[n++] = (char)lex_char();
        }
        lp++;
        buf[n] = 0;
        tokstr = buf;
        tokslen = n;
        tok = TK_STR;
    } else {
        tok = 0;
        for (int i = 0; puncts[i].s; i++) {
            size_t l = strlen(puncts[i].s);
            if (!strncmp(lp, puncts[i].s, l)) { tok = puncts[i].t; lp += l; break; }
        }
        if (!tok) {
            if (!strchr("+-*/%&|^~!<>=()[]{},;:.", c)) { lp++; die("invalid character '%c'", c); }
            tok = c;
            lp++;
        }
    }
    lasttok = tok;
    if (tok == '{') tokdepth = lex_depth++;
    else if (tok == '}') { if (lex_depth > 0) lex_depth--; tokdepth = lex_depth; }
    else tokdepth = lex_depth;
}

static int accept(int t) { if (tok == t) { next(); return 1; } return 0; }

/* Peek one token ahead: the lexer state is saved and restored. */
typedef struct {
    const char *lp;
    int lline, tok, lasttok, tokline, errline, lex_depth, tokdepth, tokslen;
    i64 tokval;
    char *tokstr;
} LexState;

static void lex_save(LexState *s)
{
    s->lp = lp; s->lline = lline; s->tok = tok; s->lasttok = lasttok; s->tokline = tokline;
    s->errline = errline; s->lex_depth = lex_depth; s->tokdepth = tokdepth; s->tokslen = tokslen;
    s->tokval = tokval; s->tokstr = tokstr;
}

static void lex_restore(LexState *s)
{
    lp = s->lp; lline = s->lline; tok = s->tok; lasttok = s->lasttok; tokline = s->tokline;
    errline = s->errline; lex_depth = s->lex_depth; tokdepth = s->tokdepth; tokslen = s->tokslen;
    tokval = s->tokval; tokstr = s->tokstr;
}

static int peek_tok(void)
{
    LexState st;
    lex_save(&st);
    next();
    int t = tok;
    lex_restore(&st);
    return t;
}

static const char *tokname(int t)
{
    static char b[8];
    if (t < 256) { b[0] = (char)t; b[1] = 0; return b; }
    for (int i = 0; keywords[i].s; i++) if (keywords[i].t == t) return keywords[i].s;
    for (int i = 0; puncts[i].s; i++) if (puncts[i].t == t) return puncts[i].s;
    switch (t) {
    case TK_ID: return "identifier";
    case TK_NUM: return "number";
    case TK_STR: return "string";
    case TK_EOF: return "end of file";
    }
    return "?";
}

static void expect(int t)
{
    if (tok != t) die("expected '%s'", tokname(t));
    next();
}

static void expect_end(void)
{
    if (tok == ';') { next(); return; }
    if (tok == '}' || tok == TK_EOF) return;
    die("expected end of statement (newline or ';')");
}

/* The text of an asm { ... } block: called when the current token '{' has already been read. */
static char *read_asm_raw(void)
{
    int depth = 1;
    const char *s = lp;
    while (*lp) {
        if (*lp == '{') depth++;
        else if (*lp == '}' && --depth == 0) break;
        else if (*lp == '\n') lline++;
        lp++;
    }
    if (!*lp) fatal("unterminated asm block");
    char *txt = xstrndup(s, lp - s);
    lp++;
    if (lex_depth > 0) lex_depth--;
    lasttok = '}';
    next();
    return txt;
}

static void skip_braces(void)
{
    volatile int depth = 0;
    jmp_buf jb;
    int lvl = nerr_stack;
    diag_silent++;
    if (setjmp(jb)) nerr_stack = lvl;      /* a lexical error in the body: pass 2 will report it */
    TRY_PUSH(jb);
    do {
        if (tok == TK_EOF) { diag_silent--; TRY_POP(); fatal("unexpected end of file"); }
        if (tok == TK_ASM) {
            next();
            if (tok == '{') read_asm_raw();
            continue;
        }
        if (tok == '{') depth = depth + 1;          /* depth is volatile: ++ and -- on it are deprecated in C++20 */
        else if (tok == '}') depth = depth - 1;
        next();
    } while (depth > 0);
    diag_silent--;
    TRY_POP();
}

static void skip_to_end(void)
{
    int depth = 0;
    while (tok != TK_EOF) {
        if (tok == '{' || tok == '(' || tok == '[') depth++;
        else if (tok == '}' || tok == ')' || tok == ']') depth--;
        else if (tok == ';' && depth == 0) { next(); return; }
        next();
    }
}

/* ================================= Types =================================== */

enum { TY_VOID, TY_BYTE, TY_CHAR, TY_UINT, TY_INT, TY_PTR, TY_ARRAY, TY_STRUCT, TY_FN, TY_LONG, TY_ULONG };

typedef struct Type Type;
typedef struct Field Field;
typedef struct StructDef StructDef;

struct Field { char *name; Type *ty; int off; Field *next; };
struct StructDef { char *name; Field *fields; int size, align, defined, line; StructDef *next; };
struct Type {
    int kind;
    Type *base;         /* PTR, ARRAY */
    int len;            /* ARRAY */
    StructDef *sd;      /* STRUCT */
    int np;             /* FN */
    Type **params;
    Type *ret;          /* first result or t_void */
    int nret;           /* FN: number of results */
    Type *rets[4];
    int is_int;         /* FN: interrupt handler */
};

static Type t_void = {TY_VOID}, t_byte = {TY_BYTE}, t_char = {TY_CHAR},
            t_uint = {TY_UINT}, t_int = {TY_INT}, t_long = {TY_LONG}, t_ulong = {TY_ULONG};

static StructDef *structs;

static Type *mk_type(int k, Type *base)
{
    Type *t = (Type*)xcalloc(sizeof *t);
    t->kind = k;
    t->base = base;
    return t;
}

static Type *ptr_to(Type *b) { return mk_type(TY_PTR, b); }

static int type_size(Type *t)
{
    switch (t->kind) {
    case TY_BYTE: case TY_CHAR: return 1;
    case TY_UINT: case TY_INT: case TY_PTR: return 2;
    case TY_LONG: case TY_ULONG: return 4;
    case TY_ARRAY: return t->len * type_size(t->base);
    case TY_STRUCT:
        if (!t->sd->defined) die("struct %s is not defined", t->sd->name);
        return t->sd->size;
    }
    die("type has no size");
    return 0;
}

static int type_align(Type *t)
{
    if (t->kind == TY_BYTE || t->kind == TY_CHAR) return 1;
    if (t->kind == TY_ARRAY) return type_align(t->base);
    if (t->kind == TY_STRUCT) return t->sd->align;
    return 2;
}

static int is_narrow(Type *t) { return t->kind == TY_BYTE || t->kind == TY_CHAR; }
static int is_word(Type *t) { return t->kind == TY_UINT || t->kind == TY_INT || t->kind == TY_PTR; }
/* A scalar that fits in one register. The two-word types long and ulong are a separate class (is_wide);
   both are language values (is_val). */
static int is_scalar(Type *t) { return is_narrow(t) || is_word(t); }
static int is_wide(Type *t) { return t->kind == TY_LONG || t->kind == TY_ULONG; }
static int is_val(Type *t) { return is_scalar(t) || is_wide(t); }
static int is_signed(Type *t) { return t->kind == TY_INT || t->kind == TY_CHAR || t->kind == TY_LONG; }

static Type *decay(Type *t)
{
    if (t->kind == TY_ARRAY) return ptr_to(t->base);
    if (t->kind == TY_FN) return ptr_to(t);
    return t;
}

static Type *promote(Type *t)
{
    if (t->kind == TY_BYTE) return &t_uint;
    if (t->kind == TY_CHAR) return &t_int;
    return t;
}

static int elem_size(Type *p)
{
    Type *b = p->base;
    if (b->kind == TY_VOID || b->kind == TY_FN) return 1;
    return type_size(b);
}

static StructDef *get_struct(const char *name)
{
    for (StructDef *s = structs; s; s = s->next) if (!strcmp(s->name, name)) return s;
    StructDef *s = (StructDef*)xcalloc(sizeof *s);
    s->name = xstrndup(name, strlen(name));
    s->align = 2;
    s->next = structs;
    structs = s;
    return s;
}

static StructDef *find_struct(const char *name)
{
    for (StructDef *s = structs; s; s = s->next) if (!strcmp(s->name, name)) return s;
    return NULL;
}

/* Untyped pointer ptr: the element is t_void. */
static Type t_ptr = {TY_PTR, &t_void};
static int is_vptr(Type *t) { return t->kind == TY_PTR && t->base->kind == TY_VOID; }
static int is_fnptr(Type *t) { return t->kind == TY_PTR && t->base->kind == TY_FN; }
/* A pointer with a known element size: address arithmetic is allowed on it. */
static int is_dataptr(Type *t) { return t->kind == TY_PTR && t->base->kind != TY_VOID && t->base->kind != TY_FN; }

static int same_type(Type *a, Type *b)
{
    if (a == b) return 1;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case TY_PTR: return same_type(a->base, b->base);
    case TY_ARRAY: return a->len == b->len && same_type(a->base, b->base);
    case TY_STRUCT: return a->sd == b->sd;
    case TY_FN:
        if (a->np != b->np || a->nret != b->nret || a->is_int != b->is_int) return 0;
        for (int i = 0; i < a->np; i++) if (!same_type(a->params[i], b->params[i])) return 0;
        for (int i = 0; i < a->nret; i++) if (!same_type(a->rets[i], b->rets[i])) return 0;
        return 1;
    }
    return 1;
}

/* A type spelled for messages, as in the source text. */
static void tname_r(Type *t, char *b, size_t n)
{
    size_t l = strlen(b);
    if (l + 1 >= n) return;
    switch (t->kind) {
    case TY_VOID: snprintf(b + l, n - l, "void"); return;
    case TY_BYTE: snprintf(b + l, n - l, "byte"); return;
    case TY_CHAR: snprintf(b + l, n - l, "char"); return;
    case TY_UINT: snprintf(b + l, n - l, "uint"); return;
    case TY_INT: snprintf(b + l, n - l, "int"); return;
    case TY_LONG: snprintf(b + l, n - l, "long"); return;
    case TY_ULONG: snprintf(b + l, n - l, "ulong"); return;
    case TY_STRUCT: snprintf(b + l, n - l, "%s", t->sd->name); return;
    case TY_PTR:
        if (t->base->kind == TY_VOID) { snprintf(b + l, n - l, "ptr"); return; }
        snprintf(b + l, n - l, "ptr ");
        tname_r(t->base, b, n);
        return;
    case TY_ARRAY:
        snprintf(b + l, n - l, "[%d]", t->len);
        tname_r(t->base, b, n);
        return;
    case TY_FN:
        snprintf(b + l, n - l, "%sfn(", t->is_int ? "interrupt " : "");
        for (int i = 0; i < t->np; i++) {
            if (i) strncat(b, ", ", n - strlen(b) - 1);
            tname_r(t->params[i], b, n);
        }
        strncat(b, ")", n - strlen(b) - 1);
        for (int i = 0; i < t->nret; i++) {
            strncat(b, i ? ", " : ": ", n - strlen(b) - 1);
            if (t->rets[i]->kind == TY_PTR && is_fnptr(t->rets[i]) && t->nret > 1) {
                strncat(b, "(", n - strlen(b) - 1);
                tname_r(t->rets[i], b, n);
                strncat(b, ")", n - strlen(b) - 1);
            } else tname_r(t->rets[i], b, n);
        }
        return;
    }
}

static const char *tname(Type *t)
{
    static char bufs[4][256];
    static int k;
    char *b = bufs[k++ & 3];
    b[0] = 0;
    tname_r(t, b, 256);
    return b;
}

/* Implicit assignment of a value of type s (after folding) to an object of type t (sect. 4.2).
   null0 - the value is an untyped constant 0. Returns the error text or NULL. */
static const char *assign_err(Type *t, Type *s, int null0)
{
    if (t->kind == TY_PTR) {
        if (null0) return NULL;
        if (s->kind != TY_PTR) return "an integer cannot be assigned to a pointer: use an explicit cast ptr T(...)";
        if (same_type(t, s)) return NULL;
        if (is_vptr(t) && !is_fnptr(s)) return NULL;
        if (is_vptr(s) && !is_fnptr(t)) return NULL;
        return "incompatible pointer types";
    }
    if (is_val(t) && s->kind == TY_PTR)
        return "a pointer cannot be assigned to an integer: use an explicit cast uint(...)";
    if (is_scalar(t) && is_wide(s))
        return "a 32-bit value cannot be assigned to a narrower integer: use an explicit cast";
    return NULL;
}

/* =========================== Symbols and AST nodes =========================== */

enum { S_VAR, S_CONST, S_FN, S_MMIO, S_ADDR };

typedef struct Node Node;
typedef struct Local Local;
typedef struct Sym Sym;
typedef struct Func Func;

struct Local {
    Sym *gsym;          /* a static local variable: its global object */
    char *name;
    Type *ty;
    int reg;            /* register or -1 (for long and ulong - the low word) */
    int reg2;           /* register of the high word of long and ulong or -1 */
    int on_stack;       /* 0 - none, 1 - frame slot, 2 - incoming argument on the stack */
    int off;
    int addr_taken;
    int pidx;           /* parameter number or -1 */
    int preg;           /* the parameter's first register (R0-R3) or -1 if the parameter is on the stack */
    int psoff;          /* offset of the parameter among the stack arguments in words or -1 */
    int uses;           /* weighted reference count */
    int mentioned;      /* the name occurs in the body (for the unused warning) */
    int line, idx;      /* declaration line; number in the function's variable list */
};

struct Sym {
    char *name;
    char *asmname;      /* label name in the assembler (reserved names get the prefix __) */
    int kind;
    Type *ty;
    i64 val;           /* constant value or MMIO address */
    int is_int, has_body, used_rt, root, line;
    int is_export, is_static;
    int is_extern;      /* extern fn: defined outside B16 (in an assembly module) */
    const char *file;   /* declaration file (for static) */
    int nstr;           /* counter of the object's string literals: name.s<n> */
    int ninit;          /* counter of the function's local-initializer templates: name.i<n> */
    Func *tmpl_of;      /* local-initializer template: the owning function */
    int tmpl_used;      /* the template is needed by the generated code */
    int local_static;   /* a static local variable: visible only through Local */
    Type *rets[4];
    int nret;
    Node *init;
    Sym *next;
};

enum {
    N_NUM, N_STR, N_LOCAL, N_GLOBAL, N_MMIO, N_FNAME, N_REGVAL, N_DISCARD,
    N_BIN, N_CMP, N_LAND, N_LOR, N_LNOT, N_NEG, N_BNOT,
    N_DEREF, N_ADDR, N_INDEX, N_FIELD, N_CALL, N_BUILTIN, N_CAST,
    N_ASSIGN, N_OPASSIGN, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC, N_INITLIST,
    N_BLOCK, N_IF, N_WHILE, N_LOOP, N_BREAK, N_CONTINUE, N_RETURN, N_EXPR,
    N_MASSIGN, N_ATOMIC, N_ASM, N_LABEL, N_GOTO,
    N_DESIG,            /* .field = value in an initializer list */
    N_SYMADDR,          /* address of an assembler label (__image_end, __bss_end) */
    N_LCOPY             /* filling a local object from a template in the data area */
};

struct Node {
    int kind, op, line;
    int sgn;            /* signed operation */
    int lit;            /* untyped constant */
    int ptrdiff;        /* pointer difference: the element size */
    int strid, used;
    Type *ty, *fty;
    Node *a, *b, *c;
    Node **list;
    int n;
    i64 val;
    Local *lv;
    Sym *sym;
    char *str;
    int slen;
    char *sname;        /* string literal: assembler name (owner.s<n>) */
    Field *field;
};

/* A goto label: visible in the whole function. atomic is the atomic block it sits in (0 - outside). */
typedef struct {
    char *name;
    int defined, line, atomic, lab, used;
} LabelDef;

struct Func {
    Sym *sym;
    Local **locals;
    int nl;
    Node *body;
    int has_call;
    int has_wide;       /* the function has computations on long and ulong: temporary registers for pairs are needed */
    LabelDef *labels;
    int nlabels;
    int *atomic_parent;     /* parent of atomic block number i (0 - none) */
    int natomic;
    Sym **refs;             /* functions referenced by the body */
    int nrefs;
    int reachable;
    int has_asm;            /* there are asm blocks: they may access variables through registers */
    int line, end_line;     /* lines of the header and of the closing '}' */
};

static Sym *syms, *syms_tail;
static Node **strings;
static int nstrings;
static int parse_pass;
static Func *cur_func;
static int loopdepth;
static int cur_atomic;      /* number of the current atomic block during parsing */
static Func **funcs;
static int nfuncs;
static int last_rbrace;     /* line of the last '}' of the block */
static int opt_source;      /* -s: the source text as comments in the output */

/* The target environment and the image layout (sect. 3.6, 9.1, 9.2). The effective values are collected by
   resolve_params after pass 1: a command-line key, otherwise the main file's option, otherwise
   the default value. */
enum { T_STD, T_KERNEL };
static int opt_target = T_STD;
static i64 code_org = -1, data_org = -1;   /* code, data; -1 - not set */
static i64 min_free = 2048, max_top = 0;   /* CX16 header fields */

/* Image parameters: option in the source text (sect. 3.6) and keys (sect. 13.2). */
enum { P_TARGET, P_CODE, P_DATA, P_MIN_FREE, P_MAX_TOP, NPARAMS };
typedef struct {
    const char *name, *key;     /* the name in option and the command-line key */
    i64 lo, hi, def;            /* allowed values; def < 0 - no default */
    int even;                   /* the value must be even */
    int src_set, src_line;      /* set by option (a line of the combined text) */
    i64 src_val;
    int cli_set;                /* set by a key */
    i64 cli_val;
} ImageParam;
static ImageParam params[NPARAMS] = {
    {"target",   "--target",   T_STD, T_KERNEL, T_STD, 0, 0, 0, 0, 0, 0},
    {"code",     "--code",     0, 0xFFFE, -1,   1, 0, 0, 0, 0, 0},
    {"data",     "--data",     0, 0xFFFE, -1,   1, 0, 0, 0, 0, 0},
    {"min_free", "--min-free", 0, 0xE000, 2048, 1, 0, 0, 0, 0, 0},
    {"max_top",  "--max-top",  0, 0xFFFF, 0,    0, 0, 0, 0, 0, 0},
};
static const char *pp_root_file;    /* the main source file: option is allowed only in it */
static int opt_all_export;  /* -n: all functions are entry points */
static int emitting;        /* 1 - the function's code is emitted, 0 - only checked */
static Sym *str_owner;      /* owner of string literals, for names owner.s<n> */

static Local *scope[1024];
static int nscope;
static int scope_marks[128];
static int nmarks;

static Sym rt_divs, rt_mods;
static Sym rt_mul32, rt_divu32, rt_modu32, rt_divs32, rt_mods32, rt_shl32, rt_shr32, rt_sar32, rt_udivc32, rt_umodc32;

static const char *file_of(int line)
{
    if (line >= 1 && line <= nlinemap) return linemap[line - 1].file;
    return srcname;
}

static const char *cur_file(void) { return file_of(tokline); }

/* A name visible in the current file: first this file's static, then the global one (sect. 5.1). */
static Sym *find_sym_in(const char *n, const char *file)
{
    for (Sym *s = syms; s; s = s->next)
        if (s->is_static && !s->local_static && s->file == file && !strcmp(s->name, n)) return s;
    for (Sym *s = syms; s; s = s->next)
        if (!s->is_static && !strcmp(s->name, n)) return s;
    return NULL;
}

static Sym *find_sym(const char *n) { return find_sym_in(n, cur_file()); }

static Sym *find_global(const char *n)
{
    for (Sym *s = syms; s; s = s->next) if (!s->is_static && !strcmp(s->name, n)) return s;
    return NULL;
}

/* The prefix of a file's static names in the assembler: the file name without directory and extension, characters outside
   [A-Za-z0-9_] are replaced with '_', and '_' is put before a digit (sect. 10.3). */
typedef struct { const char *file; char *base; } FileBase;
static FileBase *fbases;
static int nfbases;

static const char *static_prefix(const char *file)
{
    for (int i = 0; i < nfbases; i++) if (fbases[i].file == file) return fbases[i].base;
    const char *b = file;
    for (const char *p = file; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    const char *e = strrchr(b, '.');
    if (!e || e == b) e = b + strlen(b);
    char *r = (char*)xcalloc(e - b + 2);
    int k = 0;
    if (isdigit((unsigned char)*b) || b == e) r[k++] = '_';
    for (const char *p = b; p < e; p++) r[k++] = (isalnum((unsigned char)*p) || *p == '_') ? *p : '_';
    for (int i = 0; i < nfbases; i++)
        if (!strcmp(fbases[i].base, r))
            die("static names of %s and %s would both be named %s.* in assembly: rename one of the files", fbases[i].file, file, r);
    fbases = (FileBase*)xrealloc(fbases, sizeof(FileBase) * (nfbases + 1));
    fbases[nfbases].file = file;
    fbases[nfbases].base = r;
    nfbases++;
    return r;
}

static void check_name(const char *n)
{
    if (n[0] == '_' && n[1] == '_') die("names starting with __ are reserved");
}

/* Names that the assembler does not accept as labels (reference, sect. 2): registers,
   mnemonics of instructions and pseudo-instructions, lo, hi. Letter case does not matter. */
static int asm_reserved(const char *n)
{
    static const char *res[] = {
        "LR", "SP", "IP", "FLAGS",
        "LD", "LDZX", "LDSX", "XCHB", "ST", "STB", "ADD", "ADC", "SUB", "SBC", "AND", "OR", "XOR",
        "CMP", "TEST", "SHL", "SHR", "SAR", "ROR", "CALL", "TRAP", "MUL", "MULHU", "DIVU", "REMU",
        "NEG", "NOT", "BSX", "IRET", "WFI",
        "JMP", "JZ", "JE", "JNZ", "JNE", "JC", "JB", "JNC", "JAE", "JA", "JBE", "JS", "JNS",
        "JO", "JNO", "JL", "JGE", "JG", "JLE",
        "MOV", "LEA", "NOP", "RET", "PUSH", "POP", "INC", "DEC", "STI", "CLI", "STC", "CLC", "CMC",
        "LO", "HI", NULL};
    char up[16];
    size_t l = strlen(n);
    if (l >= sizeof up) return 0;
    for (size_t i = 0; i <= l; i++) up[i] = (char)toupper((unsigned char)n[i]);
    for (int i = 0; res[i]; i++) if (!strcmp(up, res[i])) return 1;
    if (up[0] == 'R' && isdigit((unsigned char)up[1]) && (!up[2] || (isdigit((unsigned char)up[2]) && !up[3])))
        return 1;
    return 0;
}

static Sym *add_sym_ex(const char *n, int kind, int is_static)
{
    const char *file = cur_file();
    for (Sym *o = syms; o; o = o->next) {
        if (o->local_static || strcmp(o->name, n)) continue;
        /* conflict: two global names; static and any name of the same file */
        if ((!o->is_static && !is_static) || o->file == file) die("redefinition of %s", n);
    }
    check_name(n);
    Sym *s = (Sym*)xcalloc(sizeof *s);
    s->name = xstrndup(n, strlen(n));
    if (is_static) s->asmname = fmt("%s.%s", static_prefix(file), n);
    else s->asmname = asm_reserved(n) ? fmt("__%s", n) : s->name;
    s->is_static = is_static;
    s->file = file;
    s->line = tokline;
    s->kind = kind;
    if (syms_tail) syms_tail->next = s; else syms = s;
    syms_tail = s;
    return s;
}

static Sym *add_sym(const char *n, int kind) { return add_sym_ex(n, kind, 0); }

static Node *mk(int kind)
{
    Node *e = (Node*)xcalloc(sizeof *e);
    e->kind = kind;
    e->line = tokline;
    e->ty = &t_void;
    return e;
}

static void list_push(Node *e, Node *x)
{
    e->list = (Node**)xrealloc(e->list, sizeof(Node *) * (e->n + 1));
    e->list[e->n++] = x;
}

static Node *mk_num(i64 v)
{
    Node *e = mk(N_NUM);
    e->val = v;
    e->ty = &t_int;
    return e;
}

static i64 norm16(i64 v, int sgn)
{
    v &= 0xFFFF;
    if (sgn && v >= 0x8000) v -= 0x10000;
    return v;
}

/* Bit width of an integer type (a pointer is 16 bits) and normalization of a value to the type. */
static int type_bits(Type *t) { return is_narrow(t) ? 8 : is_wide(t) ? 32 : 16; }

static i64 normt(i64 v, Type *t)
{
    int w = type_bits(t);
    v &= (1LL << w) - 1;
    if (is_signed(t) && v >= (1LL << (w - 1))) v -= 1LL << w;
    return v;
}

/* An untyped constant takes the integer type t (specification 4.7): the value must
   lie in -2^(n-1)...2^n-1, the bit pattern is kept (int($FFFF) = -1). */
static void adopt(Node *c, Type *t, const char *ctx)
{
    if (c->kind != N_NUM || !c->lit || t->kind == TY_PTR || !is_val(t)) return;
    int w = type_bits(t);
    if (c->val < -(1LL << (w - 1)) || c->val > (1LL << w) - 1)
        die("%s: constant %lld does not fit in %s", ctx, c->val, tname(t));
    c->val = normt(c->val, t);
    c->ty = t;
    c->lit = 0;
}

/* The type of a variable from an untyped constant (5.3): int, uint, long or ulong. */
static Type *const_type(Node *init)
{
    if (init->kind == N_NUM && init->lit) {
        i64 v = init->val;
        if (v >= -32768 && v <= 32767) return &t_int;
        if (v >= 0 && v <= 65535) return &t_uint;
        if (v >= -2147483648LL && v <= 2147483647LL) return &t_long;
        if (v >= 0 && v <= 4294967295LL) return &t_ulong;
        die("constant %lld does not fit in any integer type", v);
    }
    return decay(init->ty);
}

static void add_ref(Sym *s)
{
    if (!cur_func) { s->root = 1; return; }
    for (int i = 0; i < cur_func->nrefs; i++) if (cur_func->refs[i] == s) return;
    cur_func->refs = (Sym**)xrealloc(cur_func->refs, sizeof(Sym *) * (cur_func->nrefs + 1));
    cur_func->refs[cur_func->nrefs++] = s;
}

static void scope_push(void) { scope_marks[nmarks++] = nscope; }
static void scope_pop(void) { nscope = scope_marks[--nmarks]; }

static Local *find_local(const char *n)
{
    for (int i = nscope - 1; i >= 0; i--) if (!strcmp(scope[i]->name, n)) return scope[i];
    return NULL;
}

static Local *new_local(char *name, Type *ty)
{
    check_name(name);
    Local *v = (Local*)xcalloc(sizeof *v);
    v->name = name;
    v->ty = ty;
    v->line = tokline;
    v->reg = -1;
    v->reg2 = -1;
    v->pidx = -1;
    v->preg = v->psoff = -1;
    if (nscope >= 1024) die("too many local variables");
    scope[nscope++] = v;
    Func *f = cur_func;
    f->locals = (Local**)xrealloc(f->locals, sizeof(Local *) * (f->nl + 1));
    v->idx = f->nl;
    f->locals[f->nl++] = v;
    return v;
}

static int is_lval(Node *e)
{
    switch (e->kind) {
    case N_LOCAL: case N_GLOBAL: case N_MMIO: case N_DEREF: case N_INDEX: case N_FIELD:
        return 1;
    }
    return 0;
}

static int is_null0(Node *x) { return x->kind == N_NUM && x->lit && x->val == 0; }

/* Checking implicit assignment of the value x to an object of type t (sect. 4.2). */
static Node *mk_cast(Type *t, Node *x);

/* The value of node *px is assigned to an object of type t. A constant takes type t (with a range
   check), a narrower integer is extended with an explicit node when assigned to a two-word type. */
static void check_assign(Type *t, Node **px, const char *ctx)
{
    Node *x = *px;
    Type *s = decay(x->ty);
    const char *m = assign_err(t, s, is_null0(x));
    if (m) die("%s: %s (%s given, %s expected)", ctx, m, tname(s), tname(t));
    if (x->kind == N_NUM && x->lit) adopt(x, t, ctx);
    else if (is_wide(t) && !is_wide(s)) *px = mk_cast(t, x);
}

/* Argument layout (9.3): an argument takes a word or two words (long, ulong); slots 0-3 are the
   registers R0-R3. An argument that did not get enough slots, and all following ones, go on the stack.
   Returns the number of words on the stack; preg[i] is the first register or -1, psoff[i] is the stack offset. */
static int layout_args(Type **pt, int n, int *preg, int *psoff, int *nslots)
{
    int s = 0, sw = 0, inreg = 1;
    for (int i = 0; i < n; i++) {
        int w = is_wide(pt[i]) ? 2 : 1;
        if (inreg && s + w <= 4) { preg[i] = s; psoff[i] = -1; s += w; }
        else { inreg = 0; preg[i] = -1; psoff[i] = sw; sw += w; }
    }
    if (nslots) *nslots = s;
    return sw;
}

static void check_rets(Type **rets, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += is_wide(rets[i]) ? 2 : 1;
    if (w > 4) die("results take %d words: at most 4 (long and ulong take 2)", w);
}

/* ===================== Building nodes and type checking ================== */

static int sign_of(Node *a, Node *b)
{
    Type *ta = promote(decay(a->ty)), *tb = promote(decay(b->ty));
    int sa = is_signed(ta), sb = is_signed(tb);
    if (a->lit) return sb;
    if (b->lit) return sa;
    if (sa != sb) {
        /* an unsigned value no wider than a word is extended to long without loss of value (6.4) */
        if (ta->kind == TY_LONG && is_scalar(tb) && !sb) return 1;
        if (tb->kind == TY_LONG && is_scalar(ta) && !sa) return 1;
        die("signed and unsigned operands mixed: use an explicit int(...), uint(...), long(...) or ulong(...) cast");
    }
    return sa;
}

static void need_scalar(Node *e)
{
    if (e->ty->kind == TY_VOID) die("expression has no value");
    if (!is_val(decay(e->ty))) die("scalar value expected");
}

/* ulong(h) << 16 -> h (h is no wider than a word). */
static Node *shl16_src(Node *n)
{
    if (n->kind == N_BIN && n->op == TK_SHL && n->b->kind == N_NUM && n->b->val == 16 &&
        n->a->kind == N_CAST && is_wide(n->a->ty) && !is_wide(decay(n->a->a->ty)) &&
        decay(n->a->a->ty)->kind != TY_PTR)
        return n->a->a;
    return NULL;
}

/* ulong(l), l unsigned and no wider than a word -> l. */
static Node *zext_src(Node *n)
{
    if (n->kind == N_CAST && is_wide(n->ty) && !is_wide(decay(n->a->ty)) &&
        decay(n->a->ty)->kind != TY_PTR && !is_signed(promote(decay(n->a->ty))))
        return n->a;
    return NULL;
}

/* The constant's value is a power of two (as a 32-bit pattern): the exponent or -1. */
static int wide_pow2(Node *n)
{
    if (n->kind != N_NUM || !is_wide(n->ty)) return -1;
    unsigned long v = (unsigned long)n->val & 0xFFFFFFFFUL;
    if (!v || (v & (v - 1))) return -1;
    int k = 0;
    while (!(v & 1)) { v >>= 1; k++; }
    return k;
}

enum { WM_CALL, WM_ZERO, WM_SHIFT, WM_MULHU };

/* Multiplication of two-word values: what can be done without a runtime call. *x, *y are nodes for WM_SHIFT
   (the multiplicand, the exponent in *k) and WM_MULHU (two unsigned values no wider than a word, or a constant up to 65535). */
static int wide_mul_kind(Node *a, Node *b, Node **x, Node **y, int *k)
{
    for (int swap = 0; swap < 2; swap++) {
        Node *p = swap ? b : a, *q = swap ? a : b;     /* q - a possible constant */
        if (q->kind == N_NUM && is_wide(q->ty)) {
            if (((unsigned long)q->val & 0xFFFFFFFFUL) == 0) return WM_ZERO;
            int e = wide_pow2(q);
            if (e >= 0) { *x = p; *k = e; return WM_SHIFT; }
        }
    }
    for (int swap = 0; swap < 2; swap++) {
        Node *p = swap ? b : a, *q = swap ? a : b;
        Node *zp = zext_src(p);
        if (!zp) continue;
        Node *zq = zext_src(q);
        if (zq) { *x = zp; *y = zq; return WM_MULHU; }
        if (q->kind == N_NUM && is_wide(q->ty) && ((unsigned long)q->val & 0xFFFFFFFFUL) <= 0xFFFF) {
            *x = zp; *y = q;
            return WM_MULHU;
        }
    }
    return WM_CALL;
}

/* are * / % on two-word values performed in place (without a runtime call)? */
static int wide_inline_muldiv(int op, int sgn, Node *a, Node *b)
{
    Node *x, *y;
    int k;
    if (op == '*') return wide_mul_kind(a, b, &x, &y, &k) != WM_CALL;
    if (op == '/' || op == '%') return !sgn && wide_pow2(b) >= 0;
    return 0;
}

/* The result type of + - * & | ^ / % (6.4); an untyped constant takes the type of the other operand. */
static Type *arith_type(Node *a, Node *b)
{
    Type *ta = promote(decay(a->ty)), *tb = promote(decay(b->ty)), *r;
    if (a->lit && !b->lit) { adopt(a, tb, "operand"); ta = tb; }
    else if (b->lit && !a->lit) { adopt(b, ta, "operand"); tb = ta; }
    if ((ta->kind == TY_PTR && is_wide(tb)) || (tb->kind == TY_PTR && is_wide(ta)))
        die("a pointer in a 32-bit operation: use uint(p)");
    if (is_wide(ta)) r = ta;
    else if (is_wide(tb)) r = tb;
    else r = ta;
    if (r->kind == TY_PTR) r = &t_uint;
    return r;
}

#define CONST_LIM (1LL << 62)

/* Folding a binary operation on untyped constants: exact, without wrap-around. */
static i64 fold_exact(int op, i64 x, i64 y)
{
    i64 r = 0;
    switch (op) {
    case '+': if (__builtin_add_overflow(x, y, &r)) die("constant expression is out of range"); break;
    case '-': if (__builtin_sub_overflow(x, y, &r)) die("constant expression is out of range"); break;
    case '*': if (__builtin_mul_overflow(x, y, &r)) die("constant expression is out of range"); break;
    case '&': r = x & y; break;
    case '|': r = x | y; break;
    case '^': r = x ^ y; break;
    case TK_SHL:
        if (y < 0 || y >= 63) die("shift count out of range in constant expression");
        if (__builtin_mul_overflow(x, 1LL << y, &r)) die("constant expression is out of range");
        break;
    case TK_SHR:
        if (y < 0 || y >= 63) die("shift count out of range in constant expression");
        r = x >> y;
        break;
    case '/': if (!y) die("division by zero in constant expression"); r = x / y; break;
    case '%': if (!y) die("division by zero in constant expression"); r = x % y; break;
    }
    if (r < -CONST_LIM || r >= CONST_LIM) die("constant expression is out of range");
    return r;
}

/* Signed division by this constant is performed without a runtime call (sect. 9.6). */
static int const_divisor(Node *b)
{
    if (b->kind != N_NUM) return 0;
    i64 v = norm16(b->val, 1);
    return v != 0 && v != -32768;
}

static Node *mk_bin(int op, Node *a, Node *b)
{
    need_scalar(a);
    need_scalar(b);
    Type *ta = decay(a->ty), *tb = decay(b->ty);
    if (op == '+' && tb->kind == TY_PTR && ta->kind != TY_PTR) {
        Node *t = a; a = b; b = t;
        Type *tt = ta; ta = tb; tb = tt;
    }
    Node *e = mk(N_BIN);
    e->op = op;
    e->a = a;
    e->b = b;
    if ((op == '+' || op == '-') && (ta->kind == TY_PTR || tb->kind == TY_PTR)) {
        if (op == '-' && ta->kind != TY_PTR) die("an integer minus a pointer is not allowed");
        if (!is_dataptr(ta) || (tb->kind == TY_PTR && !is_dataptr(tb)))
            die("pointer arithmetic on %s: the element size is unknown (cast to ptr byte)", tname(is_dataptr(ta) ? tb : ta));
    }
    if ((op == '+' || op == '-') && ta->kind == TY_PTR && is_wide(tb))
        die("pointer arithmetic with a 32-bit value: use uint(...)");
    if ((op == '+' || op == '-') && ta->kind == TY_PTR && tb->kind != TY_PTR) adopt(b, &t_int, "pointer arithmetic");
    if ((op == '+' || op == '-') && ta->kind == TY_PTR) {
        if (tb->kind == TY_PTR) {
            if (op != '-') die("cannot add two pointers");
            if (!same_type(ta, tb)) die("difference of pointers of different types (%s and %s)", tname(ta), tname(tb));
            e->ty = &t_int;
            e->ptrdiff = elem_size(ta);
            if (log2exact(e->ptrdiff) < 0) die("pointer difference requires an element size that is a power of 2");
        } else {
            e->ty = ta;
        }
        return e;
    }
    if (cur_func && (is_wide(promote(ta)) || is_wide(promote(tb)))) cur_func->has_wide = 1;
    if (op == TK_SHR || op == TK_SHL) {
        if (b->kind == N_NUM && b->val < 0) die("negative shift count");
        Type *tl = a->lit ? &t_int : promote(ta);
        if (a->lit && !(b->kind == N_NUM)) adopt(a, &t_int, "operand of a shift");
        e->ty = tl;
        e->sgn = is_signed(tl);
    } else {
        if (op == '/' || op == '%') e->sgn = sign_of(a, b);
        e->ty = arith_type(a, b);
    }
    if (op != TK_SHL && op != TK_SHR && is_wide(e->ty)) {
        /* the narrower operand is extended to a double word by the sign of its type */
        if (!is_wide(decay(a->ty))) a = e->a = mk_cast(e->ty, a);
        if (!is_wide(decay(b->ty))) b = e->b = mk_cast(e->ty, b);
    }
    if ((op == TK_SHL || op == TK_SHR) && b->kind != N_NUM && is_wide(decay(b->ty)))
        b = e->b = mk_cast(&t_uint, b);        /* the shift count is the low word */
    if ((op == TK_SHL || op == TK_SHR) && b->kind != N_NUM && is_wide(e->ty) && cur_func)
        cur_func->has_call = 1;                /* a shift by a variable count is a runtime call */
    if (a->kind == N_NUM && b->kind == N_NUM) {
        if (a->lit && b->lit) {
            Node *n = mk_num(fold_exact(op, a->val, b->val));
            n->lit = 1;
            return n;
        }
        /* at least one constant is typed: computation in the width of the result type */
        if (a->lit) adopt(a, e->ty, "operand");
        if (b->lit && op != TK_SHL && op != TK_SHR) adopt(b, e->ty, "operand");
        i64 x = a->val, y = b->val, r = 0;
        int w = type_bits(e->ty);
        switch (op) {
        case '+': r = x + y; break;
        case '-': r = x - y; break;
        case '*': r = x * y; break;
        case '&': r = x & y; break;
        case '|': r = x | y; break;
        case '^': r = x ^ y; break;
        case TK_SHL: r = y >= w ? 0 : x << y; break;
        case TK_SHR: r = y >= w ? (e->sgn && x < 0 ? -1 : 0) : x >> y; break;
        case '/': if (!y) die("division by zero in constant expression"); r = x / y; break;
        case '%': if (!y) die("division by zero in constant expression"); r = x % y; break;
        }
        Node *n = mk_num(normt(r, e->ty));
        n->ty = e->ty;
        return n;
    }
    if ((op == '*' || op == '/' || op == '%') && is_wide(e->ty) && cur_func && !wide_inline_muldiv(op, e->sgn, a, b))
        cur_func->has_call = 1;                /* runtime call */
    if ((op == '/' || op == '%') && e->sgn && cur_func && !const_divisor(b)) cur_func->has_call = 1;
    return e;
}

static Node *mk_cmp(int op, Node *a, Node *b)
{
    need_scalar(a);
    need_scalar(b);
    Type *ta = decay(a->ty), *tb = decay(b->ty);
    if (ta->kind == TY_PTR || tb->kind == TY_PTR) {
        if (ta->kind == TY_PTR && tb->kind == TY_PTR) {
            if (!same_type(ta, tb) && !(is_vptr(ta) && !is_fnptr(tb)) && !(is_vptr(tb) && !is_fnptr(ta)))
                die("comparison of incompatible pointer types (%s and %s)", tname(ta), tname(tb));
        } else if (!is_null0(ta->kind == TY_PTR ? b : a)) {
            die("comparison of a pointer with an integer: only the constant 0 is allowed (or cast explicitly)");
        }
    }
    for (int k = 0; k < 2; k++) {
        Node *c = k ? a : b, *v = k ? b : a;
        if (c->kind == N_NUM && c->lit && c->val < 0 && v->kind != N_NUM && !is_signed(promote(decay(v->ty))))
            warn_at(c->line, "comparison of an unsigned value with a negative constant");
    }
    if (cur_func && (is_wide(promote(ta)) || is_wide(promote(tb)))) cur_func->has_wide = 1;
    Node *e = mk(N_CMP);
    e->op = op;
    e->a = a;
    e->b = b;
    e->ty = &t_int;
    e->sgn = (op == TK_EQ || op == TK_NE) ? 0 : sign_of(a, b);
    if (ta->kind != TY_PTR && tb->kind != TY_PTR) {
        if (a->lit && !b->lit) adopt(a, promote(tb), "comparison");
        else if (b->lit && !a->lit) adopt(b, promote(ta), "comparison");
    }
    if (ta->kind != TY_PTR && tb->kind != TY_PTR) {
        Type *pa = promote(ta), *pb = promote(tb);
        Type *wt = is_wide(pa) ? pa : is_wide(pb) ? pb : NULL;
        if (wt) {
            if (!is_wide(decay(a->ty))) a = e->a = mk_cast(wt, a);
            if (!is_wide(decay(b->ty))) b = e->b = mk_cast(wt, b);
        }
    }
    if (a->kind == N_NUM && b->kind == N_NUM) {
        i64 x = a->val, y = b->val, r = 0;
        if (op == TK_EQ || op == TK_NE) {
            /* bit patterns in the common width */
            int w = (a->lit || b->lit) ? 64 : (is_wide(a->ty) || is_wide(b->ty)) ? 32 : 16;
            if (w < 64) { x &= (1LL << w) - 1; y &= (1LL << w) - 1; }
        }
        switch (op) {
        case TK_EQ: r = x == y; break;
        case TK_NE: r = x != y; break;
        case '<': r = x < y; break;
        case '>': r = x > y; break;
        case TK_LE: r = x <= y; break;
        case TK_GE: r = x >= y; break;
        }
        Node *n = mk_num(r);
        n->lit = 1;
        return n;
    }
    return e;
}

static Node *mk_logic(int kind, Node *a, Node *b)
{
    need_scalar(a);
    if (b) need_scalar(b);
    if (a->kind == N_NUM && (!b || b->kind == N_NUM)) {
        i64 r = kind == N_LAND ? (a->val && b->val) : kind == N_LOR ? (a->val || b->val) : !a->val;
        Node *n = mk_num(r);
        n->lit = 1;
        return n;
    }
    Node *e = mk(kind);
    e->a = a;
    e->b = b;
    e->ty = &t_int;
    return e;
}

static Node *mk_cast(Type *t, Node *x)
{
    need_scalar(x);
    if (!is_val(t)) die("can only cast to a scalar type");
    Type *s = decay(x->ty);
    if (is_narrow(t) && s->kind == TY_PTR) die("a pointer can be cast only to uint, int or a pointer type");
    if (is_wide(t) && s->kind == TY_PTR) die("a pointer can be cast only to uint, int or a pointer type: use uint(p) first");
    if (t->kind == TY_PTR && is_wide(s)) die("a long or ulong value cannot be cast to a pointer: use uint(x) first");
    if (x->kind == N_NUM) {
        i64 v = x->val;
        if (x->lit) {
            int w = type_bits(t);
            if (v < -(1LL << (w - 1)) || v > (1LL << w) - 1)
                die("cast: constant %lld does not fit in %s", v, tname(t));
        }
        Node *n = mk_num(normt(v, t));
        n->ty = t;
        return n;
    }
    Node *e = mk(N_CAST);
    e->a = x;
    e->ty = t;
    return e;
}

static Node *mk_assign(Node *l, Node *r)
{
    if (!is_lval(l) || !is_val(l->ty)) die("left side of '=' must be a scalar variable");
    need_scalar(r);
    check_assign(l->ty, &r, "assignment");
    Node *e = mk(N_ASSIGN);
    e->a = l;
    e->b = r;
    e->ty = l->ty;
    return e;
}

static Node *mk_incdec(int kind, Node *l)
{
    if (!is_lval(l) || !is_val(l->ty)) die("++/-- require a scalar variable");
    if (cur_func && is_wide(l->ty)) cur_func->has_wide = 1;
    if (l->ty->kind == TY_PTR && !is_dataptr(l->ty)) die("++/-- on %s: the element size is unknown", tname(l->ty));
    Node *e = mk(kind);
    e->a = l;
    e->ty = l->ty;
    return e;
}

/* ================================= Parser ================================= */

static Node *parse_expr(void);
static Node *parse_bin(int minlev);
static Node *parse_unary(void);
static Type *parse_type(void);
static Node *parse_block(void);
static Node *parse_stmt(void);
static void gen_func(Func *f);
static int peek_char(void);
static LabelDef *get_label(const char *name);
static char *asm_refs(const char *t);

static i64 const_expr(void)
{
    Node *e = parse_bin(0);
    if (e->kind != N_NUM) die("constant expression expected");
    return e->val;
}

static int starts_scalar_type(int t)
{
    return t == TK_BYTE || t == TK_CHAR || t == TK_INT || t == TK_UINT || t == TK_LONG || t == TK_ULONG || t == TK_PTR;
}

static int starts_type(int t)
{
    return starts_scalar_type(t) || t == '[' || t == TK_ID || t == TK_FN || t == TK_INTERRUPT;
}

/* The result type of a function type: scalars only. */
static Type *parse_result_type(void)
{
    Type *r = parse_type();
    if (!is_val(r)) die("result must be a scalar");
    return r;
}

static Type *parse_type(void)
{
    switch (tok) {
    case TK_BYTE: next(); return &t_byte;
    case TK_CHAR: next(); return &t_char;
    case TK_INT: next(); return &t_int;
    case TK_UINT: next(); return &t_uint;
    case TK_LONG: next(); return &t_long;
    case TK_ULONG: next(); return &t_ulong;
    case TK_PTR:
        next();
        if (!starts_type(tok)) return &t_ptr;   /* untyped pointer */
        return ptr_to(parse_type());
    case '[': {
        next();
        i64 n = const_expr();
        expect(']');
        if (n <= 0) die("array size must be positive");
        if (n > 0xFFFF) die("array size is too large");
        Type *t = mk_type(TY_ARRAY, parse_type());
        t->len = (int)n;
        return t;
    }
    case TK_INTERRUPT: {
        next();
        if (tok != TK_FN) die("expected 'fn' after 'interrupt'");
        next();
        expect('(');
        if (tok != ')') die("an interrupt handler takes no parameters");
        next();
        if (tok == ':') die("an interrupt handler returns no values");
        Type *t = mk_type(TY_FN, NULL);
        t->is_int = 1;
        t->ret = &t_void;
        return t;
    }
    case TK_FN: {
        next();
        expect('(');
        Type *t = mk_type(TY_FN, NULL);
        while (tok != ')') {
            Type *p = parse_type();
            if (!is_val(p)) die("function parameter must be a scalar");
            if (t->np == 32) die("too many parameters");
            t->params = (Type**)xrealloc(t->params, sizeof(Type *) * (t->np + 1));
            t->params[t->np++] = p;
            if (!accept(',')) break;
        }
        expect(')');
        if (accept(':')) {
            /* A result list: ": T1, T2" (a comma followed by a type continues the list of this
               function type) or in parentheses ": (T1, T2)" to limit it. */
            if (accept('(')) {
                do {
                    if (t->nret == 4) die("a function returns at most 4 values");
                    t->rets[t->nret++] = parse_result_type();
                } while (accept(','));
                expect(')');
            } else {
                t->rets[t->nret++] = parse_result_type();
                while (tok == ',' && starts_scalar_type(peek_tok())) {
                    next();
                    if (t->nret == 4) die("a function returns at most 4 values");
                    t->rets[t->nret++] = parse_result_type();
                }
            }
        }
        check_rets(t->rets, t->nret);
        t->ret = t->nret ? t->rets[0] : &t_void;
        return t;
    }
    case TK_ID: {
        StructDef *sd = parse_pass == 1 ? get_struct(tokstr) : find_struct(tokstr);
        if (!sd) die("unknown type %s", tokstr);
        next();
        Type *t = mk_type(TY_STRUCT, NULL);
        t->sd = sd;
        return t;
    }
    }
    die("type expected");
    return NULL;
}

enum { B_MULHU, B_ROR, B_XCHB, B_DI, B_EI, B_WFI, B_TRAP };
static const char *bnames[] = {"mulhu", "ror", "xchb", "di", "ei", "wfi", "trap", NULL};
static const int bargs[] = {2, 2, 2, 0, 0, 0, 1};

static Node *parse_builtin(int b)
{
    Node *e = mk(N_BUILTIN);
    e->op = b;
    expect('(');
    while (tok != ')') {
        list_push(e, parse_expr());
        if (!accept(',')) break;
    }
    expect(')');
    if (e->n != bargs[b]) die("%s: expected %d argument(s)", bnames[b], bargs[b]);
    for (int i = 0; i < e->n; i++) need_scalar(e->list[i]);
    switch (b) {
    case B_MULHU: case B_ROR:
        for (int i = 0; i < 2; i++) check_assign(&t_uint, &e->list[i], fmt("%s: argument %d", bnames[b], i + 1));
        e->ty = &t_uint;
        break;
    case B_XCHB: {
        Type *p = decay(e->list[0]->ty);
        if (p->kind != TY_PTR || !is_narrow(p->base)) die("xchb: first argument must be a pointer to byte");
        check_assign(&t_uint, &e->list[1], "xchb: argument 2");
        e->ty = &t_uint;
        break;
    }
    case B_TRAP:
        if (e->list[0]->kind != N_NUM || e->list[0]->val < 0 || e->list[0]->val > 15)
            die("trap: vector number must be a constant 0..15");
        break;
    }
    return e;
}

static Node *parse_call(Node *f)
{
    Type *ft = f->ty;
    if (ft->kind == TY_PTR && ft->base->kind == TY_FN) ft = ft->base;
    if (ft->kind != TY_FN) die("called object is not a function");
    Node *e = mk(N_CALL);
    e->a = f;
    e->fty = ft;
    e->sym = f->kind == N_FNAME ? f->sym : NULL;
    expect('(');
    while (tok != ')') {
        Node *x = parse_expr();
        need_scalar(x);
        list_push(e, x);
        if (!accept(',')) break;
    }
    expect(')');
    if (ft->is_int) die("an interrupt handler cannot be called");
    if (e->n != ft->np) die("wrong number of arguments: expected %d", ft->np);
    for (int i = 0; i < e->n; i++) {
        errline = e->list[i]->line;
        check_assign(ft->params[i], &e->list[i], fmt("argument %d", i + 1));
    }
    e->ty = ft->ret;
    if (cur_func) cur_func->has_call = 1;
    return e;
}

static Node *parse_primary(void)
{
    Node *e;
    switch (tok) {
    case TK_NUM:
        e = mk_num(tokval);
        e->lit = 1;
        next();
        return e;
    case TK_STR:
        e = mk(N_STR);
        e->str = tokstr;
        e->slen = tokslen;
        e->ty = mk_type(TY_ARRAY, &t_byte);
        e->ty->len = tokslen + 1;
        e->strid = nstrings;
        strings = (Node**)xrealloc(strings, sizeof(Node *) * (nstrings + 1));
        strings[nstrings++] = e;
        next();
        return e;
    case '(':
        next();
        e = parse_expr();
        expect(')');
        return e;
    case TK_OFFSETOF: {
        next();
        expect('(');
        if (tok != TK_ID) die("offsetof: struct name expected");
        StructDef *sd = find_struct(tokstr);
        if (!sd || !sd->defined) die("offsetof: struct %s is not defined", tokstr);
        next();
        expect(',');
        i64 off = 0;
        for (;;) {
            if (tok != TK_ID) die("offsetof: field name expected");
            Field *f = sd->fields;
            while (f && strcmp(f->name, tokstr)) f = f->next;
            if (!f) die("struct %s has no field %s", sd->name, tokstr);
            off += f->off;
            next();
            if (!accept('.')) break;
            if (f->ty->kind != TY_STRUCT) die("offsetof: field %s is not a struct", f->name);
            sd = f->ty->sd;
        }
        expect(')');
        e = mk_num(off);
        e->lit = 1;
        return e;
    }
    case TK_SIZEOF: {
        next();
        expect('(');
        Type *t = parse_type();
        expect(')');
        e = mk_num(type_size(t));
        e->lit = 1;
        return e;
    }
    case TK_BYTE: case TK_CHAR: case TK_INT: case TK_UINT: case TK_LONG: case TK_ULONG: case TK_PTR: {
        Type *t = parse_type();
        expect('(');
        Node *x = parse_expr();
        expect(')');
        return mk_cast(t, x);
    }
    case TK_ID: {
        char *name = tokstr;
        next();
        if (!strcmp(name, "_")) return mk(N_DISCARD);
        if (tok == '(')
            for (int b = 0; bnames[b]; b++)
                if (!strcmp(bnames[b], name)) return parse_builtin(b);
        Local *v = cur_func ? find_local(name) : NULL;
        if (v && v->gsym) {
            v->mentioned = 1;
            e = mk(N_GLOBAL);
            e->sym = v->gsym;
            e->ty = v->gsym->ty;
            return e;
        }
        if (v) {
            e = mk(N_LOCAL);
            e->lv = v;
            e->ty = v->ty;
            v->uses += 1 << (2 * (loopdepth > 3 ? 3 : loopdepth));
            v->mentioned = 1;
            return e;
        }
        Sym *s = find_sym(name);
        if (!s) die("unknown name %s", name);
        switch (s->kind) {
        case S_CONST: e = mk_num(s->val); e->lit = 1; return e;
        case S_VAR: e = mk(N_GLOBAL); break;
        case S_MMIO: e = mk(N_MMIO); break;
        case S_ADDR: e = mk(N_SYMADDR); e->str = s->asmname; e->ty = s->ty; return e;
        default:
            e = mk(N_FNAME);
            add_ref(s);
            break;
        }
        e->sym = s;
        e->ty = s->ty;
        return e;
    }
    }
    die("expression expected");
    return NULL;
}

static Node *parse_postfix(void)
{
    Node *e = parse_primary();
    for (;;) {
        if (tok == '(') {
            e = parse_call(e);
        } else if (tok == '[') {
            next();
            Node *i = parse_expr();
            expect(']');
            Type *t = e->ty;
            if (t->kind != TY_ARRAY && t->kind != TY_PTR) die("only arrays and pointers can be indexed");
            if (is_wide(decay(i->ty))) die("index of a 32-bit type: use uint(...) or int(...)");
            if (!is_scalar(decay(i->ty)) || decay(i->ty)->kind == TY_PTR) die("index must be an integer");
            adopt(i, &t_int, "index");
            Node *x = mk(N_INDEX);
            x->a = e;
            x->b = i;
            x->ty = t->base;
            if (x->ty->kind == TY_VOID || x->ty->kind == TY_FN) die("indexing an untyped pointer");
            e = x;
        } else if (tok == '.') {
            next();
            if (tok != TK_ID) die("field name expected");
            Type *t = e->ty;
            if (t->kind == TY_PTR) t = t->base;
            if (t->kind != TY_STRUCT) die("'.' requires a struct or a pointer to struct");
            Field *f = t->sd->fields;
            while (f && strcmp(f->name, tokstr)) f = f->next;
            if (!f) die("struct %s has no field %s", t->sd->name, tokstr);
            next();
            Node *x = mk(N_FIELD);
            x->a = e;
            x->field = f;
            x->ty = f->ty;
            e = x;
        } else if (tok == TK_INC || tok == TK_DEC) {
            int k = tok == TK_INC ? N_POSTINC : N_POSTDEC;
            next();
            e = mk_incdec(k, e);
        } else {
            return e;
        }
    }
}

static Node *parse_unary(void)
{
    Node *x, *e;
    switch (tok) {
    case '-':
        next();
        x = parse_unary();
        need_scalar(x);
        if (x->kind == N_NUM) {
            if (x->lit) {
                if (x->val <= -CONST_LIM) die("constant expression is out of range");
                e = mk_num(-x->val);
                e->lit = 1;
            } else {
                e = mk_num(normt(-x->val, x->ty));
                e->ty = x->ty;
            }
            return e;
        }
        if (cur_func && is_wide(promote(decay(x->ty)))) cur_func->has_wide = 1;
        e = mk(N_NEG);
        e->a = x;
        e->ty = promote(decay(x->ty));
        if (e->ty->kind == TY_PTR) e->ty = &t_uint;
        return e;
    case '~':
        next();
        x = parse_unary();
        need_scalar(x);
        if (x->kind == N_NUM) {
            if (x->lit) {
                e = mk_num(~x->val);
                e->lit = 1;
            } else {
                e = mk_num(normt(~x->val, x->ty));
                e->ty = x->ty;
            }
            return e;
        }
        if (cur_func && is_wide(promote(decay(x->ty)))) cur_func->has_wide = 1;
        e = mk(N_BNOT);
        e->a = x;
        e->ty = promote(decay(x->ty));
        if (e->ty->kind == TY_PTR) e->ty = &t_uint;
        return e;
    case '!':
        next();
        return mk_logic(N_LNOT, parse_unary(), NULL);
    case '*': {
        next();
        x = parse_unary();
        Type *t = decay(x->ty);
        if (t->kind != TY_PTR) die("'*' requires a pointer");
        if (t->base->kind == TY_VOID || t->base->kind == TY_FN) die("dereferencing an untyped pointer");
        e = mk(N_DEREF);
        e->a = x;
        e->ty = t->base;
        return e;
    }
    case '&':
        next();
        x = parse_unary();
        if (x->kind == N_FNAME) return x;
        if (x->kind == N_DEREF) return x->a;
        if (x->kind == N_MMIO) { e = mk_num(x->sym->val); e->ty = ptr_to(x->ty); return e; }
        if (!is_lval(x)) die("'&' requires a variable");
        if (x->kind == N_LOCAL) x->lv->addr_taken = 1;
        e = mk(N_ADDR);
        e->a = x;
        e->ty = ptr_to(x->ty);
        return e;
    case TK_INC: case TK_DEC: {
        int k = tok == TK_INC ? N_PREINC : N_PREDEC;
        next();
        return mk_incdec(k, parse_unary());
    }
    }
    return parse_postfix();
}

static int binop_level(int t)
{
    switch (t) {
    case TK_OROR: return 0;
    case TK_ANDAND: return 1;
    case TK_EQ: case TK_NE: case '<': case '>': case TK_LE: case TK_GE: return 2;
    case '|': return 3;
    case '^': return 4;
    case '&': return 5;
    case TK_SHL: case TK_SHR: return 6;
    case '+': case '-': return 7;
    case '*': case '/': case '%': return 8;
    }
    return -1;
}

static Node *parse_bin(int minlev)
{
    Node *l = parse_unary();
    for (;;) {
        int lev = binop_level(tok);
        if (lev < 0 || lev < minlev) return l;
        int op = tok;
        next();
        Node *r = parse_bin(lev + 1);
        if (op == TK_OROR) l = mk_logic(N_LOR, l, r);
        else if (op == TK_ANDAND) l = mk_logic(N_LAND, l, r);
        else if (lev == 2) {
            l = mk_cmp(op, l, r);
            if (binop_level(tok) == 2) die("comparisons cannot be chained");
        } else l = mk_bin(op, l, r);
    }
}

static int opassign_op(int t)
{
    switch (t) {
    case TK_ADDEQ: return '+';
    case TK_SUBEQ: return '-';
    case TK_MULEQ: return '*';
    case TK_DIVEQ: return '/';
    case TK_MODEQ: return '%';
    case TK_ANDEQ: return '&';
    case TK_OREQ: return '|';
    case TK_XOREQ: return '^';
    case TK_SHLEQ: return TK_SHL;
    case TK_SHREQ: return TK_SHR;
    }
    return 0;
}

static Node *parse_expr(void)
{
    Node *l = parse_bin(0);
    if (tok == '=') {
        next();
        return mk_assign(l, parse_expr());
    }
    int op = opassign_op(tok);
    if (op) {
        next();
        Node *r = parse_expr();
        if (!is_lval(l) || !is_val(l->ty)) die("left side of compound assignment must be a variable");
        Node *e = mk(N_OPASSIGN);
        e->op = op;
        e->a = l;
        e->b = r;
        e->c = mk_bin(op, l, r);
        check_assign(l->ty, &e->c, "compound assignment");
        if (e->c->kind != N_BIN || e->c->a != l) die("invalid compound assignment");
        e->ty = l->ty;
        return e;
    }
    return l;
}

static Node *parse_init(void);

/* ---------- an initializer of a local array or struct (sect. 5.3) ----------
   The object is filled on every execution of the declaration. A small object (up to 16 bytes)
   is filled by assigning all of its scalar parts; a larger one is copied from a template in
   the data area (name function.i<n>), then the parts with non-constant
   values are assigned. */
static int is_static_init(Node *e)
{
    switch (e->kind) {
    case N_NUM: case N_STR: case N_FNAME: case N_SYMADDR: return 1;
    case N_CAST: return is_static_init(e->a);
    case N_ADDR: {
        Node *x = e->a;
        while (x->kind == N_FIELD || (x->kind == N_INDEX && x->b->kind == N_NUM)) x = x->a;
        return x->kind == N_GLOBAL;
    }
    }
    return 0;
}

static Node *mk_index_c(Node *a, int i, Type *t)
{
    Node *x = mk(N_INDEX);
    x->a = a;
    x->b = mk_num(i);
    x->b->lit = 1;
    x->ty = t;
    return x;
}

typedef struct { Node **lv, **val; int n; } Slots;

static void slot_push(Slots *sl, Node *lv, Node *val)
{
    sl->lv = (Node**)xrealloc(sl->lv, sizeof(Node *) * (sl->n + 1));
    sl->val = (Node**)xrealloc(sl->val, sizeof(Node *) * (sl->n + 1));
    sl->lv[sl->n] = lv;
    sl->val[sl->n++] = val;
}

/* Decompose the initializer in of object lv of type t into scalar parts. Non-constant values
   in the template (tmpl) are replaced with zeros. */
static Node *init_slots(Type *t, Node *in, Node *lv, Slots *sl)
{
    if (in && in->kind == N_DESIG) die("a field name is allowed only in a struct initializer");
    if (is_val(t)) {
        if (in && in->kind == N_INITLIST) die("a scalar is initialized with a single value");
        if (in) { need_scalar(in); check_assign(t, &in, "initializer"); }
        slot_push(sl, lv, in);
        return in && is_static_init(in) ? in : NULL;
    }
    if (t->kind == TY_ARRAY) {
        if (in && in->kind == N_STR) {
            if (!is_narrow(t->base)) die("only byte arrays can be initialized with a string");
            if (in->slen + 1 > t->len) die("string is longer than the array");
            for (int i = 0; i < t->len; i++) {
                Node *c = NULL;
                if (i < in->slen) { c = mk_num((unsigned char)in->str[i]); c->lit = 1; }
                slot_push(sl, mk_index_c(lv, i, t->base), c);
            }
            return in;
        }
        if (in && in->kind != N_INITLIST) die("an array is initialized with {...} or a string");
        if (in && in->n > t->len) die("too many values in initializer");
        Node *tl = mk(N_INITLIST);
        for (int i = 0; i < t->len; i++) {
            Node *it = in && i < in->n ? in->list[i] : NULL;
            Node *r = init_slots(t->base, it, mk_index_c(lv, i, t->base), sl);
            if (in && i < in->n) list_push(tl, r);
        }
        if (tl->n == 0) return NULL;
        return tl;
    }
    if (t->kind == TY_STRUCT) {
        if (in && in->kind != N_INITLIST) die("a struct is initialized with {...}");
        StructDef *sd = t->sd;
        int nf = 0;
        for (Field *f = sd->fields; f; f = f->next) nf++;
        Node **vals = (Node**)xcalloc(sizeof(Node *) * (nf + 1));
        Field **fl = (Field**)xcalloc(sizeof(Field *) * (nf + 1));
        int k = 0, pos = 0;
        for (Field *f = sd->fields; f; f = f->next) fl[k++] = f;
        for (int i = 0; in && i < in->n; i++) {
            Node *it = in->list[i];
            if (it->kind == N_DESIG) {
                int j = 0;
                while (j < nf && strcmp(fl[j]->name, it->str)) j++;
                if (j == nf) die("struct %s has no field %s", sd->name, it->str);
                pos = j;
                it = it->a;
            }
            if (pos >= nf) die("too many values in initializer of struct %s", sd->name);
            if (vals[pos]) die("field %s is initialized twice", fl[pos]->name);
            vals[pos++] = it;
        }
        Node *tl = mk(N_INITLIST);
        for (int j = 0; j < nf; j++) {
            Node *x = mk(N_FIELD);
            x->a = lv;
            x->field = fl[j];
            x->ty = fl[j]->ty;
            Node *r = init_slots(fl[j]->ty, vals[j], x, sl);
            Node *d = mk(N_DESIG);
            d->str = fl[j]->name;
            d->a = r;
            if (r) list_push(tl, d);
        }
        free(vals);
        free(fl);
        return tl;
    }
    die("this type cannot be initialized");
    return NULL;
}

static Node *local_aggregate_init(Local *v, Node *init, int line)
{
    Node *blk = mk(N_BLOCK);
    Node *lv = mk(N_LOCAL);
    lv->lv = v;
    lv->ty = v->ty;
    v->uses++;
    Slots sl = {NULL, NULL, 0};
    Node *tmpl = init_slots(v->ty, init, lv, &sl);
    int size = type_size(v->ty);
    int copy = size > 16;
    if (copy) {
        Sym *g = (Sym*)xcalloc(sizeof *g);
        g->name = v->name;
        g->asmname = fmt("%s.i%d", cur_func->sym->asmname, cur_func->sym->ninit++);
        g->kind = S_VAR;
        g->ty = v->ty;
        g->init = tmpl ? tmpl : mk(N_INITLIST);
        g->is_static = g->local_static = 1;
        g->file = cur_file();
        g->line = line;
        g->tmpl_of = cur_func;
        if (syms_tail) syms_tail->next = g; else syms = g;
        syms_tail = g;
        Node *c = mk(N_LCOPY);
        c->line = line;
        c->a = lv;
        c->sym = g;
        c->val = (size + 1) & ~1;
        list_push(blk, c);
    }
    for (int i = 0; i < sl.n; i++) {
        Node *val = sl.val[i];
        if (copy && (!val || is_static_init(val))) continue;
        if (!val) { val = mk_num(0); val->lit = 1; }
        Node *st = mk(N_EXPR);
        st->line = line;
        errline = line;
        st->a = mk_assign(sl.lv[i], val);
        list_push(blk, st);
    }
    free(sl.lv);
    free(sl.val);
    return blk;
}

static Node *parse_local_var(void)
{
    int line = tokline;
    next();
    char *names[32];
    int nn = 0;
    do {
        if (tok != TK_ID) die("variable name expected");
        if (nn == 32) die("too many names");
        names[nn++] = tokstr;
        next();
    } while (accept(','));
    Type *volatile t = NULL;
    Node *volatile init = NULL;
    jmp_buf jb;
    int lvl = nerr_stack;
    if (setjmp(jb)) {
        /* an error in the type or initializer: the names are declared anyway */
        nerr_stack = lvl;
        for (int i = 0; i < nn; i++) new_local(names[i], t ? t : &t_int)->uses++;
        rethrow();
    }
    TRY_PUSH(jb);
    if (accept(':')) t = parse_type();
    int aggregate = 0;
    if (accept('=')) {
        if (nn != 1) die("initializer allowed for a single variable only");
        if (t && !is_val(t)) {
            aggregate = 1;
            init = parse_init();
        } else {
            init = parse_expr();
            need_scalar(init);
            if (!t) {
                if (init->kind == N_STR) t = ptr_to(&t_byte);
                else t = const_type(init);
            }
        }
    }
    if (!t) die("type or initializer required");
    if (t->kind == TY_VOID || t->kind == TY_FN) die("invalid variable type");
    type_size(t);
    errline = line;
    if (init && !aggregate) { Node *iv = init; check_assign(t, &iv, "initialization"); init = iv; }
    TRY_POP();
    expect_end();
    Node *blk = mk(N_BLOCK);
    for (int i = 0; i < nn; i++) {
        Local *v = new_local(names[i], t);
        v->line = line;
        if (aggregate) {
            list_push(blk, local_aggregate_init(v, init, line));
        } else if (init) {
            Node *l = mk(N_LOCAL);
            l->lv = v;
            l->ty = t;
            v->uses++;
            Node *s = mk(N_EXPR);
            s->line = line;
            errline = line;
            s->a = mk_assign(l, init);
            list_push(blk, s);
        }
    }
    return blk;
}

static Node *parse_stmt_body(void);
static Node *parse_init(void);

/* static var inside a function: an object in the data area, the name is visible only in the block.
   In the assembler - function.name (a repeated name in the same function - function.name.2 ...). */
static Node *parse_static_local(void)
{
    int line = tokline;
    next();
    if (tok != TK_ID) die("variable name expected");
    char *name = tokstr;
    next();
    if (tok == ',') die("static var: one name per declaration");
    Type *t = NULL;
    Node *init = NULL;
    if (accept(':')) t = parse_type();
    if (accept('=')) {
        init = parse_init();
        if (!t) {
            if (init->kind == N_STR) t = init->ty;
            else if (init->kind == N_INITLIST) die("initializer list requires an array type");
            else t = decay(init->ty);
        }
    }
    if (!t) die("type or initializer required");
    if (t->kind == TY_VOID || t->kind == TY_FN) die("invalid variable type");
    type_size(t);
    expect_end();
    check_name(name);
    Sym *g = (Sym*)xcalloc(sizeof *g);
    g->name = name;
    const char *base = fmt("%s.%s", cur_func->sym->asmname, name);
    g->asmname = (char *)base;
    for (int k = 2;; k++) {
        int dup = 0;
        for (Sym *o = syms; o; o = o->next) if (!strcmp(o->asmname, g->asmname)) dup = 1;
        if (!dup) break;
        g->asmname = fmt("%s.%d", base, k);
    }
    g->kind = S_VAR;
    g->ty = t;
    g->init = init;
    g->is_static = g->local_static = 1;
    g->file = cur_file();
    g->line = line;
    if (syms_tail) syms_tail->next = g; else syms = g;
    syms_tail = g;
    Local *v = new_local(name, t);
    v->gsym = g;
    return mk(N_BLOCK);
}

/* The line where a statement starts: the -s key prints the source text by it. */
static Node *parse_stmt(void)
{
    int line = tokline;
    Node *s = parse_stmt_body();
    if (s->kind != N_BLOCK) s->line = line;
    return s;
}

static Node *parse_stmt_body(void)
{
    Node *s;
    switch (tok) {
    case '{':
        return parse_block();
    case TK_VAR:
        return parse_local_var();
    case TK_STATIC:
        next();
        if (tok != TK_VAR) die("static inside a function applies only to var");
        return parse_static_local();
    case TK_IF:
        next();
        s = mk(N_IF);
        s->a = parse_expr();
        need_scalar(s->a);
        s->b = parse_block();
        if (accept(TK_ELSE)) s->c = tok == TK_IF ? parse_stmt() : parse_block();
        return s;
    case TK_WHILE:
        next();
        s = mk(N_WHILE);
        loopdepth++;
        s->a = parse_expr();
        need_scalar(s->a);
        s->b = parse_block();
        loopdepth--;
        return s;
    case TK_LOOP:
        next();
        s = mk(N_LOOP);
        loopdepth++;
        s->b = parse_block();
        loopdepth--;
        return s;
    case TK_BREAK: case TK_CONTINUE:
        s = mk(tok == TK_BREAK ? N_BREAK : N_CONTINUE);
        if (!loopdepth) die("break/continue outside of a loop");
        next();
        expect_end();
        return s;
    case TK_RETURN: {
        s = mk(N_RETURN);
        next();
        Sym *f = cur_func->sym;
        if (tok != ';' && tok != '}') {
            do {
                Node *x = parse_expr();
                need_scalar(x);
                list_push(s, x);
            } while (accept(','));
        }
        if (s->n != f->nret) die("function %s returns %d value(s)", f->name, f->nret);
        for (int i = 0; i < s->n; i++) check_assign(f->rets[i], &s->list[i], fmt("return value %d", i + 1));
        expect_end();
        return s;
    }
    case TK_ATOMIC: {
        next();
        s = mk(N_ATOMIC);
        Func *f = cur_func;
        int id = ++f->natomic;
        f->atomic_parent = (int*)xrealloc(f->atomic_parent, sizeof(int) * (id + 1));
        f->atomic_parent[id] = cur_atomic;
        int save = cur_atomic;
        cur_atomic = id;
        s->b = parse_block();
        cur_atomic = save;
        return s;
    }
    case TK_ASM:
        s = mk(N_ASM);
        next();
        if (tok != '{') die("expected asm { ... }");
        s->str = asm_refs(read_asm_raw());
        cur_func->has_asm = 1;
        return s;
    case TK_GOTO: {
        s = mk(N_GOTO);
        next();
        if (tok != TK_ID) die("label name expected");
        LabelDef *l = get_label(tokstr);
        l->used = 1;
        s->val = l - cur_func->labels;
        s->op = cur_atomic;
        next();
        expect_end();
        return s;
    }
    case TK_INCLUDE:
        die("include must start a line: include \"file\"");
        return NULL;
    case TK_ID:
        if (peek_char() == ':') {
            s = mk(N_LABEL);
            LabelDef *l = get_label(tokstr);
            if (l->defined) die("label %s already defined at line %d", tokstr, l->line);
            l->defined = 1;
            l->line = tokline;
            l->atomic = cur_atomic;
            s->val = l - cur_func->labels;
            next();
            next();
            return s;
        }
        break;
    }
    Node *e = parse_expr();
    if (tok == ',') {
        Node *m = mk(N_MASSIGN);
        list_push(m, e);
        while (accept(',')) list_push(m, parse_bin(0));
        expect('=');
        Node *c = parse_expr();
        if (c->kind != N_CALL) die("right side of a multiple assignment must be a function call");
        int nret = c->fty->nret;
        if (m->n > nret) die("function returns fewer values than variables on the left");
        for (int i = 0; i < m->n; i++) {
            Node *l = m->list[i];
            if (l->kind == N_DISCARD) continue;
            if (!is_lval(l) || !is_val(l->ty)) die("left side of '=' must be variables");
            const char *msg = assign_err(l->ty, c->fty->rets[i], 0);
            if (msg) die("result %d: %s (%s given, %s expected)", i + 1, msg, tname(c->fty->rets[i]), tname(l->ty));
        }
        m->b = c;
        expect_end();
        return m;
    }
    if (e->kind == N_DISCARD) die("'_' is allowed only in a multiple assignment");
    switch (e->kind) {
    case N_ASSIGN: case N_OPASSIGN: case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC:
    case N_CALL: case N_BUILTIN:
        break;
    default:
        warn_at(e->line, "value of the expression is not used");
    }
    s = mk(N_EXPR);
    s->a = e;
    expect_end();
    return s;
}

/* The next non-blank character after the current token (on the same line). */
static int peek_char(void)
{
    const char *p = lp;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return *p;
}

static LabelDef *get_label(const char *name)
{
    Func *f = cur_func;
    for (int i = 0; i < f->nlabels; i++) if (!strcmp(f->labels[i].name, name)) return &f->labels[i];
    f->labels = (LabelDef*)xrealloc(f->labels, sizeof(LabelDef) * (f->nlabels + 1));
    LabelDef *l = &f->labels[f->nlabels++];
    memset(l, 0, sizeof *l);
    l->name = xstrndup(name, strlen(name));
    l->line = tokline;
    return l;
}

/* Function names in the text of an asm block count as references (for removing unused functions),
   and a CALL instruction makes the function non-leaf. */
/* The text of an asm block: function names count as references (for removing unused functions),
   a CALL instruction makes the function non-leaf, and the names of the current file's static objects and
   of static local variables are replaced with their assembler names (file.name). */
static char *asm_refs(const char *t)
{
    size_t cap = strlen(t) * 2 + 64, n = 0;
    char *r = (char*)xcalloc(cap);
#define PUT(p, len) do { size_t l_ = (len); while (n + l_ + 1 >= cap) { cap *= 2; r = (char*)xrealloc(r, cap); } \
                         memcpy(r + n, (p), l_); n += l_; r[n] = 0; } while (0)
    const char *file = cur_file();
    while (*t) {
        if (*t == ';') {
            const char *b = t;
            while (*t && *t != '\n') t++;
            PUT(b, t - b);
        } else if (*t == '"' || *t == '\'') {
            const char *b = t;
            char q = *t++;
            while (*t && *t != q && *t != '\n') { if (*t == '\\' && t[1]) t++; t++; }
            if (*t == q) t++;
            PUT(b, t - b);
        } else if (*t == '$' || isdigit((unsigned char)*t)) {
            const char *b = t++;
            while (isalnum((unsigned char)*t) || *t == '_') t++;
            PUT(b, t - b);
        } else if (isalpha((unsigned char)*t) || *t == '_') {
            const char *b = t;
            while (isalnum((unsigned char)*t) || *t == '_') t++;
            int qualified = *t == '.' || (n > 0 && (r[n - 1] == '.' || r[n - 1] == '@'));
            char *id = xstrndup(b, t - b);
            /* CALL in an asm block: the function is non-leaf (LR is saved, variables in R4+) */
            if (cur_func && t - b == 4 && toupper((unsigned char)b[0]) == 'C' && toupper((unsigned char)b[1]) == 'A' &&
                toupper((unsigned char)b[2]) == 'L' && toupper((unsigned char)b[3]) == 'L')
                cur_func->has_call = 1;
            const char *repl = NULL;
            if (!qualified) {
                Local *lv = cur_func ? find_local(id) : NULL;
                if (lv && lv->gsym) repl = lv->gsym->asmname;
                else if (!lv) {
                    Sym *s = find_sym_in(id, file);
                    if (!s) {
                        /* name in assembler form (__neg) */
                        for (Sym *o = syms; o; o = o->next)
                            if (!o->is_static && !strcmp(o->asmname, id)) { s = o; break; }
                    }
                    if (s && s->kind == S_FN) add_ref(s);
                    if (s && s->is_static && (s->kind == S_FN || s->kind == S_VAR)) repl = s->asmname;
                }
            }
            if (repl) PUT(repl, strlen(repl)); else PUT(b, t - b);
            free(id);
        } else {
            PUT(t, 1);
            t++;
        }
    }
#undef PUT
    return r;
}

/* A jump is allowed if the label's atomic block encloses the goto place (or the label is outside atomic). */
static int atomic_depth_of(Func *f, int a)
{
    int d = 0;
    while (a) { d++; a = f->atomic_parent[a]; }
    return d;
}

static void check_labels(Func *f, Node *s)
{
    if (!s) return;
    if (s->kind == N_GOTO) {
        LabelDef *l = &f->labels[s->val];
        errline = s->line;
        if (!l->defined) die("label %s is not defined", l->name);
        int a = s->op;
        while (a && a != l->atomic) a = f->atomic_parent[a];
        if (a != l->atomic) die("goto %s: jumping into an atomic block is not allowed", l->name);
        s->slen = atomic_depth_of(f, s->op) - atomic_depth_of(f, l->atomic);   /* how many POP FLAGS */
        return;
    }
    check_labels(f, s->a);
    check_labels(f, s->b);
    check_labels(f, s->c);
    if (s->kind == N_BLOCK) for (int i = 0; i < s->n; i++) check_labels(f, s->list[i]);
}

/* After an error: skip the rest of the statement standing at bracket depth d
   (up to ';' at this depth or up to the '}' that closes the block). */
static void skip_stmt(int d)
{
    while (tok != TK_EOF) {
        if (tok == ';' && tokdepth <= d) { next(); return; }
        if (tok == '}' && tokdepth < d) return;
        if (tok == TK_ASM) { next(); if (tok == '{') read_asm_raw(); continue; }
        next();
    }
}

static Node *parse_block(void)
{
    expect('{');
    scope_push();
    int d = lex_depth, lvl = nerr_stack;
    int sv_nscope, sv_nmarks, sv_loop, sv_atomic;
    Node *b = mk(N_BLOCK);
    jmp_buf jb;
    while (tok != '}') {
        if (tok == TK_EOF) fatal("unexpected end of file: missing '}'");
        if (tok == ';') { next(); continue; }
        sv_nscope = nscope; sv_nmarks = nmarks; sv_loop = loopdepth; sv_atomic = cur_atomic;
        if (setjmp(jb)) {
            /* an error in a statement: restore the parse state and go to the next one */
            nerr_stack = lvl;
            /* variables declared by the erroneous statement at block level stay visible,
               to avoid a cascade of "unknown name" messages */
            if (nmarks > sv_nmarks) nscope = scope_marks[sv_nmarks];
            if (nscope < sv_nscope) nscope = sv_nscope;
            nmarks = sv_nmarks; loopdepth = sv_loop; cur_atomic = sv_atomic;
            TRY_PUSH(jb);           /* a lexical error while skipping - back here again */
            skip_stmt(d);
            TRY_POP();
            continue;
        }
        TRY_PUSH(jb);
        Node *st = parse_stmt();
        TRY_POP();
        list_push(b, st);
    }
    last_rbrace = tokline;
    next();
    scope_pop();
    return b;
}

/* ------------------------- top-level declarations -------------------- */

static void parse_const(void)
{
    next();
    if (tok != TK_ID) die("constant name expected");
    char *name = tokstr;
    next();
    if (parse_pass == 2) { skip_to_end(); return; }
    expect('=');
    i64 v = const_expr();
    Sym *s = add_sym(name, S_CONST);
    s->val = v;
    expect_end();
}

static StructDef **struct_order;
static int nstruct_order;

static void parse_struct(void)
{
    int sline = tokline;
    next();
    if (tok != TK_ID) die("struct name expected");
    char *name = tokstr;
    next();
    if (parse_pass == 2) { skip_braces(); return; }
    StructDef *sd = get_struct(name);
    if (sd->defined) die("redefinition of struct %s", name);
    expect('{');
    Field **tail = &sd->fields;
    int off = 0, al = 1;
    while (tok != '}') {
        if (tok == ';') { next(); continue; }
        char *fn[16];
        int nf = 0;
        do {
            if (tok != TK_ID) die("field name expected");
            if (nf == 16) die("too many names");
            fn[nf++] = tokstr;
            next();
        } while (accept(','));
        expect(':');
        Type *t = parse_type();
        int sz = type_size(t), a = type_align(t);
        if (a > al) al = a;
        for (int i = 0; i < nf; i++) {
            for (Field *g = sd->fields; g; g = g->next)
                if (!strcmp(g->name, fn[i])) die("duplicate field %s in struct %s", fn[i], name);
            for (int j = 0; j < i; j++)
                if (!strcmp(fn[j], fn[i])) die("duplicate field %s in struct %s", fn[i], name);
            if (a == 2 && (off & 1)) off++;
            Field *f = (Field*)xcalloc(sizeof *f);
            f->name = fn[i];
            f->ty = t;
            f->off = off;
            off += sz;
            *tail = f;
            tail = &f->next;
        }
        expect_end();
    }
    next();
    sd->size = (off + 1) & ~1;
    sd->align = al;
    sd->defined = 1;
    sd->line = sline;
    struct_order = (StructDef**)xrealloc(struct_order, sizeof(StructDef *) * (nstruct_order + 1));
    struct_order[nstruct_order++] = sd;
}

static Node *parse_init(void)
{
    if (tok == '{') {
        next();
        Node *l = mk(N_INITLIST);
        while (tok != '}') {
            if (tok == ';') { next(); continue; }
            Node *it;
            if (tok == '.') {
                it = mk(N_DESIG);
                next();
                if (tok != TK_ID) die("field name expected after '.'");
                it->str = tokstr;
                next();
                expect('=');
                it->a = parse_init();
            } else it = parse_init();
            list_push(l, it);
            while (tok == ';') next();
            if (!accept(',')) { while (tok == ';') next(); break; }
        }
        expect('}');
        return l;
    }
    return parse_bin(0);
}

static void parse_global_var(int is_static)
{
    next();
    char *names[32];
    int nn = 0;
    do {
        if (tok != TK_ID) die("variable name expected");
        if (nn == 32) die("too many names");
        names[nn++] = tokstr;
        next();
    } while (accept(','));
    if (accept(TK_AT)) {
        if (nn != 1) die("var ... at: exactly one name allowed");
        if (parse_pass == 2) { skip_to_end(); return; }
        i64 addr = const_expr();
        expect(':');
        Type *t = parse_type();
        if (!is_val(t) && t->kind != TY_ARRAY && t->kind != TY_STRUCT) die("invalid type");
        if (addr < 0 || addr > 0xFFFF) die("address does not fit in 16 bits");
        Sym *s = add_sym_ex(names[0], S_MMIO, is_static);
        s->val = addr;
        s->ty = t;
        expect_end();
        return;
    }
    Type *t = NULL;
    if (accept(':')) t = parse_type();
    if (tok == '=') {
        if (nn != 1) die("initializer allowed for a single variable only");
        next();
        if (t) {
            /* the type is known: the initializer is parsed in the second pass, when all names are known */
            if (parse_pass == 1) { add_sym_ex(names[0], S_VAR, is_static)->ty = t; skip_to_end(); return; }
            find_sym(names[0])->init = parse_init();
            expect_end();
            return;
        }
        if (parse_pass == 2) { skip_to_end(); return; }
        Node *init = parse_init();
        if (init->kind == N_STR) t = init->ty;
        else if (init->kind == N_INITLIST) die("initializer list requires an array type");
        else t = const_type(init);
        Sym *s = add_sym_ex(names[0], S_VAR, is_static);
        s->ty = t;
        s->init = init;
        expect_end();
        return;
    }
    if (!t) die("type or initializer required");
    if (parse_pass == 1) {
        if (t->kind == TY_VOID || t->kind == TY_FN) die("invalid variable type");
        type_size(t);
        for (int i = 0; i < nn; i++) add_sym_ex(names[i], S_VAR, is_static)->ty = t;
    }
    expect_end();
}

/* ------------- warnings about function variables (sect. 12.2) ------------- */
/* Reading before assignment: a conservative analysis by the structure of the body. The state is two sets:
   which variables certainly got a value (st[0..n-1]; st[n] = 1 - the place is unreachable) and which
   got it on at least one path (st[n+1..2n]). Reading a variable not assigned on any path
   always warns; reading a variable assigned not on all paths - only with the key
   -Wmaybe-uninit (the conditions may be consistent so that the read is safe, but the analysis does not see it).
   After a goto label, an asm block and in unreachable code variables count as assigned. */
static int opt_copy_params;       /* -fcopy-params: always copy parameters to R4+ (for comparison and debugging) */
static int opt_maybe_uninit;      /* -Wmaybe-uninit: also warn about a possible read before assignment */
static Func *ua_f;
static unsigned char *ua_warned;
typedef struct { unsigned char *brk; } UaLoop;
static UaLoop ua_loops[64];
static int ua_nloops;

static int ua_tracked(Local *v)
{
    return v->pidx < 0 && !v->gsym && !v->addr_taken && is_scalar(v->ty);
}

#define UA_SIZE (2 * (ua_f->nl + 1))
#define UA_MAY(st, i) ((st)[ua_f->nl + 1 + (i)])

static unsigned char *ua_new(void) { return (unsigned char*)xcalloc(UA_SIZE); }
static void ua_copy(unsigned char *d, unsigned char *s) { memcpy(d, s, UA_SIZE); }
static void ua_all(unsigned char *s) { memset(s, 1, UA_SIZE); }

/* Merging paths: certainly - intersection, on at least one - union. */
static void ua_meet(unsigned char *d, unsigned char *s)
{
    int n = ua_f->nl;
    if (s[n]) return;
    if (d[n]) { ua_copy(d, s); return; }
    for (int i = 0; i < n; i++) { d[i] &= s[i]; UA_MAY(d, i) |= UA_MAY(s, i); }
}

/* Which variables are assigned anywhere inside x (set[i] = 1). */
static void ua_assigned(Node *x, unsigned char *set)
{
    if (!x) return;
    if (x->kind == N_ASSIGN && x->a->kind == N_LOCAL) set[x->a->lv->idx] = 1;
    if (x->kind == N_MASSIGN)
        for (int i = 0; i < x->n; i++) if (x->list[i]->kind == N_LOCAL) set[x->list[i]->lv->idx] = 1;
    ua_assigned(x->a, set);
    ua_assigned(x->b, set);
    ua_assigned(x->c, set);
    for (int i = 0; i < x->n; i++) ua_assigned(x->list[i], set);
}

static void ua_expr(Node *e, unsigned char *st);

static void ua_lval(Node *l, unsigned char *st)
{
    if (l->kind == N_LOCAL) return;
    if (l->kind == N_DISCARD) return;
    ua_expr(l, st);
}

static void ua_set(Node *l, unsigned char *st)
{
    if (l->kind == N_LOCAL && ua_tracked(l->lv)) { st[l->lv->idx] = 1; UA_MAY(st, l->lv->idx) = 1; }
}

static void ua_expr(Node *e, unsigned char *st)
{
    if (!e) return;
    switch (e->kind) {
    case N_LOCAL: {
        Local *v = e->lv;
        if (ua_tracked(v) && !st[v->idx] && !ua_warned[v->idx]) {
            if (!UA_MAY(st, v->idx)) {
                warn_at(e->line, "variable %s is used before it is assigned", v->name);
                ua_warned[v->idx] = 1;
            } else if (opt_maybe_uninit) {
                warn_at(e->line, "variable %s may be used before it is assigned", v->name);
                ua_warned[v->idx] = 1;
            }
        }
        return;
    }
    case N_ASSIGN:
        ua_expr(e->b, st);
        ua_lval(e->a, st);
        ua_set(e->a, st);
        return;
    case N_LAND: case N_LOR: {
        ua_expr(e->a, st);
        unsigned char *t = ua_new();
        ua_copy(t, st);
        ua_expr(e->b, t);
        for (int i = 0; i < ua_f->nl; i++) UA_MAY(st, i) |= UA_MAY(t, i);   /* the right-hand side may have been executed */
        free(t);
        return;
    }
    case N_OPASSIGN:
        ua_expr(e->a, st);
        ua_expr(e->b, st);
        return;
    }
    ua_expr(e->a, st);
    ua_expr(e->b, st);
    if (e->kind != N_OPASSIGN) ua_expr(e->c, st);
    for (int i = 0; i < e->n; i++) ua_expr(e->list[i], st);
}

static void ua_stmt(Node *s, unsigned char *st)
{
    int n = ua_f->nl;
    if (!s) return;
    switch (s->kind) {
    case N_BLOCK:
        for (int i = 0; i < s->n; i++) ua_stmt(s->list[i], st);
        return;
    case N_EXPR:
        ua_expr(s->a, st);
        return;
    case N_IF: {
        ua_expr(s->a, st);
        unsigned char *t = ua_new();
        ua_copy(t, st);
        ua_stmt(s->b, t);
        if (s->c) ua_stmt(s->c, st);
        ua_meet(st, t);
        free(t);
        return;
    }
    case N_WHILE: case N_LOOP: {
        int inf = s->kind == N_LOOP || (s->a->kind == N_NUM && s->a->val);
        {
            /* an assignment in the body reaches the start of the next pass: on at least one path it has already happened */
            unsigned char *as = ua_new();
            ua_assigned(s, as);
            for (int i = 0; i < n; i++) if (as[i]) UA_MAY(st, i) = 1;
            free(as);
        }
        if (s->kind == N_WHILE) ua_expr(s->a, st);
        UaLoop *l = &ua_loops[ua_nloops < 63 ? ua_nloops++ : 63];
        l->brk = ua_new();
        if (inf) ua_all(l->brk); else ua_copy(l->brk, st);
        unsigned char *t = ua_new();
        ua_copy(t, st);
        ua_stmt(s->b, t);
        ua_copy(st, l->brk);
        free(t);
        free(l->brk);
        ua_nloops--;
        return;
    }
    case N_BREAK:
        if (ua_nloops > 0) ua_meet(ua_loops[ua_nloops - 1].brk, st);
        ua_all(st);
        return;
    case N_CONTINUE: case N_GOTO:
        ua_all(st);
        return;
    case N_RETURN:
        for (int i = 0; i < s->n; i++) ua_expr(s->list[i], st);
        ua_all(st);
        return;
    case N_LABEL: case N_ASM:
        ua_all(st);
        st[n] = 0;
        return;
    case N_MASSIGN:
        ua_expr(s->b, st);
        for (int i = 0; i < s->n; i++) { ua_lval(s->list[i], st); ua_set(s->list[i], st); }
        return;
    case N_ATOMIC:
        ua_stmt(s->b, st);
        return;
    default:
        ua_expr(s->a, st);
        ua_expr(s->b, st);
        return;
    }
}

static void check_func_warnings(Func *f)
{
    if (!f->has_asm)
        for (int i = 0; i < f->nl; i++) {
            Local *v = f->locals[i];
            if (v->mentioned || v->name[0] == '_') continue;
            /* the signature of main is set by the startup convention: taking only argv and skipping argc is not possible */
            if (v->pidx >= 0 && opt_target == T_STD && !strcmp(f->sym->name, "main")) continue;
            warn_at(v->line, "%s %s is not used", v->pidx >= 0 ? "parameter" : "variable", v->name);
        }
    for (int i = 0; i < f->nlabels; i++)
        if (f->labels[i].defined && !f->labels[i].used) warn_at(f->labels[i].line, "label %s is not used", f->labels[i].name);
    ua_f = f;
    ua_warned = ua_new();
    unsigned char *st = ua_new();
    ua_nloops = 0;
    ua_stmt(f->body, st);
    free(st);
    free(ua_warned);
}

static void parse_fn(int is_export, int is_static, int is_extern)
{
    int is_int = 0;
    if (tok == TK_INTERRUPT) { is_int = 1; next(); }
    expect(TK_FN);
    if (tok != TK_ID) die("function name expected");
    char *name = tokstr;
    int fn_line = tokline;
    next();
    char *pn[32], *pend[32];
    Type *pt[32];
    int np = 0, npend = 0;
    expect('(');
    while (tok != ')') {
        if (tok != TK_ID) die("parameter name expected");
        if (np + npend >= 32) die("too many parameters");
        pend[npend++] = tokstr;
        next();
        if (accept(',')) continue;
        expect(':');
        Type *t = parse_type();
        if (!is_val(t)) die("parameter must be a scalar");
        for (int i = 0; i < npend; i++) { pn[np] = pend[i]; pt[np] = t; np++; }
        npend = 0;
        if (!accept(',')) break;
    }
    if (npend) die("parameter type missing");
    expect(')');
    Type *rets[4];
    int nret = 0;
    if (accept(':')) {
        int paren = accept('(');            /* Results = Type { "," Type } | "(" Type { "," Type } ")" (sect. 4.5) */
        do {
            if (nret == 4) die("a function returns at most 4 values");
            rets[nret++] = parse_result_type();
        } while (accept(','));
        if (paren) expect(')');
        check_rets(rets, nret);
    }
    if (is_int && (np || nret)) die("interrupt handler takes no parameters and returns no values");
    int has_body = tok == '{';
    Sym *s = find_sym(name);
    if (parse_pass == 1) {
        Type *t = mk_type(TY_FN, NULL);
        t->np = np;
        t->params = (Type**)xcalloc(sizeof(Type *) * (np + 1));
        for (int i = 0; i < np; i++) t->params[i] = pt[i];
        t->nret = nret;
        for (int i = 0; i < nret; i++) t->rets[i] = rets[i];
        t->ret = nret ? rets[0] : &t_void;
        t->is_int = is_int;
        if (is_extern && has_body) die("extern function %s cannot have a body: it is defined outside B16", name);
        if (s) {
            if (s->kind != S_FN) die("redefinition of %s", name);
            if (s->has_body && has_body) die("redefinition of function %s", name);
            if ((s->is_extern && has_body) || (is_extern && s->has_body))
                die("function %s is declared extern but defined in B16", name);
            if (!same_type(s->ty, t))
                die("declaration of %s does not match its prototype: %s and %s", name, tname(t), tname(s->ty));
            if (s->is_static != is_static) die("declaration of %s: static does not match its prototype", name);
        } else {
            s = add_sym_ex(name, S_FN, is_static);
            s->ty = t;
            s->nret = nret;
            for (int i = 0; i < nret; i++) s->rets[i] = rets[i];
            s->is_int = is_int;
            s->is_static = is_static;
        }
        if (is_static && !strcmp(name, "main")) die("main cannot be static");
        if (is_export) {
            if (is_static) die("export and static cannot be combined");
            s->is_export = 1;
        }
        if (is_extern) s->is_extern = 1;
        if (s->is_extern && s->is_export) die("function %s: extern and export cannot be combined", name);
        if (has_body) { s->has_body = 1; skip_braces(); }
        else expect_end();
        return;
    }
    if (!has_body) { expect_end(); return; }
    Func *f = (Func*)xcalloc(sizeof *f);
    f->sym = s;
    f->line = fn_line;
    cur_func = f;
    scope_push();
    {
        int preg[32], psoff[32];
        layout_args(pt, np, preg, psoff, NULL);
        for (int i = 0; i < np; i++) {
            Local *pv = new_local(pn[i], pt[i]);
            pv->pidx = i;
            pv->preg = preg[i];
            pv->psoff = psoff[i];
        }
    }
    cur_atomic = 0;
    f->body = parse_block();
    f->end_line = last_rbrace;
    scope_pop();
    check_labels(f, f->body);
    check_func_warnings(f);
    cur_func = NULL;
    funcs = (Func**)xrealloc(funcs, sizeof(Func *) * (nfuncs + 1));
    funcs[nfuncs++] = f;
}

/* A string in the machine encoding -> UTF-8 (for compiler messages). */
static char *to_utf8(const char *str, int n)
{
    char *r = (char*)xcalloc(3 * n + 1), *q = r;
    for (int i = 0; i < n; i++) {
        unsigned long cp = charset[(unsigned char)str[i]];
        if ((unsigned char)str[i] < 0x20) cp = '?';
        if (cp < 0x80) *q++ = (char)cp;
        else if (cp < 0x800) { *q++ = (char)(0xC0 | (cp >> 6)); *q++ = (char)(0x80 | (cp & 0x3F)); }
        else { *q++ = (char)(0xE0 | (cp >> 12)); *q++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *q++ = (char)(0x80 | (cp & 0x3F)); }
    }
    return r;
}

/* assert ConstantExpression [ "," string ] - a check at translation time (sect. 5.7). */
static void parse_assert(void)
{
    int line = tokline;
    next();
    if (parse_pass == 2) { skip_to_end(); return; }
    Node *e = parse_bin(0);
    if (e->kind != N_NUM) die("assert: constant expression expected");
    char *msg = NULL;
    if (accept(',')) {
        if (tok != TK_STR) die("assert: message string expected");
        msg = to_utf8(tokstr, tokslen);
        next();
    }
    expect_end();
    if (!e->val) {
        /* the declaration has already been parsed: the error is reported without skipping text */
        errline = line;
        int save = nerr_stack;
        jmp_buf jb;
        if (!setjmp(jb)) {
            TRY_PUSH(jb);
            if (msg) die("assertion failed: %s", msg);
            die("assertion failed");
        }
        nerr_stack = save;
    }
}

/* The value of an image parameter for messages and for a comment in the output. */
static char *param_text(int k, i64 v)
{
    if (k == P_TARGET) return fmt("%s", v == T_KERNEL ? "kernel" : "std");
    if (k == P_MIN_FREE) return fmt("%lld", v);
    return fmt("$%04llX", v);
}

/* option NAME = value - an image parameter (sect. 3.6). option is a contextual word: at top
   level a declaration cannot begin with an identifier, so the word is not reserved.
   Parsed in pass 1; constant expressions see the constants declared above. */
static void parse_option(void)
{
    int line = tokline;
    next();
    if (parse_pass == 2) { skip_to_end(); return; }
    const char *file;
    int fl;
    where(line, &file, &fl);
    if (file != pp_root_file) die("option is allowed only in the main source file");
    if (tok != TK_ID) die("option name expected (target, code, data, min_free, max_top)");
    int k = 0;
    while (k < NPARAMS && strcmp(params[k].name, tokstr)) k++;
    if (k == NPARAMS) die("unknown option %s (expected target, code, data, min_free, max_top)", tokstr);
    ImageParam *p = &params[k];
    if (p->src_set) {
        const char *f2;
        int l2;
        where(p->src_line, &f2, &l2);
        die("option %s is already set at line %d", p->name, l2);
    }
    next();
    expect('=');
    i64 v;
    if (k == P_TARGET) {
        if (tok != TK_ID || (strcmp(tokstr, "std") && strcmp(tokstr, "kernel"))) die("option target must be std or kernel");
        v = strcmp(tokstr, "kernel") ? T_STD : T_KERNEL;
        next();
    } else {
        v = const_expr();
        if (v < p->lo || v > p->hi)
            die("option %s: value %lld is out of range %s..%s", p->name, v, param_text(k, p->lo), param_text(k, p->hi));
        if (p->even && (v & 1)) die("option %s must be even", p->name);
    }
    expect_end();
    p->src_set = 1;
    p->src_line = line;
    p->src_val = v;
}

/* An error at line line of the combined text (0 - no line) without a jump to the recovery point. */
static void error_at(int line, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    diag("error", line, f, ap);
    va_end(ap);
    nerrors++;
    too_many();
}

/* The effective image parameters: a key, otherwise option, otherwise the default. A key that differs from
   option is a warning at the option's line. A parameter that does not belong to the target is an error. */
static int param_set(int k) { return params[k].cli_set || params[k].src_set; }
static i64 param_val(int k)
{
    ImageParam *p = &params[k];
    return p->cli_set ? p->cli_val : p->src_set ? p->src_val : p->def;
}
static int param_line(int k) { return params[k].cli_set ? 0 : params[k].src_line; }
static char *param_label(int k)
{
    return params[k].cli_set ? fmt("%s", params[k].key) : fmt("option %s", params[k].name);
}

static void resolve_params(void)
{
    for (int k = 0; k < NPARAMS; k++) {
        ImageParam *p = &params[k];
        if (p->cli_set && p->src_set && p->cli_val != p->src_val)
            warn_at(p->src_line, "%s %s overrides option %s = %s", p->key, param_text(k, p->cli_val),
                    p->name, param_text(k, p->src_val));
    }
    opt_target = (int)param_val(P_TARGET);
    if (opt_target == T_STD && param_set(P_CODE))
        error_at(param_line(P_CODE), "%s applies only to target kernel", param_label(P_CODE));
    if (opt_target == T_KERNEL)
        for (int k = P_MIN_FREE; k <= P_MAX_TOP; k++)
            if (param_set(k)) error_at(param_line(k), "%s applies only to target std", param_label(k));
    code_org = param_val(P_CODE);
    data_org = param_val(P_DATA);
    min_free = param_val(P_MIN_FREE);
    max_top = param_val(P_MAX_TOP);
    if (opt_target == T_STD && max_top > 0xE000)
        warn_at(param_line(P_MAX_TOP), "%s %s is above $E000; the monitor limits it to $E000",
                param_label(P_MAX_TOP), param_text(P_MAX_TOP, max_top));
}

static void parse_toplevel(void)
{
    jmp_buf jb;
    int lvl = nerr_stack;
    while (tok != TK_EOF) {
        if (setjmp(jb)) {
            nerr_stack = lvl;
            cur_func = NULL; nscope = 0; nmarks = 0; loopdepth = 0; cur_atomic = 0;
            TRY_PUSH(jb);
            skip_stmt(0);
            TRY_POP();
            continue;
        }
        TRY_PUSH(jb);
        switch (tok) {
        case ';': next(); break;
        case TK_CONST: parse_const(); break;
        case TK_VAR: parse_global_var(0); break;
        case TK_STRUCT: parse_struct(); break;
        case TK_FN: case TK_INTERRUPT: parse_fn(0, 0, 0); break;
        case TK_EXTERN:
            next();
            if (tok == TK_STATIC || tok == TK_EXPORT) die("extern cannot be combined with static or export");
            if (tok != TK_FN && tok != TK_INTERRUPT) die("extern applies only to functions");
            parse_fn(0, 0, 1);
            break;
        case TK_EXPORT:
            next();
            if (tok == TK_STATIC) die("export and static cannot be combined");
            if (tok == TK_EXTERN) die("extern and export cannot be combined");
            if (tok != TK_FN && tok != TK_INTERRUPT) die("export applies only to functions");
            parse_fn(1, 0, 0);
            break;
        case TK_STATIC:
            next();
            if (tok == TK_EXPORT) die("export and static cannot be combined");
            if (tok == TK_EXTERN) die("extern cannot be combined with static or export");
            if (tok == TK_FN || tok == TK_INTERRUPT) parse_fn(0, 1, 0);
            else if (tok == TK_VAR) parse_global_var(1);
            else die("static applies only to fn and var");
            break;
        case TK_ASSERT: parse_assert(); break;
        case TK_ID:
            if (!strcmp(tokstr, "option")) { parse_option(); break; }
            die("declaration expected (fn, extern fn, var, const, struct, assert, option)");
            break;
        default: die("declaration expected (fn, extern fn, var, const, struct, assert, option)");
        }
        TRY_POP();
    }
}

/* ============================ Code generation ============================== */

enum { R_LR = 12, R_SP = 13, R_IP = 14, R_FLAGS = 15 };
enum { M_REG, M_IMM, M_IND, M_INC, M_DEC, M_ABS, M_IDX, M_LEA };
enum { I_INS, I_LABEL, I_JMP };
enum { CC_JMP, CC_Z, CC_NZ, CC_C, CC_NC, CC_A, CC_BE, CC_S, CC_NS, CC_O, CC_NO, CC_L, CC_GE, CC_G, CC_LE };

typedef struct {
    int mode, reg;
    const char *sym;
    i64 off;
    int tmp;            /* a temporary register that must be freed */
} Opnd;

typedef struct {
    int kind;
    char *text;
    int size, label, cc, dead, term;
    int sline;          /* source line (of the combined text) */
} Ins;

static Ins *code;
static int ncode, capcode;
static int nlabels;
static int busy, used_mask, sp_adj, flags_reg = -1;
static Local *reg_var[16];
static Func *cf;
static int cur_frame, cur_nsaved;
static int ret_label, atomic_depth;

typedef struct { int brk, cont, atomic, cont_used; } LoopCtx;
static LoopCtx loops[64];
static int nloops;

static void gen(Node *e, int d);
static void gen_cond(Node *e, int L, int jt);
static void gen_void(Node *e);
static void gen_stmt(Node *s);
static int try_operand(Node *e, Opnd *o, int word_only);
static int get_lval(Node *e, Opnd *o, int dry);
static int lval_ptr(Node *p, int acc, Opnd *o, int dry);
static void gen_call(Node *e, int d);
static void gen_w(Node *e, int lo, int hi);
static void par_move(int *src, int *dst, int n);
static void gen_bin(Node *e, int d);
static void addr_into(Node *x, int d);
static void cmp_operands(Node *a, Node *b, int *ra, int *ta, int *tb);
static void add_index(int d, Node *i, int es);

static const char *rn(int r)
{
    static const char *n[16] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9",
                                "R10", "R11", "LR", "SP", "IP", "FLAGS"};
    return n[r];
}

static int newlabel(void) { return nlabels++; }
static int src_line;        /* line of the statement for which code is being generated */

static Ins *add_ins(int kind)
{
    if (ncode == capcode) {
        capcode = capcode ? capcode * 2 : 256;
        code = (Ins*)xrealloc(code, sizeof(Ins) * capcode);
    }
    Ins *i = &code[ncode++];
    memset(i, 0, sizeof *i);
    i->kind = kind;
    i->label = -1;
    i->sline = src_line;
    return i;
}

/* --------------------------- registers ---------------------------------- */

/* Rollback when registers run out. A piece of code where several intermediate values
   are live at once is first generated in the ordinary way; if the registers
   run out, the generator state is restored and the piece is generated again
   with an intermediate value on the stack (PUSH/POP). */
typedef struct {
    jmp_buf jb;
    int ncode, busy, used_mask, sp_adj, flags_reg, nlabels, depth;
} Retry;

static Retry *retry_stack[256];
static int nretry;

static void retry_begin(Retry *r)
{
    r->ncode = ncode;
    r->busy = busy;
    r->used_mask = used_mask;
    r->sp_adj = sp_adj;
    r->flags_reg = flags_reg;
    r->nlabels = nlabels;
    r->depth = nretry;
    if (nretry == 256) die("expression too complex: nested too deeply");
    retry_stack[nretry++] = r;
}

static void retry_end(Retry *r) { nretry = r->depth; }

/* After longjmp: return the generator to its state at the moment of retry_begin. */
static void retry_restore(Retry *r)
{
    nretry = r->depth;
    ncode = r->ncode;
    busy = r->busy;
    used_mask = r->used_mask;
    sp_adj = r->sp_adj;
    flags_reg = r->flags_reg;
    nlabels = r->nlabels;
}

static int alloc_temp(void)
{
    for (int r = 0; r < 12; r++)
        if (!(busy >> r & 1) && !reg_var[r]) {
            busy |= 1 << r;
            used_mask |= 1 << r;
            return r;
        }
    if (nretry > 0) longjmp(retry_stack[nretry - 1]->jb, 1);
    die("expression too complex: out of registers");
    return -1;
}

static int alloc_hi(void)
{
    for (int r = 4; r < 12; r++)
        if (!(busy >> r & 1) && !reg_var[r]) {
            busy |= 1 << r;
            used_mask |= 1 << r;
            return r;
        }
    die("out of registers for function address");
    return -1;
}

static void free_temp(int r) { busy &= ~(1 << r); }

static void free_opnd(Opnd *o)
{
    if (o->tmp >= 0) free_temp(o->tmp);
    o->tmp = -1;
}

static Opnd reg_opnd(int r)
{
    Opnd o = {M_REG, r, NULL, 0, -1};
    return o;
}

static Opnd imm_opnd(i64 v)
{
    Opnd o = {M_IMM, 0, NULL, v, -1};
    return o;
}

/* ------------------------- operand text ----------------------------- */

static char *numstr(i64 v)
{
    v &= 0xFFFF;
    if (v < 256) return fmt("%lld", v);
    return fmt("$%04llX", v);
}

static char *symoff(const char *s, i64 off)
{
    if (!off) return fmt("%s", s);
    return fmt("%s%+lld", s, off);
}

static i64 eff_off(Opnd *o)
{
    if (o->reg == R_SP && (o->mode == M_IDX || o->mode == M_LEA)) return o->off + sp_adj;
    return o->off;
}

static char *opnd_str(Opnd *o)
{
    i64 off = eff_off(o);
    switch (o->mode) {
    case M_REG: return fmt("%s", rn(o->reg));
    case M_IMM: return o->sym ? fmt("#%s", symoff(o->sym, off)) : fmt("#%s", numstr(off));
    case M_IND: return fmt("[%s]", rn(o->reg));
    case M_INC: return fmt("[%s++]", rn(o->reg));
    case M_DEC: return fmt("[--%s]", rn(o->reg));
    case M_ABS: return o->sym ? fmt("[%s]", symoff(o->sym, off)) : fmt("[%s]", numstr(off));
    case M_IDX: case M_LEA: {
        const char *l = o->mode == M_IDX ? "[" : "", *r = o->mode == M_IDX ? "]" : "";
        if (o->sym) return fmt("%s%s+%s%s", l, rn(o->reg), symoff(o->sym, off), r);
        if (!off) return fmt("%s%s%s", l, rn(o->reg), r);
        if (off < 0) return fmt("%s%s-%lld%s", l, rn(o->reg), -off, r);
        return fmt("%s%s+%lld%s", l, rn(o->reg), off, r);
    }
    }
    return NULL;
}

/* The length of an instruction in words by the rule of sect. 3 of the specification. */
static int opnd_words(Opnd *o)
{
    i64 off = eff_off(o);
    switch (o->mode) {
    case M_IMM: return (!o->sym && (off & 0xFFFF) <= 15) ? 1 : 2;
    case M_ABS: return 2;
    case M_IDX: case M_LEA: return (!o->sym && !off) ? 1 : 2;
    }
    return 1;
}

static int opnd_uses(Opnd *o, int r)
{
    switch (o->mode) {
    case M_REG: case M_IND: case M_INC: case M_DEC: case M_IDX: case M_LEA:
        return o->reg == r;
    }
    return 0;
}

/* ----------------------------- emitters -------------------------------- */
/* flags_reg - the register by whose value ZF and SF are currently set (or -1). */

static int is_in(const char *mn, const char *const *set)
{
    for (int i = 0; set[i]; i++) if (!strcmp(mn, set[i])) return 1;
    return 0;
}

static const char *const alu_set[] = {"ADD", "ADC", "SUB", "SBC", "AND", "OR", "XOR",
                                      "MUL", "MULHU", "DIVU", "REMU", NULL};
static const char *const shift_set[] = {"SHL", "SHR", "SAR", "ROR", NULL};

static void e_text(const char *s, int size)
{
    Ins *i = add_ins(I_INS);
    i->text = fmt("%s", s);
    i->size = size;
}

static void e_op(const char *mn, int r, Opnd *o)
{
    e_text(fmt("%-5s %s, %s", mn, rn(r), opnd_str(o)), opnd_words(o));
    if ((o->mode == M_INC || o->mode == M_DEC) && o->reg == flags_reg) flags_reg = -1;
    if (!strcmp(mn, "CMP"))
        flags_reg = (o->mode == M_IMM && !o->sym && !o->off) ? r : -1;
    else if (!strcmp(mn, "TEST"))
        flags_reg = (o->mode == M_REG && o->reg == r) ? r : -1;
    else if (r == R_FLAGS || r == R_SP)
        flags_reg = -1;
    else if (is_in(mn, alu_set))
        flags_reg = r;
    else if (is_in(mn, shift_set))
        flags_reg = (o->mode == M_IMM && !o->sym && (o->off & 15)) ? r : (flags_reg == r ? r : -1);
    else if (r == flags_reg)
        flags_reg = -1;
}

static void e_rr(const char *mn, int r1, int r2)
{
    Opnd o = reg_opnd(r2);
    e_op(mn, r1, &o);
}

static void e_ri(const char *mn, int r, i64 v)
{
    Opnd o = imm_opnd(v);
    e_op(mn, r, &o);
}

static void e_st(int narrow, Opnd *o, int r)
{
    e_text(fmt("%-5s %s, %s", narrow ? "STB" : "ST", opnd_str(o), rn(r)), opnd_words(o));
    if ((o->mode == M_INC || o->mode == M_DEC) && o->reg == flags_reg) flags_reg = -1;
}

static void e_misc(const char *mn, int r)
{
    e_text(fmt("%-5s %s", mn, rn(r)), 1);
    if (!strcmp(mn, "NEG")) flags_reg = r;
    else if (r == flags_reg) flags_reg = -1;
}

static void e_push(int r)
{
    e_text(fmt("PUSH  %s", rn(r)), 1);
    sp_adj += 2;
}

static void e_pop(int r)
{
    e_text(fmt("POP   %s", rn(r)), 1);
    sp_adj -= 2;
    if (r == flags_reg || r == R_FLAGS) flags_reg = -1;
}

static void e_term(const char *s)
{
    e_text(s, 1);
    code[ncode - 1].term = 1;
    flags_reg = -1;
}

static void e_label(int l)
{
    Ins *i = add_ins(I_LABEL);
    i->label = l;
    flags_reg = -1;
}

static void e_named_label(const char *name)
{
    Ins *i = add_ins(I_LABEL);
    i->text = fmt("%s", name);
    flags_reg = -1;
}

static void e_jmp(int cc, int l)
{
    Ins *i = add_ins(I_JMP);
    i->cc = cc;
    i->label = l;
    i->size = 1;
}

static void e_ext(Type *t, int d, int s)
{
    e_rr(t->kind == TY_CHAR ? "LDSX" : "LDZX", d, s);
}

/* An ALU instruction with constant simplification: ADD #-k -> SUB #k, shifts by 0 and >= 16. */
static void e_alu(const char *mn, int d, Opnd *o)
{
    if (o->mode == M_IMM && !o->sym) {
        i64 v = norm16(o->off, 1);
        if (!strcmp(mn, "ADD") || !strcmp(mn, "SUB")) {
            if (!v) return;
            if (v < 0 && v >= -15) {
                Opnd t = imm_opnd(-v);
                e_op(mn[0] == 'A' ? "SUB" : "ADD", d, &t);
                return;
            }
        }
        if (!strcmp(mn, "SHL") || !strcmp(mn, "SHR") || !strcmp(mn, "SAR")) {
            i64 c = o->off & 0xFFFF;
            if (!c) return;
            if (c >= 16) {
                if (!strcmp(mn, "SAR")) e_ri("SAR", d, 15);
                else e_ri("LD", d, 0);
                return;
            }
        }
        if (!strcmp(mn, "ROR") && !(o->off & 15)) return;
    }
    e_op(mn, d, o);
}

static void scale_reg(int r, int sz)
{
    if (sz == 1) return;
    int k = log2exact(sz);
    if (k >= 0) e_ri("SHL", r, k);
    else e_ri("MUL", r, sz);
}

static void load_opnd(int d, Opnd *o, Type *ty)
{
    if (o->mode == M_REG) {
        if (o->reg != d) e_rr("LD", d, o->reg);
        return;
    }
    if (is_narrow(ty) && o->mode != M_IMM && o->mode != M_LEA) {
        e_op(ty->kind == TY_CHAR ? "LDSX" : "LDZX", d, o);
        return;
    }
    e_op("LD", d, o);
}

/* --------------------------- operands ---------------------------------- */

static int local_off(Local *v)
{
    if (v->on_stack == 2) return cur_frame + 2 * cur_nsaved + 2 * v->psoff;
    return v->off;
}

static void opnd_add_off(Opnd *o, i64 off)
{
    if (!off) return;
    switch (o->mode) {
    case M_IND: o->mode = M_IDX; o->off = off; break;
    case M_IDX: case M_ABS: o->off += off; break;
    default: die("internal error: operand offset");
    }
}

static int addr_operand(Node *x, Opnd *o)
{
    Opnd t;
    if (!get_lval(x, &t, 1)) return 0;
    *o = t;
    switch (t.mode) {
    case M_ABS: o->mode = M_IMM; return 1;
    case M_IDX: o->mode = M_LEA; return 1;
    case M_IND: o->mode = M_REG; return 1;
    }
    return 0;
}

/* The R2/MOD operand for an expression value without generating code (if possible).
   word_only: an operand for ALU instructions, byte memory will not do. */
static int try_operand(Node *e, Opnd *o, int word_only)
{
    memset(o, 0, sizeof *o);
    o->tmp = -1;
    switch (e->kind) {
    case N_NUM: o->mode = M_IMM; o->off = e->val; return 1;
    case N_STR:
        o->mode = M_IMM;
        if (!emitting) { o->sym = "__str"; return 1; }
        if (!e->sname) e->sname = fmt("%s.s%d", str_owner->asmname, str_owner->nstr++);
        e->used = 1;
        o->sym = e->sname;
        return 1;
    case N_SYMADDR: o->mode = M_IMM; o->sym = e->str; return 1;
    case N_FNAME: o->mode = M_IMM; o->sym = e->sym->asmname; return 1;
    case N_REGVAL: o->mode = M_REG; o->reg = (int)e->val; return 1;
    case N_ADDR: return addr_operand(e->a, o);
    case N_CAST:
        if (is_word(e->ty) && (is_word(decay(e->a->ty)) || e->a->kind == N_NUM))
            return try_operand(e->a, o, word_only);
        if (is_word(e->ty) && e->a->kind == N_LOCAL && e->a->lv->reg >= 0)
            return try_operand(e->a, o, word_only);
        return 0;
    }
    if (is_lval(e) && e->ty->kind == TY_ARRAY) return addr_operand(e, o);
    if (is_lval(e) && is_scalar(e->ty)) {
        if (!get_lval(e, o, 1)) return 0;
        if (word_only && is_narrow(e->ty) && o->mode != M_REG) return 0;
        return 1;
    }
    return 0;
}

/* The memory operand for pointer p. acc - the access size (1, 2) for auto-increment, 0 - forbidden. */
static int lval_ptr(Node *p, int acc, Opnd *o, int dry)
{
    memset(o, 0, sizeof *o);
    o->tmp = -1;
    if (p->ty->kind == TY_ARRAY && is_lval(p)) return get_lval(p, o, dry);
    if (p->kind == N_ADDR) return get_lval(p->a, o, dry);
    Type *pt = decay(p->ty);
    if (p->kind == N_LOCAL && p->lv->reg >= 0) {
        o->mode = M_IND;
        o->reg = p->lv->reg;
        return 1;
    }
    if ((p->kind == N_POSTINC || p->kind == N_PREDEC) && p->a->kind == N_LOCAL && p->a->lv->reg >= 0 &&
        (acc == 1 || acc == 2) && pt->kind == TY_PTR && elem_size(pt) == acc) {
        o->mode = p->kind == N_POSTINC ? M_INC : M_DEC;
        o->reg = p->a->lv->reg;
        return 1;
    }
    if (p->kind == N_BIN && (p->op == '+' || p->op == '-') && !p->ptrdiff && p->b->kind == N_NUM &&
        decay(p->a->ty)->kind == TY_PTR) {
        i64 k = p->b->val * elem_size(decay(p->a->ty));
        if (p->op == '-') k = -k;
        if (!lval_ptr(p->a, 0, o, dry)) return 0;
        if (o->mode == M_REG) die("internal error: lval_ptr");
        opnd_add_off(o, k);
        return 1;
    }
    if (dry) return 0;
    int t = alloc_temp();
    gen(p, t);
    o->mode = M_IND;
    o->reg = t;
    o->tmp = t;
    return 1;
}

static int lval_index(Node *e, Opnd *o, int dry)
{
    Node *a = e->a, *i = e->b;
    int es = type_size(e->ty);
    if (i->kind == N_NUM) {
        if (!lval_ptr(a, 0, o, dry)) return 0;
        opnd_add_off(o, i->val * es);
        return 1;
    }
    Opnd base;
    int isarr = a->ty->kind == TY_ARRAY;
    int have_base = isarr && get_lval(a, &base, 1);
    if (dry) {
        if (have_base && base.mode == M_ABS && es == 1 && i->kind == N_LOCAL && i->lv->reg >= 0) {
            *o = base;
            o->mode = M_IDX;
            o->reg = i->lv->reg;
            return 1;
        }
        return 0;
    }
    memset(o, 0, sizeof *o);
    o->tmp = -1;
    if (have_base && base.mode == M_ABS && es == 1 && i->kind == N_LOCAL && i->lv->reg >= 0) {
        *o = base;
        o->mode = M_IDX;
        o->reg = i->lv->reg;
        return 1;
    }
    Opnd po;
    int ptr_opnd = !isarr && try_operand(a, &po, 1) && po.mode != M_INC && po.mode != M_DEC;
    if (!(have_base && (base.mode == M_ABS || base.mode == M_IDX || base.mode == M_IND)) && !ptr_opnd) {
        /* the base needs computing (for example, (*p)[i][j]): the base address goes straight into t,
           the index is added to it - this way only one temporary register beyond the base is needed */
        int t = alloc_temp();
        if (isarr) addr_into(a, t); else gen(a, t);
        add_index(t, i, es);
        o->mode = M_IND;
        o->reg = t;
        o->tmp = t;
        return 1;
    }
    int t = alloc_temp();
    gen(i, t);
    scale_reg(t, es);
    if (have_base) {
        switch (base.mode) {
        case M_ABS:
            *o = base;
            o->mode = M_IDX;
            o->reg = t;
            o->tmp = t;
            return 1;
        case M_IDX:
            e_rr("ADD", t, base.reg);
            o->mode = M_IDX;
            o->reg = t;
            o->sym = base.sym;
            o->off = eff_off(&base);
            o->tmp = t;
            return 1;
        case M_IND:
            e_rr("ADD", t, base.reg);
            o->mode = M_IND;
            o->reg = t;
            o->tmp = t;
            return 1;
        }
    }
    e_alu("ADD", t, &po);
    o->mode = M_IND;
    o->reg = t;
    o->tmp = t;
    return 1;
}

/* The memory operand (or the variable's register) for an lvalue. */
static int get_lval(Node *e, Opnd *o, int dry)
{
    memset(o, 0, sizeof *o);
    o->tmp = -1;
    switch (e->kind) {
    case N_LOCAL: {
        Local *v = e->lv;
        if (v->reg >= 0) { o->mode = M_REG; o->reg = v->reg; return 1; }
        if (!v->on_stack) die("internal error: variable %s has no storage", v->name);
        o->mode = M_IDX;
        o->reg = R_SP;
        o->off = local_off(v);
        return 1;
    }
    case N_GLOBAL: o->mode = M_ABS; o->sym = e->sym->asmname; return 1;
    case N_MMIO: o->mode = M_ABS; o->off = e->sym->val; return 1;
    case N_DEREF: return lval_ptr(e->a, is_scalar(e->ty) ? type_size(e->ty) : 0, o, dry);
    case N_INDEX: return lval_index(e, o, dry);
    case N_FIELD: {
        Node *b = e->a;
        int ok = b->ty->kind == TY_PTR ? lval_ptr(b, 0, o, dry) : get_lval(b, o, dry);
        if (!ok) return 0;
        if (o->mode == M_REG) die("internal error: field of a register variable");
        opnd_add_off(o, e->field->off);
        return 1;
    }
    }
    return 0;
}

/* An auto-increment operand cannot be used twice (a read and a write). */
static void fix_autoinc(Opnd *o, int step)
{
    if (o->mode == M_INC) {
        int t = alloc_temp();
        e_rr("LD", t, o->reg);
        e_ri("ADD", o->reg, step);
        o->mode = M_IND;
        o->reg = t;
        o->tmp = t;
    } else if (o->mode == M_DEC) {
        e_ri("SUB", o->reg, step);
        o->mode = M_IND;
    }
}

/* An address from a memory operand into register d. */
static void addr_from_opnd(Opnd *o, int d)
{
    switch (o->mode) {
    case M_ABS: o->mode = M_IMM; e_op("LD", d, o); break;
    case M_IDX: o->mode = M_LEA; e_op("LD", d, o); break;
    case M_IND: if (o->reg != d) e_rr("LD", d, o->reg); break;
    default: die("cannot take the address");
    }
}

/* d += i * es */
static void add_index(int d, Node *i, int es)
{
    Opnd o;
    if (es == 1 && try_operand(i, &o, 1) && o.mode != M_INC && o.mode != M_DEC && !opnd_uses(&o, d)) {
        e_alu("ADD", d, &o);
        return;
    }
    Retry rt;
    retry_begin(&rt);
    if (!setjmp(rt.jb)) {
        int t = alloc_temp();
        gen(i, t);
        scale_reg(t, es);
        e_rr("ADD", d, t);
        free_temp(t);
        retry_end(&rt);
    } else {
        /* the base waits on the stack while the index is computed */
        retry_restore(&rt);
        e_push(d);
        gen(i, d);
        scale_reg(d, es);
        int t = alloc_temp();
        e_pop(t);
        e_rr("ADD", d, t);
        free_temp(t);
    }
}

/* The address of lvalue x into register d: the base is computed straight into d, then the
   indexes and field offsets are added. One temporary register per index level is needed. */
static void addr_into(Node *x, int d)
{
    Opnd o;
    if (get_lval(x, &o, 1) && (o.mode == M_ABS || o.mode == M_IDX || o.mode == M_IND)) {
        addr_from_opnd(&o, d);
        return;
    }
    switch (x->kind) {
    case N_DEREF:
        gen(x->a, d);
        return;
    case N_INDEX: {
        Node *a = x->a, *i = x->b;
        int es = type_size(x->ty);
        Opnd base;
        if (a->ty->kind == TY_ARRAY && i->kind != N_NUM && get_lval(a, &base, 1) &&
            (base.mode == M_ABS || base.mode == M_IDX || base.mode == M_IND) && !opnd_uses(&base, d)) {
            /* the base is known without computation: the index goes straight into d, then d += base (LEA) */
            gen(i, d);
            scale_reg(d, es);
            if (base.mode == M_ABS) {
                Opnd l = base;
                l.mode = M_LEA;
                l.reg = d;
                e_op("LD", d, &l);
            } else {
                i64 off = eff_off(&base);
                e_rr("ADD", d, base.reg);
                if (off || base.sym) {
                    Opnd l = base;
                    l.mode = M_LEA;
                    l.reg = d;
                    l.off = off;
                    e_op("LD", d, &l);
                }
            }
            return;
        }
        if (a->ty->kind == TY_ARRAY) addr_into(a, d); else gen(a, d);
        if (i->kind == N_NUM) {
            Opnd k = imm_opnd(i->val * es);
            e_alu("ADD", d, &k);
        } else {
            add_index(d, i, es);
        }
        return;
    }
    case N_FIELD: {
        Node *b = x->a;
        if (b->ty->kind == TY_PTR) gen(b, d); else addr_into(b, d);
        Opnd k = imm_opnd(x->field->off);
        e_alu("ADD", d, &k);
        return;
    }
    }
    get_lval(x, &o, 0);
    addr_from_opnd(&o, d);
    free_opnd(&o);
}

static void gen_addr(Node *x, int d) { addr_into(x, d); }

/* ------------------------- expressions ----------------------------------- */

static int need(Node *e)
{
    Opnd o;
    if (try_operand(e, &o, 0)) return 1;
    switch (e->kind) {
    case N_CALL: return 20;
    case N_BIN: {
        if (is_wide(e->ty) && ((e->op == '*' || e->op == '/' || e->op == '%') ? !wide_inline_muldiv(e->op, e->sgn, e->a, e->b)
                                                                           : ((e->op == TK_SHL || e->op == TK_SHR) && e->b->kind != N_NUM)))
            return 20;
        if (!is_wide(e->ty) && (e->op == '/' || e->op == '%') && e->sgn && !const_divisor(e->b)) return 20;
        int l = need(e->a), r = try_operand(e->b, &o, 1) ? 0 : need(e->b);
        return l == r ? l + 1 : (l > r ? l : r);
    }
    }
    int m = 1;
    if (e->a) { int x = need(e->a); if (x > m) m = x; }
    if (e->b) { int x = need(e->b) + 1; if (x > m) m = x; }
    for (int i = 0; i < e->n; i++) { int x = need(e->list[i]) + 1; if (x > m) m = x; }
    return m;
}

static int mentions(Node *e, Local *v)
{
    if (!e) return 0;
    if (e->kind == N_LOCAL) return e->lv == v;
    if (mentions(e->a, v) || mentions(e->b, v) || mentions(e->c, v)) return 1;
    for (int i = 0; i < e->n; i++) if (mentions(e->list[i], v)) return 1;
    return 0;
}

/* Compute e straight into the register of variable d if that is safe. */
static int is_var(Node *e, Local *v) { return e->kind == N_LOCAL && e->lv == v; }

/* Whether e can be computed straight into the register of variable v: v must not be read
   after the first write to that register. */
static int safe_into(Node *e, Local *v)
{
    if (!mentions(e, v) || is_var(e, v)) return 1;
    switch (e->kind) {
    case N_BIN:
        /* gen(a, d), then OP d, b: b must not read v */
        return safe_into(e->a, v) && !mentions(e->b, v);
    case N_NEG: case N_BNOT: case N_CAST:
        return safe_into(e->a, v);
    }
    return 0;
}

static void gen_to(Node *e, int d)
{
    Local *v = reg_var[d];
    Opnd o;
    int safe = !v || safe_into(e, v) ||
        /* v op x: v is read before d is written */
        (e->kind == N_BIN && is_var(e->a, v)) ||
        /* a single load instruction, for example v = v.next */
        (try_operand(e, &o, 0) && o.mode != M_INC && o.mode != M_DEC);
    if (!safe) {
        int t = alloc_temp();
        gen(e, t);
        e_rr("LD", d, t);
        free_temp(t);
        return;
    }
    gen(e, d);
}

static const char *alu_mn(Node *e)
{
    switch (e->op) {
    case '+': return "ADD";
    case '-': return "SUB";
    case '&': return "AND";
    case '|': return "OR";
    case '^': return "XOR";
    case '*': return "MUL";
    case '/': return "DIVU";
    case '%': return "REMU";
    case TK_SHL: return "SHL";
    case TK_SHR: return e->sgn ? "SAR" : "SHR";
    }
    die("internal error: operator");
    return NULL;
}

static int is_commut(int op) { return op == '+' || op == '&' || op == '|' || op == '^' || op == '*'; }

static int ptr_scale(Node *e)
{
    if ((e->op == '+' || e->op == '-') && !e->ptrdiff && decay(e->a->ty)->kind == TY_PTR)
        return elem_size(decay(e->a->ty));
    return 1;
}

static void gen_rtcall(Sym *rt, Node *a, Node *b, int d)
{
    Node c, f;
    Node *args[2] = {a, b};
    memset(&c, 0, sizeof c);
    memset(&f, 0, sizeof f);
    f.kind = N_FNAME;
    f.sym = rt;
    f.ty = rt->ty;
    c.kind = N_CALL;
    c.a = &f;
    c.sym = rt;
    c.fty = rt->ty;
    c.list = args;
    c.n = 2;
    c.ty = &t_int;
    rt->used_rt = 1;
    gen_call(&c, d);
}

/* The multiplier M and the shift s for signed division by d >= 3, not a power of two
   (Warren, "Hacker's Delight", sect. 10.3): q = ((M·n) >> 16) >> s,
   plus 1 when n < 0; M is unsigned. */
static int div_magic(i64 d, i64 *M, int *sh)
{
    unsigned long two15 = 1UL << 15, anc = two15 - 1 - two15 % d;
    unsigned long q1 = two15 / anc, r1 = two15 - q1 * anc, q2 = two15 / d, r2 = two15 - q2 * d;
    int p = 15;
    for (;;) {
        p++;
        q1 *= 2; r1 *= 2;
        if (r1 >= anc) { q1++; r1 -= anc; }
        q2 *= 2; r2 *= 2;
        if (r2 >= (unsigned long)d) { q2++; r2 -= d; }
        unsigned long delta = d - r2;
        if (!(q1 < delta || (q1 == delta && r1 == 0))) break;
    }
    *M = (i64)(q2 + 1);
    *sh = p - 16;
    return *M <= 0xFFFF;
}

/* Signed x / k and x % k with a constant k: by shifts (k is a power of two) or
   by multiplying by the reciprocal via MULHU. The dividend is already in d. Returns 0 if it did not work out. */
static void gen_sdiv_const(int op, i64 k, int d, int t, int u)
{
    i64 m = k < 0 ? -k : k;
    int lg = log2exact(m);
    if (m == 1) {
        if (op == '%') e_ri("LD", d, 0);
        else if (k < 0) e_misc("NEG", d);
        return;
    }
    if (lg > 0) {
        /* correction m - 1 for a negative dividend: rounding toward zero */
        e_rr("LD", t, d);
        e_ri("SAR", t, 15);
        e_ri("SHR", t, 16 - lg);
        e_rr("ADD", d, t);
        if (op == '/') {
            e_ri("SAR", d, lg);
            if (k < 0) e_misc("NEG", d);
        } else {
            Opnd mask = imm_opnd(m - 1);
            e_op("AND", d, &mask);
            e_rr("SUB", d, t);
        }
        return;
    }
    i64 M;
    int sh;
    div_magic(m, &M, &sh);
    Opnd mo = imm_opnd(M);
    e_rr("LD", t, d);
    e_op("MULHU", t, &mo);
    e_rr("LD", u, d);              /* MULHU is unsigned: for n < 0 subtract M */
    e_ri("SAR", u, 15);
    e_op("AND", u, &mo);
    e_rr("SUB", t, u);
    if (sh) e_ri("SAR", t, sh);
    e_rr("LD", u, d);
    e_ri("SHR", u, 15);
    e_rr("ADD", t, u);             /* +1 for a negative dividend */
    if (op == '/') {
        e_rr("LD", d, t);
        if (k < 0) e_misc("NEG", d);
    } else {
        Opnd mm = imm_opnd(m);
        e_op("MUL", t, &mm);
        e_rr("SUB", d, t);
    }
}

static void gen_bin(Node *e, int d)
{
    Node *a = e->a, *b = e->b;
    if (e->op == '*' && a->kind == N_NUM && b->kind != N_NUM) { Node *x = a; a = b; b = x; }
    if ((e->op == '/' || e->op == '%') && e->sgn) {
        if (!const_divisor(b)) {
            gen_rtcall(e->op == '/' ? &rt_divs : &rt_mods, a, b, d);
            return;
        }
        i64 k = norm16(b->val, 1), M;
        int sh, lg = log2exact(k < 0 ? -k : k);
        if (lg < 0 && !div_magic(k < 0 ? -k : k, &M, &sh)) {
            gen_rtcall(e->op == '/' ? &rt_divs : &rt_mods, a, b, d);
            return;
        }
        gen(a, d);
        Retry rt;
        retry_begin(&rt);
        if (!setjmp(rt.jb)) {
            int t = alloc_temp(), u = alloc_temp();
            gen_sdiv_const(e->op, k, d, t, u);
            free_temp(u);
            free_temp(t);
            retry_end(&rt);
        } else {
            /* registers ran out: two registers are saved on the stack for the duration */
            retry_restore(&rt);
            int t = d == 0 ? 1 : 0, u = d == 2 ? 1 : 2;
            if (u == t) u = 3;
            e_push(t);
            e_push(u);
            gen_sdiv_const(e->op, k, d, t, u);
            e_pop(u);
            e_pop(t);
        }
        return;
    }
    if (b->kind == N_NUM && ptr_scale(e) == 1 && !e->ptrdiff) {
        i64 k = b->val & 0xFFFF;
        int lg = log2exact(k), lgn = log2exact((-k) & 0xFFFF);
        if (e->op == '*' && (lg >= 0 || lgn > 0 || k == 0)) {
            gen(a, d);
            if (k == 0) e_ri("LD", d, 0);
            else if (lg >= 0) { if (lg) e_ri("SHL", d, lg); }
            else { e_ri("SHL", d, lgn); e_misc("NEG", d); }
            return;
        }
        if ((e->op == '/' || e->op == '%') && lg >= 0) {
            gen(a, d);
            if (e->op == '/') { if (lg > 0) e_ri("SHR", d, lg); }
            else e_ri("AND", d, k - 1);
            return;
        }
    }
    const char *mn = alu_mn(e);
    int sz = ptr_scale(e);
    Opnd o;
    if (sz != 1 && b->kind == N_NUM) {
        o = imm_opnd(b->val * sz);
        gen(a, d);
        e_alu(mn, d, &o);
    } else if (sz == 1 && try_operand(b, &o, 1)) {
        gen(a, d);
        e_alu(mn, d, &o);
    } else if (sz == 1 && is_commut(e->op) && try_operand(a, &o, 1) && !opnd_uses(&o, d)) {
        gen(b, d);
        e_alu(mn, d, &o);
    } else {
        Retry rt;
        retry_begin(&rt);
        if (!setjmp(rt.jb)) {
            int t;
            if (need(b) > need(a)) {
                t = alloc_temp();
                gen(b, t);
                scale_reg(t, sz);
                gen(a, d);
            } else {
                gen(a, d);
                t = alloc_temp();
                gen(b, t);
                scale_reg(t, sz);
            }
            Opnd r = reg_opnd(t);
            e_alu(mn, d, &r);
            free_temp(t);
            retry_end(&rt);
        } else {
            /* registers ran out: the right operand goes through the stack */
            retry_restore(&rt);
            if (a->kind == N_REGVAL && (int)a->val == d) {
                /* the left value is already in d (compound assignment): first onto the stack */
                e_push(d);
                gen(b, d);
                scale_reg(d, sz);
                int t = alloc_temp();
                e_pop(t);
                Opnd r = reg_opnd(d);
                e_alu(mn, t, &r);
                e_rr("LD", d, t);
                free_temp(t);
            } else {
                gen(b, d);
                scale_reg(d, sz);
                e_push(d);
                gen(a, d);
                int t = alloc_temp();
                e_pop(t);
                Opnd r = reg_opnd(t);
                e_alu(mn, d, &r);
                free_temp(t);
            }
        }
    }
    if (e->ptrdiff > 1) e_ri("SAR", d, log2exact(e->ptrdiff));
}

/* An argument onto the stack: a word or two words (the low one at the lower address). */
static void push_arg(Node *a, Type *pt)
{
    if (is_wide(pt)) {
        int lo = alloc_temp(), hi = alloc_temp();
        gen_w(a, lo, hi);
        e_push(hi);
        e_push(lo);
        free_temp(hi);
        free_temp(lo);
    } else {
        int t = alloc_temp();
        gen(a, t);
        e_push(t);
        free_temp(t);
    }
}

/* A call. Result: d is the register of a one-word result; for a two-word result d is the low and d2 the high word. */
static void gen_call2(Node *e, int d, int d2)
{
    Node *f = e->a;
    int n = e->n;
    int dmask = ((d >= 0 && d < 4) ? 1 << d : 0) | ((d2 >= 0 && d2 < 4) ? 1 << d2 : 0);
    int save = busy & 0xF & ~dmask;
    for (int r = 0; r < 4; r++) if (save >> r & 1) e_push(r);
    int busy0 = busy;
    /* the registers holding N_REGVAL arguments (compound assignment) stay occupied:
       they are also the result registers, but must not be overwritten before the argument is passed */
    int keep = 0;
    for (int i = 0; i < n; i++)
        if (e->list[i]->kind == N_REGVAL) {
            i64 v = e->list[i]->val;
            keep |= is_wide(e->list[i]->ty) ? (1 << (v & 0xFF)) | (1 << ((v >> 8) & 0xFF)) : 1 << (int)v;
        }
    busy &= ~(save | (dmask & ~keep));
    int direct = f->kind == N_FNAME, fop = 0, ft = -1;
    Opnd fo;
    if (!direct) {
        if (try_operand(f, &fo, 1) && fo.mode != M_INC && fo.mode != M_DEC && fo.mode != M_IMM &&
            !(fo.reg < 4 && opnd_uses(&fo, fo.reg)))
            fop = 1;
        else {
            ft = alloc_hi();
            gen(f, ft);
        }
    }
    int preg[32], psoff[32], nslots;
    int nst = layout_args(e->fty->params, n, preg, psoff, &nslots);
    for (int i = n - 1; i >= 0; i--)
        if (preg[i] < 0) push_arg(e->list[i], e->fty->params[i]);
    Retry rt;
    retry_begin(&rt);
    if (!setjmp(rt.jb)) {
        int argbits = 0;
        for (int i = 0; i < n; i++) {
            if (preg[i] < 0) continue;
            int w = is_wide(e->fty->params[i]) ? 2 : 1;
            argbits |= ((1 << w) - 1) << preg[i];
            if (w == 2) {
                busy |= 3 << preg[i];
                gen_w(e->list[i], preg[i], preg[i] + 1);
            } else {
                busy |= 1 << preg[i];
                gen(e->list[i], preg[i]);
            }
            if (e->list[i]->kind == N_REGVAL) busy &= ~(keep & ~argbits);   /* the value is passed: the source is free */
        }
        retry_end(&rt);
    } else {
        /* registers ran out: arguments go through the stack, then POP into R0..R3 */
        retry_restore(&rt);
        if (n > 0 && preg[0] >= 0 && e->list[0]->kind == N_REGVAL) {
            /* the left value (compound assignment) goes onto the stack first, its registers are free */
            i64 v = e->list[0]->val;
            int w0 = is_wide(e->fty->params[0]) ? 2 : 1;
            if (w0 == 2) { e_push((int)((v >> 8) & 0xFF)); e_push((int)(v & 0xFF)); }
            else e_push((int)v);
            busy &= ~keep;
            for (int i = n - 1; i >= 1; i--)
                if (preg[i] >= 0) push_arg(e->list[i], e->fty->params[i]);
            for (int i = 1; i < n; i++) {
                if (preg[i] < 0) continue;
                int w = is_wide(e->fty->params[i]) ? 2 : 1;
                for (int k = 0; k < w; k++) { busy |= 1 << (preg[i] + k); e_pop(preg[i] + k); }
            }
            for (int k = 0; k < w0; k++) { busy |= 1 << (preg[0] + k); e_pop(preg[0] + k); }
        } else {
            for (int i = n - 1; i >= 0; i--)
                if (preg[i] >= 0) push_arg(e->list[i], e->fty->params[i]);
            for (int r = 0; r < nslots; r++) {
                busy |= 1 << r;
                e_pop(r);
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (preg[i] < 0) continue;
        Type *pt = e->fty->params[i], *at = e->list[i]->ty;
        if (is_narrow(pt) && !(at->kind == pt->kind) && e->list[i]->kind != N_NUM) e_ext(pt, preg[i], preg[i]);
    }
    if (direct) e_text(fmt("CALL  %s", f->sym->asmname), 2);
    else if (fop) e_text(fmt("CALL  %s", opnd_str(&fo)), opnd_words(&fo));
    else e_text(fmt("CALL  %s", rn(ft)), 1);
    flags_reg = -1;
    if (nst) {
        e_ri("ADD", R_SP, 2 * nst);
        sp_adj -= 2 * nst;
    }
    busy = busy0;
    if (d2 >= 0) {
        int src[2] = {0, 1}, dst[2] = {d, d2};
        par_move(src, dst, 2);
    } else if (d >= 0 && d != 0) e_rr("LD", d, 0);
    for (int r = 3; r >= 0; r--) if (save >> r & 1) e_pop(r);
}

static void gen_call(Node *e, int d) { gen_call2(e, d, -1); }

/* The operands of the low and high words of a 32-bit object (a long or ulong variable is a register pair). */
static void obj32_opnds(Node *x, Opnd *lo, Opnd *hi)
{
    if (x->kind == N_LOCAL && x->lv->reg >= 0 && is_wide(x->lv->ty)) {
        *lo = reg_opnd(x->lv->reg);
        *hi = reg_opnd(x->lv->reg2);
        return;
    }
    get_lval(x, lo, 0);
    if (lo->mode == M_INC || lo->mode == M_DEC) fix_autoinc(lo, 4);
    *hi = *lo;
    hi->tmp = -1;
    opnd_add_off(hi, 2);
}

static void gen_builtin(Node *e, int d)
{
    Node **a = e->list;
    switch (e->op) {
    case B_MULHU: case B_ROR: {
        const char *mn = e->op == B_MULHU ? "MULHU" : "ROR";
        int r = d >= 0 ? d : alloc_temp();
        Opnd o;
        if (try_operand(a[1], &o, 1) && !opnd_uses(&o, r)) {
            gen(a[0], r);
            e_alu(mn, r, &o);
        } else {
            int t = alloc_temp();
            gen(a[1], t);
            gen(a[0], r);
            Opnd tr = reg_opnd(t);
            e_op(mn, r, &tr);
            free_temp(t);
        }
        if (d < 0) free_temp(r);
        return;
    }
    case B_XCHB: {
        int r = d >= 0 ? d : alloc_temp();
        gen(a[1], r);
        Opnd o;
        lval_ptr(a[0], 1, &o, 0);
        e_text(fmt("XCHB  %s, %s", opnd_str(&o), rn(r)), opnd_words(&o));
        if (r == flags_reg) flags_reg = -1;
        free_opnd(&o);
        if (d < 0) free_temp(r);
        return;
    }
    case B_DI: e_text("CLI", 1); flags_reg = -1; return;
    case B_EI: e_text("STI", 1); return;
    case B_WFI: e_text("WFI", 1); return;
    case B_TRAP: e_text(fmt("TRAP  #%lld", a[0]->val), 1); flags_reg = -1; return;
    }
}

static int fits(Type *t, Node *r)
{
    if (r->kind != N_NUM) return 0;
    if (t->kind == TY_BYTE) return r->val >= 0 && r->val <= 255;
    return r->val >= -128 && r->val <= 127;
}

static void do_assign(Node *e, int d)
{
    Node *l = e->a, *r = e->b;
    int narrow = is_narrow(l->ty);
    int trunc = narrow && !(r->ty->kind == l->ty->kind) && !fits(l->ty, r);
    if (l->kind == N_LOCAL && l->lv->reg >= 0) {
        int rv = l->lv->reg;
        gen_to(r, rv);
        if (trunc) e_ext(l->ty, rv, rv);
        if (d >= 0 && d != rv) e_rr("LD", d, rv);
        return;
    }
    int rr, tmp = -1;
    if (r->kind == N_LOCAL && r->lv->reg >= 0) rr = r->lv->reg;
    else if (d >= 0) { rr = d; gen(r, d); }
    else { rr = tmp = alloc_temp(); gen(r, rr); }
    Opnd o;
    get_lval(l, &o, 0);
    e_st(narrow, &o, rr);
    free_opnd(&o);
    if (d >= 0) {
        if (rr != d) e_rr("LD", d, rr);
        if (trunc) e_ext(l->ty, d, d);
    }
    if (tmp >= 0) free_temp(tmp);
}

static void do_opassign(Node *e, int d)
{
    Node *l = e->a, *bin = e->c;
    int narrow = is_narrow(l->ty);
    if (l->kind == N_LOCAL && l->lv->reg >= 0) {
        int rv = l->lv->reg;
        gen_to(bin, rv);
        if (narrow) e_ext(l->ty, rv, rv);
        if (d >= 0 && d != rv) e_rr("LD", d, rv);
        return;
    }
    Opnd o;
    get_lval(l, &o, 0);
    fix_autoinc(&o, type_size(l->ty));
    int t = alloc_temp();
    load_opnd(t, &o, l->ty);
    Node rv, b2 = *bin;
    memset(&rv, 0, sizeof rv);
    rv.kind = N_REGVAL;
    rv.val = t;
    rv.ty = is_narrow(l->ty) ? promote(l->ty) : l->ty;
    b2.a = &rv;
    gen_bin(&b2, t);
    e_st(narrow, &o, t);
    if (d >= 0) {
        e_rr("LD", d, t);
        if (narrow) e_ext(l->ty, d, d);
    }
    free_temp(t);
    free_opnd(&o);
}

static void do_incdec(Node *e, int d)
{
    Node *l = e->a;
    int post = e->kind == N_POSTINC || e->kind == N_POSTDEC;
    const char *mn = (e->kind == N_PREINC || e->kind == N_POSTINC) ? "ADD" : "SUB";
    Type *lt = decay(l->ty);
    int step = lt->kind == TY_PTR ? elem_size(lt) : 1;
    int narrow = is_narrow(l->ty);
    if (l->kind == N_LOCAL && l->lv->reg >= 0) {
        int rv = l->lv->reg;
        if (post && d >= 0) e_rr("LD", d, rv);
        e_ri(mn, rv, step);
        if (narrow) e_ext(l->ty, rv, rv);
        if (!post && d >= 0 && d != rv) e_rr("LD", d, rv);
        return;
    }
    Opnd o;
    get_lval(l, &o, 0);
    fix_autoinc(&o, type_size(l->ty));
    int t = alloc_temp();
    load_opnd(t, &o, l->ty);
    if (post && d >= 0) e_rr("LD", d, t);
    e_ri(mn, t, step);
    e_st(narrow, &o, t);
    if (!post && d >= 0) {
        e_rr("LD", d, t);
        if (narrow) e_ext(l->ty, d, d);
    }
    free_temp(t);
    free_opnd(&o);
}

/* A two-word value in registers lo (the low word) and hi (the high word). */
/* Storing the pair (lo, hi) into a 32-bit object: memory or the variable's register pair. */
static void st_w(Opnd *xl, Opnd *xh, int lo, int hi)
{
    if (xl->mode == M_REG) {
        int src[2] = {lo, hi}, dst[2] = {xl->reg, xh->reg};
        par_move(src, dst, 2);
        return;
    }
    e_st(0, xl, lo);
    e_st(0, xh, hi);
}

static int is_var(Node *e, Local *v);
static int is_regvar_w(Node *l) { return l->kind == N_LOCAL && l->lv->reg >= 0 && is_wide(l->lv->ty); }

static void do_assign_w(Node *e, int dlo, int dhi);
static void do_opassign_w(Node *e, int dlo, int dhi);
static void do_incdec_w(Node *e, int dlo, int dhi);
static void gen_w_bin(Node *e, int lo, int hi);

/* Operands of a two-word value without computing into registers: a constant, a 32-bit object
   in memory, an unsigned word value (the high word is 0). Otherwise 0 (no code is emitted). */
static int wide_operand(Node *e, Opnd *lo, Opnd *hi)
{
    memset(lo, 0, sizeof *lo);
    lo->tmp = -1;
    *hi = *lo;
    if (e->kind == N_NUM) {
        *lo = imm_opnd(e->val & 0xFFFF);
        *hi = imm_opnd((e->val >> 16) & 0xFFFF);
        return 1;
    }
    if (is_lval(e) && is_wide(e->ty)) {
        obj32_opnds(e, lo, hi);
        return 1;
    }
    if (e->kind == N_CAST && is_wide(e->ty) && !is_wide(decay(e->a->ty)) && !is_signed(promote(decay(e->a->ty)))) {
        Opnd o;
        if (try_operand(e->a, &o, 1) && o.mode != M_INC && o.mode != M_DEC) {
            *lo = o;
            *hi = imm_opnd(0);
            return 1;
        }
    }
    return 0;
}

static int is_imm0(Opnd *o) { return o->mode == M_IMM && !o->sym && !(o->off & 0xFFFF); }

/* An operand that needs no registers for computation (a constant, an object in memory). */
static int simple_w(Node *e)
{
    if (e->kind == N_NUM) return 1;
    if (is_lval(e) && is_wide(e->ty)) return 1;
    return e->kind == N_CAST && is_wide(e->ty) && !is_wide(decay(e->a->ty)) &&
           !is_signed(promote(decay(e->a->ty))) && need(e->a) <= 1;
}

/* The right operand is computed first if it is more complex (so the left pair does not occupy registers
   while it is being computed). */
static int b_first(Node *a, Node *b)
{
    if (a->kind == N_REGVAL) return 0;      /* a is already in the result registers */
    return !simple_w(b) && (simple_w(a) || need(b) > need(a));
}

/* A shift of the double word (lo, hi) by the constant n >= 0. */
static void gen_w_shift_const(int op, int sgn, i64 n, int lo, int hi)
{
    if (n == 0) return;
    if (n >= 32) {
        if (op == TK_SHR && sgn) { e_ri("SAR", hi, 15); e_rr("LD", lo, hi); }
        else { e_ri("LD", lo, 0); e_ri("LD", hi, 0); }
        return;
    }
    if (op == TK_SHL) {
        if (n >= 16) {
            e_rr("LD", hi, lo);
            if (n > 16) e_ri("SHL", hi, n - 16);
            e_ri("LD", lo, 0);
        } else if (n == 1) {
            e_rr("ADD", lo, lo);
            e_rr("ADC", hi, hi);
        } else {
            int t = alloc_temp();
            e_rr("LD", t, lo);
            e_ri("SHR", t, 16 - n);
            e_ri("SHL", hi, n);
            e_rr("OR", hi, t);
            e_ri("SHL", lo, n);
            free_temp(t);
        }
        return;
    }
    if (n >= 16) {
        e_rr("LD", lo, hi);
        if (n > 16) e_ri(sgn ? "SAR" : "SHR", lo, n - 16);
        if (sgn) e_ri("SAR", hi, 15);
        else e_ri("LD", hi, 0);
    } else {
        int t = alloc_temp();
        e_rr("LD", t, hi);
        e_ri("SHL", t, 16 - n);
        e_ri("SHR", lo, n);
        e_rr("OR", lo, t);
        e_ri(sgn ? "SAR" : "SHR", hi, n);
        free_temp(t);
    }
}

/* d = d op b over the register pair (d1, d2) and the operand b. */
static void wide_alu(int op, int d1, int d2, Opnd *bl, Opnd *bh)
{
    switch (op) {
    case '+': case '-': {
        const char *mn = op == '+' ? "ADD" : "SUB", *mc = op == '+' ? "ADC" : "SBC";
        if (is_imm0(bl)) e_op(mn, d2, bh);          /* no carry from the low word */
        else { e_op(mn, d1, bl); e_op(mc, d2, bh); }
        break;
    }
    case '&': e_op("AND", d1, bl); e_op("AND", d2, bh); break;
    case '|': e_op("OR", d1, bl); e_op("OR", d2, bh); break;
    case '^': e_op("XOR", d1, bl); e_op("XOR", d2, bh); break;
    default: die("this 32-bit operation is not implemented yet");
    }
}

/* a op b for + - & | ^ and shifts by a constant; the result is in the pair (lo, hi). If there are not
   enough registers, a is saved on the stack, b is computed in the freed registers, then a is popped. */
/* A call of a runtime function on two-word values: the result is in the pair (lo, hi). */
static void gen_rtcall_w(Sym *rt, Node *a, Node *b, int lo, int hi)
{
    Node c, f;
    Node *args[2] = {a, b};
    memset(&c, 0, sizeof c);
    memset(&f, 0, sizeof f);
    f.kind = N_FNAME;
    f.sym = rt;
    f.ty = rt->ty;
    c.kind = N_CALL;
    c.a = &f;
    c.sym = rt;
    c.fty = rt->ty;
    c.list = args;
    c.n = 2;
    c.ty = &t_long;
    rt->used_rt = 1;
    gen_call2(&c, lo, hi);
}

/* * / % and shifts by a variable count. */
static void gen_w_muldiv(Node *e, int lo, int hi)
{
    Node *a = e->a, *b = e->b;
    int op = e->op;
    if (op == TK_SHL || op == TK_SHR) {
        gen_rtcall_w(op == TK_SHL ? &rt_shl32 : e->sgn ? &rt_sar32 : &rt_shr32, a, b, lo, hi);
        return;
    }
    if (op == '*') {
        Node *x = NULL, *y = NULL;
        int k = 0;
        switch (wide_mul_kind(a, b, &x, &y, &k)) {
        case WM_ZERO:
            e_ri("LD", lo, 0);
            e_ri("LD", hi, 0);
            return;
        case WM_SHIFT:
            gen_w(x, lo, hi);
            gen_w_shift_const(TK_SHL, 0, k, lo, hi);
            return;
        case WM_MULHU: {
            /* two unsigned values no wider than a word: MUL gives the low word, MULHU the high one */
            /* x and y are already the original values no wider than a word; hi is not needed until the last step */
            int held = reg_var[hi] ? 0 : busy & (1 << hi);
            busy &= ~held;
            Retry rt;
            retry_begin(&rt);
            if (!setjmp(rt.jb)) {
                Opnd o;
                int tmp = -1;
                int yconst = y->kind == N_NUM && is_wide(y->ty);
                if (yconst) o = imm_opnd(y->val & 0xFFFF);
                if (yconst || (try_operand(y, &o, 1) && o.mode != M_INC && o.mode != M_DEC)) {
                    gen(x, lo);
                } else if (need(y) > need(x)) {
                    busy |= held;               /* tmp must not coincide with hi */
                    tmp = alloc_temp();
                    busy &= ~held;
                    gen(y, tmp);
                    gen(x, lo);
                    o = reg_opnd(tmp);
                } else {
                    gen(x, lo);
                    busy |= held;
                    tmp = alloc_temp();
                    busy &= ~held;
                    gen(y, tmp);
                    o = reg_opnd(tmp);
                }
                busy |= held;
                e_rr("LD", hi, lo);
                e_op("MUL", lo, &o);
                e_op("MULHU", hi, &o);
                if (tmp >= 0) free_temp(tmp);
                retry_end(&rt);
            } else {
                /* registers ran out: y onto the stack, then x */
                retry_restore(&rt);
                if (y->kind == N_NUM && is_wide(y->ty)) {
                    Opnd o = imm_opnd(y->val & 0xFFFF);
                    gen(x, lo);
                    busy |= held;
                    e_rr("LD", hi, lo);
                    e_op("MUL", lo, &o);
                    e_op("MULHU", hi, &o);
                } else {
                    gen(y, lo);
                    e_push(lo);
                    gen(x, lo);
                    busy |= held;
                    int t = alloc_temp();
                    e_pop(t);
                    Opnd o = reg_opnd(t);
                    e_rr("LD", hi, lo);
                    e_op("MUL", lo, &o);
                    e_op("MULHU", hi, &o);
                    free_temp(t);
                }
            }
            return;
        }
        }
        gen_rtcall_w(&rt_mul32, a, b, lo, hi);
        return;
    }
    int k = !e->sgn ? wide_pow2(b) : -1;
    if (k >= 0) {
        gen_w(a, lo, hi);
        if (op == '/') gen_w_shift_const(TK_SHR, 0, k, lo, hi);
        else {
            unsigned long mask = (1UL << k) - 1;
            Opnd ml = imm_opnd(mask & 0xFFFF), mh = imm_opnd((mask >> 16) & 0xFFFF);
            e_op("AND", lo, &ml);
            e_op("AND", hi, &mh);
        }
        return;
    }
    if (!e->sgn && b->kind == N_NUM && (unsigned long)(b->val & 0xFFFFFFFFUL) >= 3 && (unsigned long)(b->val & 0xFFFFFFFFUL) <= 255) {
        /* divisor 3..255: four DIVU/REMU steps over the bytes of the dividend */
        Node cn;
        memset(&cn, 0, sizeof cn);
        cn.kind = N_NUM;
        cn.val = b->val & 0xFF;
        cn.ty = &t_uint;
        gen_rtcall_w(op == '/' ? &rt_udivc32 : &rt_umodc32, a, &cn, lo, hi);
        return;
    }
    Sym *rt = op == '/' ? (e->sgn ? &rt_divs32 : &rt_divu32) : (e->sgn ? &rt_mods32 : &rt_modu32);
    gen_rtcall_w(rt, a, b, lo, hi);
}

static void gen_w_bin(Node *e, int lo, int hi)
{
    Node *a = e->a, *b = e->b;
    int op = e->op;
    if (op == '*' || op == '/' || op == '%' || ((op == TK_SHL || op == TK_SHR) && b->kind != N_NUM)) {
        gen_w_muldiv(e, lo, hi);
        return;
    }
    if (op == '|' || op == '+' || op == '^') {
        /* (ulong(h) << 16) | l: the words are assembled without computation */
        Node *h = shl16_src(a), *l = zext_src(b);
        if (!h || !l) { h = shl16_src(b); l = zext_src(a); }
        if (h && l) {
            gen(h, hi);
            gen(l, lo);
            return;
        }
    }
    if (op != TK_SHL && op != TK_SHR && b_first(a, b)) {
        /* b -> (lo, hi); a is an operand or a pair (a1, a2); the result is in (lo, hi).
           Not enough registers - the ordinary order with the stack. */
        Retry r1;
        retry_begin(&r1);
        if (!setjmp(r1.jb)) {
            gen_w(b, lo, hi);
            Opnd al, ah;
            Opnd rl = reg_opnd(lo), rh = reg_opnd(hi);
            int commut = op != '-';
            if (commut && wide_operand(a, &al, &ah)) {
                wide_alu(op, lo, hi, &al, &ah);
                free_opnd(&al);
            } else {
                int a1 = alloc_temp(), a2 = alloc_temp();
                gen_w(a, a1, a2);
                if (commut) {
                    Opnd x1 = reg_opnd(a1), x2 = reg_opnd(a2);
                    wide_alu(op, lo, hi, &x1, &x2);
                } else {
                    wide_alu(op, a1, a2, &rl, &rh);
                    int src[2] = {a1, a2}, dst[2] = {lo, hi};
                    par_move(src, dst, 2);
                }
                free_temp(a2);
                free_temp(a1);
            }
            retry_end(&r1);
            return;
        }
        retry_restore(&r1);
    }
    gen_w(a, lo, hi);
    if (op == TK_SHL || op == TK_SHR) {
        gen_w_shift_const(op, e->sgn, b->val, lo, hi);
        return;
    }
    Retry rt;
    retry_begin(&rt);
    if (!setjmp(rt.jb)) {
        Opnd bl, bh;
        int t1 = -1, t2 = -1;
        if (!wide_operand(b, &bl, &bh)) {
            t1 = alloc_temp();
            t2 = alloc_temp();
            gen_w(b, t1, t2);
            bl = reg_opnd(t1);
            bh = reg_opnd(t2);
        }
        wide_alu(op, lo, hi, &bl, &bh);
        free_opnd(&bl);
        if (t1 >= 0) { free_temp(t2); free_temp(t1); }
        retry_end(&rt);
    } else {
        retry_restore(&rt);
        int held = busy & ((1 << lo) | (1 << hi));
        e_push(hi);
        e_push(lo);
        busy &= ~held;
        int b1 = alloc_temp(), b2 = alloc_temp();
        gen_w(b, b1, b2);
        int a1 = alloc_temp(), a2 = alloc_temp();
        e_pop(a1);
        e_pop(a2);
        Opnd r1 = reg_opnd(b1), r2 = reg_opnd(b2);
        wide_alu(op, a1, a2, &r1, &r2);
        int src[2] = {a1, a2}, dst[2] = {lo, hi};
        par_move(src, dst, 2);
        free_temp(a2); free_temp(a1); free_temp(b2); free_temp(b1);
        busy |= held;
    }
}

static void gen_w(Node *e, int lo, int hi)
{
    Opnd xl, xh;
    errline = e->line;
    switch (e->kind) {
    case N_NUM:
        e_ri("LD", lo, e->val & 0xFFFF);
        e_ri("LD", hi, (e->val >> 16) & 0xFFFF);
        return;
    case N_LOCAL: case N_GLOBAL: case N_MMIO: case N_DEREF: case N_INDEX: case N_FIELD:
        obj32_opnds(e, &xl, &xh);
        e_op("LD", lo, &xl);
        e_op("LD", hi, &xh);
        free_opnd(&xl);
        return;
    case N_CAST: {
        Type *from = decay(e->a->ty);
        if (is_wide(from)) { gen_w(e->a, lo, hi); return; }
        {
            /* while the value is computed, hi is not needed - free it for temporaries */
            int held = busy & (1 << hi);
            if (reg_var[hi]) held = 0;
            busy &= ~held;
            gen(e->a, lo);                  /* a byte extended to a word by a load */
            busy |= held;
        }
        if (is_signed(promote(from))) { e_rr("LD", hi, lo); e_ri("SAR", hi, 15); }
        else e_ri("LD", hi, 0);
        return;
    }
    case N_ASSIGN: do_assign_w(e, lo, hi); return;
    case N_CALL: gen_call2(e, lo, hi); return;
    case N_BIN: gen_w_bin(e, lo, hi); return;
    case N_NEG:
        gen_w(e->a, lo, hi);
        e_misc("NEG", lo);
        e_misc("NOT", hi);
        e_ri("SBC", hi, 0xFFFF);          /* hi = ~hi + (lo == 0) */
        return;
    case N_BNOT:
        gen_w(e->a, lo, hi);
        e_misc("NOT", lo);
        e_misc("NOT", hi);
        return;
    case N_OPASSIGN: do_opassign_w(e, lo, hi); return;
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: do_incdec_w(e, lo, hi); return;
    case N_REGVAL: {
        int sl = (int)(e->val & 0xFF), sh = (int)((e->val >> 8) & 0xFF);
        if (sl != lo || sh != hi) {
            int src[2] = {sl, sh}, dst[2] = {lo, hi};
            par_move(src, dst, 2);
        }
        return;
    }
    }
    die("this 32-bit expression is not implemented yet");
}

static void do_assign_w(Node *e, int dlo, int dhi)
{
    Node *l = e->a, *r = e->b;
    if (is_regvar_w(l)) {
        int xl = l->lv->reg, xh = l->lv->reg2;
        if (!mentions(r, l->lv)) {
            gen_w(r, xl, xh);                       /* straight into the variable's registers */
        } else if (r->kind == N_BIN && is_var(r->a, l->lv) && !mentions(r->b, l->lv)) {
            Node rv, b2 = *r;                       /* x = x op b: in place; a is x itself */
            memset(&rv, 0, sizeof rv);
            rv.kind = N_REGVAL;
            rv.val = xl | (xh << 8);
            rv.ty = l->ty;
            b2.a = &rv;
            gen_w_bin(&b2, xl, xh);
        } else {
            int t1 = alloc_temp(), t2 = alloc_temp();
            gen_w(r, t1, t2);
            int src[2] = {t1, t2}, dst[2] = {xl, xh};
            par_move(src, dst, 2);
            free_temp(t2);
            free_temp(t1);
        }
        if (dlo >= 0) {
            int src[2] = {xl, xh}, dst[2] = {dlo, dhi};
            par_move(src, dst, 2);
        }
        return;
    }
    int lo = dlo >= 0 ? dlo : alloc_temp(), hi = dhi >= 0 ? dhi : alloc_temp();
    gen_w(r, lo, hi);
    Opnd xl, xh;
    obj32_opnds(l, &xl, &xh);
    st_w(&xl, &xh, lo, hi);
    free_opnd(&xl);
    if (dhi < 0) free_temp(hi);
    if (dlo < 0) free_temp(lo);
}

static void do_opassign_w(Node *e, int dlo, int dhi)
{
    Node *l = e->a, *bin = e->c;
    Node rv, b2 = *bin;
    memset(&rv, 0, sizeof rv);
    rv.kind = N_REGVAL;
    rv.ty = l->ty;
    if (is_regvar_w(l)) {
        int xl = l->lv->reg, xh = l->lv->reg2;      /* in place in the variable's registers */
        rv.val = xl | (xh << 8);
        b2.a = &rv;
        gen_w_bin(&b2, xl, xh);
        if (dlo >= 0) {
            int src[2] = {xl, xh}, dst[2] = {dlo, dhi};
            par_move(src, dst, 2);
        }
        return;
    }
    int lo = dlo >= 0 ? dlo : alloc_temp(), hi = dhi >= 0 ? dhi : alloc_temp();
    Opnd xl, xh;
    obj32_opnds(l, &xl, &xh);
    e_op("LD", lo, &xl);
    e_op("LD", hi, &xh);
    rv.val = lo | (hi << 8);
    b2.a = &rv;
    gen_w_bin(&b2, lo, hi);
    st_w(&xl, &xh, lo, hi);
    free_opnd(&xl);
    if (dhi < 0) free_temp(hi);
    if (dlo < 0) free_temp(lo);
}

static void do_incdec_w(Node *e, int dlo, int dhi)
{
    Node *l = e->a;
    int post = e->kind == N_POSTINC || e->kind == N_POSTDEC;
    int inc = e->kind == N_PREINC || e->kind == N_POSTINC;
    Opnd one = imm_opnd(1), zero = imm_opnd(0);
    if (is_regvar_w(l)) {
        int xl = l->lv->reg, xh = l->lv->reg2;
        if (post && dlo >= 0) { int src[2] = {xl, xh}, dst[2] = {dlo, dhi}; par_move(src, dst, 2); }
        e_op(inc ? "ADD" : "SUB", xl, &one);
        e_op(inc ? "ADC" : "SBC", xh, &zero);
        if (!post && dlo >= 0) { int src[2] = {xl, xh}, dst[2] = {dlo, dhi}; par_move(src, dst, 2); }
        return;
    }
    int lo = alloc_temp(), hi = alloc_temp();
    Opnd xl, xh;
    obj32_opnds(l, &xl, &xh);
    e_op("LD", lo, &xl);
    e_op("LD", hi, &xh);
    if (post && dlo >= 0) { e_rr("LD", dlo, lo); e_rr("LD", dhi, hi); }
    e_op(inc ? "ADD" : "SUB", lo, &one);
    e_op(inc ? "ADC" : "SBC", hi, &zero);
    st_w(&xl, &xh, lo, hi);
    if (!post && dlo >= 0) { e_rr("LD", dlo, lo); e_rr("LD", dhi, hi); }
    free_opnd(&xl);
    free_temp(hi);
    free_temp(lo);
}

static void gen_cast(Node *e, int d)
{
    Type *from = decay(e->a->ty), *to = e->ty;
    if (is_wide(from)) {
        Node *a = e->a;
        /* casts long <-> ulong only change the type */
        while (a->kind == N_CAST && is_wide(a->ty) && is_wide(decay(a->a->ty))) a = a->a;
        /* the low word of an extended value is the value itself */
        if (a->kind == N_CAST && is_wide(a->ty) && !is_wide(decay(a->a->ty))) {
            Type *s = decay(a->a->ty);
            gen(a->a, d);
            if (is_narrow(to) && s->kind != to->kind) e_ext(to, d, d);
            return;
        }
        Local *dv0 = reg_var[d];
        /* uint(x >> k), 16 <= k < 32: the high word shifted by k - 16 */
        if (a->kind == N_BIN && a->op == TK_SHR && a->b->kind == N_NUM && a->b->val >= 16 && a->b->val < 32 &&
            is_wide(decay(a->a->ty))) {
            i64 k = a->b->val - 16;
            Opnd xl, xh;
            if (is_lval(a->a)) {
                obj32_opnds(a->a, &xl, &xh);
                load_opnd(d, &xh, &t_uint);
                free_opnd(&xl);
            } else {
                int t = alloc_temp();
                if (dv0 && mentions(a->a, dv0)) {
                    int h = alloc_temp();
                    gen_w(a->a, t, h);
                    e_rr("LD", d, h);
                    free_temp(h);
                } else gen_w(a->a, t, d);
                free_temp(t);
            }
            if (k) e_ri(a->sgn ? "SAR" : "SHR", d, k);
            if (is_narrow(to)) e_ext(to, d, d);
            return;
        }
        /* uint(x << k): the low word shifted by k; for k >= 16 - zero */
        if (a->kind == N_BIN && a->op == TK_SHL && a->b->kind == N_NUM && a->b->val >= 1 && a->b->val < 16 &&
            is_wide(decay(a->a->ty)) && is_lval(a->a)) {
            Opnd xl, xh;
            obj32_opnds(a->a, &xl, &xh);
            load_opnd(d, &xl, &t_uint);
            free_opnd(&xl);
            e_ri("SHL", d, a->b->val);
            if (is_narrow(to)) e_ext(to, d, d);
            return;
        }
        /* the low word: directly from memory or from a register pair */
        if (is_lval(a)) {
            Opnd xl, xh;
            obj32_opnds(a, &xl, &xh);
            if (xl.mode == M_REG) {
                if (is_narrow(to)) e_ext(to, d, xl.reg);
                else if (xl.reg != d) e_rr("LD", d, xl.reg);
            } else load_opnd(d, &xl, to);
            free_opnd(&xl);
            return;
        }
        Local *dv = reg_var[d];
        int lo = (dv && mentions(a, dv)) ? alloc_temp() : d, hi = alloc_temp();
        gen_w(a, lo, hi);
        if (is_narrow(to)) e_ext(to, d, lo);
        else if (d != lo) e_rr("LD", d, lo);
        free_temp(hi);
        if (lo != d) free_temp(lo);
        return;
    }
    if (is_narrow(to) && from->kind != to->kind) {
        if (e->a->kind == N_LOCAL && e->a->lv->reg >= 0) { e_ext(to, d, e->a->lv->reg); return; }
        gen(e->a, d);
        e_ext(to, d, d);
        return;
    }
    gen(e->a, d);
}

/* A condition as a 0/1 value. The short form keeps d occupied during the test;
   if registers are short - the form with d free until the end of the test. */
static void gen_bool(Node *e, int d)
{
    Retry rt;
    retry_begin(&rt);
    if (!setjmp(rt.jb)) {
        int L = newlabel();
        e_ri("LD", d, 0);
        gen_cond(e, L, 0);
        e_ri("LD", d, 1);
        e_label(L);
        retry_end(&rt);
    } else {
        retry_restore(&rt);
        int Lf = newlabel(), Le = newlabel();
        int held = busy & (1 << d);
        if (!reg_var[d]) busy &= ~(1 << d);
        gen_cond(e, Lf, 0);
        busy |= held;
        e_ri("LD", d, 1);           /* LD does not change flags */
        e_jmp(CC_JMP, Le);
        e_label(Lf);
        e_ri("LD", d, 0);
        e_label(Le);
    }
}

static void gen(Node *e, int d)
{
    Opnd o;
    errline = e->line;
    if (e->ty->kind == TY_VOID) die("expression has no value");
    if (is_wide(e->ty)) die("internal error: a 32-bit value in a 16-bit context");
    if (try_operand(e, &o, 0)) {
        load_opnd(d, &o, e->ty);
        return;
    }
    switch (e->kind) {
    case N_LOCAL: case N_GLOBAL: case N_MMIO: case N_DEREF: case N_INDEX: case N_FIELD:
        if (e->ty->kind == TY_ARRAY) { gen_addr(e, d); return; }
        get_lval(e, &o, 0);
        load_opnd(d, &o, e->ty);
        free_opnd(&o);
        return;
    case N_ADDR: gen_addr(e->a, d); return;
    case N_BIN: gen_bin(e, d); return;
    case N_NEG: gen(e->a, d); e_misc("NEG", d); return;
    case N_BNOT: gen(e->a, d); e_misc("NOT", d); return;
    case N_CMP: case N_LAND: case N_LOR: case N_LNOT:
        gen_bool(e, d);
        return;
    case N_CALL: gen_call(e, d); return;
    case N_BUILTIN: gen_builtin(e, d); return;
    case N_ASSIGN: do_assign(e, d); return;
    case N_OPASSIGN: do_opassign(e, d); return;
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: do_incdec(e, d); return;
    case N_CAST: gen_cast(e, d); return;
    }
    die("internal error: gen %d", e->kind);
}

static void gen_void(Node *e)
{
    errline = e->line;
    if (is_wide(e->ty)) {
        if (e->kind == N_ASSIGN) { do_assign_w(e, -1, -1); return; }
        if (e->kind == N_CALL) { gen_call(e, -1); return; }
        if (e->kind == N_OPASSIGN) { do_opassign_w(e, -1, -1); return; }
        if (e->kind == N_PREINC || e->kind == N_PREDEC || e->kind == N_POSTINC || e->kind == N_POSTDEC) {
            do_incdec_w(e, -1, -1);
            return;
        }
        int lo = alloc_temp(), hi = alloc_temp();
        gen_w(e, lo, hi);
        free_temp(hi);
        free_temp(lo);
        return;
    }
    switch (e->kind) {
    case N_ASSIGN: do_assign(e, -1); return;
    case N_OPASSIGN: do_opassign(e, -1); return;
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: do_incdec(e, -1); return;
    case N_CALL: gen_call(e, -1); return;
    case N_BUILTIN: gen_builtin(e, -1); return;
    }
    int t = alloc_temp();
    gen(e, t);
    free_temp(t);
}

/* -------------------------- conditions ------------------------------------ */

static int neg_cc(int cc) { return cc == CC_JMP ? cc : ((cc & 1) ? cc + 1 : cc - 1); }

static int cmp_cc(int op, int sgn)
{
    switch (op) {
    case TK_EQ: return CC_Z;
    case TK_NE: return CC_NZ;
    case '<': return sgn ? CC_L : CC_C;
    case '>': return sgn ? CC_G : CC_A;
    case TK_LE: return sgn ? CC_LE : CC_BE;
    case TK_GE: return sgn ? CC_GE : CC_NC;
    }
    return CC_JMP;
}

static int mirror(int op)
{
    switch (op) {
    case '<': return '>';
    case '>': return '<';
    case TK_LE: return TK_GE;
    case TK_GE: return TK_LE;
    }
    return op;
}

/* A value in a register: the variable's register without a copy, otherwise a temporary. */
static int value_reg(Node *e, int *tmp)
{
    *tmp = -1;
    if (e->kind == N_LOCAL && e->lv->reg >= 0) return e->lv->reg;
    if ((e->kind == N_PREINC || e->kind == N_PREDEC || e->kind == N_ASSIGN || e->kind == N_OPASSIGN) &&
        e->a->kind == N_LOCAL && e->a->lv->reg >= 0) {
        gen_void(e);
        return e->a->lv->reg;
    }
    int t = alloc_temp();
    gen(e, t);
    *tmp = t;
    return t;
}

/* Both operands of a comparison need computing: a into *ra, b into *tb. A separate function,
   so that the caller's local variables do not depend on longjmp. */
static void cmp_operands(Node *a, Node *b, int *ra, int *ta, int *tb)
{
    Retry rt;
    retry_begin(&rt);
    if (!setjmp(rt.jb)) {
        if (need(b) > need(a)) {
            *tb = alloc_temp();
            gen(b, *tb);
            *ra = value_reg(a, ta);
        } else {
            *ra = value_reg(a, ta);
            *tb = alloc_temp();
            gen(b, *tb);
        }
        retry_end(&rt);
    } else {
        /* the right operand through the stack */
        retry_restore(&rt);
        *ta = -1;
        int t = alloc_temp();
        gen(b, t);
        e_push(t);
        free_temp(t);
        *ra = value_reg(a, ta);
        *tb = alloc_temp();
        e_pop(*tb);
    }
}

static void gen_cmp_w(Node *e, int L, int jt);

static void gen_cmp(Node *e, int L, int jt)
{
    Node *a = e->a, *b = e->b;
    int op = e->op;
    if (is_wide(a->ty) || is_wide(b->ty)) { gen_cmp_w(e, L, jt); return; }
    if (a->kind == N_NUM && b->kind != N_NUM) {
        Node *t = a; a = b; b = t;
        op = mirror(op);
    }
    /* a register variable on the left, a memory operand on the right: CMP Rv, [g] without a load */
    if (!(a->kind == N_LOCAL && a->lv->reg >= 0) && b->kind == N_LOCAL && b->lv->reg >= 0) {
        Opnd t;
        if (try_operand(a, &t, 1) && t.mode != M_INC && t.mode != M_DEC) {
            Node *x = a; a = b; b = x;
            op = mirror(op);
        }
    }
    if (b->kind == N_NUM && !b->val && (op == TK_EQ || op == TK_NE)) {
        gen_cond(a, L, op == TK_NE ? jt : !jt);
        return;
    }
    Opnd o;
    int ta = -1, tb = -1, ra;
    if (try_operand(b, &o, 1)) {
        ra = value_reg(a, &ta);
    } else {
        cmp_operands(a, b, &ra, &ta, &tb);
        o = reg_opnd(tb);
    }
    e_op("CMP", ra, &o);
    int cc = cmp_cc(op, e->sgn);
    e_jmp(jt ? cc : neg_cc(cc), L);
    if (ta >= 0) free_temp(ta);
    if (tb >= 0) free_temp(tb);
}

/* A condition on a two-word value: true if the value is not zero. */
static void gen_cond_w(Node *e, int L, int jt)
{
    int t = alloc_temp();
    if (is_lval(e)) {
        Opnd xl, xh;
        obj32_opnds(e, &xl, &xh);
        e_op("LD", t, &xl);
        e_op("OR", t, &xh);
        free_opnd(&xl);
    } else {
        int t2 = alloc_temp();
        gen_w(e, t, t2);
        e_rr("OR", t, t2);
        free_temp(t2);
    }
    e_jmp(jt ? CC_NZ : CC_Z, L);
    free_temp(t);
}

/* Comparison of two-word values. == and !=: XOR of the words and OR of the results; order: CMP of the low words
   and SBC of the high ones (the borrow and signed-overflow flags refer to the whole value); for > and <=
   the operands are swapped. The value a is in the pair (t1, t2), b is the operand (bl, bh). */
static void cmp_w_emit(int op, int sgn, int t1, int t2, Opnd *bl, Opnd *bh, int L, int jt)
{
    int cc;
    if (op == TK_EQ || op == TK_NE) {
        if (!is_imm0(bl)) e_op("XOR", t1, bl);
        if (!is_imm0(bh)) e_op("XOR", t2, bh);
        e_rr("OR", t1, t2);
        cc = op == TK_EQ ? CC_Z : CC_NZ;
    } else {
        e_op("CMP", t1, bl);
        e_op("SBC", t2, bh);
        cc = cmp_cc(op, sgn);
    }
    e_jmp(jt ? cc : neg_cc(cc), L);
}

static void gen_cmp_w(Node *e, int L, int jt)
{
    Node *a = e->a, *b = e->b;
    int op = e->op;
    if ((op == TK_EQ || op == TK_NE) && a->kind == N_NUM && b->kind != N_NUM) { Node *x = a; a = b; b = x; }
    if (op == '>' || op == TK_LE) { Node *x = a; a = b; b = x; op = mirror(op); }
    if ((op == TK_EQ || op == TK_NE) && b->kind == N_NUM && b->val == 0) {
        gen_cond_w(a, L, op == TK_NE ? jt : !jt);       /* the value is zero */
        return;
    }
    if ((op == '<' || op == TK_GE) && e->sgn && b->kind == N_NUM && b->val == 0) {
        /* sign: the high word, TEST sets SF and OF = 0 */
        int tr = -1, tr2 = -1, hr;
        Opnd xl, xh;
        if (is_lval(a)) {
            obj32_opnds(a, &xl, &xh);
            if (xh.mode == M_REG) hr = xh.reg;
            else { hr = tr = alloc_temp(); e_op("LD", hr, &xh); }
            free_opnd(&xl);
        } else {
            tr2 = alloc_temp();
            hr = tr = alloc_temp();
            gen_w(a, tr2, tr);
        }
        e_rr("TEST", hr, hr);
        int cc = cmp_cc(op, 1);
        e_jmp(jt ? cc : neg_cc(cc), L);
        if (tr2 >= 0) free_temp(tr2);
        if (tr >= 0) free_temp(tr);
        return;
    }
    if (b_first(a, b)) {
        Retry r0;
        retry_begin(&r0);
        if (!setjmp(r0.jb)) {
            int u1 = alloc_temp(), u2 = alloc_temp();
            gen_w(b, u1, u2);
            int t1 = alloc_temp(), t2 = alloc_temp();
            gen_w(a, t1, t2);
            Opnd r1 = reg_opnd(u1), r2 = reg_opnd(u2);
            cmp_w_emit(op, e->sgn, t1, t2, &r1, &r2, L, jt);
            free_temp(t2);
            free_temp(t1);
            free_temp(u2);
            free_temp(u1);
            retry_end(&r0);
            return;
        }
        retry_restore(&r0);
    }
    int t1, t2, own1 = 1;
    if (op != TK_EQ && op != TK_NE && is_regvar_w(a)) {
        /* CMP does not change the low word: it is taken straight from the variable's register */
        t1 = a->lv->reg;
        own1 = 0;
        t2 = alloc_temp();
        e_rr("LD", t2, a->lv->reg2);
    } else {
        t1 = alloc_temp();
        t2 = alloc_temp();
        gen_w(a, t1, t2);
    }
    Retry rt;
    retry_begin(&rt);
    if (!setjmp(rt.jb)) {
        Opnd bl, bh;
        int u1 = -1, u2 = -1;
        if (!wide_operand(b, &bl, &bh)) {
            u1 = alloc_temp();
            u2 = alloc_temp();
            gen_w(b, u1, u2);
            bl = reg_opnd(u1);
            bh = reg_opnd(u2);
        }
        cmp_w_emit(op, e->sgn, t1, t2, &bl, &bh, L, jt);
        free_opnd(&bl);
        if (u1 >= 0) { free_temp(u2); free_temp(u1); }
        retry_end(&rt);
    } else {
        retry_restore(&rt);
        int held = busy & ((1 << t2) | (own1 ? 1 << t1 : 0));
        e_push(t2);
        e_push(t1);
        busy &= ~held;
        int b1 = alloc_temp(), b2 = alloc_temp();
        gen_w(b, b1, b2);
        int a1 = alloc_temp(), a2 = alloc_temp();
        e_pop(a1);
        e_pop(a2);
        Opnd r1 = reg_opnd(b1), r2 = reg_opnd(b2);
        cmp_w_emit(op, e->sgn, a1, a2, &r1, &r2, L, jt);
        free_temp(a2); free_temp(a1); free_temp(b2); free_temp(b1);
        busy |= held;
    }
    free_temp(t2);
    if (own1) free_temp(t1);
}

static void gen_cond(Node *e, int L, int jt)
{
    errline = e->line;
    if (is_wide(e->ty) && e->kind != N_NUM) { gen_cond_w(e, L, jt); return; }
    switch (e->kind) {
    case N_NUM:
        if ((e->val != 0) == jt) e_jmp(CC_JMP, L);
        return;
    case N_LNOT:
        gen_cond(e->a, L, !jt);
        return;
    case N_LAND:
        if (!jt) {
            gen_cond(e->a, L, 0);
            gen_cond(e->b, L, 0);
        } else {
            int s = newlabel();
            gen_cond(e->a, s, 0);
            gen_cond(e->b, L, 1);
            e_label(s);
        }
        return;
    case N_LOR:
        if (jt) {
            gen_cond(e->a, L, 1);
            gen_cond(e->b, L, 1);
        } else {
            int s = newlabel();
            gen_cond(e->a, s, 1);
            gen_cond(e->b, L, 0);
            e_label(s);
        }
        return;
    case N_CMP:
        gen_cmp(e, L, jt);
        return;
    case N_BIN:
        if (e->op == '&') {
            Opnd o;
            int ta = -1, tb = -1, ra;
            if (try_operand(e->b, &o, 1)) ra = value_reg(e->a, &ta);
            else if (try_operand(e->a, &o, 1)) ra = value_reg(e->b, &ta);
            else {
                cmp_operands(e->a, e->b, &ra, &ta, &tb);
                o = reg_opnd(tb);
            }
            e_op("TEST", ra, &o);
            e_jmp(jt ? CC_NZ : CC_Z, L);
            if (ta >= 0) free_temp(ta);
            if (tb >= 0) free_temp(tb);
            return;
        }
        break;
    }
    int t, r = value_reg(e, &t);
    if (flags_reg != r) e_rr("TEST", r, r);
    e_jmp(jt ? CC_NZ : CC_Z, L);
    if (t >= 0) free_temp(t);
}

/* ------------------------- statements ----------------------------------- */

static void par_move(int *src, int *dst, int n)
{
    int done[4] = {0}, tmps[4], ntmp = 0;
    for (;;) {
        int rem = 0, prog = 0;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            if (src[i] == dst[i]) { done[i] = 1; continue; }
            int blocked = 0;
            for (int j = 0; j < n; j++) if (j != i && !done[j] && src[j] == dst[i]) blocked = 1;
            if (!blocked) { e_rr("LD", dst[i], src[i]); done[i] = 1; prog = 1; }
            else rem++;
        }
        if (!rem) break;
        if (!prog) {
            for (int i = 0; i < n; i++) {
                if (done[i]) continue;
                int t = alloc_temp();
                tmps[ntmp++] = t;
                e_rr("LD", t, dst[i]);
                for (int j = 0; j < n; j++) if (!done[j] && src[j] == dst[i]) src[j] = t;
                break;
            }
        }
    }
    for (int i = 0; i < ntmp; i++) free_temp(tmps[i]);
}

static void gen_return(Node *s)
{
    Sym *f = cf->sym;
    int n = s->n;
    if (!n) return;
    if (n == 1) {
        if (is_wide(f->rets[0])) {
            Node *x = s->list[0];
            Local *v0 = reg_var[0], *v1 = reg_var[1];
            busy |= 3;
            if ((v0 && mentions(x, v0)) || (v1 && mentions(x, v1))) {
                /* the parameters in R0/R1 must not be overwritten until the end of the computation */
                int lo = alloc_temp(), hi = alloc_temp();
                gen_w(x, lo, hi);
                int src[2] = {lo, hi}, dst[2] = {0, 1};
                par_move(src, dst, 2);
                free_temp(hi);
                free_temp(lo);
            } else gen_w(x, 0, 1);
            busy &= ~3;
            return;
        }
        busy |= 1;
        gen_to(s->list[0], 0);
        busy &= ~1;
        if (is_narrow(f->rets[0]) && s->list[0]->ty->kind != f->rets[0]->kind && !fits(f->rets[0], s->list[0]))
            e_ext(f->rets[0], 0, 0);
        return;
    }
    int slot[4], sl = 0;
    for (int i = 0; i < n; i++) { slot[i] = sl; sl += is_wide(f->rets[i]) ? 2 : 1; }
    int mask = (1 << sl) - 1, src[4], dst[4], nm = 0, tmps[8], nt = 0;
    int deferred[4] = {0};
    Opnd dops[4];
    busy |= mask;
    for (int i = 0; i < n; i++) {
        Node *v = s->list[i];
        Opnd o;
        if (is_wide(f->rets[i])) {
            int lo = alloc_temp(), hi = alloc_temp();
            tmps[nt++] = lo;
            tmps[nt++] = hi;
            gen_w(v, lo, hi);
            src[nm] = lo; dst[nm++] = slot[i];
            src[nm] = hi; dst[nm++] = slot[i] + 1;
            continue;
        }
        Node *u = v;
        while (u->kind == N_CAST && is_word(u->ty) && !is_narrow(decay(u->a->ty))) u = u->a;
        if (u->kind == N_LOCAL && u->lv->reg >= 0) {
            src[nm] = u->lv->reg;
            dst[nm++] = slot[i];
        } else if (try_operand(v, &o, 0) && (o.mode == M_IMM || o.mode == M_ABS)) {
            deferred[i] = 1;
            dops[i] = o;
        } else {
            int t = alloc_temp();
            tmps[nt++] = t;
            gen(v, t);
            src[nm] = t;
            dst[nm++] = slot[i];
        }
    }
    par_move(src, dst, nm);
    for (int i = 0; i < n; i++) if (deferred[i]) load_opnd(slot[i], &dops[i], s->list[i]->ty);
    for (int i = 0; i < nt; i++) free_temp(tmps[i]);
    busy &= ~mask;
    for (int i = 0; i < n; i++)
        if (is_narrow(f->rets[i]) && s->list[i]->ty->kind != f->rets[i]->kind && !fits(f->rets[i], s->list[i]))
            e_ext(f->rets[i], slot[i], slot[i]);
}

static void store_reg(Node *l, int r)
{
    if (l->kind == N_LOCAL && l->lv->reg >= 0) {
        if (is_narrow(l->ty)) e_ext(l->ty, l->lv->reg, r);
        else if (l->lv->reg != r) e_rr("LD", l->lv->reg, r);
        return;
    }
    Opnd o;
    get_lval(l, &o, 0);
    e_st(is_narrow(l->ty), &o, r);
    free_opnd(&o);
}

/* The result in registers lo (and hi) into a two-word target; hi < 0 - the result is one word of type src. */
static void store_wide_reg(Node *l, int lo, int hi, Type *src)
{
    int t = -1;
    if (hi < 0) {
        t = hi = alloc_temp();
        if (is_signed(promote(src))) { e_rr("LD", t, lo); e_ri("SAR", t, 15); }
        else e_ri("LD", t, 0);
    }
    Opnd xl, xh;
    obj32_opnds(l, &xl, &xh);
    st_w(&xl, &xh, lo, hi);
    free_opnd(&xl);
    if (t >= 0) free_temp(t);
}

static void unwind_atomic(int to)
{
    int save = sp_adj;
    for (int k = atomic_depth; k > to; k--) e_pop(R_FLAGS);
    sp_adj = save;
}

static void gen_stmt(Node *s)
{
    errline = s->line;
    if (s->kind != N_BLOCK) src_line = s->line;
    if (busy) die("internal error: registers busy between statements");
    switch (s->kind) {
    case N_BLOCK:
        for (int i = 0; i < s->n; i++) gen_stmt(s->list[i]);
        return;
    case N_EXPR:
        gen_void(s->a);
        return;
    case N_IF: {
        int Lelse = newlabel();
        gen_cond(s->a, Lelse, 0);
        gen_stmt(s->b);
        if (s->c) {
            int Lend = newlabel();
            e_jmp(CC_JMP, Lend);
            e_label(Lelse);
            gen_stmt(s->c);
            e_label(Lend);
        } else {
            e_label(Lelse);
        }
        return;
    }
    case N_WHILE: case N_LOOP: {
        int inf = s->kind == N_LOOP || s->a->kind == N_NUM;
        if (s->kind == N_WHILE && s->a->kind == N_NUM && !s->a->val) return;
        int Ltop = newlabel(), Lcont = newlabel(), Lend = newlabel();
        if (!inf) gen_cond(s->a, Lend, 0);
        e_label(Ltop);
        LoopCtx *lc = &loops[nloops++];
        if (nloops >= 64) die("loops nested too deeply");
        lc->brk = Lend;
        lc->cont = inf ? Ltop : Lcont;
        lc->atomic = atomic_depth;
        lc->cont_used = 0;
        gen_stmt(s->b);
        int cu = loops[--nloops].cont_used;
        if (inf) {
            e_jmp(CC_JMP, Ltop);
        } else {
            if (cu) e_label(Lcont);
            src_line = s->line;
            gen_cond(s->a, Ltop, 1);
        }
        e_label(Lend);
        return;
    }
    case N_BREAK: case N_CONTINUE: {
        LoopCtx *lc = &loops[nloops - 1];
        unwind_atomic(lc->atomic);
        if (s->kind == N_CONTINUE) lc->cont_used = 1;
        e_jmp(CC_JMP, s->kind == N_BREAK ? lc->brk : lc->cont);
        return;
    }
    case N_RETURN:
        gen_return(s);
        unwind_atomic(0);
        e_jmp(CC_JMP, ret_label);
        return;
    case N_MASSIGN: {
        int k = s->n, slot[4], sl = 0;
        Type **rets = s->b->fty->rets;
        for (int i = 0; i < k; i++) { slot[i] = sl; sl += is_wide(rets[i]) ? 2 : 1; }
        gen_call(s->b, -2);
        busy |= (1 << sl) - 1;
        for (int i = 0; i < k; i++) {
            Node *l = s->list[i];
            int w = is_wide(rets[i]) ? 2 : 1;
            if (l->kind != N_DISCARD) {
                if (is_wide(l->ty)) store_wide_reg(l, slot[i], w == 2 ? slot[i] + 1 : -1, rets[i]);
                else store_reg(l, slot[i]);
            }
            busy &= ~(((1 << w) - 1) << slot[i]);
        }
        busy = 0;
        return;
    }
    case N_LABEL:
        e_label(cf->labels[s->val].lab);
        return;
    case N_LCOPY: {
        /* a local object from a template: word-by-word copy */
        int rs = alloc_temp(), rd = alloc_temp(), x = alloc_temp(), L = newlabel();
        Opnd o;
        get_lval(s->a, &o, 0);
        addr_from_opnd(&o, rd);
        const char *tn = emitting ? s->sym->asmname : "__tmpl";
        e_text(fmt("LD    %s, #%s", rn(rs), tn), 2);
        e_label(L);
        e_text(fmt("LD    %s, [%s++]", rn(x), rn(rs)), 1);
        e_text(fmt("ST    [%s++], %s", rn(rd), rn(x)), 1);
        e_text(fmt("CMP   %s, #%s+%lld", rn(rs), tn, s->val), 2);
        flags_reg = -1;
        e_jmp(CC_NZ, L);
        free_temp(x); free_temp(rd); free_temp(rs);
        if (emitting) s->sym->tmpl_used = 1;
        return;
    }
    case N_GOTO: {
        int save = sp_adj;
        for (int k = 0; k < s->slen; k++) e_pop(R_FLAGS);
        sp_adj = save;
        e_jmp(CC_JMP, cf->labels[s->val].lab);
        return;
    }
    case N_ATOMIC:
        e_push(R_FLAGS);
        e_text("CLI", 1);
        atomic_depth++;
        gen_stmt(s->b);
        atomic_depth--;
        e_pop(R_FLAGS);
        return;
    case N_ASM: {
        const char *p = s->str;
        while (*p) {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            const char *a = p, *b = e;
            while (a < b && isspace((unsigned char)*a)) a++;
            while (b > a && isspace((unsigned char)b[-1])) b--;
            const char *sc = a;   /* the ; comment is dropped */
            while (sc < b && *sc != ';') sc++;
            b = sc;
            while (b > a && isspace((unsigned char)b[-1])) b--;
            if (b > a) {
                /* a label at the start of a line: only a local @name, otherwise the function's
                   local labels @L<n> would break */
                const char *q = a;
                if (*q == '@') q++;
                if (isalpha((unsigned char)*q) || *q == '_') {
                    while (q < b && (isalnum((unsigned char)*q) || *q == '_')) q++;
                    const char *r = q;
                    while (r < b && isspace((unsigned char)*r)) r++;
                    if (r < b && *r == ':') {
                        if (*a != '@') die("only local labels @name are allowed in an asm block");
                        if (q - a > 2 && a[1] == 'L' && isdigit((unsigned char)a[2]))
                            die("labels @L<number> are reserved by the compiler");
                        e_named_label(xstrndup(a, q - a));
                        a = r + 1;
                        while (a < b && isspace((unsigned char)*a)) a++;
                    }
                }
                if (b > a) e_text(xstrndup(a, b - a), 2);
            }
            p = *e ? e + 1 : e;
        }
        flags_reg = -1;
        return;
    }
    }
    die("internal error: statement %d", s->kind);
}

/* --------------------- optimization and function output -------------------- */

static int next_live(int i)
{
    for (int j = i + 1; j < ncode; j++) if (!code[j].dead) return j;
    return -1;
}

/* ----------------- source text in comments (-s) ------------------ */

static const char **src_lines;   /* starts of lines of the combined text */
static int nsrc_lines;
static int shown_line;           /* the last line shown */
static int max_shown;            /* the farthest line shown */
static const char *shown_file;

static void index_source(void)
{
    for (const char *p = src; *p;) {
        src_lines = (const char **)xrealloc(src_lines, sizeof(char *) * (nsrc_lines + 1));
        src_lines[nsrc_lines++] = p;
        const char *e = strchr(p, '\n');
        p = e ? e + 1 : p + strlen(p);
    }
}

static void put_src_line(int line)
{
    const char *p = src_lines[line - 1], *e = p;
    while (*e && *e != '\n') e++;
    while (e > p && isspace((unsigned char)e[-1])) e--;
    if (e == p) return;                     /* blank lines are not shown */
    const char *file;
    int fl;
    where(line, &file, &fl);
    fprintf(out, "\t; %4d  %.*s\n", fl, (int)(e - p), p);
}

/* Show line line, and if it comes right after the already shown ones (up to 8 lines
   ahead in the same file) - the lines in between as well: declarations, comments. */
static void print_source(int line)
{
    if (line <= 0 || line > nsrc_lines || line == shown_line) return;
    const char *file;
    int fl;
    where(line, &file, &fl);
    if (file != shown_file) {
        fprintf(out, "\t; ---- %s\n", file);
        shown_file = file;
        shown_line = max_shown = 0;
    }
    int from = line;
    if (max_shown > 0 && line > max_shown && line - max_shown <= 8) {
        from = max_shown + 1;
        for (int l = from; l < line; l++) {
            const char *f2;
            int x;
            where(l, &f2, &x);
            if (f2 != file) { from = line; break; }
        }
    }
    for (int l = from; l <= line; l++) put_src_line(l);
    shown_line = line;
    if (line > max_shown) max_shown = line;
}

/* ------------------- line locations for the assembler (.line) ----------------- */
/* Before the code of each B16 line, .line "file", N (when the file changes) or .line N is emitted;
   before code without a B16 line (header, startup code, runtime) - .line without arguments. */
static const char *loc_file;
static int loc_line, loc_on;

static void emit_noloc(void)
{
    if (loc_on) { fprintf(out, "\t.line\n"); loc_on = 0; }
}

static void emit_loc(int cline)
{
    if (cline <= 0 || cline > nlinemap) { emit_noloc(); return; }
    const char *file;
    int fl;
    where(cline, &file, &fl);
    if (loc_on && file == loc_file) {
        if (fl != loc_line) fprintf(out, "\t.line %d\n", fl);
    } else {
        fprintf(out, "\t.line \"%s\", %d\n", file, fl);
    }
    loc_file = file;
    loc_line = fl;
    loc_on = 1;
}

/* Dead code removal and function output; with emitting = 0 only analysis. */
static void finish_code(void)
{
    int *refs = (int*)xcalloc(sizeof(int) * (nlabels + 1));
    int changed = 1;
    while (changed) {
        changed = 0;
        memset(refs, 0, sizeof(int) * (nlabels + 1));
        for (int i = 0; i < ncode; i++)
            if (!code[i].dead && code[i].kind == I_JMP) refs[code[i].label]++;
        for (int i = 0; i < ncode; i++)
            if (!code[i].dead && code[i].kind == I_LABEL && code[i].label >= 0 && !refs[code[i].label]) {
                code[i].dead = 1;
                changed = 1;
            }
        /* unreachable code after an unconditional jump or return */
        for (int i = 0; i < ncode; i++) {
            Ins *c = &code[i];
            if (c->dead || !((c->kind == I_JMP && c->cc == CC_JMP) || c->term)) continue;
            for (int j = i + 1; j < ncode && code[j].kind != I_LABEL; j++)
                if (!code[j].dead) { code[j].dead = 1; changed = 1; }
        }
        /* a jump to the next instruction */
        for (int i = 0; i < ncode; i++) {
            if (code[i].dead || code[i].kind != I_JMP) continue;
            for (int j = i + 1; j < ncode; j++) {
                if (code[j].dead) continue;
                if (code[j].kind != I_LABEL) break;
                if (code[j].label == code[i].label) { code[i].dead = 1; changed = 1; break; }
            }
        }
        /* Jcc L1; JMP L2; L1:  ->  J!cc L2; L1: */
        for (int i = 0; i < ncode; i++) {
            if (code[i].dead || code[i].kind != I_JMP || code[i].cc == CC_JMP) continue;
            int j = next_live(i);
            if (j < 0 || code[j].kind != I_JMP || code[j].cc != CC_JMP) continue;
            for (int k = j + 1; k < ncode; k++) {
                if (code[k].dead) continue;
                if (code[k].kind != I_LABEL) break;
                if (code[k].label == code[i].label) {
                    code[i].cc = neg_cc(code[i].cc);
                    code[i].label = code[j].label;
                    code[j].dead = 1;
                    changed = 1;
                    break;
                }
            }
        }
    }
    for (int i = 0; emitting && i < ncode; i++) {
        Ins *c = &code[i];
        if (c->dead) continue;
        if (opt_source && (c->kind != I_LABEL || c->label < 0)) print_source(c->sline);
        if (c->kind != I_LABEL || c->label < 0) emit_loc(c->sline);
        switch (c->kind) {
        case I_LABEL:
            if (c->label >= 0) fprintf(out, "@L%d:\n", c->label);
            else fprintf(out, "%s:\n", c->text);
            break;
        case I_INS:
            fprintf(out, "\t%s\n", c->text);
            break;
        case I_JMP:
            fprintf(out, "\t%-5s @L%d\n", j_name[c->cc], c->label);
            break;
        }
    }
    free(refs);
}

static int saved_mask(int used)
{
    if (cf->sym->is_int) return (used & 0x0FFF) | (cf->has_call ? 0x000F : 0);
    return used & 0x0FF0;
}

/* Whether the end of a function body (a fall-through past the last instruction of the body) is reachable from its start.
   return is a jump to the exit label outside the body, it does not reach the end. */
static int end_reachable(Ins *b, int n)
{
    if (!n) return 1;
    int maxl = 0;
    for (int i = 0; i < n; i++) if (b[i].label > maxl) maxl = b[i].label;
    int *at = (int*)xcalloc(sizeof(int) * (maxl + 2));
    for (int i = 0; i <= maxl; i++) at[i] = -1;
    for (int i = 0; i < n; i++) if (b[i].kind == I_LABEL && b[i].label >= 0) at[b[i].label] = i;
    char *seen = (char*)xcalloc(n + 1);
    int *stk = (int*)xcalloc(sizeof(int) * (2 * n + 2)), sp = 0, end = 0;
    stk[sp++] = 0;
    while (sp) {
        int i = stk[--sp];
        if (i >= n) { end = 1; continue; }
        if (seen[i]) continue;
        seen[i] = 1;
        Ins *c = &b[i];
        if (c->kind == I_JMP) {
            int t = c->label <= maxl ? at[c->label] : -1;
            if (t >= 0) stk[sp++] = t;
            if (c->cc == CC_JMP) continue;
        } else if (c->term) continue;
        stk[sp++] = i + 1;
    }
    free(at); free(seen); free(stk);
    return end;
}

static int var_weight(Local *v) { return v->uses * (is_wide(v->ty) ? 2 : 1); }

/* Whether a statement or expression clobbers R0-R3 directly (not through temporary registers): a function or
   runtime call, an asm block, a multiple assignment, a label and goto. */
static int node_clobbers(Node *e)
{
    if (!e) return 0;
    switch (e->kind) {
    case N_CALL: case N_ASM: case N_MASSIGN: case N_LABEL: case N_GOTO: return 1;
    case N_BIN:
        if (is_wide(e->ty)) {
            if (e->op == '*' || e->op == '/' || e->op == '%') {
                if (!wide_inline_muldiv(e->op, e->sgn, e->a, e->b)) return 1;
            } else if ((e->op == TK_SHL || e->op == TK_SHR) && e->b->kind != N_NUM) return 1;
        } else if ((e->op == '/' || e->op == '%') && e->sgn && !const_divisor(e->b)) return 1;
        break;
    }
    if (node_clobbers(e->a) || node_clobbers(e->b) || node_clobbers(e->c)) return 1;
    for (int i = 0; i < e->n; i++) if (node_clobbers(e->list[i])) return 1;
    return 0;
}

static void gen_func(Func *f)
{
    cf = f;
    str_owner = f->sym;
    Sym *s = f->sym;
    int leaf = !f->has_call;
    memset(reg_var, 0, sizeof reg_var);
    Local **cand = (Local**)xcalloc(sizeof(Local *) * (f->nl + 1));
    int nc = 0;
    cur_frame = 0;
    for (int i = 0; i < f->nl; i++) {
        Local *v = f->locals[i];
        v->reg = -1;
        v->reg2 = -1;
        v->on_stack = 0;
        if (v->pidx >= 0 && v->preg < 0) { v->on_stack = 2; continue; }
        if (v->addr_taken || !is_val(v->ty)) { v->on_stack = 1; continue; }
        if (!v->uses) continue;
        cand[nc++] = v;
    }
    for (int i = 1; i < nc; i++)
        for (int j = i; j > 0 && var_weight(cand[j]) > var_weight(cand[j - 1]); j--) {
            Local *t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t;
        }
    /* In a function with calls, a parameter that is read only before the first statement clobbering R0-R3
       stays in its argument register: a copy into R4+ and saving that register in the prologue are not needed. */
    Local *early[4];
    int nearly = 0, barrier = 0;
    if (!leaf && !opt_copy_params && f->nlabels == 0 && f->body && f->body->kind == N_BLOCK) {
        Node *body = f->body;
        barrier = body->n;
        for (int i = 0; i < body->n; i++) if (node_clobbers(body->list[i])) { barrier = i; break; }
        int words = 0, keep_temps = f->has_wide ? 3 : 2;     /* how many of R0-R3 must stay temporary */
        for (int i = 0; i < nc && barrier < body->n; i++) {
            Local *v = cand[i];
            int w = is_wide(v->ty) ? 2 : 1;
            if (v->pidx < 0 || v->preg < 0 || words + w > 4 - keep_temps) continue;
            int late = 0;
            for (int k = barrier; k < body->n && !late; k++) late = mentions(body->list[k], v);
            if (late) continue;
            v->reg = v->preg;
            reg_var[v->reg] = v;
            if (w == 2) { v->reg2 = v->preg + 1; reg_var[v->reg2] = v; }
            early[nearly++] = v;
            words += w;
        }
    }
    int nregs = 0;
    if (leaf) {
        for (int i = 0; i < nc; i++)
            if (cand[i]->pidx >= 0) {
                cand[i]->reg = cand[i]->preg;
                reg_var[cand[i]->reg] = cand[i];
                nregs++;
                if (is_wide(cand[i]->ty)) {
                    cand[i]->reg2 = cand[i]->preg + 1;
                    reg_var[cand[i]->reg2] = cand[i];
                    nregs++;
                }
            }
        for (int i = 0; i < nc; i++) {
            if (cand[i]->reg >= 0) continue;
            int need = is_wide(cand[i]->ty) ? 2 : 1, r = -1, r2 = -1;
            if (nregs + need <= (f->has_wide ? 6 : 9)) {
                for (int k = 0; k < 12; k++) if (!reg_var[k]) { r = k; break; }
                if (need == 2 && r >= 0)
                    for (int k = r + 1; k < 12; k++) if (!reg_var[k]) { r2 = k; break; }
                if (need == 2 && r2 < 0) r = -1;
            }
            if (r < 0) cand[i]->on_stack = 1;
            else {
                cand[i]->reg = r;
                reg_var[r] = cand[i];
                nregs++;
                if (need == 2) { cand[i]->reg2 = r2; reg_var[r2] = cand[i]; nregs++; }
            }
        }
    } else {
        for (int i = 0; i < nc; i++) {
            if (cand[i]->reg >= 0) continue;               /* the early parameter is already in R0-R3 */
            int need = is_wide(cand[i]->ty) ? 2 : 1, r = -1, r2 = -1;
            int lim = f->has_wide ? 10 : 12;      /* R10, R11 are temporaries if there are computations on pairs */
            for (int k = 4; k < lim; k++) if (!reg_var[k]) { r = k; break; }
            if (need == 2 && r >= 0)
                for (int k = r + 1; k < lim; k++) if (!reg_var[k]) { r2 = k; break; }
            if (need == 2 && r2 < 0) r = -1;
            if (r < 0) cand[i]->on_stack = 1;
            else {
                cand[i]->reg = r;
                reg_var[r] = cand[i];
                if (need == 2) { cand[i]->reg2 = r2; reg_var[r2] = cand[i]; }
            }
        }
    }
    free(cand);
    for (int i = 0; i < f->nl; i++) {
        Local *v = f->locals[i];
        if (v->on_stack == 1) {
            v->off = cur_frame;
            cur_frame += (type_size(v->ty) + 1) & ~1;
        }
    }
    int var_mask = 0;
    for (int r = 0; r < 12; r++) if (reg_var[r]) var_mask |= 1 << r;
    int start_label = nlabels, assumed = saved_mask(var_mask);
    for (int pass = 0;; pass++) {
        cur_nsaved = popcount(assumed) + f->has_call;
        nlabels = start_label;
        ncode = 0;
        busy = 0;
        sp_adj = 0;
        flags_reg = -1;
        nloops = 0;
        atomic_depth = 0;
        used_mask = var_mask;
        ret_label = newlabel();
        for (int i = 0; i < f->nlabels; i++) f->labels[i].lab = newlabel();
        if (nearly) {
            for (int k = 0; k < nearly; k++) {
                reg_var[early[k]->reg] = early[k];
                if (early[k]->reg2 >= 0) reg_var[early[k]->reg2] = early[k];
            }
            for (int i = 0; i < f->body->n; i++) {
                if (i == barrier)                               /* parameters are not read further: R0-R3 are free */
                    for (int k = 0; k < nearly; k++) {
                        reg_var[early[k]->reg] = NULL;
                        if (early[k]->reg2 >= 0) reg_var[early[k]->reg2] = NULL;
                    }
                gen_stmt(f->body->list[i]);
            }
            for (int k = 0; k < nearly; k++) {
                reg_var[early[k]->reg] = NULL;
                if (early[k]->reg2 >= 0) reg_var[early[k]->reg2] = NULL;
            }
        } else gen_stmt(f->body);
        int sm = saved_mask(used_mask);
        if (sm == assumed || pass >= 4) break;
        assumed = sm;
    }
    Ins *body = code;
    int nbody = ncode;
    code = NULL;
    ncode = capcode = 0;
    if (s->nret && end_reachable(body, nbody)) {
        errline = f->end_line;
        die("function %s can reach the end without return", s->name);
    }
    if (emitting) fprintf(out, "\n; fn %s%s\n", s->name, leaf ? " (leaf)" : "");
    src_line = f->line;
    e_named_label(s->asmname);
    if (f->has_call) e_push(R_LR);
    for (int r = 0; r < 12; r++) if (assumed >> r & 1) e_push(r);
    if (cur_frame) e_ri("SUB", R_SP, cur_frame);
    sp_adj = 0;
    for (int i = 0; i < f->nl; i++) {
        Local *v = f->locals[i];
        if (v->pidx < 0 || v->preg < 0) continue;
        if (is_wide(v->ty)) {
            if (v->reg >= 0) {
                if (v->reg != v->preg || v->reg2 != v->preg + 1) {
                    int src[2] = {v->preg, v->preg + 1}, dst[2] = {v->reg, v->reg2};
                    par_move(src, dst, 2);
                }
            } else if (v->on_stack == 1) {
                Opnd o = {M_IDX, R_SP, NULL, v->off, -1}, o2 = {M_IDX, R_SP, NULL, v->off + 2, -1};
                e_st(0, &o, v->preg);
                e_st(0, &o2, v->preg + 1);
            }
        } else if (v->reg >= 0 && v->reg != v->preg) e_rr("LD", v->reg, v->preg);
        else if (v->on_stack == 1) {
            Opnd o = {M_IDX, R_SP, NULL, v->off, -1};
            e_st(is_narrow(v->ty), &o, v->preg);
        }
    }
    for (int i = 0; i < nbody; i++) *add_ins(body[i].kind) = body[i];
    free(body);
    src_line = f->end_line;
    e_label(ret_label);
    if (cur_frame) e_ri("ADD", R_SP, cur_frame);
    for (int r = 11; r >= 0; r--) if (assumed >> r & 1) e_pop(r);
    if (s->is_int) {
        if (f->has_call) e_pop(R_LR);
        e_term("IRET");
    } else if (f->has_call) {
        e_term("POP   IP");
    } else {
        e_term("RET");
    }
    finish_code();
    ncode = 0;
}

/* ============================ Data and runtime ============================ */

static const char *rt_divs_text =
    "\n; runtime: signed division R0 = R0 / R1\n"
    "__divs:\n"
    "\tLD    R2, R0\n"
    "\tXOR   R2, R1\n"
    "\tTEST  R0, R0\n"
    "\tJNS   @s1\n"
    "\tNEG   R0\n"
    "@s1:\n"
    "\tTEST  R1, R1\n"
    "\tJNS   @s2\n"
    "\tNEG   R1\n"
    "@s2:\n"
    "\tDIVU  R0, R1\n"
    "\tTEST  R2, R2\n"
    "\tJNS   @s3\n"
    "\tNEG   R0\n"
    "@s3:\n"
    "\tRET\n";

static const char *rt_mods_text =
    "\n; runtime: signed remainder R0 = R0 % R1 (the sign of the dividend)\n"
    "__mods:\n"
    "\tLD    R2, R0\n"
    "\tTEST  R0, R0\n"
    "\tJNS   @s1\n"
    "\tNEG   R0\n"
    "@s1:\n"
    "\tTEST  R1, R1\n"
    "\tJNS   @s2\n"
    "\tNEG   R1\n"
    "@s2:\n"
    "\tREMU  R0, R1\n"
    "\tTEST  R2, R2\n"
    "\tJNS   @s3\n"
    "\tNEG   R0\n"
    "@s3:\n"
    "\tRET\n";

static const char *rt_mul32_text =
    "\n; runtime: R0:R1 = R0:R1 * R2:R3 (the low 32 bits)\n"
    "__mul32:\n"
    "\tMUL   R1, R2\n"
    "\tMUL   R3, R0\n"
    "\tADD   R1, R3\n"
    "\tLD    R3, R0\n"
    "\tMULHU R3, R2\n"
    "\tADD   R1, R3\n"
    "\tMUL   R0, R2\n"
    "\tRET\n";

/* The quotient R0:R1 and remainder R2:R3 of the unsigned dividend R0:R1 divided by R2:R3. Divisor 0: quotient $FFFFFFFF, remainder = dividend.
   Before each step the remainder is not greater than a prefix of the dividend of i < 32 bits, that is, below 2^31: the shift does not overflow. */
static const char *rt_divu32_text =
    "\n; runtime: unsigned division R0:R1 / R2:R3: the quotient in R0:R1, the remainder in R2:R3\n"
    "__divu32:\n"
    "\tTEST  R1, R1\n"
    "\tJNZ   @slow\n"
    "\tTEST  R3, R3\n"
    "\tJNZ   @slow\n"
    "\tTEST  R2, R2\n"
    "\tJZ    @slow\n"
    "\tLD    R3, R0\n"
    "\tDIVU  R0, R2\n"
    "\tREMU  R3, R2\n"
    "\tLD    R2, R3\n"
    "\tLD    R3, #0\n"
    "\tRET\n"
    "@slow:\n"
    "\tPUSH  R4\n"
    "\tPUSH  R5\n"
    "\tPUSH  R6\n"
    "\tLD    R4, #0\n"
    "\tLD    R5, #0\n"
    "\tLD    R6, #32\n"
    "@loop:\n"
    "\tADD   R0, R0\n"
    "\tADC   R1, R1\n"
    "\tADC   R4, R4\n"
    "\tADC   R5, R5\n"
    "\tSUB   R4, R2\n"
    "\tSBC   R5, R3\n"
    "\tJNC   @set\n"
    "\tADD   R4, R2\n"
    "\tADC   R5, R3\n"
    "\tJMP   @next\n"
    "@set:\n"
    "\tOR    R0, #1\n"
    "@next:\n"
    "\tSUB   R6, #1\n"
    "\tJNZ   @loop\n"
    "\tLD    R2, R4\n"
    "\tLD    R3, R5\n"
    "\tPOP   R6\n"
    "\tPOP   R5\n"
    "\tPOP   R4\n"
    "\tRET\n";

static const char *rt_modu32_text =
    "\n; runtime: unsigned remainder R0:R1 % R2:R3\n"
    "__modu32:\n"
    "\tPUSH  LR\n"
    "\tCALL  __divu32\n"
    "\tPOP   LR\n"
    "\tLD    R0, R2\n"
    "\tLD    R1, R3\n"
    "\tRET\n";

static const char *rt_divs32_text =
    "\n; runtime: signed division R0:R1 / R2:R3 (the quotient is rounded toward zero)\n"
    "__divs32:\n"
    "\tPUSH  LR\n"
    "\tPUSH  R4\n"
    "\tLD    R4, R1\n"
    "\tXOR   R4, R3\n"
    "\tTEST  R1, R1\n"
    "\tJNS   @a\n"
    "\tNEG   R0\n"
    "\tNOT   R1\n"
    "\tSBC   R1, #$FFFF\n"
    "@a:\n"
    "\tTEST  R3, R3\n"
    "\tJNS   @b\n"
    "\tNEG   R2\n"
    "\tNOT   R3\n"
    "\tSBC   R3, #$FFFF\n"
    "@b:\n"
    "\tCALL  __divu32\n"
    "\tTEST  R4, R4\n"
    "\tJNS   @c\n"
    "\tNEG   R0\n"
    "\tNOT   R1\n"
    "\tSBC   R1, #$FFFF\n"
    "@c:\n"
    "\tPOP   R4\n"
    "\tPOP   LR\n"
    "\tRET\n";

static const char *rt_mods32_text =
    "\n; runtime: signed remainder R0:R1 % R2:R3 (the sign of the dividend)\n"
    "__mods32:\n"
    "\tPUSH  LR\n"
    "\tPUSH  R4\n"
    "\tLD    R4, R1\n"
    "\tTEST  R1, R1\n"
    "\tJNS   @a\n"
    "\tNEG   R0\n"
    "\tNOT   R1\n"
    "\tSBC   R1, #$FFFF\n"
    "@a:\n"
    "\tTEST  R3, R3\n"
    "\tJNS   @b\n"
    "\tNEG   R2\n"
    "\tNOT   R3\n"
    "\tSBC   R3, #$FFFF\n"
    "@b:\n"
    "\tCALL  __divu32\n"
    "\tLD    R0, R2\n"
    "\tLD    R1, R3\n"
    "\tTEST  R4, R4\n"
    "\tJNS   @c\n"
    "\tNEG   R0\n"
    "\tNOT   R1\n"
    "\tSBC   R1, #$FFFF\n"
    "@c:\n"
    "\tPOP   R4\n"
    "\tPOP   LR\n"
    "\tRET\n";

static const char *rt_shl32_text =
    "\n; runtime: R0:R1 << (R2 mod 32)\n"
    "__shl32:\n"
    "\tAND   R2, #31\n"
    "\tJZ    @done\n"
    "\tCMP   R2, #16\n"
    "\tJC    @small\n"
    "\tLD    R1, R0\n"
    "\tLD    R0, #0\n"
    "\tSUB   R2, #16\n"
    "\tJZ    @done\n"
    "\tSHL   R1, R2\n"
    "\tRET\n"
    "@small:\n"
    "\tLD    R3, R0\n"
    "\tSHL   R0, R2\n"
    "\tSHR   R3, #1\n"
    "\tXOR   R2, #15\n"
    "\tSHR   R3, R2\n"
    "\tXOR   R2, #15\n"
    "\tSHL   R1, R2\n"
    "\tOR    R1, R3\n"
    "@done:\n"
    "\tRET\n";

static const char *rt_shr32_text =
    "\n; runtime: R0:R1 >> (R2 mod 32), logical\n"
    "__shr32:\n"
    "\tAND   R2, #31\n"
    "\tJZ    @done\n"
    "\tCMP   R2, #16\n"
    "\tJC    @small\n"
    "\tLD    R0, R1\n"
    "\tLD    R1, #0\n"
    "\tSUB   R2, #16\n"
    "\tJZ    @done\n"
    "\tSHR   R0, R2\n"
    "\tRET\n"
    "@small:\n"
    "\tLD    R3, R1\n"
    "\tSHR   R1, R2\n"
    "\tSHL   R3, #1\n"
    "\tXOR   R2, #15\n"
    "\tSHL   R3, R2\n"
    "\tXOR   R2, #15\n"
    "\tSHR   R0, R2\n"
    "\tOR    R0, R3\n"
    "@done:\n"
    "\tRET\n";

static const char *rt_sar32_text =
    "\n; runtime: R0:R1 >> (R2 mod 32), arithmetic\n"
    "__sar32:\n"
    "\tAND   R2, #31\n"
    "\tJZ    @done\n"
    "\tCMP   R2, #16\n"
    "\tJC    @small\n"
    "\tLD    R0, R1\n"
    "\tSAR   R1, #15\n"
    "\tSUB   R2, #16\n"
    "\tJZ    @done\n"
    "\tSAR   R0, R2\n"
    "\tRET\n"
    "@small:\n"
    "\tLD    R3, R1\n"
    "\tSAR   R1, R2\n"
    "\tSHL   R3, #1\n"
    "\tXOR   R2, #15\n"
    "\tSHL   R3, R2\n"
    "\tXOR   R2, #15\n"
    "\tSHR   R0, R2\n"
    "\tOR    R0, R3\n"
    "@done:\n"
    "\tRET\n";

static const char *rt_udivc32_text =
    "\n; runtime: R0:R1 / R2 unsigned, R2 = 3..255: the quotient in R0:R1, the remainder in R2 (four DIVU/REMU steps over the bytes)\n"
    "__udivc32:\n"
    "\tPUSH  R4\n"
    "\tPUSH  R5\n"
    "\tPUSH  R6\n"
    "\tLD    R4, R1\n"
    "\tSHR   R4, #8\n"
    "\tLD    R5, R4\n"
    "\tDIVU  R5, R2\n"
    "\tREMU  R4, R2\n"
    "\tSHL   R4, #8\n"
    "\tLD    R3, R1\n"
    "\tAND   R3, #255\n"
    "\tOR    R4, R3\n"
    "\tLD    R3, R4\n"
    "\tDIVU  R3, R2\n"
    "\tREMU  R4, R2\n"
    "\tSHL   R5, #8\n"
    "\tOR    R5, R3\n"
    "\tSHL   R4, #8\n"
    "\tLD    R3, R0\n"
    "\tSHR   R3, #8\n"
    "\tOR    R4, R3\n"
    "\tLD    R6, R4\n"
    "\tDIVU  R6, R2\n"
    "\tREMU  R4, R2\n"
    "\tSHL   R4, #8\n"
    "\tLD    R3, R0\n"
    "\tAND   R3, #255\n"
    "\tOR    R4, R3\n"
    "\tLD    R3, R4\n"
    "\tDIVU  R3, R2\n"
    "\tREMU  R4, R2\n"
    "\tSHL   R6, #8\n"
    "\tOR    R6, R3\n"
    "\tLD    R0, R6\n"
    "\tLD    R1, R5\n"
    "\tLD    R2, R4\n"
    "\tPOP   R6\n"
    "\tPOP   R5\n"
    "\tPOP   R4\n"
    "\tRET\n";

static const char *rt_umodc32_text =
    "\n; runtime: R0:R1 % R2 unsigned, R2 = 3..255\n"
    "__umodc32:\n"
    "\tPUSH  LR\n"
    "\tCALL  __udivc32\n"
    "\tPOP   LR\n"
    "\tLD    R0, R2\n"
    "\tLD    R1, #0\n"
    "\tRET\n";

static void init_rt(void)
{
    static Type *pp32[2] = {&t_long, &t_long};
    static Type *ppsh[2] = {&t_long, &t_uint};
    static Type fnt32 = {TY_FN, NULL, 0, NULL, 2, pp32, &t_long};
    static Type fntsh = {TY_FN, NULL, 0, NULL, 2, ppsh, &t_long};
    Sym *r32[] = {&rt_mul32, &rt_divu32, &rt_modu32, &rt_divs32, &rt_mods32, &rt_shl32, &rt_shr32, &rt_sar32,
                  &rt_udivc32, &rt_umodc32};
    const char *nm[] = {"__mul32", "__divu32", "__modu32", "__divs32", "__mods32", "__shl32", "__shr32", "__sar32",
                        "__udivc32", "__umodc32"};
    for (int i = 0; i < 10; i++) {
        r32[i]->name = r32[i]->asmname = (char *)nm[i];
        r32[i]->kind = S_FN;
        r32[i]->ty = i >= 5 ? &fntsh : &fnt32;
        r32[i]->nret = 1;
        r32[i]->rets[0] = &t_long;
    }
    static Type *pp[2] = {&t_int, &t_int};
    static Type fnt = {TY_FN, NULL, 0, NULL, 2, pp, &t_int};
    rt_divs.name = rt_divs.asmname = (char *)"__divs";
    rt_divs.kind = S_FN;
    rt_divs.ty = &fnt;
    rt_divs.nret = 1;
    rt_divs.rets[0] = &t_int;
    rt_mods = rt_divs;
    rt_mods.name = rt_mods.asmname = (char *)"__mods";
}

static void put_escaped(const char *str, int n);

static char *init_value(Node *v)
{
    if (v->kind == N_CAST) return init_value(v->a);
    Opnd o;
    if (!try_operand(v, &o, 0) || o.mode != M_IMM)
        die("global initializer must be a constant or an address");
    return o.sym ? symoff(o.sym, o.off) : numstr(o.off);
}

/* A string in the machine encoding -> a UTF-8 comment (as it looks on the screen). */
static void put_escaped(const char *str, int n)
{
    fputc('"', out);
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)str[i];
        unsigned long cp = charset[c];
        if (c == '\n') fputs("\\n", out);
        else if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c < 0x20 || c == 0x7F) fprintf(out, "\\x%02X", c);
        else if (cp < 0x80) fputc((int)cp, out);
        else if (cp < 0x800) { fputc(0xC0 | (int)(cp >> 6), out); fputc(0x80 | (int)(cp & 0x3F), out); }
        else {
            fputc(0xE0 | (int)(cp >> 12), out);
            fputc(0x80 | (int)((cp >> 6) & 0x3F), out);
            fputc(0x80 | (int)(cp & 0x3F), out);
        }
    }
    fputc('"', out);
}

/* ---------------- data: a stream of bytes and words (sect. 9.1) ----------------
   An object's initializer is expanded into a sequence of bytes and words. Bytes
   accumulate and are printed as .byte lines (long runs of zeros as .space), a word as
   a .word directive with a value or an address. With ds_dry nothing is printed:
   this way the initializers are checked before output. */
static int ds_dry;
static unsigned char ds_buf[16];
static int ds_n;

static void ds_flush(void)
{
    if (!ds_n) return;
    if (!ds_dry) {
        fprintf(out, "\t%s ", ASM_DB);
        for (int j = 0; j < ds_n; j++) fprintf(out, "%s%d", j ? ", " : "", ds_buf[j]);
        fprintf(out, "\n");
    }
    ds_n = 0;
}

static void ds_byte(int v)
{
    if (ds_n == 16) ds_flush();
    ds_buf[ds_n++] = (unsigned char)v;
}

static void ds_zero(int n)
{
    if (n >= 8) {
        ds_flush();
        if (!ds_dry) fprintf(out, "\t%s %d\n", ASM_DS, n);
        return;
    }
    while (n-- > 0) ds_byte(0);
}

static void ds_word(const char *v)
{
    ds_flush();
    if (!ds_dry) fprintf(out, "\t%s %s\n", ASM_DW, v);
}

static void ds_comment(const char *str, int n)
{
    ds_flush();
    if (ds_dry) return;
    fprintf(out, "\t; ");
    put_escaped(str, n);
    fprintf(out, "\n");
}

static void emit_init(Type *t, Node *in);

/* A struct initialization list: positional values and .field = value. */
static void emit_struct_init(Type *t, Node *in)
{
    StructDef *sd = t->sd;
    int nf = 0;
    for (Field *f = sd->fields; f; f = f->next) nf++;
    Node **vals = (Node**)xcalloc(sizeof(Node *) * (nf + 1));
    Field **fl = (Field**)xcalloc(sizeof(Field *) * (nf + 1));
    int k = 0;
    for (Field *f = sd->fields; f; f = f->next) fl[k++] = f;
    int pos = 0;
    for (int i = 0; i < in->n; i++) {
        Node *it = in->list[i];
        if (!it) { pos++; continue; }
        errline = it->line;
        if (it->kind == N_DESIG) {
            int j = 0;
            while (j < nf && strcmp(fl[j]->name, it->str)) j++;
            if (j == nf) die("struct %s has no field %s", sd->name, it->str);
            pos = j;
            it = it->a;
        }
        if (pos >= nf) die("too many values in initializer of struct %s", sd->name);
        if (vals[pos]) die("field %s is initialized twice", fl[pos]->name);
        vals[pos++] = it;
    }
    int cur = 0;
    for (int j = 0; j < nf; j++) {
        if (fl[j]->off > cur) ds_zero(fl[j]->off - cur);
        emit_init(fl[j]->ty, vals[j]);
        cur = fl[j]->off + type_size(fl[j]->ty);
    }
    if (sd->size > cur) ds_zero(sd->size - cur);
    free(vals);
    free(fl);
}

/* The value of an object of type t from initializer in (NULL - zeros). */
static void emit_init(Type *t, Node *in)
{
    int size = type_size(t);
    if (!in) { ds_zero(size); return; }
    errline = in->line;
    if (in->kind == N_DESIG) die("a field name is allowed only in a struct initializer");
    if (is_wide(t)) {
        if (in->kind == N_INITLIST) die("a scalar is initialized with a single value");
        check_assign(t, &in, "initializer");
        if (in->kind != N_NUM) die("a long variable is initialized with a constant");
        ds_word(fmt("$%04llX", in->val & 0xFFFF));
        ds_word(fmt("$%04llX", (in->val >> 16) & 0xFFFF));
        return;
    }
    if (is_scalar(t)) {
        if (in->kind == N_INITLIST) die("a scalar is initialized with a single value");
        check_assign(t, &in, "initializer");
        if (is_narrow(t)) {
            if (in->kind != N_NUM) die("a byte variable is initialized with a number");
            ds_byte((int)(in->val & 0xFF));
        } else {
            ds_word(init_value(in));
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        if (in->kind != N_INITLIST) die("a struct is initialized with {...}");
        emit_struct_init(t, in);
        return;
    }
    if (t->kind != TY_ARRAY) die("this type cannot be initialized");
    if (in->kind == N_STR) {
        if (!is_narrow(t->base)) die("only byte arrays can be initialized with a string");
        if (in->slen + 1 > t->len) die("string is longer than the array");
        ds_comment(in->str, in->slen);
        for (int i = 0; i < in->slen; i++) ds_byte((unsigned char)in->str[i]);
        ds_zero(t->len - in->slen);
        return;
    }
    if (in->kind != N_INITLIST) die("an array is initialized with {...} or a string");
    if (in->n > t->len) die("too many values in initializer");
    int es = type_size(t->base);
    for (int i = 0; i < in->n; i++) {
        if (in->list[i] && in->list[i]->kind == N_DESIG) { errline = in->list[i]->line; die("a field name is allowed only in a struct initializer"); }
        emit_init(t->base, in->list[i]);
    }
    if (in->n < t->len) ds_zero(es * (t->len - in->n));
}

/* An initialized global variable: values aligned to an even size. */
static void emit_global_value(Sym *s)
{
    int size = type_size(s->ty);
    str_owner = s;
    emitting = !ds_dry;
    emit_init(s->ty, s->init);
    if (size & 1) ds_byte(0);
    ds_flush();
}

/* String literals referenced by the emitted code and data. */
static void emit_strings(void)
{
    for (int i = 0; i < nstrings; i++) {
        Node *e = strings[i];
        if (!e->used) continue;
        emit_loc(e->line);
        fprintf(out, "%s:\t; ", e->sname);
        put_escaped(e->str, e->slen);
        fprintf(out, "\n");
        for (int k = 0; k < e->slen; k++) ds_byte((unsigned char)e->str[k]);
        ds_byte(0);
        if ((e->slen + 1) & 1) ds_byte(0);
        ds_flush();
    }
}

static int is_data_var(Sym *s) { return s->kind == S_VAR && s->init && (!s->tmpl_of || s->tmpl_used || ds_dry); }
static int is_bss_var(Sym *s) { return s->kind == S_VAR && !s->init; }

/* Checking the initializers of global variables before output (errors - with recovery). */
static void check_globals(void)
{
    jmp_buf jb;
    int lvl = nerr_stack;
    ds_dry = 1;
    Sym *volatile gnext = syms;      /* a loop with while: assigning a volatile in a for header is deprecated in C++20 */
    while (gnext) {
        Sym *g = gnext;
        gnext = g->next;
        if (!is_data_var(g)) continue;
        if (setjmp(jb)) { nerr_stack = lvl; ds_n = 0; continue; }
        TRY_PUSH(jb);
        emit_global_value(g);
        TRY_POP();
    }
    ds_dry = 0;
    emitting = 0;
}

/* The data area (sect. 9.1):
     without --data: initialized variables, literals, .align 2, __image_end,
                 uninitialized variables (.reserve), __bss_end;
     with --data A: literals and the image of initialized data __data_init next to the code,
                 __image_end, then .org A: initialized variables (.reserve,
                 filled by the startup code or __init_data), uninitialized ones, __bss_end. */
static void emit_data(void)
{
    fprintf(out, "\n; data\n");
    if (data_org < 0) {
        emit_noloc();
        fprintf(out, "__data_start:\n");
        for (Sym *s = syms; s; s = s->next) {
            if (!is_data_var(s)) continue;
            emit_loc(s->line);
            fprintf(out, "%s:\n", s->asmname);
            emit_global_value(s);
        }
        emitting = 1;
        emit_strings();
    } else {
        emitting = 1;
        emit_strings();
        emit_noloc();
        fprintf(out, "__data_init:\t; image of initialized data\n");
        for (Sym *s = syms; s; s = s->next) {
            if (!is_data_var(s)) continue;
            emit_loc(s->line);
            fprintf(out, "\t; %s\n", s->asmname);
            emit_global_value(s);
        }
    }
    emit_noloc();
    fprintf(out, "\t.align 2\n__image_end:\n");
    if (data_org >= 0) {
        fprintf(out, "\t.org  $%04llX\n__data_start:\n", data_org);
        for (Sym *s = syms; s; s = s->next) {
            if (!is_data_var(s)) continue;
            emit_loc(s->line);
            fprintf(out, "%s:\t.reserve %d\n", s->asmname, (type_size(s->ty) + 1) & ~1);
        }
        emit_noloc();
    }
    fprintf(out, "__bss_start:\n");
    for (Sym *s = syms; s; s = s->next) {
        if (!is_bss_var(s)) continue;
        emit_loc(s->line);
        fprintf(out, "%s:\t.reserve %d\n", s->asmname, (type_size(s->ty) + 1) & ~1);
    }
    emit_noloc();
    fprintf(out, "__bss_end:\n");
}

/* Constants for assembly code (sect. 10.3): const, addresses of variables at,
   field offsets S.field and sizes S.size of structs. */
static void emit_equs(void)
{
    int any = 0;
    for (Sym *s = syms; s; s = s->next) if (s->kind == S_CONST || s->kind == S_MMIO) any = 1;
    if (!any && !nstruct_order) return;
    fprintf(out, "\n; constants\n");
    for (Sym *s = syms; s; s = s->next) {
        if (s->kind != S_CONST && s->kind != S_MMIO) continue;
        emit_loc(s->line);
        if (s->kind == S_CONST) fprintf(out, "\t.equ  %s, %lld\n", s->asmname, s->val);
        else fprintf(out, "\t.equ  %s, $%04llX\n", s->asmname, s->val & 0xFFFF);
    }
    for (int i = 0; i < nstruct_order; i++) {
        StructDef *sd = struct_order[i];
        emit_loc(sd->line);
        for (Field *f = sd->fields; f; f = f->next)
            fprintf(out, "\t.equ  %s.%s, %d\n", sd->name, f->name, f->off);
        fprintf(out, "\t.equ  %s.sizeof, %d\n", sd->name, sd->size);
    }
    emit_noloc();
}

/* The CX16 executable header and the startup code of the std target (sect. 9.2). The image
   runs both under the monitor and on a bare machine: how it was started is determined by the
   signature at address $0004 ("CXPB" - the monitor's PIB, "CX16" - the header). */
static void emit_startup(Sym *m)
{
    emit_noloc();
    fprintf(out,
        "\n; CX16 executable file header\n"
        "__header:\n"
        "\tJMP   #__start\n"
        "\t.ascii \"CX16\"\n"
        "\t.word 1, 0\t\t; version, flags\n"
        "\t.word __image_end, __bss_end\n"
        "\t.word %lld, %lld\t; min_free, max_top\n"
        "\t.word 0, 0, 0, 0\n", min_free, max_top);
    fprintf(out,
        "\n; startup code\n"
        "__start:\n"
        "\tLD    R4, [$0006]\t; \"CXPB\" - monitor, \"CX16\" - bare machine\n"
        "\tCMP   R4, #$4250\t; \"PB\"\n"
        "\tJZ    @monitor\n"
        "\tLD    SP, #%s\t; bare machine: its own stack\n"
        "\tLD    R2, #__fault\t; ERRCMD first of all\n"
        "\tST    [$FF10], R2\n"
        "\tLD    R3, #__irq_ignore\t; the other vectors - to an empty stub\n"
        "\tLD    R2, #$FF00\n"
        "@vec:\n"
        "\tCMP   R2, #$FF10\n"
        "\tJZ    @skip\n"
        "\tST    [R2], R3\n"
        "@skip:\n"
        "\tADD   R2, #2\n"
        "\tCMP   R2, #$FF20\n"
        "\tJNZ   @vec\n"
        "\tLD    R2, #__bss_start\t; bss: memory after reset is undefined\n"
        "\tLD    R3, #0\n"
        "@bss:\n"
        "\tCMP   R2, #__bss_end\n"
        "\tJZ    @args\n"
        "\tST    [R2++], R3\n"
        "\tJMP   @bss\n"
        "@args:\n"
        "\tLD    R0, #0\t\t; argc = 0, argv = 0\n"
        "\tLD    R1, #0\n"
        "\tLD    LR, #__halt\t; return from main - halt\n"
        "@monitor:\t\t\t; monitor: R0 = argc, R1 = argv, SP = top, LR - exit\n"
        "\tPUSH  LR\n", STACK_TOP);
    if (data_org >= 0)
        fprintf(out,
            "\tLD    R2, #__data_init\t; initialized data - from the image\n"
            "\tLD    R3, #__data_start\n"
            "@copy:\n"
            "\tCMP   R3, #__bss_start\n"
            "\tJZ    @run\n"
            "\tLD    R4, [R2++]\n"
            "\tST    [R3++], R4\n"
            "\tJMP   @copy\n"
            "@run:\n");
    fprintf(out, "\tCALL  %s\n", m->asmname);
    if (!m->nret) fprintf(out, "\tLD    R0, #0\t\t; exit code\n");
    fprintf(out,
        "\tPOP   IP\n"
        "\n; bare machine: halt after main and on ERRCMD\n"
        "__fault:\n"
        "__halt:\n"
        "\tLD    R1, #0\n"
        "\tSTB   [%s], R1\t; timer off\n"
        "\tSTI\t\t\t; accept a pending timer request (the __irq_ignore handler):\n"
        "\tCLI\t\t\t; otherwise WFI with IF = 0 wakes on it again and again\n"
        "@stop:\n"
        "\tWFI\n"
        "\tJMP   @stop\n"
        "\n; an empty handler for unused vectors\n"
        "__irq_ignore:\n"
        "\tIRET\n", PORT_TIMER);
}

/* The kernel target with --data: a routine that fills the data area (called by the kernel prologue). */
static void emit_init_data(void)
{
    emit_noloc();
    fprintf(out,
        "\n; __init_data: initialized data from the image, bss - zeros (clobbers R0..R2)\n"
        "__init_data:\n"
        "\tLD    R0, #__data_init\n"
        "\tLD    R1, #__data_start\n"
        "@copy:\n"
        "\tCMP   R1, #__bss_start\n"
        "\tJZ    @zero\n"
        "\tLD    R2, [R0++]\n"
        "\tST    [R1++], R2\n"
        "\tJMP   @copy\n"
        "@zero:\n"
        "\tLD    R2, #0\n"
        "@z:\n"
        "\tCMP   R1, #__bss_end\n"
        "\tJZ    @done\n"
        "\tST    [R1++], R2\n"
        "\tJMP   @z\n"
        "@done:\n"
        "\tRET\n");
}

/* Assembly modules (include asm, --asm): .include after the functions and runtime, before the data,
   so that the module's code and initialized data end up in the image before __image_end. First the
   include asm modules in order of first appearance, then the --asm modules; a repeat of the same file (by
   canonical path) is dropped. */
#define MAX_ASM_MODS 16
typedef struct { const char *path, *canon, *from; } AsmMod;     /* from: file:line or key */
static AsmMod asm_mods[MAX_ASM_MODS];
static int nasm_mods;
static const char *cli_asm[MAX_ASM_MODS];
static int ncli_asm;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* A path with forward slashes, without links and dots; 0 - error. */
static int full_path(const char *p, char *out)
{
#ifdef _WIN32
    if (!_fullpath(out, p, PATH_MAX)) return 0;
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
    return 1;
#else
    return realpath(p, out) != NULL;
#endif
}

static int path_is_abs(const char *p)
{
#ifdef _WIN32
    return p[0] == '/' || p[0] == '\\' || (isalpha((unsigned char)p[0]) && p[1] == ':');
#else
    return p[0] == '/';
#endif
}

static const char *last_sep(const char *p)
{
    const char *a = strrchr(p, '/');
#ifdef _WIN32
    const char *b = strrchr(p, '\\');
    if (b > a) a = b;
#endif
    return a;
}

static int comp_eq(const char *a, const char *b)
{
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static int split_path(char *path, char **comp)
{
    int n = 0;
    for (char *t = strtok(path, "/"); t && n < 255; t = strtok(NULL, "/")) comp[n++] = t;
    return n;
}

/* The path for .include: the assembler looks for the file relative to the directory of the output file, while the name on the
   command line is given relative to the current directory. */
static char *asm_include_path(const char *file, const char *oname)
{
    char fa[PATH_MAX], da[PATH_MAX], dir[PATH_MAX];
    const char *sl = oname ? last_sep(oname) : NULL;
    if (!sl || path_is_abs(file)) return xstrndup(file, strlen(file));      /* output to the current directory or an absolute path */
    size_t n = (size_t)(sl - oname);
    if (n == 0) n = 1;                                                      /* output to the root directory */
    if (n >= sizeof dir) return xstrndup(file, strlen(file));
    memcpy(dir, oname, n);
    dir[n] = 0;
    if (!full_path(file, fa) || !full_path(dir, da)) return xstrndup(file, strlen(file));
    char *fc[256], *dc[256];
    int nf = split_path(fa, fc), nd = split_path(da, dc), k = 0;
    while (k < nd && k < nf - 1 && comp_eq(dc[k], fc[k])) k++;              /* common directory prefix */
#ifdef _WIN32
    if (k == 0) return xstrndup(file, strlen(file));                        /* a different drive: there is no relative path */
#endif
    char *r = (char*)xcalloc(PATH_MAX * 2);
    for (int q = k; q < nd; q++) strcat(r, "../");
    for (int q = k; q < nf; q++) { strcat(r, fc[q]); if (q < nf - 1) strcat(r, "/"); }
    return r;
}

static void emit_asm_modules(const char *oname)
{
    if (!nasm_mods) return;
    emit_noloc();
    fprintf(out, "\n; assembly modules (include asm, --asm)\n");
    for (int i = 0; i < nasm_mods; i++)
        fprintf(out, "\t.include \"%s\"\t; %s\n", asm_include_path(asm_mods[i].path, oname), asm_mods[i].from);
}

static void emit_runtime(void)
{
    if (rt_divs.used_rt || rt_mods.used_rt) emit_noloc();
    if (rt_divs.used_rt) fputs(rt_divs_text, out);
    if (rt_mods.used_rt) fputs(rt_mods_text, out);
    if (rt_modu32.used_rt || rt_divs32.used_rt || rt_mods32.used_rt) rt_divu32.used_rt = 1;   /* common division core */
    if (rt_mul32.used_rt) fputs(rt_mul32_text, out);
    if (rt_divu32.used_rt) fputs(rt_divu32_text, out);
    if (rt_modu32.used_rt) fputs(rt_modu32_text, out);
    if (rt_divs32.used_rt) fputs(rt_divs32_text, out);
    if (rt_mods32.used_rt) fputs(rt_mods32_text, out);
    if (rt_shl32.used_rt) fputs(rt_shl32_text, out);
    if (rt_shr32.used_rt) fputs(rt_shr32_text, out);
    if (rt_sar32.used_rt) fputs(rt_sar32_text, out);
    if (rt_umodc32.used_rt) rt_udivc32.used_rt = 1;
    if (rt_udivc32.used_rt) fputs(rt_udivc32_text, out);
    if (rt_umodc32.used_rt) fputs(rt_umodc32_text, out);
}

/* ================================= main ================================== */

/* ============================= include ==================================== */
/* include "file" at the start of a line substitutes the text of the file (once per file).
   The path is searched relative to the including file, then in the -I directories. */

static char *pp_buf;
static size_t pp_len, pp_cap;
static char **pp_seen;
static int pp_nseen;
static const char *inc_dirs[32];
static int ninc_dirs;

static void pp_line(const char *s, size_t n, const char *file, int line)
{
    if (pp_len + n + 2 > pp_cap) {
        pp_cap = (pp_len + n + 2) * 2;
        pp_buf = (char*)xrealloc(pp_buf, pp_cap);
    }
    memcpy(pp_buf + pp_len, s, n);
    pp_len += n;
    pp_buf[pp_len++] = '\n';
    pp_buf[pp_len] = 0;
    linemap = (LineInfo*)xrealloc(linemap, sizeof(LineInfo) * (nlinemap + 1));
    linemap[nlinemap].file = file;
    linemap[nlinemap].line = line;
    nlinemap++;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    i64 n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char*)xcalloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

/* Paths: POSIX (realpath, separator /) and Windows (_fullpath, separators / and \\, drive C:). */
static int is_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int is_abs_path(const char *p)
{
#ifdef _WIN32
    if (isalpha((unsigned char)p[0]) && p[1] == ':') return 1;
#endif
    return is_sep(p[0]);
}

static char *dir_of(const char *path)
{
    const char *sl = NULL;
    for (const char *p = path; *p; p++) if (is_sep(*p)) sl = p;
    return sl ? xstrndup(path, sl - path + 1) : xstrndup("", 0);
}

/* Remove "./" and "directory/../" from a path (for messages and -s comments). */
static void tidy_path(char *p)
{
    char *seg[256];
    int n = 0, abs_ = is_sep(p[0]);
    char *q = p + abs_;
    while (*q && n < 256) {
        char *e = q;
        while (*e && !is_sep(*e)) e++;
        int last = !*e;
        *e = 0;
        if (!strcmp(q, ".") && !last) ;
        else if (!strcmp(q, "..") && n > 0 && strcmp(seg[n - 1], "..")) n--;
        else seg[n++] = q;
        if (last) break;
        q = e + 1;
    }
    char buf[4096];
    size_t len = 0;
    if (abs_) buf[len++] = '/';
    for (int i = 0; i < n && len + strlen(seg[i]) + 2 < sizeof buf; i++) {
        if (i) buf[len++] = '/';
        memcpy(buf + len, seg[i], strlen(seg[i]));
        len += strlen(seg[i]);
    }
    buf[len] = 0;
    strcpy(p, buf);
}

/* The canonical file name: include uses it to tell already included files apart. */
static char *canon(const char *path)
{
    char buf[4096];
#ifdef _WIN32
    const char *r = _fullpath(buf, path, sizeof buf) ? buf : path;
    char *c = xstrndup(r, strlen(r));
    for (char *q = c; *q; q++) *q = *q == '\\' ? '/' : (char)tolower((unsigned char)*q);
    return c;
#else
    if (realpath(path, buf)) return xstrndup(buf, strlen(buf));
    return xstrndup(path, strlen(path));
#endif
}

/* Add an assembly module; 0 - the limit is exceeded. A repeat of the same file is dropped. */
static int add_asm_module(const char *path, const char *from)
{
    char *c = canon(path);
    for (int i = 0; i < nasm_mods; i++) if (!strcmp(asm_mods[i].canon, c)) { free(c); return 1; }
    if (nasm_mods == MAX_ASM_MODS) { free(c); return 0; }
    asm_mods[nasm_mods].path = xstrndup(path, strlen(path));
    asm_mods[nasm_mods].canon = c;
    asm_mods[nasm_mods].from = from;
    nasm_mods++;
    return 1;
}

static void preprocess(const char *path, int depth)
{
    if (depth > 16) die("include nested too deeply");
    char *c = canon(path);
    for (int i = 0; i < pp_nseen; i++) if (!strcmp(pp_seen[i], c)) { free(c); return; }
    pp_seen = (char**)xrealloc(pp_seen, sizeof(char *) * (pp_nseen + 1));
    pp_seen[pp_nseen++] = c;
    char *text = read_file(path);
    if (!text) die("cannot read file %s", path);
    const char *file = xstrndup(path, strlen(path));
    if (depth == 0) pp_root_file = file;
    char *dir = dir_of(path);
    int line = 0, in_comment = 0;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        line++;
        const char *q = p;
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        int is_inc = !in_comment && e - q > 7 && !strncmp(q, "include", 7) &&
                     (q[7] == ' ' || q[7] == '\t' || q[7] == '"');
        if (is_inc) {
            q += 7;
            while (q < e && (*q == ' ' || *q == '\t')) q++;
            errline = 0;
            /* include asm "file" - an assembly module in the image (sect. 3.2, 10.4) */
            int is_asm = e - q > 3 && !strncmp(q, "asm", 3) && (q[3] == ' ' || q[3] == '\t' || q[3] == '"');
            if (is_asm) {
                q += 3;
                while (q < e && (*q == ' ' || *q == '\t')) q++;
            }
            if (q >= e || *q != '"') {
                fprintf(stderr, "%s:%d: error: expected include %s\"file\"\n", file, line, is_asm ? "asm " : "");
                exit(1);
            }
            const char *n0 = ++q;
            while (q < e && *q != '"') q++;
            if (q >= e) { fprintf(stderr, "%s:%d: error: unterminated string in include\n", file, line); exit(1); }
            char *name = xstrndup(n0, q - n0);
            q++;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
            if (q < e && !(q[0] == '/' && q + 1 < e && q[1] == '/')) {
                fprintf(stderr, "%s:%d: error: unexpected text after include\n", file, line);
                exit(1);
            }
            char *full = is_abs_path(name) ? xstrndup(name, strlen(name)) : fmt("%s%s", dir, name);
            tidy_path(full);
            FILE *t = fopen(full, "rb");
            for (int i = 0; !t && i < ninc_dirs && !is_abs_path(name); i++) {
                free(full);
                full = fmt("%s/%s", inc_dirs[i], name);
                tidy_path(full);
                t = fopen(full, "rb");
            }
            if (!t) {
                fprintf(stderr, "%s:%d: error: %s %s not found\n", file, line, is_asm ? "assembly module" : "file", name);
                exit(1);
            }
            fclose(t);
            pp_line("", 0, file, line);
            if (is_asm) {
                if (!add_asm_module(full, fmt("%s:%d", file, line))) {
                    fprintf(stderr, "%s:%d: error: too many assembly modules (at most %d)\n", file, line, MAX_ASM_MODS);
                    exit(1);
                }
            } else preprocess(full, depth + 1);
        } else {
            pp_line(p, e - p, file, line);
            /* we track block comments so as not to mistake an include inside them */
            for (const char *r = p; r < e; r++) {
                if (in_comment) { if (r[0] == '*' && r + 1 < e && r[1] == '/') { in_comment = 0; r++; } }
                else if (r[0] == '/' && r + 1 < e && r[1] == '/') break;
                else if (r[0] == '"') { r++; while (r < e && *r != '"') { if (*r == '\\') r++; r++; } }
                else if (r[0] == '/' && r + 1 < e && r[1] == '*') { in_comment = 1; r++; }
            }
        }
        p = *e ? e + 1 : e;
    }
    free(text);
}

/* ===================== unused functions ============================ */

static Func *func_of(Sym *s)
{
    for (int i = 0; i < nfuncs; i++) if (funcs[i]->sym == s) return funcs[i];
    return NULL;
}

static void mark(Func *f)
{
    if (!f || f->reachable) return;
    f->reachable = 1;
    for (int i = 0; i < f->nrefs; i++) mark(func_of(f->refs[i]));
}

static void usage(void)
{
    fprintf(stderr,
        "usage: b16c [options] prog.b16\n"
        "  -o file        output file (default: stdout)\n"
        "  -I dir         directory to search for include files (repeatable)\n"
        "image parameters (normally set in the source: option NAME = VALUE, include asm \"file\";\n"
        "a key overrides the option of the main file):\n"
        "  --target std   executable CX16 file: runs under the monitor and on a bare machine (default)\n"
        "  --target kernel  code without header and startup: kernel, modules called from assembly\n"
        "  --code ADDR    start address of the code (kernel only)\n"
        "  --data ADDR    start address of all data (initialized data is copied from an image)\n"
        "  --asm file     assembly module (repeatable): .include after the code and before the data, so\n"
        "                 its code and initialized data are inside the image (no .reserve in the module)\n"
        "  --min-free N   CX16 header: minimum free memory for stack and heap (std only, default 2048)\n"
        "  --max-top N    CX16 header: desired upper memory bound, 0 - maximum (std only, default 0)\n"
        "other keys:\n"
        "  -n             all functions are entry points (as if all were export); deprecated: use export\n"
        "  -a             emit unreachable functions too\n"
        "  -s             interleave source lines as comments in the output\n"
        "  -Werror        treat warnings as errors\n"
        "  -Wmaybe-uninit also warn when a variable may be read before assignment on some path\n"
        "  -fcopy-params  always copy parameters to R4+ (by default a parameter read only before the first\n"
        "                 call stays in its argument register)\n"
        "  -w             no warnings\n");
    exit(2);
}

static i64 parse_num(const char *opt, const char *v, i64 lo, i64 hi)
{
    char *e;
    i64 x;
    if (v[0] == '$') x = strtoll(v + 1, &e, 16);
    else x = strtoll(v, &e, 0);
    if (!*v || *e || x < lo || x > hi) {
        fprintf(stderr, "b16c: %s: invalid value '%s' (%lld..%lld)\n", opt, v, lo, hi);
        exit(2);
    }
    return x;
}

static void predeclare_addr(const char *name)
{
    Sym *s = (Sym*)xcalloc(sizeof *s);
    s->name = s->asmname = (char *)name;
    s->kind = S_ADDR;
    s->ty = &t_ptr;
    if (syms_tail) syms_tail->next = s; else syms = s;
    syms_tail = s;
}

/* main of the std target: fn main(), fn main(): int, fn main(argc: uint, argv: ptr ptr byte) [: int]. */
static void check_main(Sym *m)
{
    Type *t = m->ty;
    errline = m->line;
    int ok = (t->np == 0 || (t->np == 2 && t->params[0]->kind == TY_UINT &&
              same_type(t->params[1], ptr_to(ptr_to(&t_byte))))) &&
             (t->nret == 0 || (t->nret == 1 && t->rets[0]->kind == TY_INT)) && !t->is_int;
    if (!ok) die("main must be fn main() or fn main(argc: uint, argv: ptr ptr byte), optionally with result int");
}

static int is_root(Func *f, Sym *m)
{
    Sym *s = f->sym;
    if (s->is_export || s->is_int || s->root) return 1;
    if (opt_all_export && !s->is_static) return 1;
    return opt_target == T_STD && s == m;
}

/* The effective image parameters and their source - as a comment at the start of the output. */
static void emit_params(void)
{
    fprintf(out, "; image parameters:\n");
    for (int k = 0; k < NPARAMS; k++) {
        ImageParam *p = &params[k];
        if (k == P_CODE && opt_target == T_STD) continue;
        if ((k == P_MIN_FREE || k == P_MAX_TOP) && opt_target == T_KERNEL) continue;
        const char *from;
        if (p->cli_set) from = p->key;
        else if (p->src_set) {
            const char *f;
            int fl;
            where(p->src_line, &f, &fl);
            from = fmt("%s:%d", f, fl);
        } else from = "default";
        i64 v = param_val(k);
        const char *val = v >= 0 ? param_text(k, v) : k == P_CODE ? "$0000" : "right after the code";
        int w = 0;      /* width in UTF-8 characters, not in bytes */
        for (const char *c = val; *c; c++) if ((*c & 0xC0) != 0x80) w++;
        fprintf(out, ";   %-9s %s%*s %s\n", p->name, val, w < 16 ? 16 - w : 0, "", from);
    }
}

int main(int argc, char **argv)
{
    const char *in = NULL, *oname = NULL;
    int keep_all = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) oname = argv[++i];
        else if (!strcmp(a, "-n")) opt_all_export = 1;
        else if (!strcmp(a, "-a")) keep_all = 1;
        else if (!strcmp(a, "-s")) opt_source = 1;
        else if (!strcmp(a, "-Werror")) opt_werror = 1;
        else if (!strcmp(a, "-w")) opt_nowarn = 1;
        else if (!strcmp(a, "-Wmaybe-uninit")) opt_maybe_uninit = 1;
        else if (!strcmp(a, "-fcopy-params")) opt_copy_params = 1;
        else if (!strcmp(a, "-I") && i + 1 < argc) {
            if (ninc_dirs == 32) usage();
            inc_dirs[ninc_dirs++] = argv[++i];
        }
        else if (!strcmp(a, "--target") && i + 1 < argc) {
            const char *t = argv[++i];
            if (!strcmp(t, "std")) params[P_TARGET].cli_val = T_STD;
            else if (!strcmp(t, "kernel")) params[P_TARGET].cli_val = T_KERNEL;
            else { fprintf(stderr, "b16c: --target: expected std or kernel\n"); exit(2); }
            params[P_TARGET].cli_set = 1;
        }
        else if (!strcmp(a, "--asm") && i + 1 < argc) {
            if (ncli_asm == MAX_ASM_MODS) { fprintf(stderr, "b16c: --asm: at most %d modules\n", MAX_ASM_MODS); exit(2); }
            FILE *fm = fopen(argv[++i], "rb");
            if (!fm) { perror(argv[i]); return 1; }
            fclose(fm);
            cli_asm[ncli_asm++] = argv[i];
        }
        else if (a[0] == '-' && i + 1 < argc && (!strcmp(a, "--code") || !strcmp(a, "--data") ||
                                                 !strcmp(a, "--min-free") || !strcmp(a, "--max-top"))) {
            int k = P_CODE;
            while (strcmp(params[k].key, a)) k++;
            ImageParam *p = &params[k];
            p->cli_val = parse_num(a, argv[++i], p->lo, p->hi);
            if (p->even && (p->cli_val & 1)) { fprintf(stderr, "b16c: %s must be even\n", a); exit(2); }
            p->cli_set = 1;
        }
        else if (a[0] == '-') usage();
        else if (!in) in = a;
        else usage();
    }
    if (!in) usage();
    srcname = in;
    FILE *fi = fopen(in, "rb");
    if (!fi) { perror(in); return 1; }
    fclose(fi);
    preprocess(in, 0);
    for (int i = 0; i < ncli_asm; i++)
        if (!add_asm_module(cli_asm[i], "--asm")) {
            fprintf(stderr, "b16c: too many assembly modules (include asm and --asm, at most %d)\n", MAX_ASM_MODS);
            return 1;
        }
    src = pp_buf ? pp_buf : (char*)xcalloc(1);
    index_source();
    init_rt();
    predeclare_addr("__image_end");
    predeclare_addr("__bss_end");

    /* pass 1: declarations (function bodies are skipped) */
    parse_pass = 1;
    lp = src; lline = 1; lasttok = 0; lex_depth = 0;
    next();
    parse_toplevel();
    if (nerrors) return 1;
    resolve_params();
    if (nerrors) return 1;
    for (Sym *s = syms; s; s = s->next)
        if (s->kind == S_FN && s->is_export && !s->has_body) {
            errline = s->line;
            jmp_buf jb;
            int lvl = nerr_stack;
            if (!setjmp(jb)) { TRY_PUSH(jb); die("exported function %s is not defined", s->name); }
            nerr_stack = lvl;
        }
    if (nerrors) return 1;
    Sym *m = find_global("main");
    if (m && m->kind != S_FN) m = NULL;
    if (opt_target == T_STD) {
        if (!m) { errline = 0; fatal("program has no main (to build a module without startup code use option target = kernel)"); }
        check_main(m);
    }

    /* pass 2: function bodies */
    parse_pass = 2;
    lp = src; lline = 1; lasttok = 0; lex_depth = 0;
    next();
    parse_toplevel();
    if (nerrors) return 1;
    check_globals();
    if (nerrors) return 1;

    /* reachability: from main (std), export, interrupt handlers and initializers */
    for (int i = 0; i < nfuncs; i++) if (keep_all || is_root(funcs[i], m)) mark(funcs[i]);

    out = oname ? fopen(oname, "w") : stdout;
    if (!out) { perror(oname); return 1; }
    fprintf(out, "; generated by b16c from %s\n", in);
    emit_params();
    emit_equs();
    if (opt_target == T_STD) emit_startup(m);
    else if (code_org >= 0) { emit_noloc(); fprintf(out, "\n\t.org  $%04llX\n", code_org); }

    /* code: unreachable functions are also translated (their errors and warnings
       are reported), but not emitted */
    jmp_buf jb;
    int lvl = nerr_stack;
    volatile int fidx = 0;
    while (fidx < nfuncs) {
        Func *fn_cur = funcs[fidx];
        fidx = fidx + 1;
        if (setjmp(jb)) {
            nerr_stack = lvl; nretry = 0; ncode = 0; busy = 0; nloops = 0;
            continue;
        }
        TRY_PUSH(jb);
        emitting = fn_cur->reachable;
        gen_func(fn_cur);
        TRY_POP();
    }
    emitting = 1;
    if (nerrors) { if (out != stdout) { fclose(out); remove(oname); } return 1; }
    emit_runtime();
    if (opt_target == T_KERNEL && data_org >= 0) emit_init_data();
    emit_asm_modules(oname);
    emit_data();
    /* a prototype without a definition: a forgotten function body (assembly functions are extern fn) */
    for (Sym *s = syms; s; s = s->next)
        if (s->kind == S_FN && !s->has_body && !s->is_extern)
            warn_at(s->line, "function %s is declared but not defined (if it is defined in assembly, declare it extern fn)", s->name);
    if (out != stdout) fclose(out);
    if (nerrors) { if (oname) remove(oname); return 1; }
    return 0;
}

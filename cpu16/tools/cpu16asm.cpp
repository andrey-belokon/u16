// cpu16asm.cpp - the assembler of the 16-bit processor.
// Implements "The Assembly Language of the 16-bit Processor. Reference" (specification, revision 6).
//
// Build: make tools  or  g++ -std=c++17 -O2 -Wall -o cpu16asm cpu16asm.cpp
//         (cpu16_isa.hpp and cpu16_machine.hpp must lie next to it)
// Run: ./cpu16asm [-o image.bin] [-l listing.lst] [-m map.map] [--base ADDRESS] [--fill BYTE] [--exec] [--no-line] source.asm
//   -o       binary image (by default the source name with the .bin extension)
//   -l       listing: address, words/bytes, the source line
//   -m       memory map: image objects with addresses, types and sizes, a summary, constants
//   --base   the address of the first byte of the image (0 by default)
//   --fill   the byte that fills gaps in the image (0 by default)
//   --exec   check the header of a CX16 executable ("CPU16 Monitor: Executable File
//            Format", sect. 3.3; the flags, reserved and min_free fields are not checked); requires --base 0
//   --no-line  do not apply .line: messages, the map and the listing use the places in the assembler text
// Numbers in parameters: 123, $FF, 0xFF. Options, file and message formats - the reference, sect. 17.
//
// Structure (sect. 10 of the reference):
//   1. parsing lines into elements (instructions, directives) with a check of syntax and of the validity of forms;
//   2. evaluating the arguments of .org/.space/.reserve/.align (they do not depend on labels);
//   3. choosing sizes: variable-size elements start SHORT and irreversibly
//      become LONG until the layout stabilizes;
//   4. the final pass: encoding, range and overlap checks, output.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu16_isa.hpp"
#include "cpu16_machine.hpp"

namespace cpu16asm {

using namespace cpu16;

// ============================================================================
// Diagnostics and source text
// ============================================================================

struct Diag {
    std::string file;
    int line;        // 0 - a message without a line
    std::string msg;
    // The line of the assembler text if file:line is given by the .line directive (otherwise asmLine = 0).
    std::string asmFile;
    int asmLine = 0;
};

struct SrcLine {
    int file;        // index in the file list
    int line;        // line number (from 1)
    std::string text;
    int mapName = -1;   // .line: the index of the name in lineNames (-1 - the line has its own place)
    long mapLine = 0;   // .line: the line number in that source
    bool lineDir = false;   // the line is the .line directive itself (in the listing without a place)
};

// The state of .line within one file: the place that all following lines get until the next
// .line (sect. 9 of the reference). name = -1 - the lines have their own places.
struct LineMap {
    int name = -1;
    long line = 0;
};

struct SyntaxError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

static std::string hex4(int64_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "$%04X", (unsigned)(v & 0xFFFF));
    return b;
}

// ============================================================================
// Lexer (sect. 2)
// ============================================================================

enum class T {
    End, Ident, Local, Number, String, Directive,
    Comma, Colon, Hash, LBr, RBr, LPar, RPar,
    Plus, Minus, Star, Slash, Tilde, Amp, Pipe, Caret, Shl, Shr,
    PlusPlus, MinusMinus, Dollar
};

struct Tok {
    T t = T::End;
    std::string s;       // name (Ident, Local without '@', Directive in lower case), string bytes in the machine encoding
    std::string raw;     // string: bytes as in the source (for .include)
    std::string err;     // string: a transcoding error (reported if the string is used as data)
    int64_t v = 0;       // the value of a number or a character
};

// A character for a message: printable ASCII as is, other bytes as \xHH
// (a zero byte would otherwise truncate the message, and half of a UTF-8 character would give corrupted text).
static std::string showChar(char c) {
    const unsigned char u = (unsigned char)c;
    if (u >= 0x20 && u < 0x7F) return std::string(1, c);
    char b[8];
    std::snprintf(b, sizeof b, "\\x%02X", u);
    return b;
}

static bool isIdStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool isIdChar(char c) { return isIdStart(c) || (c >= '0' && c <= '9'); }
// A character of a label or symbol name: a dot as well (not as the first character; directive names have no dots).
static bool isSymChar(char c) { return isIdChar(c) || c == '.'; }
// A dot in a name stands only between isIdChar characters: not at the end and not two in a row.
static void checkDots(const std::string& name) {
    for (size_t k = 0; k < name.size(); k++)
        if (name[k] == '.' && (k + 1 == name.size() || name[k + 1] == '.'))
            throw SyntaxError("invalid name '" + name + "': '.' must be followed by a letter, digit or '_'");
}
static bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hexVal(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

// Parsing an escape sequence after '\'. i points to the character after '\'.
static int parseEscape(const std::string& s, size_t& i) {
    if (i >= s.size()) throw SyntaxError("unterminated escape sequence");
    const char c = s[i++];
    switch (c) {
    case 'n': return 10;
    case 'r': return 13;
    case 't': return 9;
    case '0': return 0;
    case '\\': return '\\';
    case '"': return '"';
    case '\'': return '\'';
    case 'x':
        if (i + 1 < s.size() && isHex(s[i]) && isHex(s[i + 1])) {
            const int v = hexVal(s[i]) * 16 + hexVal(s[i + 1]);
            i += 2;
            return v;
        }
        throw SyntaxError("\\x must be followed by two hex digits");
    default:
        throw SyntaxError("unknown escape sequence '\\" + showChar(c) + "'");
    }
}

static int64_t parseDigits(const std::string& s, size_t& i, int base, const char* what) {
    const size_t start = i;
    int64_t v = 0;
    while (i < s.size()) {
        const char c = s[i];
        int d;
        if (base == 16 && isHex(c)) d = hexVal(c);
        else if (base == 10 && c >= '0' && c <= '9') d = c - '0';
        else if (base == 2 && (c == '0' || c == '1')) d = c - '0';
        else break;
        v = v * base + d;
        if (v > 0xFFFFFFFFLL) throw SyntaxError("number too large");
        i++;
    }
    if (i == start) throw SyntaxError(std::string("expected digits of ") + what);
    if (i < s.size() && isSymChar(s[i])) throw SyntaxError("malformed number");
    return v;
}

// The next character of a literal (not an escape): ASCII as is, otherwise a UTF-8 character is transcoded
// to the machine encoding ("CPU16 Machine", sect. 5.1). raw receives the source bytes; on an error
// err receives the message (the first one), the result is -1.
static int literalChar(const std::string& s, size_t& i, std::string& raw, std::string& err, const char* what) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { raw.push_back((char)c); i++; return c; }
    size_t j = i;
    const long cp = cpu16::utf8Decode(s, j);
    const std::string bytes = s.substr(i, j - i);
    raw += bytes;
    i = j;
    if (cp < 0) { if (err.empty()) err = std::string("invalid UTF-8 in ") + what; return -1; }
    const int code = cpu16::charsetFind((char32_t)cp);
    if (code < 0) { if (err.empty()) err = "character '" + bytes + "' is not in the CPU16 character set"; return -1; }
    return code;
}

static std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == ';') break;
        Tok t;
        if (isIdStart(c)) {
            const size_t b = i;
            while (i < s.size() && isSymChar(s[i])) i++;
            t.t = T::Ident; t.s = s.substr(b, i - b);
            checkDots(t.s);
        } else if (c == '@') {
            i++;
            if (i >= s.size() || !isIdStart(s[i])) throw SyntaxError("expected local label name after '@'");
            const size_t b = i;
            while (i < s.size() && isSymChar(s[i])) i++;
            t.t = T::Local; t.s = s.substr(b, i - b);
            checkDots("@" + t.s);
        } else if (c == '.') {
            i++;
            if (i >= s.size() || !isIdStart(s[i])) throw SyntaxError("expected directive name after '.'");
            const size_t b = i;
            while (i < s.size() && isIdChar(s[i])) i++;
            t.t = T::Directive; t.s = s.substr(b, i - b);
            for (auto& ch : t.s) ch = (char)std::tolower((unsigned char)ch);
        } else if (c >= '0' && c <= '9') {
            t.t = T::Number;
            if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2; t.v = parseDigits(s, i, 16, "hex number");
            } else if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'b' || s[i + 1] == 'B')) {
                i += 2; t.v = parseDigits(s, i, 2, "binary number");
            } else {
                t.v = parseDigits(s, i, 10, "number");
            }
        } else if (c == '$') {
            i++;
            if (i < s.size() && isHex(s[i])) { t.t = T::Number; t.v = parseDigits(s, i, 16, "hex number"); }
            else t.t = T::Dollar;
        } else if (c == '%') {
            i++;
            if (i < s.size() && (s[i] == '0' || s[i] == '1')) { t.t = T::Number; t.v = parseDigits(s, i, 2, "binary number"); }
            else throw SyntaxError("expected binary number after '%' (there is no modulo operator)");
        } else if (c == '\'') {
            i++;
            if (i >= s.size()) throw SyntaxError("unterminated character literal");
            int v;
            if (s[i] == '\\') { i++; v = parseEscape(s, i); }
            else if (s[i] == '\'') throw SyntaxError("empty character literal");
            else {
                std::string raw, err;
                v = literalChar(s, i, raw, err, "character literal");
                if (!err.empty()) throw SyntaxError(err);
            }
            if (i >= s.size() || s[i] != '\'') throw SyntaxError("unterminated character literal");
            i++;
            t.t = T::Number; t.v = v;
        } else if (c == '"') {
            i++;
            std::string b, raw, err;
            bool closed = false;
            while (i < s.size()) {
                if (s[i] == '"') { closed = true; i++; break; }
                if (s[i] == '\\') { i++; const char e = (char)parseEscape(s, i); b.push_back(e); raw.push_back(e); }
                else {
                    const int v = literalChar(s, i, raw, err, "string");
                    if (v >= 0) b.push_back((char)v);
                }
            }
            if (!closed) throw SyntaxError("unterminated string");
            t.t = T::String; t.s = b; t.raw = raw; t.err = err;
        } else {
            const char n = i + 1 < s.size() ? s[i + 1] : 0;
            if (c == '<' && n == '<') { t.t = T::Shl; i += 2; }
            else if (c == '>' && n == '>') { t.t = T::Shr; i += 2; }
            else if (c == '+' && n == '+') { t.t = T::PlusPlus; i += 2; }
            else if (c == '-' && n == '-') { t.t = T::MinusMinus; i += 2; }
            else {
                i++;
                switch (c) {
                case ',': t.t = T::Comma; break;
                case ':': t.t = T::Colon; break;
                case '#': t.t = T::Hash; break;
                case '[': t.t = T::LBr; break;
                case ']': t.t = T::RBr; break;
                case '(': t.t = T::LPar; break;
                case ')': t.t = T::RPar; break;
                case '+': t.t = T::Plus; break;
                case '-': t.t = T::Minus; break;
                case '*': t.t = T::Star; break;
                case '/': t.t = T::Slash; break;
                case '~': t.t = T::Tilde; break;
                case '&': t.t = T::Amp; break;
                case '|': t.t = T::Pipe; break;
                case '^': t.t = T::Caret; break;
                default: throw SyntaxError("unexpected character '" + showChar(c) + "'");
                }
            }
        }
        out.push_back(t);
    }
    out.push_back(Tok{});  // T::End
    return out;
}

// ============================================================================
// Expressions (sect. 3)
// ============================================================================

struct Expr;
using EP = std::shared_ptr<Expr>;

struct Expr {
    enum K { Num, Sym, Dollar, Neg, Not, Lo, Hi, Bin } k = Num;
    int64_t v = 0;
    std::string name;    // Sym: the full name (local ones: "scope@name")
    char op = 0;         // Bin: * / + - < (<<) > (>>) & ^ |
    EP a, b;
};

static EP mkNum(int64_t v) { auto e = std::make_shared<Expr>(); e->k = Expr::Num; e->v = v; return e; }
static EP mkUn(Expr::K k, EP a) { auto e = std::make_shared<Expr>(); e->k = k; e->a = std::move(a); return e; }
static EP mkBin(char op, EP a, EP b) {
    auto e = std::make_shared<Expr>(); e->k = Expr::Bin; e->op = op; e->a = std::move(a); e->b = std::move(b); return e;
}

// The displayed symbol name: "scope@name" -> "@name".
static std::string displayName(const std::string& n) {
    const size_t p = n.find('@');
    return p == std::string::npos ? n : n.substr(p);
}

static bool isReservedName(const std::string& s) {
    return regByName(s) >= 0 || opByName(s) >= 0 || miscByName(s) >= 0 || condByName(s) >= 0 ||
           pseudoByName(s) != Pseudo::None || iequals(s, "lo") || iequals(s, "hi");
}

// A cursor over the tokens of a line.
struct Cur {
    const std::vector<Tok>& t;
    size_t i = 0;
    const std::string& scope;   // the current scope of local labels
    const Tok& peek() const { return t[i]; }
    bool is(T k) const { return t[i].t == k; }
    const Tok& next() { return t[i++]; }
    void expect(T k, const char* msg) {
        if (!is(k)) throw SyntaxError(msg);
        i++;
    }
    int regHere() const { return is(T::Ident) ? regByName(t[i].s) : -1; }
};

static EP parseExpr(Cur& c);

static EP parsePrimary(Cur& c) {
    const Tok& t = c.peek();
    switch (t.t) {
    case T::Number: c.next(); return mkNum(t.v);
    case T::Dollar: { c.next(); auto e = std::make_shared<Expr>(); e->k = Expr::Dollar; return e; }
    case T::LPar: {
        c.next();
        EP e = parseExpr(c);
        c.expect(T::RPar, "missing ')'");
        return e;
    }
    case T::Local: {
        c.next();
        auto e = std::make_shared<Expr>(); e->k = Expr::Sym; e->name = c.scope + "@" + t.s;
        return e;
    }
    case T::Ident: {
        if (regByName(t.s) >= 0) throw SyntaxError("register '" + t.s + "' is not allowed in an expression");
        if (iequals(t.s, "lo") || iequals(t.s, "hi")) {
            const bool lo = iequals(t.s, "lo");
            c.next();
            c.expect(T::LPar, lo ? "expected '(' after lo" : "expected '(' after hi");
            EP a = parseExpr(c);
            c.expect(T::RPar, "missing ')'");
            return mkUn(lo ? Expr::Lo : Expr::Hi, a);
        }
        c.next();
        auto e = std::make_shared<Expr>(); e->k = Expr::Sym; e->name = t.s;
        return e;
    }
    case T::String: throw SyntaxError("strings are only allowed in .ascii, .asciz, .include and .line");
    default: throw SyntaxError("expected expression");
    }
}

static EP parseUnary(Cur& c) {
    if (c.is(T::Minus)) { c.next(); return mkUn(Expr::Neg, parseUnary(c)); }
    if (c.is(T::Plus)) { c.next(); return parseUnary(c); }
    if (c.is(T::Tilde)) { c.next(); return mkUn(Expr::Not, parseUnary(c)); }
    if (c.is(T::MinusMinus)) { c.next(); return mkUn(Expr::Neg, mkUn(Expr::Neg, parseUnary(c))); }
    if (c.is(T::PlusPlus)) { c.next(); return parseUnary(c); }
    return parsePrimary(c);
}
static EP parseMul(Cur& c) {
    EP a = parseUnary(c);
    while (c.is(T::Star) || c.is(T::Slash)) {
        const char op = c.next().t == T::Star ? '*' : '/';
        a = mkBin(op, a, parseUnary(c));
    }
    return a;
}
static EP parseAdd(Cur& c) {
    EP a = parseMul(c);
    for (;;) {
        if (c.is(T::Plus)) { c.next(); a = mkBin('+', a, parseMul(c)); }
        else if (c.is(T::Minus)) { c.next(); a = mkBin('-', a, parseMul(c)); }
        else if (c.is(T::PlusPlus)) { c.next(); a = mkBin('+', a, parseMul(c)); }                    // a ++b = a + (+b)
        else if (c.is(T::MinusMinus)) { c.next(); a = mkBin('-', a, mkUn(Expr::Neg, parseMul(c))); }  // a --b = a - (-b)
        else return a;
    }
}
static EP parseShift(Cur& c) {
    EP a = parseAdd(c);
    while (c.is(T::Shl) || c.is(T::Shr)) {
        const char op = c.next().t == T::Shl ? '<' : '>';
        a = mkBin(op, a, parseAdd(c));
    }
    return a;
}
static EP parseAnd(Cur& c) {
    EP a = parseShift(c);
    while (c.is(T::Amp)) { c.next(); a = mkBin('&', a, parseShift(c)); }
    return a;
}
static EP parseXor(Cur& c) {
    EP a = parseAnd(c);
    while (c.is(T::Caret)) { c.next(); a = mkBin('^', a, parseAnd(c)); }
    return a;
}
static EP parseExpr(Cur& c) {
    EP a = parseXor(c);
    while (c.is(T::Pipe)) { c.next(); a = mkBin('|', a, parseXor(c)); }
    return a;
}

// ============================================================================
// Operands (sect. 6, 7.5)
// ============================================================================

struct Operand {
    enum K { Reg, Imm, Ind, PostInc, PreDec, Abs, Idx, Sum, Bare } k = Reg;
    int reg = 0;
    EP e;            // Imm, Abs: a value/address; Idx, Sum: an offset (with sign); Bare: the target
};

static Operand parseOperand(Cur& c) {
    Operand o;
    if (c.is(T::Hash)) {
        c.next();
        o.k = Operand::Imm; o.e = parseExpr(c);
        return o;
    }
    if (c.is(T::LBr)) {
        c.next();
        if (c.is(T::MinusMinus)) {
            c.next();
            const int r = c.regHere();
            if (r < 0) throw SyntaxError("expected register after '--'");
            c.next();
            o.k = Operand::PreDec; o.reg = r;
        } else if (c.regHere() >= 0) {
            const int r = c.regHere();
            c.next();
            o.reg = r;
            if (c.is(T::PlusPlus)) {
                if (r == IP) throw SyntaxError("[IP++] is not allowed: write an immediate as #expr");
                c.next();
                o.k = Operand::PostInc;
            } else if (c.is(T::Plus) || c.is(T::Minus)) {
                const bool neg = c.next().t == T::Minus;
                EP e = parseExpr(c);
                o.k = Operand::Idx; o.e = neg ? mkUn(Expr::Neg, e) : e;
            } else if (c.is(T::RBr)) {
                o.k = Operand::Ind;
            } else {
                throw SyntaxError("invalid operand syntax inside brackets");
            }
        } else {
            o.k = Operand::Abs; o.e = parseExpr(c);
        }
        c.expect(T::RBr, "missing ']'");
        return o;
    }
    if (c.regHere() >= 0) {
        o.reg = c.regHere();
        c.next();
        if (c.is(T::Plus) || c.is(T::Minus)) {
            const bool neg = c.next().t == T::Minus;
            EP e = parseExpr(c);
            o.k = Operand::Sum; o.e = neg ? mkUn(Expr::Neg, e) : e;
        } else if (c.is(T::PlusPlus) || c.is(T::MinusMinus)) {
            throw SyntaxError("auto-increment and auto-decrement must be in brackets: [Rn++], [--Rn]");
        } else {
            o.k = Operand::Reg;
        }
        return o;
    }
    o.k = Operand::Bare; o.e = parseExpr(c);
    return o;
}

static const char* formName(Operand::K k) {
    switch (k) {
    case Operand::Reg: return "Rn";
    case Operand::Imm: return "#expr";
    case Operand::Ind: return "[Rn]";
    case Operand::PostInc: return "[Rn++]";
    case Operand::PreDec: return "[--Rn]";
    case Operand::Abs: return "[expr]";
    case Operand::Idx: return "[Rn+expr]";
    case Operand::Sum: return "Rn+expr";
    case Operand::Bare: return "expr";
    }
    return "?";
}

// ============================================================================
// Program elements
// ============================================================================

struct Item {
    enum K { Instr, Word, Byte, Ascii, Space, Reserve, Align, Org, Equ, End } k = Instr;
    int src = -1;                   // source line

    // --- instruction
    enum Var { Fixed, ImmVar, BranchVar } var = Fixed;
    unsigned op = 0, r1 = 0, r2 = 0, mod = 0;
    bool fixedWord = false;         // the word is fully known (pseudo-instructions without operands)
    uint16_t word = 0;
    EP imm4;                        // R2 = a value 0..15 (shifts, LDZX/LDSX, TRAP)
    EP second;                      // the expression of the second word (fixed forms)
    EP target;                      // ImmVar: the value; BranchVar: the target
    bool isCall = false;            // BranchVar: CALL (otherwise Jxx, r1 = condition)
    bool isLong = false;            // the size-selection state
    int fixedWords = 1;
    const char* imm4What = "";      // the text of the range error for imm4

    // --- data and directives
    std::vector<EP> exprs;          // .word, .byte
    std::string bytes;              // .ascii/.asciz
    EP argN, argFill;               // .space/.align: n, fill; .reserve: n; .org: the address
    int64_t n = 0;                  // the computed n (.space, .reserve, .align) or the address (.org)
    std::string equName;

    // --- layout
    int64_t lcBefore = 0;           // the counter before the element
    int64_t addr = 0;               // the address of the first byte (after implicit alignment)
    int64_t size = 0;               // the byte after addr
    bool padded = false;            // an implicit alignment byte was inserted

    // --- the result of the final pass (for the listing)
    std::vector<uint16_t> outWords;
    std::vector<uint8_t> outBytes;
};

struct Sym {
    enum K { Label, Equ } k = Label;
    int item = -1;                  // Label: the element it is bound to; Equ: the .equ element
    bool atAddr = true;             // Label: the address of the element (true) or the counter before it (false)
    int src = -1;                   // the definition line
    EP expr;                        // Equ
    bool visiting = false;          // protection against cycles when evaluating .equ
};

struct Val {
    bool ok = true;
    int64_t v = 0;
    std::string err;
};

// ============================================================================
// The assembler
// ============================================================================

class Assembler {
public:
    std::vector<Diag> diags;
    std::vector<uint8_t> image = std::vector<uint8_t>(65536, 0);
    std::vector<int> owner = std::vector<int>(65536, -1);   // the element that produced the byte (-1 - none)
    int64_t minAddr = -1, maxAddr = -1;

    // Assemble a file from disk.
    bool assembleFile(const std::string& path) {
        processFile(path, -1, 0);
        return finish();
    }
    // Assemble a text (the name is used in messages and for .include paths).
    bool assembleText(const std::string& name, const std::string& text) {
        processText(name, text, 0);
        return finish();
    }

    int errorCount() const { return (int)diags.size(); }

    std::string formatDiags() const {
        std::string out;
        for (const auto& d : diags) {
            out += d.file;
            if (d.line > 0) out += ":" + std::to_string(d.line);
            if (d.asmLine > 0) out += " (" + d.asmFile + ":" + std::to_string(d.asmLine) + ")";
            out += ": error: " + d.msg + "\n";
        }
        return out;
    }

    // The value of a symbol after assembling (for tests and the map).
    bool symbolValue(const std::string& name, int64_t& v) {
        auto it = syms.find(name);
        if (it == syms.end()) return false;
        const Val r = symVal(it->second, false);
        v = r.v;
        return r.ok;
    }

    // The binary image: from base to the highest output address.
    std::vector<uint8_t> binary(int64_t base, uint8_t fill) const {
        std::vector<uint8_t> out;
        if (maxAddr < 0 || maxAddr < base) return out;
        for (int64_t a = base; a <= maxAddr; a++) out.push_back(owner[a] >= 0 ? image[a] : fill);
        return out;
    }

    // The first address not below from at which a byte from the source text was output (an instruction, data,
    // alignment), as opposed to a .reserve reservation; -1 - there are no such addresses.
    int64_t firstOutputFrom(int64_t from) const {
        for (int64_t a = from < 0 ? 0 : from; a <= maxAddr; a++)
            if (owner[a] >= 0 && items[owner[a]].k != Item::Reserve) return a;
        return -1;
    }

    // Checking the header of a CX16 executable ("CPU16 Monitor: Executable File Format and Program Startup",
    // sect. 3.3 and 10) on the finished image file (output from address 0). Checked: the initial jump,
    // the signature, the version, image_end/bss_end, start, the file length and the absence of output past image_end.
    // The flags, reserved and min_free fields are not checked. firstAbove is the first output byte not below
    // image_end (firstOutputFrom), only for the message; -1 - unknown.
    // Returns error messages (empty - the header is correct).
    static std::vector<std::string> checkExecHeader(const std::vector<uint8_t>& file, int64_t firstAbove = -1) {
        std::vector<std::string> e;
        const std::string pre = "executable header: ";
        if (file.size() < 0x1C) {
            e.push_back(pre + "file is " + std::to_string(file.size()) + " bytes, shorter than the header ($001C bytes)");
            return e;
        }
        auto w = [&](size_t a) { return (int64_t)(file[a] | (file[a + 1] << 8)); };
        const int64_t jmp = w(0x00), start = w(0x02), version = w(0x08), imageEnd = w(0x0C), bssEnd = w(0x0E);
        if (jmp != 0xC873) e.push_back(pre + "word at $0000 is " + hex4(jmp) + ", expected $C873 (JMP #start)");
        if (std::memcmp(&file[4], "CX16", 4) != 0) e.push_back(pre + "bytes $0004..$0007 must be \"CX16\"");
        if (version != 1) e.push_back(pre + "version is " + std::to_string(version) + ", expected 1");
        if (imageEnd < 0x1C) e.push_back(pre + "image_end " + hex4(imageEnd) + " is below $001C");
        if (bssEnd < imageEnd) e.push_back(pre + "bss_end " + hex4(bssEnd) + " is below image_end " + hex4(imageEnd));
        if (bssEnd > 0xE000) e.push_back(pre + "bss_end " + hex4(bssEnd) + " is above $E000");
        if (start & 1) e.push_back(pre + "start " + hex4(start) + " is odd");
        if (start < 0x1C || start >= imageEnd)
            e.push_back(pre + "start " + hex4(start) + " is outside the image ($001C <= start < image_end " + hex4(imageEnd) + ")");
        if (imageEnd > (int64_t)file.size())
            e.push_back(pre + "image_end " + hex4(imageEnd) + " exceeds the file length (" + std::to_string(file.size()) + " bytes)");
        // Nothing from the source text is output past image_end: the monitor will not load such code.
        // Additional file data (sect. 3.1 of the format) is appended to the finished image.
        if (imageEnd >= 0x1C && imageEnd < (int64_t)file.size())
            e.push_back(pre + "output above image_end " + hex4(imageEnd) + ": " +
                        std::to_string(file.size() - imageEnd) + " byte(s) up to the end of the file" +
                        (firstAbove >= 0 ? ", first output byte at " + hex4(firstAbove) : std::string()));
        return e;
    }

    // The same for an assembled program: the image from address 0 and the address of the first byte past image_end.
    std::vector<std::string> checkExec() const {
        const std::vector<uint8_t> bin = binary(0, 0);
        const int64_t imageEnd = bin.size() >= 0x10 ? (int64_t)(bin[0x0C] | (bin[0x0D] << 8)) : -1;
        return checkExecHeader(bin, imageEnd >= 0 ? firstOutputFrom(imageEnd) : -1);
    }

    std::string listing();
    std::string memoryMap(int64_t base = 0);

private:
    std::vector<std::string> files;
    std::vector<SrcLine> lines;
    std::vector<std::string> lineNames;   // source names from .line
    LineMap* curLineMap = nullptr;           // the .line state of the current file
public:
    bool ignoreLine = false;                 // --no-line: .line is checked but does not change places
private:
    std::vector<Item> items;
    std::unordered_map<std::string, Sym> syms;
    std::vector<std::string> symOrder;
    std::vector<std::string> pending;   // labels waiting to be bound to the next element
    std::string scope;                  // the last global label
    bool parseFailed = false;

    // ---------------- messages
    // The place of a line for messages and the memory map: by .line if it is set, otherwise its own.
    std::string locFile(int src) const {
        return lines[src].mapName >= 0 ? lineNames[lines[src].mapName] : files[lines[src].file];
    }
    long locLine(int src) const { return lines[src].mapName >= 0 ? lines[src].mapLine : lines[src].line; }
    std::string loc(int src) const { return src < 0 ? std::string("?") : locFile(src) + ":" + std::to_string(locLine(src)); }
    // " (first defined on line N)": the line number, or file:line if the place is in another file.
    // what - the beginning of the explanation: "first defined on" -> "on line N" or "at file:line".
    std::string refTo(int src, int from, const char* what, const char* whatAt) const {
        if (src < 0) return "";
        if (from >= 0 && locFile(from) == locFile(src)) return std::string(" (") + what + " line " + std::to_string(locLine(src)) + ")";
        return std::string(" (") + whatAt + " " + loc(src) + ")";
    }

    void errAt(int src, const std::string& m) {
        if (src >= 0) {
            Diag d{locFile(src), (int)locLine(src), m, "", 0};
            if (lines[src].mapName >= 0) { d.asmFile = files[lines[src].file]; d.asmLine = lines[src].line; }
            diags.push_back(d);
        }
        else diags.push_back({files.empty() ? "?" : files[0], 0, m, "", 0});
    }

    // ---------------- reading files
    static std::string dirOf(const std::string& p) {
        const size_t s = p.find_last_of('/');
        return s == std::string::npos ? "" : p.substr(0, s + 1);
    }

    void processFile(const std::string& path, int fromSrc, int depth) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            if (fromSrc >= 0) errAt(fromSrc, "cannot open file '" + path + "'");
            else diags.push_back({path, 0, "cannot open file", "", 0});
            parseFailed = true;
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        processText(path, ss.str(), depth);
    }

    void processText(const std::string& name, const std::string& text, int depth) {
        const int fi = (int)files.size();
        files.push_back(name);
        LineMap map;                       // .line is in effect until the end of its file
        LineMap* const outer = curLineMap;
        curLineMap = &map;
        size_t p = 0;
        int ln = 0;
        while (p <= text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            std::string l = text.substr(p, e - p);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            ln++;
            if (!(e == text.size() && l.empty() && p == text.size())) {
                lines.push_back({fi, ln, l, map.name, map.line});
                parseLine((int)lines.size() - 1, depth);
            }
            p = e + 1;
        }
        curLineMap = outer;
    }

    // ---------------- symbols
    bool defineSym(const std::string& name, const Sym& s, int src) {
        auto it = syms.find(name);
        if (it != syms.end()) {
            const std::string where = refTo(it->second.src, src, "first defined on", "first defined at");
            errAt(src, "duplicate definition of '" + displayName(name) + "'" + where);
            return false;
        }
        syms[name] = s;
        symOrder.push_back(name);
        return true;
    }

    // Bind the waiting labels to the element idx.
    void flushLabels(int idx, bool atAddr) {
        for (const auto& n : pending) {
            Sym& s = syms[n];
            s.item = idx;
            s.atAddr = atAddr;
        }
        pending.clear();
    }

    int addItem(Item it) {
        items.push_back(std::move(it));
        return (int)items.size() - 1;
    }

    // ---------------- parsing a line
    void parseLine(int src, int depth) {
        try {
            const std::vector<Tok> toks = lex(lines[src].text);
            Cur c{toks, 0, scope};
            // label
            if ((c.is(T::Ident) || c.is(T::Local)) && toks[1].t == T::Colon) {
                const Tok& t = c.next();
                c.next();
                std::string name;
                if (t.t == T::Ident) {
                    if (isReservedName(t.s)) throw SyntaxError("name '" + t.s + "' is reserved and cannot be a label");
                    name = t.s;
                    scope = t.s;   // a new scope of local labels starts with this line
                } else {
                    name = scope + "@" + t.s;
                }
                Sym s;
                s.k = Sym::Label;
                s.src = src;
                if (defineSym(name, s, src)) pending.push_back(name);
            }
            if (c.is(T::End)) return;
            if (c.is(T::Directive)) { parseDirective(c, src, depth); return; }
            if (c.is(T::Ident)) { parseInstruction(c, src); return; }
            if (c.is(T::Colon)) throw SyntaxError("expected label name before ':'");
            throw SyntaxError("expected instruction or directive");
        } catch (const SyntaxError& e) {
            errAt(src, e.what());
            parseFailed = true;
        }
    }

    static void expectEnd(Cur& c) {
        if (c.is(T::Comma)) throw SyntaxError("too many operands");
        if (!c.is(T::End)) throw SyntaxError("unexpected text at end of line");
    }

    static std::vector<EP> parseExprList(Cur& c) {
        std::vector<EP> v;
        v.push_back(parseExpr(c));
        while (c.is(T::Comma)) { c.next(); v.push_back(parseExpr(c)); }
        return v;
    }

    void parseDirective(Cur& c, int src, int depth) {
        const std::string d = c.next().s;
        Item it;
        it.src = src;
        if (d == "org") {
            it.k = Item::Org;
            it.argN = parseExpr(c);
            expectEnd(c);
            const int idx = addItem(it);
            flushLabels(idx, false);
        } else if (d == "word" || d == "dw" || d == "byte" || d == "db") {
            it.k = (d == "word" || d == "dw") ? Item::Word : Item::Byte;
            if (c.is(T::End)) throw SyntaxError("." + d + " requires at least one value");
            it.exprs = parseExprList(c);
            expectEnd(c);
            const int idx = addItem(it);
            flushLabels(idx, true);
        } else if (d == "ascii" || d == "asciz") {
            it.k = Item::Ascii;
            do {
                if (!c.is(T::String)) throw SyntaxError("." + d + " expects a quoted string");
                const Tok& str = c.next();
                if (!str.err.empty()) throw SyntaxError(str.err);
                it.bytes += str.s;
                if (d == "asciz") it.bytes.push_back('\0');
                if (!c.is(T::Comma)) break;
                c.next();
            } while (true);
            expectEnd(c);
            const int idx = addItem(it);
            flushLabels(idx, true);
        } else if (d == "space" || d == "align") {
            it.k = d == "space" ? Item::Space : Item::Align;
            it.argN = parseExpr(c);
            if (c.is(T::Comma)) { c.next(); it.argFill = parseExpr(c); }
            expectEnd(c);
            const int idx = addItem(it);
            flushLabels(idx, d == "space");
        } else if (d == "reserve") {
            // reserve: the counter moves without output; the area is considered occupied (an overlap is an error)
            it.k = Item::Reserve;
            it.argN = parseExpr(c);
            expectEnd(c);
            const int idx = addItem(it);
            flushLabels(idx, true);
        } else if (d == "equ") {
            if (c.is(T::Local)) throw SyntaxError(".equ cannot define a local name");
            if (!c.is(T::Ident)) throw SyntaxError(".equ expects a constant name");
            const std::string name = c.next().s;
            if (isReservedName(name)) throw SyntaxError("name '" + name + "' is reserved");
            c.expect(T::Comma, "expected ',' after constant name");
            it.k = Item::Equ;
            it.equName = name;
            EP e = parseExpr(c);
            expectEnd(c);
            const int idx = addItem(it);
            Sym s;
            s.k = Sym::Equ;
            s.item = idx;
            s.src = src;
            s.expr = e;
            defineSym(name, s, src);
        } else if (d == "include") {
            if (!c.is(T::String)) throw SyntaxError(".include expects a quoted file name");
            const std::string fn = c.next().raw;   // the file name - bytes as in the source
            expectEnd(c);
            if (depth >= 16) throw SyntaxError(".include nesting too deep (more than 16)");
            const std::string path = (!fn.empty() && fn[0] == '/') ? fn : dirOf(files[lines[src].file]) + fn;
            processFile(path, src, depth + 1);
        } else if (d == "line") {
            lines[src].lineDir = true;
            // .line "file", N - all following lines up to the next .line have the place file:N
            // (for messages, the memory map and the listing); .line N - the same file, another number;
            // .line without arguments - the lines have their own places again. Until the end of the current file.
            if (c.is(T::End)) {
                if (!ignoreLine) { curLineMap->name = -1; curLineMap->line = 0; }
                return;
            }
            int idx = curLineMap->name;
            if (c.is(T::String)) {
                const std::string fn = c.next().raw;
                if (fn.empty()) throw SyntaxError(".line: file name is empty");
                if (!c.is(T::Comma)) throw SyntaxError(".line expects a quoted file name and a line number");
                c.next();
                idx = -1;
                for (size_t k = 0; k < lineNames.size(); k++) if (lineNames[k] == fn) { idx = (int)k; break; }
                if (idx < 0) { idx = (int)lineNames.size(); lineNames.push_back(fn); }
            } else if (!c.is(T::Number)) {
                throw SyntaxError(".line expects a quoted file name and a line number");
            }
            if (!c.is(T::Number)) throw SyntaxError(".line: line number must be a number");
            const int64_t n = c.next().v;
            expectEnd(c);
            if (n < 1 || n > 1000000) throw SyntaxError(".line: line number out of range 1..1000000");
            if (idx < 0 && !ignoreLine) throw SyntaxError(".line N without a file name needs an earlier .line \"file\", N in this file");
            if (!ignoreLine) { curLineMap->name = idx; curLineMap->line = (long)n; }
        } else {
            throw SyntaxError("unknown directive '." + d + "'");
        }
    }

    // ---------------- instructions
    static std::vector<Operand> parseOperands(Cur& c) {
        std::vector<Operand> ops;
        if (c.is(T::End)) return ops;
        ops.push_back(parseOperand(c));
        while (c.is(T::Comma)) { c.next(); ops.push_back(parseOperand(c)); }
        if (!c.is(T::End)) throw SyntaxError("expected ',' or end of line");
        return ops;
    }

    static void needCount(const std::vector<Operand>& ops, size_t n, const std::string& mn) {
        if (ops.size() != n)
            throw SyntaxError("wrong number of operands: " + mn + " expects " + std::to_string(n) +
                              ", got " + std::to_string(ops.size()));
    }

    static int needReg(const Operand& o, const std::string& what) {
        if (o.k != Operand::Reg) throw SyntaxError(what + " must be a register");
        return o.reg;
    }

    // The common mapping of an operand to R2/MOD/the second word (except #expr and a bare expression).
    static void mapGeneral(const Operand& o, Item& it) {
        switch (o.k) {
        case Operand::Reg: it.r2 = o.reg; it.mod = M_REG; break;
        case Operand::Ind: it.r2 = o.reg; it.mod = M_IND; break;
        case Operand::PostInc: it.r2 = o.reg; it.mod = M_POSTINC; break;
        case Operand::PreDec: it.r2 = o.reg; it.mod = M_PREDEC; break;
        case Operand::Abs: it.r2 = 0; it.mod = M_ABS; it.second = o.e; break;
        case Operand::Idx: it.r2 = o.reg; it.mod = M_IDX; it.second = o.e; break;
        case Operand::Sum: it.r2 = o.reg; it.mod = M_SUM; it.second = o.e; break;
        default: throw SyntaxError("internal error: mapGeneral");
        }
    }

    // Check a fixed encoding against the ISA rules and compute the length.
    static void finishFixed(Item& it, const std::string& mn, const Operand* o) {
        const Check ch = check(it.op, it.r1, it.mod == M_IMM4 ? 0 : it.r2, it.mod);
        if (ch.validity != Validity::Valid) {
            std::string form = o ? std::string(" ") + formName(o->k) : "";
            if (o && o->k != Operand::Imm && o->k != Operand::Abs && o->k != Operand::Bare)
                form += std::string(" (") + regName(o->reg) + ")";
            throw SyntaxError("invalid form" + form + " for " + mn + ": " + reasonText(ch.reason));
        }
        it.fixedWords = (int)instrWords(encode(it.op, it.r1, it.mod == M_IMM4 ? 0 : it.r2, it.mod));
    }

    void parseInstruction(Cur& c, int src) {
        const std::string mn = c.next().s;
        std::string MN = mn;
        for (auto& ch : MN) ch = (char)std::toupper((unsigned char)ch);
        const std::vector<Operand> ops = parseOperands(c);
        Item it;
        it.k = Item::Instr;
        it.src = src;

        const Pseudo ps = pseudoByName(mn);
        const int cond = condByName(mn);
        const int opn = opByName(mn);
        const int misc = miscByName(mn);

        auto fixedWord = [&](uint16_t w) { needCount(ops, 0, MN); it.fixedWord = true; it.word = w; };

        if (ps != Pseudo::None && ps != Pseudo::MOV && ps != Pseudo::LEA) {
            switch (ps) {
            case Pseudo::NOP: fixedWord(W_NOP); break;
            case Pseudo::RET: fixedWord(W_RET); break;
            case Pseudo::STI: fixedWord(W_STI); break;
            case Pseudo::CLI: fixedWord(W_CLI); break;
            case Pseudo::STC: fixedWord(W_STC); break;
            case Pseudo::CLC: fixedWord(W_CLC); break;
            case Pseudo::CMC: fixedWord(W_CMC); break;
            case Pseudo::PUSH: {
                needCount(ops, 1, MN);
                it.op = ST; it.r1 = needReg(ops[0], "operand of PUSH"); it.r2 = SP; it.mod = M_PREDEC;
                finishFixed(it, MN, nullptr);
                break;
            }
            case Pseudo::POP: {
                needCount(ops, 1, MN);
                it.op = LD; it.r1 = needReg(ops[0], "operand of POP"); it.r2 = SP; it.mod = M_POSTINC;
                finishFixed(it, MN, nullptr);
                break;
            }
            case Pseudo::INC: case Pseudo::DEC: {
                needCount(ops, 1, MN);
                it.op = ps == Pseudo::INC ? ADD : SUB;
                it.r1 = needReg(ops[0], "operand of " + MN); it.r2 = 1; it.mod = M_IMM4;
                it.fixedWords = 1;
                break;
            }
            default: break;
            }
        } else if (ps == Pseudo::LEA) {
            needCount(ops, 2, MN);
            it.op = LD;
            it.r1 = needReg(ops[0], "first operand of LEA");
            if (ops[1].k != Operand::Sum && ops[1].k != Operand::Idx)
                throw SyntaxError("LEA only accepts Rs+expr, Rs-expr or [Rs+expr]");
            Operand o = ops[1];
            o.k = Operand::Sum;
            mapGeneral(o, it);
            finishFixed(it, MN, &o);
        } else if (cond >= 0 || (opn >= 0 && opn == CALL)) {
            // --- jumps and call
            needCount(ops, 1, MN);
            const Operand& o = ops[0];
            const bool isCall = cond < 0;
            it.op = isCall ? CALL : JXX;
            it.r1 = isCall ? 0 : (unsigned)cond;
            if (o.k == Operand::Bare) {
                it.var = Item::BranchVar; it.target = o.e; it.isCall = isCall;
            } else if (o.k == Operand::Imm) {
                it.r2 = IP; it.mod = M_POSTINC; it.second = o.e;
                finishFixed(it, MN, &o);
            } else {
                mapGeneral(o, it);
                finishFixed(it, MN, &o);
            }
        } else if (misc >= 0) {
            // --- the combined opcode MISC: NEG, NOT, BSX (the operand is a register), IRET, WFI (no operands)
            it.op = MISC; it.r2 = (unsigned)misc; it.mod = M_REG;
            if (miscUsesR1((unsigned)misc)) {
                needCount(ops, 1, MN);
                it.r1 = needReg(ops[0], "operand of " + MN);
            } else {
                needCount(ops, 0, MN);
            }
            finishFixed(it, MN, nullptr);
        } else if (opn >= 0 || ps == Pseudo::MOV) {
            const unsigned op = ps == Pseudo::MOV ? (unsigned)LD : (unsigned)opn;
            it.op = op;
            switch (opClass(op)) {
            case OpClass::Load: case OpClass::Alu: {
                needCount(ops, 2, MN);
                it.r1 = needReg(ops[0], "first operand of " + MN);
                const Operand& o = ops[1];
                if (o.k == Operand::Bare)
                    throw SyntaxError("second operand of " + MN + " has no # or brackets: write a constant as #expr, "
                                      "a memory address as [expr]");
                if (o.k == Operand::Imm) { it.var = Item::ImmVar; it.target = o.e; }
                else { mapGeneral(o, it); finishFixed(it, MN, &o); }
                break;
            }
            case OpClass::Shift: case OpClass::ByteLoad: {
                needCount(ops, 2, MN);
                it.r1 = needReg(ops[0], "first operand of " + MN);
                const Operand& o = ops[1];
                if (o.k == Operand::Bare)
                    throw SyntaxError("second operand of " + MN + " has no # or brackets: write a constant as #expr, "
                                      "a memory address as [expr]");
                if (o.k == Operand::Imm) {
                    it.mod = M_IMM4; it.imm4 = o.e;
                    it.imm4What = opClass(op) == OpClass::Shift
                                      ? "shift count must be 0..15"
                                      : "byte load immediate must be 0..15 (use LD for larger values)";
                    finishFixed(it, MN, &o);
                } else {
                    mapGeneral(o, it);
                    finishFixed(it, MN, &o);
                }
                break;
            }
            case OpClass::Store: {
                needCount(ops, 2, MN);
                const Operand& m = ops[0];
                if (m.k != Operand::Ind && m.k != Operand::PostInc && m.k != Operand::PreDec &&
                    m.k != Operand::Abs && m.k != Operand::Idx)
                    throw SyntaxError("first operand of " + MN +
                                      " must be a memory address: [Rn], [Rn++], [--Rn], [expr] or [Rn+expr]");
                it.r1 = needReg(ops[1], "second operand of " + MN);
                mapGeneral(m, it);
                finishFixed(it, MN, &m);
                break;
            }
            case OpClass::Trap: {
                needCount(ops, 1, MN);
                const Operand& o = ops[0];
                if (o.k == Operand::Imm) {
                    it.mod = M_IMM4; it.imm4 = o.e; it.imm4What = "TRAP vector number must be 0..15";
                } else if (o.k == Operand::Reg) {
                    it.r2 = o.reg; it.mod = M_REG;
                } else {
                    throw SyntaxError("TRAP only accepts #n and Rn");
                }
                finishFixed(it, MN, &o);
                break;
            }
            default:
                throw SyntaxError("unknown mnemonic '" + mn + "'");
            }
        } else {
            throw SyntaxError("unknown mnemonic '" + mn + "'");
        }
        const int idx = addItem(it);
        flushLabels(idx, true);
    }

    // ---------------- expression evaluation
    Val symVal(Sym& s, bool noLabels) {
        Val r;
        if (s.k == Sym::Label) {
            if (noLabels) { r.ok = false; r.err = "value depends on a label"; return r; }
            if (s.item < 0) { r.ok = false; r.err = "label is not bound"; return r; }
            const Item& it = items[s.item];
            r.v = s.atAddr ? it.addr : it.lcBefore;
            return r;
        }
        if (s.visiting) { r.ok = false; r.err = "circular definition"; return r; }
        s.visiting = true;
        r = eval(s.expr, items[s.item].lcBefore, noLabels);
        s.visiting = false;
        return r;
    }

    Val eval(const EP& e, int64_t dollar, bool noLabels) {
        Val r;
        switch (e->k) {
        case Expr::Num: r.v = e->v; return r;
        case Expr::Dollar:
            if (noLabels) { r.ok = false; r.err = "value depends on the location counter '$'"; return r; }
            r.v = dollar;
            return r;
        case Expr::Sym: {
            auto it = syms.find(e->name);
            if (it == syms.end()) { r.ok = false; r.err = "undefined symbol '" + displayName(e->name) + "'"; return r; }
            r = symVal(it->second, noLabels);
            if (!r.ok && r.err == "circular definition") r.err = "circular definition of '" + e->name + "'";
            else if (!r.ok && r.err == "value depends on a label") r.err = "value depends on label '" + displayName(e->name) + "'";
            return r;
        }
        case Expr::Neg: case Expr::Not: case Expr::Lo: case Expr::Hi: {
            r = eval(e->a, dollar, noLabels);
            if (!r.ok) return r;
            if (e->k == Expr::Neg) r.v = -r.v;
            else if (e->k == Expr::Not) r.v = ~r.v;
            else if (e->k == Expr::Lo) r.v = r.v & 0xFF;
            else r.v = (r.v >> 8) & 0xFF;
            return r;
        }
        case Expr::Bin: {
            const Val a = eval(e->a, dollar, noLabels);
            if (!a.ok) return a;
            const Val b = eval(e->b, dollar, noLabels);
            if (!b.ok) return b;
            switch (e->op) {
            case '+': r.v = a.v + b.v; break;
            case '-': r.v = a.v - b.v; break;
            case '*': r.v = a.v * b.v; break;
            case '/':
                if (b.v == 0) { r.ok = false; r.err = "division by zero"; return r; }
                r.v = a.v / b.v;
                break;
            case '<': case '>':
                if (b.v < 0 || b.v > 31) { r.ok = false; r.err = "shift count out of range 0..31"; return r; }
                r.v = e->op == '<' ? (int64_t)((uint64_t)a.v << b.v) : (a.v >> b.v);
                break;
            case '&': r.v = a.v & b.v; break;
            case '^': r.v = a.v ^ b.v; break;
            case '|': r.v = a.v | b.v; break;
            }
            if (r.v > (int64_t)1 << 40 || r.v < -((int64_t)1 << 40)) { r.ok = false; r.err = "overflow in expression"; }
            return r;
        }
        }
        r.ok = false;
        r.err = "internal expression error";
        return r;
    }

    // ---------------- layout
    int64_t itemSize(const Item& it) const {
        switch (it.k) {
        case Item::Instr:
            if (it.fixedWord) return 2;
            if (it.var != Item::Fixed) return it.isLong ? 4 : 2;
            return 2 * it.fixedWords;
        case Item::Word: return 2 * (int64_t)it.exprs.size();
        case Item::Byte: return (int64_t)it.exprs.size();
        case Item::Ascii: return (int64_t)it.bytes.size();
        case Item::Space: case Item::Reserve: return it.n;
        default: return 0;
        }
    }

    void layout() {
        int64_t lc = 0;
        for (auto& it : items) {
            it.lcBefore = lc;
            it.padded = false;
            switch (it.k) {
            case Item::Org:
                lc = it.n;
                it.addr = lc;
                it.size = 0;
                break;
            case Item::Align: {
                const int64_t pad = it.n > 0 ? (it.n - lc % it.n) % it.n : 0;
                it.addr = lc;
                it.size = pad;
                lc += pad;
                break;
            }
            case Item::Instr: case Item::Word:
                if (lc & 1) { it.padded = true; lc++; }
                it.addr = lc;
                it.size = itemSize(it);
                lc += it.size;
                break;
            default:
                it.addr = lc;
                it.size = itemSize(it);
                lc += it.size;
                break;
            }
        }
    }

    // Whether a variable-size element fits the short form with the current layout.
    bool fitsShort(Item& it) {
        const Val v = eval(it.target, it.addr, false);
        if (!v.ok) return false;
        if (it.var == Item::ImmVar) return v.v >= 0 && v.v <= 15;
        if (v.v < -32768 || v.v > 65535 || (v.v & 1)) return false;
        const int d = (int16_t)(uint16_t)((uint16_t)v.v - (uint16_t)(it.addr + 2));
        const int k = d / 2;
        return it.isCall ? (k >= -8 && k <= 7) : (k >= -64 && k <= 63);
    }

    // ---------------- the final pass
    bool checkWord(const Val& v, int src, const char* what) {
        if (!v.ok) { errAt(src, v.err); return false; }
        if (v.v < -32768 || v.v > 65535) {
            errAt(src, std::string("value ") + std::to_string(v.v) + " does not fit in a " + what);
            return false;
        }
        return true;
    }

    void encodeInstr(Item& it) {
        auto& w = it.outWords;
        if (it.fixedWord) { w.push_back(it.word); return; }
        if (it.var == Item::ImmVar) {
            const Val v = eval(it.target, it.addr, false);
            if (!it.isLong) {
                w.push_back(encode(it.op, it.r1, (unsigned)v.v, M_IMM4));
            } else {
                if (!checkWord(v, it.src, "word")) return;
                w.push_back(encode(it.op, it.r1, IP, M_POSTINC));
                w.push_back((uint16_t)v.v);
            }
            return;
        }
        if (it.var == Item::BranchVar) {
            const Val t = eval(it.target, it.addr, false);
            if (!t.ok) { errAt(it.src, t.err); return; }
            if (t.v < -32768 || t.v > 65535) { errAt(it.src, "branch target outside address space: " + std::to_string(t.v)); return; }
            if (t.v & 1) { errAt(it.src, "branch target is odd (" + hex4(t.v) + ")"); return; }
            if (!it.isLong) {
                const int k = ((int16_t)(uint16_t)((uint16_t)t.v - (uint16_t)(it.addr + 2))) / 2;
                w.push_back(it.isCall ? encode(CALL, 0, (unsigned)k & 15, M_IMM4) : encodeJR(it.r1, k));
            } else {
                w.push_back(encode(it.op, it.r1, IP, M_SUM));
                w.push_back((uint16_t)((uint16_t)t.v - (uint16_t)(it.addr + 4)));
            }
            return;
        }
        unsigned r2 = it.r2;
        if (it.imm4) {
            const Val v = eval(it.imm4, it.addr, false);
            if (!v.ok) { errAt(it.src, v.err); return; }
            if (v.v < 0 || v.v > 15) { errAt(it.src, std::string(it.imm4What) + " (got " + std::to_string(v.v) + ")"); return; }
            r2 = (unsigned)v.v;
        }
        w.push_back(encode(it.op, it.r1, r2, it.mod));
        if (it.fixedWords == 2) {
            if (!it.second) { errAt(it.src, "internal error: missing second word"); return; }
            const Val v = eval(it.second, it.addr, false);
            if (!checkWord(v, it.src, "word")) return;
            w.push_back((uint16_t)v.v);
        }
    }

    void place(int idx, int64_t addr, const std::vector<uint8_t>& bytes) {
        bool reportedOverlap = false;
        for (size_t i = 0; i < bytes.size(); i++) {
            const int64_t a = addr + (int64_t)i;
            if (a > 0xFFFF) { errAt(items[idx].src, "output exceeds 64 KB address space"); return; }
            if (owner[a] >= 0) {
                if (!reportedOverlap) { overlapError(idx, owner[a], a); reportedOverlap = true; }
                continue;
            }
            owner[a] = idx;
            image[a] = bytes[i];
            if (minAddr < 0 || a < minAddr) minAddr = a;
            if (a > maxAddr) maxAddr = a;
        }
    }

    // A reservation (.reserve): the bytes occupy addresses but are not output. Inside the image (up to the last
    // output byte) they are zeros regardless of --fill; they do not affect minAddr/maxAddr.
    void reserve(int idx, int64_t addr, int64_t size) {
        bool reportedOverlap = false;
        for (int64_t i = 0; i < size; i++) {
            const int64_t a = addr + i;
            if (a > 0xFFFF) { errAt(items[idx].src, "reserved area exceeds 64 KB address space"); return; }
            if (owner[a] >= 0) {
                if (!reportedOverlap) { overlapError(idx, owner[a], a); reportedOverlap = true; }
                continue;
            }
            owner[a] = idx;
            image[a] = 0;
        }
    }

    void overlapError(int idx, int other, int64_t a) {
        const int src = items[other].src;
        const std::string where = refTo(src, items[idx].src, items[other].k == Item::Reserve ? "reserved by" : "already used by",
                                         items[other].k == Item::Reserve ? "reserved by" : "already used by");
        errAt(items[idx].src, std::string(items[idx].k == Item::Reserve ? "reserved area overlaps output" : "overlapping output") +
                                  " at address " + hex4(a) + where);
    }

    void emitAll() {
        for (int idx = 0; idx < (int)items.size(); idx++) {
            Item& it = items[idx];
            std::vector<uint8_t> bytes;
            if (it.padded) place(idx, it.addr - 1, {0});
            switch (it.k) {
            case Item::Instr:
                encodeInstr(it);
                for (uint16_t w : it.outWords) { bytes.push_back((uint8_t)w); bytes.push_back((uint8_t)(w >> 8)); }
                break;
            case Item::Word:
                for (const auto& e : it.exprs) {
                    const Val v = eval(e, it.addr, false);
                    uint16_t w = 0;
                    if (checkWord(v, it.src, "word")) w = (uint16_t)v.v;
                    it.outWords.push_back(w);
                    bytes.push_back((uint8_t)w); bytes.push_back((uint8_t)(w >> 8));
                }
                break;
            case Item::Byte:
                for (const auto& e : it.exprs) {
                    const Val v = eval(e, it.addr, false);
                    uint8_t b = 0;
                    if (!v.ok) errAt(it.src, v.err);
                    else if (v.v < -128 || v.v > 255) errAt(it.src, "value " + std::to_string(v.v) + " does not fit in a byte");
                    else b = (uint8_t)v.v;
                    bytes.push_back(b);
                }
                break;
            case Item::Ascii:
                for (char ch : it.bytes) bytes.push_back((uint8_t)ch);
                break;
            case Item::Space: case Item::Align: {
                uint8_t fill = 0;
                if (it.argFill) {
                    const Val v = eval(it.argFill, it.addr, false);
                    if (!v.ok) errAt(it.src, v.err);
                    else if (v.v < -128 || v.v > 255) errAt(it.src, "fill value " + std::to_string(v.v) + " does not fit in a byte");
                    else fill = (uint8_t)v.v;
                }
                bytes.assign((size_t)it.size, fill);
                break;
            }
            case Item::Reserve:
                reserve(idx, it.addr, it.size);
                break;
            default:
                break;
            }
            it.outBytes = bytes;
            if (!bytes.empty()) place(idx, it.addr, bytes);
        }
        // labels outside the address space
        for (const auto& n : symOrder) {
            Sym& s = syms[n];
            if (s.k != Sym::Label) continue;
            const Val v = symVal(s, false);
            if (v.ok && v.v > 0xFFFF) errAt(s.src, "address of label '" + displayName(n) + "' is outside 64 KB");
        }
        // .equ values: evaluation errors (including cycles) are reported on the definition line
        for (const auto& n : symOrder) {
            Sym& s = syms[n];
            if (s.k != Sym::Equ) continue;
            const Val v = symVal(s, false);
            if (!v.ok) errAt(s.src, ".equ " + n + ": " + v.err);
        }
    }

    // ---------------- the overall course of assembly
    bool finish() {
        if (parseFailed) return false;
        // labels at the end of the file
        if (!pending.empty()) {
            Item e;
            e.k = Item::End;
            const int idx = addItem(e);
            flushLabels(idx, false);
        }
        // the arguments of .org/.space/.reserve/.align (they do not depend on labels)
        bool argErr = false;
        for (auto& it : items) {
            if (it.k != Item::Org && it.k != Item::Space && it.k != Item::Reserve && it.k != Item::Align) continue;
            const Val v = eval(it.argN, 0, true);
            const char* dn = it.k == Item::Org ? ".org" : it.k == Item::Space ? ".space" : it.k == Item::Reserve ? ".reserve" : ".align";
            if (!v.ok) {
                std::string m = v.err;
                if (m.rfind("value depends", 0) == 0) m = std::string(dn) + " cannot depend on labels or '$': " + m;
                errAt(it.src, m);
                argErr = true;
                continue;
            }
            if (it.k == Item::Org && (v.v < 0 || v.v > 0xFFFF)) { errAt(it.src, ".org: address out of range 0..65535"); argErr = true; continue; }
            if (it.k == Item::Space && (v.v < 0 || v.v > 0x10000)) { errAt(it.src, ".space: size out of range 0..65536"); argErr = true; continue; }
            if (it.k == Item::Reserve && (v.v < 0 || v.v > 0x10000)) { errAt(it.src, ".reserve: size out of range 0..65536"); argErr = true; continue; }
            if (it.k == Item::Align && (v.v < 1 || v.v > 0x10000)) { errAt(it.src, ".align: value must be 1..65536"); argErr = true; continue; }
            it.n = v.v;
        }
        if (argErr) return false;

        // choosing sizes (sect. 10)
        size_t nVar = 0;
        for (const auto& it : items) if (it.k == Item::Instr && it.var != Item::Fixed) nVar++;
        for (size_t iter = 0; iter <= nVar + 1; iter++) {
            layout();
            bool changed = false;
            for (auto& it : items) {
                if (it.k != Item::Instr || it.var == Item::Fixed || it.isLong) continue;
                if (!fitsShort(it)) { it.isLong = true; changed = true; }
            }
            if (!changed) break;
        }
        layout();
        emitAll();
        return diags.empty();
    }
};

// ============================================================================
// The listing and the memory map
// ============================================================================

std::string Assembler::listing() {
    std::vector<int> lineItem(lines.size(), -1);
    for (int i = 0; i < (int)items.size(); i++)
        if (items[i].src >= 0) lineItem[items[i].src] = i;
    std::string out;
    char buf[64];
    // If the text contains .line, between the data and the source line there is a column with the place from .line
    // (empty for lines with their own places and for continuation lines).
    size_t placeW = 0;
    for (size_t li = 0; li < lines.size(); li++)
        if (lines[li].mapName >= 0) placeW = std::max(placeW, loc((int)li).size());
    std::string place;                      // the place of the current source line (the first line of the listing)
    auto row = [&](int64_t addr, const std::string& data, const std::string& text) {
        if (addr >= 0) std::snprintf(buf, sizeof buf, "%04X  ", (unsigned)(addr & 0xFFFF));
        else std::snprintf(buf, sizeof buf, "      ");
        std::string d = data;
        if (d.size() < 22) d.append(22 - d.size(), ' ');
        std::string p;
        if (placeW > 0) {
            p = text.empty() ? std::string() : place;
            p.append(placeW - p.size(), ' ');
            p += "  ";
        }
        out += buf + d + " " + p + text + "\n";
    };
    for (size_t li = 0; li < lines.size(); li++) {
        const std::string& text = lines[li].text;
        place = lines[li].mapName >= 0 && !lines[li].lineDir ? loc((int)li) : std::string();
        const int ii = lineItem[li];
        if (ii < 0) { row(-1, "", text); continue; }
        Item& it = items[ii];
        if (it.padded) row(it.addr - 1, "00", "; alignment");
        switch (it.k) {
        case Item::Instr: case Item::Word: {
            if (it.outWords.empty()) { row(it.addr, "", text); break; }
            for (size_t i = 0; i < it.outWords.size(); i += 4) {
                std::string d;
                for (size_t j = i; j < it.outWords.size() && j < i + 4; j++) {
                    std::snprintf(buf, sizeof buf, "%04X ", it.outWords[j]);
                    d += buf;
                }
                row(it.addr + 2 * (int64_t)i, d, i == 0 ? text : "");
            }
            break;
        }
        case Item::Byte: case Item::Ascii: case Item::Space: case Item::Align: {
            if (it.outBytes.empty()) { row(it.addr, "", text); break; }
            const size_t rows = (it.outBytes.size() + 7) / 8;
            for (size_t r = 0; r < rows && r < 4; r++) {
                std::string d;
                for (size_t j = r * 8; j < it.outBytes.size() && j < r * 8 + 8; j++) {
                    std::snprintf(buf, sizeof buf, "%02X ", it.outBytes[j]);
                    d += buf;
                }
                row(it.addr + 8 * (int64_t)r, d, r == 0 ? text : "");
            }
            if (rows > 4) row(-1, "... (" + std::to_string(it.outBytes.size()) + " bytes)", "");
            break;
        }
        case Item::Equ: {
            int64_t v = 0;
            std::string d = "= ?";
            if (symbolValue(it.equName, v)) d = "= " + hex4(v);
            row(-1, d, text);
            break;
        }
        case Item::Reserve:
            row(it.addr, "(" + std::to_string(it.size) + " bytes reserved)", text);
            break;
        case Item::Org:
            row(it.addr, "", text);
            break;
        default:
            row(-1, "", text);
            break;
        }
    }
    return out;
}

// The memory map (-m): a summary by kinds of memory, image objects by address with type and size,
// objects by size and constants. An object is a global label: from its address to the next
// global label or to the end of the contiguous occupied area; occupied bytes without a label are
// the object "(no label)". Local labels - indented lines inside their object.
// The type is the kinds of bytes in the object: code (instructions), data (.word, .byte, .ascii, .asciz, .space),
// bss (.reserve), pad (implicit alignment and .align); "-" - an object of zero size.
// The format is the same as in the debugger's JS assembler (checked by test_assembler.js). base - the start of the file.
std::string Assembler::memoryMap(int64_t base) {
    if (parseFailed || !diags.empty()) return "";
    enum : uint8_t { K_NONE, K_CODE, K_DATA, K_BSS, K_PAD };
    std::vector<uint8_t> kind(65536, K_NONE);
    std::vector<int> srcAt(65536, -1);
    for (const auto& it : items) {
        uint8_t k;
        switch (it.k) {
        case Item::Instr: k = K_CODE; break;
        case Item::Word: case Item::Byte: case Item::Ascii: case Item::Space: k = K_DATA; break;
        case Item::Reserve: k = K_BSS; break;
        case Item::Align: k = K_PAD; break;
        default: continue;
        }
        if (it.padded && it.addr >= 1) { kind[it.addr - 1] = K_PAD; srcAt[it.addr - 1] = it.src; }
        for (int64_t a = it.addr; a < it.addr + it.size && a <= 0xFFFF; a++) { kind[a] = k; srcAt[a] = it.src; }
    }
    auto where = [&](int src) {
        return loc(src);
    };
    auto padR = [](const std::string& t, size_t w) { return t.size() < w ? t + std::string(w - t.size(), ' ') : t; };
    auto padL = [](const std::string& t, size_t w) { return t.size() < w ? std::string(w - t.size(), ' ') + t : t; };
    auto typeOf = [&](int64_t a, int64_t e) {
        if (e <= a) return std::string("-");
        bool has[5] = {false, false, false, false, false};
        for (int64_t x = a; x < e; x++) has[kind[x]] = true;
        std::string t;
        if (has[K_CODE]) t += "code";
        if (has[K_DATA]) t += t.empty() ? "data" : "+data";
        if (has[K_BSS]) t += t.empty() ? "bss" : "+bss";
        return t.empty() ? std::string("pad") : t;
    };

    // symbols
    struct L { int64_t addr; std::string name; int src; int order; };
    std::vector<L> globals, locals, consts;
    for (size_t i = 0; i < symOrder.size(); i++) {
        const std::string& n = symOrder[i];
        Sym& sy = syms[n];
        const Val r = symVal(sy, false);
        const L l{r.ok ? r.v : 0, n, sy.src, (int)i};
        if (sy.k != Sym::Label) consts.push_back(l);
        else if (n.find('@') == std::string::npos) globals.push_back(l);
        else locals.push_back(l);
    }
    auto byAddr = [](const L& a, const L& b) { return a.addr != b.addr ? a.addr < b.addr : a.order < b.order; };
    std::sort(locals.begin(), locals.end(), byAddr);

    // objects: global labels and the starts of occupied areas without a label
    struct O { int64_t addr, end; std::string name; int src; int order; std::vector<size_t> locs; };
    std::vector<O> objs;
    for (const auto& g : globals) objs.push_back({g.addr, g.addr, g.name, g.src, g.order, {}});
    for (int64_t a = 0; a <= 0xFFFF; a++) {
        if (kind[a] == K_NONE || (a > 0 && kind[a - 1] != K_NONE)) continue;
        bool named = false;
        for (const auto& g : globals) if (g.addr == a) { named = true; break; }
        if (!named) objs.push_back({a, a, "(no label)", srcAt[a], -1, {}});
    }
    if (!locals.empty()) {
        int64_t first = INT64_MAX;
        for (const auto& o : objs) first = std::min(first, o.addr);
        if (locals[0].addr < first) objs.push_back({locals[0].addr, locals[0].addr, "(no label)", locals[0].src, -1, {}});
    }
    std::sort(objs.begin(), objs.end(), [](const O& a, const O& b) { return a.addr != b.addr ? a.addr < b.addr : a.order < b.order; });
    for (size_t i = 0; i < objs.size(); i++) {
        O& o = objs[i];
        const int64_t a = o.addr;
        if (a < 0 || a > 0xFFFF || kind[a] == K_NONE || (i + 1 < objs.size() && objs[i + 1].addr == a)) { o.end = a; continue; }
        int64_t e = a;
        while (e <= 0xFFFF && kind[e] != K_NONE) e++;
        if (i + 1 < objs.size() && objs[i + 1].addr < e) e = objs[i + 1].addr;
        o.end = e;
    }
    for (size_t k = 0; k < locals.size(); k++) {
        size_t j = 0;
        for (size_t i = 0; i < objs.size(); i++) if (objs[i].addr <= locals[k].addr) j = i;
        objs[j].locs.push_back(k);
    }

    // object lines
    // Alignment at the end of an object is a separate "(padding)" line: the size of an object is its
    // own bytes. An object consisting only of alignment remains an object of type pad.
    struct Row { int64_t addr, size; std::string type, name, src; bool top, pad; };
    std::vector<Row> rows;
    for (auto& o : objs) {
        const int64_t full = o.end;
        int64_t own = full;
        while (own > o.addr && kind[own - 1] == K_PAD) own--;
        if (own == o.addr) own = full;
        o.end = own;
        rows.push_back({o.addr, o.end - o.addr, typeOf(o.addr, o.end), o.name, where(o.src), true, false});
        for (size_t q = 0; q < o.locs.size(); q++) {
            const L& l = locals[o.locs[q]];
            int64_t e = l.addr;
            if (l.addr < o.end && !(q + 1 < o.locs.size() && locals[o.locs[q + 1]].addr == l.addr)) {
                e = o.end;
                if (q + 1 < o.locs.size() && locals[o.locs[q + 1]].addr < e) e = locals[o.locs[q + 1]].addr;
            }
            const std::string scopeName = l.name.substr(0, l.name.find('@'));
            const std::string shown = scopeName.empty() || scopeName == o.name ? l.name.substr(l.name.find('@')) : l.name;
            rows.push_back({l.addr, e - l.addr, typeOf(l.addr, e), "  " + shown, where(l.src), false, false});
        }
        if (own < full) rows.push_back({own, full - own, "pad", "(padding)", where(srcAt[own]), true, true});
    }

    // summary
    int64_t cnt[5] = {0, 0, 0, 0, 0}, lo = -1, hi = -1, gaps = 0;
    for (int64_t a = 0; a <= 0xFFFF; a++) {
        cnt[kind[a]]++;
        if (kind[a] != K_NONE) { if (lo < 0) lo = a; hi = a; }
    }
    if (maxAddr >= base) for (int64_t a = base; a <= maxAddr; a++) if (kind[a] == K_NONE) gaps++;
    const int64_t total = cnt[K_CODE] + cnt[K_DATA] + cnt[K_BSS] + cnt[K_PAD];
    auto num = [&](const char* what, int64_t v, const char* note) {
        return padR(what, 10) + padL(std::to_string(v), 6) + " bytes" + (*note ? std::string("   ") + note : "") + "\n";
    };
    std::string out = "; CPU16 memory map: " + (files.empty() ? std::string("?") : files[0]) + "\n\n== Summary ==\n";
    out += padR("occupied", 10) + (lo < 0 ? std::string("none") : hex4(lo) + ".." + hex4(hi)) + "\n";
    out += padR("file", 10) + (maxAddr >= base ? hex4(base) + ".." + hex4(maxAddr) + "   " + std::to_string(maxAddr - base + 1) + " bytes"
                                                 : std::string("empty")) + "\n";
    out += num("code", cnt[K_CODE], "");
    out += num("data", cnt[K_DATA], "");
    out += num("bss", cnt[K_BSS], ".reserve");
    out += num("padding", cnt[K_PAD], "alignment");
    out += num("gaps", gaps, "unused addresses inside the file");
    out += num("total", total, "code + data + bss + padding");

    // by source files: which files take how much (for example, the program and the library).
    // A file is the place of a line (by .line - the compiler source); the order is the first appearance in the text,
    // a file included again - on one line.
    std::vector<std::string> fileNames;
    std::vector<int> fileOfLine(lines.size(), 0);
    for (size_t li = 0; li < lines.size(); li++) {
        const std::string f = locFile((int)li);
        size_t k = 0;
        while (k < fileNames.size() && fileNames[k] != f) k++;
        if (k == fileNames.size()) fileNames.push_back(f);
        fileOfLine[li] = (int)k;
    }
    std::vector<std::array<int64_t, 5>> perFile(fileNames.size(), std::array<int64_t, 5>{0, 0, 0, 0, 0});
    for (int64_t a = 0; a <= 0xFFFF; a++)
        if (kind[a] != K_NONE && srcAt[a] >= 0) perFile[fileOfLine[srcAt[a]]][kind[a]]++;
    out += "\n== By source file ==\n  code    data     bss     pad   total  file\n";
    for (size_t f = 0; f < fileNames.size(); f++) {
        const auto& c = perFile[f];
        const int64_t t = c[K_CODE] + c[K_DATA] + c[K_BSS] + c[K_PAD];
        if (t == 0) continue;
        out += padL(std::to_string(c[K_CODE]), 6) + "  " + padL(std::to_string(c[K_DATA]), 6) + "  " + padL(std::to_string(c[K_BSS]), 6) +
               "  " + padL(std::to_string(c[K_PAD]), 6) + "  " + padL(std::to_string(t), 6) + "  " + fileNames[f] + "\n";
    }

    size_t typeW = 4, nameW = 4;
    for (const auto& r : rows) { typeW = std::max(typeW, r.type.size()); nameW = std::max(nameW, r.name.size()); }
    out += "\n== Objects by address ==\nstart  end      size  " + padR("type", typeW) + "  " + padR("name", nameW) + "  source\n";
    for (const auto& r : rows)
        out += hex4(r.addr) + "  " + (r.size > 0 ? hex4(r.addr + r.size - 1) : std::string("     ")) + "  " +
               padL(std::to_string(r.size), 6) + "  " + padR(r.type, typeW) + "  " + padR(r.name, nameW) + "  " + r.src + "\n";

    std::vector<const Row*> big;
    for (const auto& r : rows) if (r.top && !r.pad && r.size > 0) big.push_back(&r);
    std::stable_sort(big.begin(), big.end(), [](const Row* a, const Row* b) { return a->size > b->size; });
    if (!big.empty() && total > 0) {
        out += "\n== Objects by size ==\n  size   share  " + padR("type", typeW) + "  " + padR("name", nameW) + "  start\n";
        for (const Row* r : big) {
            const int64_t t = (r->size * 2000 + total) / (2 * total);    // tenths of a percent, rounded
            const std::string share = std::to_string(t / 10) + "." + std::to_string(t % 10) + "%";
            out += padL(std::to_string(r->size), 6) + "  " + padL(share, 6) + "  " + padR(r->type, typeW) + "  " + padR(r->name, nameW) +
                   "  " + hex4(r->addr) + "\n";
        }
    }

    std::stable_sort(consts.begin(), consts.end(), [](const L& a, const L& b) { return a.addr != b.addr ? a.addr < b.addr : a.name < b.name; });
    if (!consts.empty()) {
        size_t w = 4, vw = 5;
        auto val = [&](int64_t v) { return v >= -32768 && v <= 65535 ? hex4(v) : std::to_string(v); };
        for (const auto& c : consts) { w = std::max(w, c.name.size()); vw = std::max(vw, val(c.addr).size()); }
        out += "\n== Constants ==\n" + padR("value", vw) + "  " + padR("name", w) + "  source\n";
        for (const auto& c : consts) {
            out += padR(val(c.addr), vw) + "  " + padR(c.name, w) + "  " + where(c.src) + "\n";
        }
    }
    return out;
}

}  // namespace cpu16asm

// ============================================================================
// Command line
// ============================================================================

static bool parseNum(const char* s, int64_t& v) {
    char* end = nullptr;
    if (s[0] == '$') v = std::strtoll(s + 1, &end, 16);
    else v = std::strtoll(s, &end, 0);
    return end && *end == 0 && end != s;
}

static bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

int main(int argc, char** argv) {
    std::string in, outBin, outLst, outMap;
    int64_t base = 0, fill = 0;
    bool exec = false, noLine = false;
    for (int k = 1; k < argc; k++) {
        const std::string a = argv[k];
        auto need = [&](const char* what) -> const char* {
            if (k + 1 >= argc) { std::fprintf(stderr, "cpu16asm: %s requires an argument\n", what); std::exit(2); }
            return argv[++k];
        };
        if (a == "-o") outBin = need("-o");
        else if (a == "-l") outLst = need("-l");
        else if (a == "-m") outMap = need("-m");
        else if (a == "--no-line") noLine = true;
        else if (a == "--base") {
            if (!parseNum(need("--base"), base) || base < 0 || base > 0xFFFF) { std::fprintf(stderr, "cpu16asm: invalid --base\n"); return 2; }
        } else if (a == "--fill") {
            if (!parseNum(need("--fill"), fill) || fill < 0 || fill > 255) { std::fprintf(stderr, "cpu16asm: invalid --fill\n"); return 2; }
        } else if (a == "--exec") {
            exec = true;
        } else if (a == "-h" || a == "--help") {
            std::printf("Usage: cpu16asm [-o image.bin] [-l listing.lst] [-m memory.map] [--base ADDR] [--fill BYTE] [--exec] [--no-line] source.asm\n");
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "cpu16asm: unknown option %s\n", a.c_str());
            return 2;
        } else if (in.empty()) {
            in = a;
        } else {
            std::fprintf(stderr, "cpu16asm: unexpected argument %s\n", a.c_str());
            return 2;
        }
    }
    if (in.empty()) {
        std::fprintf(stderr, "Usage: cpu16asm [-o image.bin] [-l listing.lst] [-m memory.map] [--base ADDR] [--fill BYTE] [--exec] [--no-line] source.asm\n");
        return 2;
    }
    if (exec && base != 0) {
        std::fprintf(stderr, "cpu16asm: --exec requires --base 0 (an executable is an image from address $0000)\n");
        return 2;
    }
    if (outBin.empty()) {
        const size_t dot = in.find_last_of('.');
        const size_t slash = in.find_last_of('/');
        outBin = (dot != std::string::npos && (slash == std::string::npos || dot > slash) ? in.substr(0, dot) : in) + ".bin";
    }

    cpu16asm::Assembler as;
    as.ignoreLine = noLine;
    const bool ok = as.assembleFile(in);
    if (!ok) {
        std::fputs(as.formatDiags().c_str(), stderr);
        std::fprintf(stderr, "cpu16asm: %d error(s)\n", as.errorCount());
        return 1;
    }
    if (as.minAddr >= 0 && as.minAddr < base) {
        std::fprintf(stderr, "%s: error: output at address $%04X is below base $%04X\n", in.c_str(),
                     (unsigned)as.minAddr, (unsigned)base);
        return 1;
    }
    const std::vector<uint8_t> bin = as.binary(base, (uint8_t)fill);
    if (exec) {
        const std::vector<std::string> errs = as.checkExec();
        for (const auto& m : errs) std::fprintf(stderr, "%s: error: %s\n", in.c_str(), m.c_str());
        if (!errs.empty()) {
            std::fprintf(stderr, "cpu16asm: %d error(s)\n", (int)errs.size());
            return 1;
        }
    }
    if (!writeFile(outBin, std::string(bin.begin(), bin.end()))) {
        std::fprintf(stderr, "cpu16asm: cannot write %s\n", outBin.c_str());
        return 1;
    }
    if (!outLst.empty() && !writeFile(outLst, as.listing())) {
        std::fprintf(stderr, "cpu16asm: cannot write %s\n", outLst.c_str());
        return 1;
    }
    if (!outMap.empty() && !writeFile(outMap, as.memoryMap(base))) {
        std::fprintf(stderr, "cpu16asm: cannot write %s\n", outMap.c_str());
        return 1;
    }
    if (as.maxAddr >= 0)
        std::fprintf(stderr, "cpu16asm: %s: %zu bytes, addresses $%04X..$%04X\n", outBin.c_str(), bin.size(),
                     (unsigned)as.minAddr, (unsigned)as.maxAddr);
    else
        std::fprintf(stderr, "cpu16asm: %s: program is empty\n", outBin.c_str());
    return 0;
}

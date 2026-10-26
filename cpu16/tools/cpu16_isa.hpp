// cpu16_isa.hpp - description of the instruction set of the 16-bit processor (specification, revision 6).
//
// A common header for the emulator, the assembler and the disassembler. It holds only facts about the ISA:
// encodings, fields, instruction classes, validity and length rules, names (mnemonics, registers,
// conditions, pseudo-instructions) and helper functions for encoding and decoding a word.
// There is no execution logic, text parsing or output formatting here.
//
// Everything is in namespace cpu16, C++17 and the standard library only, no dependencies.
// Functions are constexpr wherever possible: tables and checks can be evaluated at compile time.

#pragma once

#include <cstdint>
#include <string_view>

namespace cpu16 {

constexpr int SPEC_REVISION = 6;

// ============================================================================
// Opcodes (sect. 5). The instruction word: OPCODE:5 R1:4 R2:4 R2MOD:3, most significant bit on the left.
// ============================================================================

enum Op : unsigned {
    LD = 0, LDZX = 1, LDSX = 2, XCHB = 3, ST = 4, STB = 5, JR = 6,
    ADD = 8, ADC = 9, SUB = 10, SBC = 11, AND = 12, OR = 13, XOR = 14,
    CMP = 16, TEST = 17, SHL = 18, SHR = 19, SAR = 20, ROR = 21,
    MISC = 22, JXX = 25, CALL = 26, TRAP = 27,
    MUL = 28, MULHU = 29, DIVU = 30, REMU = 31
    // 7, 15, 23, 24 - reserved (sect. 6.2 item 1)
};

// Subcodes of the combined opcode MISC (field R2, MOD = 0), revision 6.
enum Misc : unsigned { MISC_NEG = 0, MISC_NOT = 1, MISC_BSX = 2, MISC_IRET = 3, MISC_WFI = 4 };
constexpr unsigned MISC_COUNT = 5;   // 5..15 - reserved (sect. 6.2 item 5)
inline constexpr const char* MISC_NAMES[MISC_COUNT] = {"NEG", "NOT", "BSX", "IRET", "WFI"};
// For NEG, NOT, BSX the R1 field is a register; for IRET and WFI the R1 field must be 0.
constexpr bool miscUsesR1(unsigned sub) { return sub <= MISC_BSX; }
constexpr unsigned OPCODE_COUNT = 32;

// Special registers (sect. 1). R0..R11 are general purpose.
enum Reg : unsigned { LR = 12, SP = 13, IP = 14, FL = 15 };
constexpr unsigned REG_COUNT = 16;

// Flags (sect. 2, in the order of revision 4). Bits 5..15 of the FLAGS register are always 0.
enum Flag : uint16_t { ZF = 1, CF = 2, SF = 4, FIF = 8, OF = 16 };
constexpr uint16_t FLAGS_MASK = 0x001F;

// Addressing modes R2MOD (sect. 3).
enum Mod : unsigned {
    M_REG = 0,      // Rn
    M_IMM4 = 1,     // #0..15 (for Jxx/CALL - a signed offset of -8..+7 words)
    M_IND = 2,      // [Rn]
    M_POSTINC = 3,  // [Rn++]; with R2=IP - an immediate word
    M_PREDEC = 4,   // [--Rn]
    M_ABS = 5,      // [disp], R2 must be 0
    M_IDX = 6,      // [Rn+disp]
    M_SUM = 7       // Rn+disp
};

// Condition codes of Jxx and JR (field R1).
enum Cond : unsigned {
    C_ALWAYS = 0, C_Z = 1, C_NZ = 2, C_C = 3, C_NC = 4, C_A = 5, C_BE = 6, C_S = 7,
    C_NS = 8, C_O = 9, C_NO = 10, C_L = 11, C_GE = 12, C_G = 13, C_LE = 14,
    C_RESERVED = 15
};

// Interrupts (sect. 8).
constexpr uint16_t VEC_BASE = 0xFF00;
constexpr unsigned VEC_COUNT = 16;
constexpr unsigned VEC_ERRCMD = 8;

// ============================================================================
// Instruction classes and opcode properties
// ============================================================================

enum class OpClass : uint8_t {
    Reserved,     // reserved opcode
    Load,         // LD: R1 = operand value
    Alu,          // ADD..XOR, CMP, TEST: R1 op operand value
    Shift,        // SHL, SHR, SAR, ROR: the count comes from the operand
    ByteLoad,     // LDZX, LDSX
    Store,        // ST, STB, XCHB: the operand is a memory address, R1 is a register
    Branch,       // Jxx (R1 = condition), CALL: the operand is the target
    Trap,         // TRAP: the operand is the vector number
    Misc,         // MISC: the subcode is in R2 (NEG, NOT, BSX, IRET, WFI), MOD = 0
    ShortBranch   // JR: R1 = condition, R2:MOD = a 7-bit offset
};

struct OpInfo {
    const char* name;   // mnemonic (for Jxx - "Jxx", the real names are in COND_NAMES); nullptr for a reserved opcode
    OpClass cls;
    bool byteOp;        // byte instruction: the auto-increment step is 1
    bool writesR1;      // writes the result to R1
    bool setsFlags;     // may change ZF, CF, SF, OF (shifts - only when the count != 0)
};

inline constexpr OpInfo OP_INFO[OPCODE_COUNT] = {
    /* 0 */ {"LD", OpClass::Load, false, true, false},
    /* 1 */ {"LDZX", OpClass::ByteLoad, true, true, false},
    /* 2 */ {"LDSX", OpClass::ByteLoad, true, true, false},
    /* 3 */ {"XCHB", OpClass::Store, true, true, false},
    /* 4 */ {"ST", OpClass::Store, false, false, false},
    /* 5 */ {"STB", OpClass::Store, true, false, false},
    /* 6 */ {"JR", OpClass::ShortBranch, false, false, false},
    /* 7 */ {nullptr, OpClass::Reserved, false, false, false},
    /* 8 */ {"ADD", OpClass::Alu, false, true, true},
    /* 9 */ {"ADC", OpClass::Alu, false, true, true},
    /*10 */ {"SUB", OpClass::Alu, false, true, true},
    /*11 */ {"SBC", OpClass::Alu, false, true, true},
    /*12 */ {"AND", OpClass::Alu, false, true, true},
    /*13 */ {"OR", OpClass::Alu, false, true, true},
    /*14 */ {"XOR", OpClass::Alu, false, true, true},
    /*15 */ {nullptr, OpClass::Reserved, false, false, false},
    /*16 */ {"CMP", OpClass::Alu, false, false, true},
    /*17 */ {"TEST", OpClass::Alu, false, false, true},
    /*18 */ {"SHL", OpClass::Shift, false, true, true},
    /*19 */ {"SHR", OpClass::Shift, false, true, true},
    /*20 */ {"SAR", OpClass::Shift, false, true, true},
    /*21 */ {"ROR", OpClass::Shift, false, true, true},
    /*22 */ {"MISC", OpClass::Misc, false, true, true},
    /*23 */ {nullptr, OpClass::Reserved, false, false, false},
    /*24 */ {nullptr, OpClass::Reserved, false, false, false},
    /*25 */ {"Jxx", OpClass::Branch, false, false, false},
    /*26 */ {"CALL", OpClass::Branch, false, false, false},
    /*27 */ {"TRAP", OpClass::Trap, false, false, false},
    /*28 */ {"MUL", OpClass::Alu, false, true, true},
    /*29 */ {"MULHU", OpClass::Alu, false, true, true},
    /*30 */ {"DIVU", OpClass::Alu, false, true, true},
    /*31 */ {"REMU", OpClass::Alu, false, true, true},
};

constexpr const OpInfo& opInfo(unsigned op) { return OP_INFO[op & 31]; }
constexpr OpClass opClass(unsigned op) { return OP_INFO[op & 31].cls; }
constexpr bool isByteOp(unsigned op) { return OP_INFO[op & 31].byteOp; }

// The instruction uses the operand R2/MOD (and possibly a second word).
constexpr bool hasOperand(unsigned op) {
    switch (opClass(op)) {
    case OpClass::Load: case OpClass::Alu: case OpClass::Shift: case OpClass::ByteLoad:
    case OpClass::Store: case OpClass::Branch: case OpClass::Trap:
        return true;
    default:
        return false;
    }
}

// Groups of sect. 6.1.
constexpr bool isReadGroup(unsigned op) {   // "word read / value"
    const OpClass c = opClass(op);
    return c == OpClass::Load || c == OpClass::Alu || c == OpClass::Shift;
}
constexpr bool isControlGroup(unsigned op) { return op == JXX || op == CALL || op == TRAP; }

// The auto-increment/auto-decrement step.
constexpr unsigned autoStep(unsigned op) { return isByteOp(op) ? 1 : 2; }

// ============================================================================
// Word fields and encoding
// ============================================================================

constexpr unsigned fieldOp(uint16_t w) { return w >> 11; }
constexpr unsigned fieldR1(uint16_t w) { return (w >> 7) & 15; }
constexpr unsigned fieldR2(uint16_t w) { return (w >> 3) & 15; }
constexpr unsigned fieldMod(uint16_t w) { return w & 7; }
constexpr unsigned fieldJrOffset(uint16_t w) { return w & 0x7F; }   // JR: the R2:MOD fields

constexpr uint16_t encode(unsigned op, unsigned r1, unsigned r2, unsigned mod) {
    return (uint16_t)(((op & 31) << 11) | ((r1 & 15) << 7) | ((r2 & 15) << 3) | (mod & 7));
}
// JR: offsetWords in -64..+63 (the range check is up to the caller).
constexpr uint16_t encodeJR(unsigned cond, int offsetWords) {
    return (uint16_t)((JR << 11) | ((cond & 15) << 7) | ((unsigned)offsetWords & 0x7F));
}

// Sign extension of the fields.
constexpr int sext4(unsigned v) { return (int)((v ^ 0x8u) & 0xFu) - 0x8; }
constexpr int sext7(unsigned v) { return (int)((v ^ 0x40u) & 0x7Fu) - 0x40; }

// Instruction length in words from the first word (sect. 3). For instructions with no operand,
// JR and reserved opcodes - 1 word.
constexpr unsigned instrWords(uint16_t w) {
    const unsigned op = fieldOp(w), r2 = fieldR2(w), mod = fieldMod(w);
    if (!hasOperand(op)) return 1;
    return (mod >= M_ABS || (mod == M_POSTINC && r2 == IP)) ? 2 : 1;
}

// Targets of relative jumps. addr is the address of the first word of the instruction.
constexpr uint16_t jrTarget(uint16_t addr, uint16_t w) {          // JR
    return (uint16_t)(addr + 2 + 2 * sext7(fieldJrOffset(w)));
}
constexpr uint16_t shortTarget(uint16_t addr, unsigned r2) {      // Jxx/CALL with MOD=1
    return (uint16_t)(addr + 2 + 2 * sext4(r2));
}
constexpr uint16_t ipRelTarget(uint16_t addr, uint16_t disp) {    // MOD=7, R2=IP: IP+disp
    return (uint16_t)(addr + 4 + disp);
}

// ============================================================================
// Validity (sect. 6)
// ============================================================================

enum class Validity : uint8_t {
    Valid,          // valid encoding
    Reserved,       // reserved: always ERRCMD (sect. 6.2)
    Unpredictable   // constrained unpredictable (sect. 6.3); the emulator chooses ERRCMD
};

enum class Reason : uint8_t {
    None,
    // reserved (6.2)
    ReservedOpcode,     // item 1
    ReservedCond,       // item 2: Jxx/JR with condition 1111
    ReservedMod,        // item 3: MOD from the "reserved" column of table 6.1
    NonZeroField,       // item 4: a field that must be zero is non-zero
    ReservedSubcode,    // item 5: MISC subcode 5..15
    // constrained unpredictable (6.3)
    FlagsAddressing,    // item 1: R2=R15 with MOD 2,3,4,6,7
    PredecIP,           // item 2: [--IP]
    ImmediateViaIP,     // item 3: [IP++] with LDZX, LDSX, ST, STB, XCHB
    IndirectIPStore,    // item 4: [IP] with ST, STB, XCHB
    ByteAutoSpIp,       // item 5: a byte instruction with [SP++]/[--SP]/[IP++]/[--IP]
    BranchAutoinc       // item 6: Jxx/CALL/TRAP with MOD=3 when R2 != IP
};

struct Check {
    Validity validity;
    Reason reason;
};

// Classification of an encoding by its first word. The reserved reasons are checked
// before the constrained unpredictable ones (an encoding that falls under both is reserved).
constexpr Check check(unsigned op, unsigned r1, unsigned r2, unsigned mod) {
    using V = Validity;
    using R = Reason;
    const OpClass cls = opClass(op);

    // --- 6.2: reserved
    if (cls == OpClass::Reserved) return {V::Reserved, R::ReservedOpcode};
    if (cls == OpClass::ShortBranch)
        return r1 == C_RESERVED ? Check{V::Reserved, R::ReservedCond} : Check{V::Valid, R::None};
    if (cls == OpClass::Misc) {
        if (mod != 0) return {V::Reserved, R::NonZeroField};
        if (r2 >= MISC_COUNT) return {V::Reserved, R::ReservedSubcode};
        if (!miscUsesR1(r2) && r1 != 0) return {V::Reserved, R::NonZeroField};
        return {V::Valid, R::None};
    }

    if (op == JXX && r1 == C_RESERVED) return {V::Reserved, R::ReservedCond};
    if ((op == CALL || op == TRAP) && r1 != 0) return {V::Reserved, R::NonZeroField};
    if (mod == M_ABS && r2 != 0) return {V::Reserved, R::NonZeroField};

    const bool store = (cls == OpClass::Store);
    const bool byteLoad = (cls == OpClass::ByteLoad);
    const bool ctrl = isControlGroup(op);
    if (store && (mod == M_REG || mod == M_IMM4 || mod == M_SUM)) return {V::Reserved, R::ReservedMod};
    if (byteLoad && mod == M_SUM) return {V::Reserved, R::ReservedMod};
    if (ctrl && mod == M_PREDEC) return {V::Reserved, R::ReservedMod};

    // --- 6.3: constrained unpredictable
    if (r2 == FL && (mod == M_IND || mod == M_POSTINC || mod == M_PREDEC || mod == M_IDX || mod == M_SUM))
        return {V::Unpredictable, R::FlagsAddressing};
    if (r2 == IP && mod == M_PREDEC) return {V::Unpredictable, R::PredecIP};
    if (r2 == IP && mod == M_POSTINC && (store || byteLoad)) return {V::Unpredictable, R::ImmediateViaIP};
    if (r2 == IP && mod == M_IND && store) return {V::Unpredictable, R::IndirectIPStore};
    if (isByteOp(op) && (mod == M_POSTINC || mod == M_PREDEC) && (r2 == SP || r2 == IP))
        return {V::Unpredictable, R::ByteAutoSpIp};
    if (ctrl && mod == M_POSTINC && r2 != IP) return {V::Unpredictable, R::BranchAutoinc};

    return {V::Valid, R::None};
}

constexpr Check check(uint16_t w) { return check(fieldOp(w), fieldR1(w), fieldR2(w), fieldMod(w)); }
constexpr bool valid(unsigned op, unsigned r1, unsigned r2, unsigned mod) {
    return check(op, r1, r2, mod).validity == Validity::Valid;
}
constexpr bool valid(uint16_t w) { return check(w).validity == Validity::Valid; }

// The reason text for diagnostics (assembler, disassembler, trace).
constexpr const char* reasonText(Reason r) {
    switch (r) {
    case Reason::None: return "valid encoding";
    case Reason::ReservedOpcode: return "reserved opcode (spec 6.2.1)";
    case Reason::ReservedCond: return "condition code 1111 is reserved (spec 6.2.2)";
    case Reason::ReservedMod: return "addressing mode is reserved for this instruction (spec 6.2.3)";
    case Reason::NonZeroField: return "unused field must be zero (spec 6.2.4)";
    case Reason::ReservedSubcode: return "reserved MISC subcode (spec 6.2.5)";
    case Reason::FlagsAddressing: return "FLAGS cannot be used in an addressing mode (spec 6.3.1)";
    case Reason::PredecIP: return "[--IP] is not allowed (spec 6.3.2)";
    case Reason::ImmediateViaIP: return "immediate operand via IP is not allowed for this instruction (spec 6.3.3)";
    case Reason::IndirectIPStore: return "store to [IP] is not allowed (spec 6.3.4)";
    case Reason::ByteAutoSpIp: return "byte instruction with SP or IP auto-increment/decrement (spec 6.3.5)";
    case Reason::BranchAutoinc: return "branch with [Rn++] where Rn != IP is not allowed (spec 6.3.6)";
    }
    return "?";
}

// ============================================================================
// A full decode of the first word (convenient for the disassembler and the debugger)
// ============================================================================

struct Decoded {
    uint16_t word;
    unsigned op, r1, r2, mod;
    OpClass cls;
    Check chk;
    unsigned words;      // instruction length in words
    bool immViaIP;       // the second word is an immediate value (MOD=3, R2=IP)
    bool hasDisp;        // the second word is a disp/address (MOD 5, 6, 7)
};

constexpr Decoded decode(uint16_t w) {
    const unsigned op = fieldOp(w), r1 = fieldR1(w), r2 = fieldR2(w), mod = fieldMod(w);
    const bool operand = hasOperand(op);
    return Decoded{w, op, r1, r2, mod, opClass(op), check(op, r1, r2, mod), instrWords(w),
                   operand && mod == M_POSTINC && r2 == IP,
                   operand && mod >= M_ABS};
}

// ============================================================================
// Names: registers, conditions, mnemonics
// ============================================================================

// The canonical register name for output: R0..R11, LR, SP, IP, FLAGS.
inline constexpr const char* REG_NAMES[REG_COUNT] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "R9", "R10", "R11", "LR", "SP", "IP", "FLAGS"};
constexpr const char* regName(unsigned n) { return REG_NAMES[n & 15]; }

// Names of conditional jumps: the main one and a synonym (nullptr if there is no synonym).
struct CondName {
    const char* primary;
    const char* alias;
};
inline constexpr CondName COND_NAMES[16] = {
    {"JMP", nullptr}, {"JZ", "JE"}, {"JNZ", "JNE"}, {"JC", "JB"},
    {"JNC", "JAE"}, {"JA", nullptr}, {"JBE", nullptr}, {"JS", nullptr},
    {"JNS", nullptr}, {"JO", nullptr}, {"JNO", nullptr}, {"JL", nullptr},
    {"JGE", nullptr}, {"JG", nullptr}, {"JLE", nullptr}, {nullptr, nullptr}};
constexpr const char* condName(unsigned c) { return COND_NAMES[c & 15].primary; }

// Case-insensitive comparison (ASCII).
constexpr char asciiUpper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
constexpr bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (asciiUpper(a[i]) != asciiUpper(b[i])) return false;
    return true;
}

// A register by name: R0..R15, LR, SP, IP, FLAGS (case-insensitive). -1 if it is not a register.
constexpr int regByName(std::string_view s) {
    if (iequals(s, "LR")) return LR;
    if (iequals(s, "SP")) return SP;
    if (iequals(s, "IP")) return IP;
    if (iequals(s, "FLAGS")) return FL;
    if (s.size() < 2 || s.size() > 3 || asciiUpper(s[0]) != 'R') return -1;
    int n = 0;
    for (size_t i = 1; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    if (s.size() == 3 && s[1] == '0') return -1;   // "R05" is not allowed
    return n < 16 ? n : -1;
}

// The opcode by the mnemonic of a real instruction (except conditional jumps: see condByName).
// -1 if not found.
constexpr int opByName(std::string_view s) {
    for (unsigned op = 0; op < OPCODE_COUNT; op++) {
        const char* n = OP_INFO[op].name;
        if (!n || op == JXX || op == JR || op == MISC) continue;   // these opcodes have different mnemonics
        if (iequals(s, n)) return (int)op;
    }
    return -1;
}

// The MISC subcode by mnemonic (NEG, NOT, BSX, IRET, WFI). -1 if not found.
constexpr int miscByName(std::string_view s) {
    for (unsigned i = 0; i < MISC_COUNT; i++)
        if (iequals(s, MISC_NAMES[i])) return (int)i;
    return -1;
}

// The condition code by the jump mnemonic (JMP, JZ, JE, ...). -1 if not found.
constexpr int condByName(std::string_view s) {
    for (unsigned c = 0; c < 15; c++) {
        if (iequals(s, COND_NAMES[c].primary)) return (int)c;
        if (COND_NAMES[c].alias && iequals(s, COND_NAMES[c].alias)) return (int)c;
    }
    return -1;
}

// ============================================================================
// Pseudo-instructions (the assembler reference, sect. 7.4)
// ============================================================================

enum class Pseudo : uint8_t {
    None, MOV, LEA, NOP, RET, PUSH, POP, INC, DEC, STI, CLI, STC, CLC, CMC
};
inline constexpr const char* PSEUDO_NAMES[] = {
    nullptr, "MOV", "LEA", "NOP", "RET", "PUSH", "POP", "INC", "DEC", "STI", "CLI", "STC", "CLC", "CMC"};
constexpr unsigned PSEUDO_COUNT = sizeof(PSEUDO_NAMES) / sizeof(PSEUDO_NAMES[0]);

constexpr Pseudo pseudoByName(std::string_view s) {
    for (unsigned i = 1; i < PSEUDO_COUNT; i++)
        if (iequals(s, PSEUDO_NAMES[i])) return (Pseudo)i;
    return Pseudo::None;
}
constexpr const char* pseudoName(Pseudo p) { return PSEUDO_NAMES[(unsigned)p]; }

// Expansions of pseudo-instructions without a register parameter.
constexpr uint16_t W_NOP = encode(LD, 0, 0, M_REG);                     // LD R0, R0
constexpr uint16_t W_RET = encode(LD, IP, LR, M_REG);                   // LD IP, LR
constexpr uint16_t W_STI = encode(OR, FL, FIF, M_IMM4);                 // OR FLAGS, #$08
constexpr uint16_t W_CLI = encode(AND, FL, 0xF & ~FIF, M_IMM4);         // AND FLAGS, #$07 (as a side effect OF=0)
constexpr uint16_t W_STC = encode(OR, FL, CF, M_IMM4);                  // OR FLAGS, #$02
constexpr uint16_t W_CLC = encode(AND, FL, 0xF & ~CF, M_IMM4);          // AND FLAGS, #$0D (as a side effect OF=0)
constexpr uint16_t W_CMC = encode(XOR, FL, CF, M_IMM4);                 // XOR FLAGS, #$02

// Instructions of the combined opcode MISC.
constexpr uint16_t encodeMisc(unsigned sub, unsigned r1 = 0) { return encode(MISC, r1, sub, M_REG); }
constexpr uint16_t W_IRET = encodeMisc(MISC_IRET);
constexpr uint16_t W_WFI = encodeMisc(MISC_WFI);

// Expansions with a register.
constexpr uint16_t encodePush(unsigned r) { return encode(ST, r, SP, M_PREDEC); }    // ST [--SP], r
constexpr uint16_t encodePop(unsigned r) { return encode(LD, r, SP, M_POSTINC); }    // LD r, [SP++]
constexpr uint16_t encodeInc(unsigned r) { return encode(ADD, r, 1, M_IMM4); }       // ADD r, #1
constexpr uint16_t encodeDec(unsigned r) { return encode(SUB, r, 1, M_IMM4); }       // SUB r, #1

// Recognition of a one-word pseudo-instruction from the word (for the disassembler).
// MOV and LEA are not recognized: that is a matter of output style (LD Rd,src and LD Rd,Rs+d).
struct PseudoMatch {
    Pseudo kind;
    unsigned reg;   // for PUSH, POP, INC, DEC
};
constexpr PseudoMatch matchPseudo(uint16_t w) {
    switch (w) {
    case W_NOP: return {Pseudo::NOP, 0};
    case W_RET: return {Pseudo::RET, 0};
    case W_STI: return {Pseudo::STI, 0};
    case W_CLI: return {Pseudo::CLI, 0};
    case W_STC: return {Pseudo::STC, 0};
    case W_CLC: return {Pseudo::CLC, 0};
    case W_CMC: return {Pseudo::CMC, 0};
    default: break;
    }
    const unsigned r = fieldR1(w);
    if (w == encodePush(r)) return {Pseudo::PUSH, r};
    if (w == encodePop(r)) return {Pseudo::POP, r};
    if (w == encodeInc(r)) return {Pseudo::INC, r};
    if (w == encodeDec(r)) return {Pseudo::DEC, r};
    return {Pseudo::None, 0};
}

// ============================================================================
// Self-check against the control encodings (the assembler reference, sect. 14)
// ============================================================================

static_assert(encode(LD, 1, 2, M_REG) == 0x0090);
static_assert(encode(LD, 3, 5, M_IMM4) == 0x01A9);
static_assert(encode(ST, 0, 2, M_IDX) == 0x2016);
static_assert(W_NOP == 0x0000 && W_RET == 0x0760);
static_assert(W_STI == 0x6FC1 && W_CLI == 0x67B9 && W_STC == 0x6F91 && W_CLC == 0x67E9 && W_CMC == 0x7791);
static_assert(encodePush(0) == 0x206C && encodePop(0) == 0x006B);
static_assert(encodeJR(C_ALWAYS, 3) == 0x3003 && encodeJR(C_NZ, -2) == 0x317E);
static_assert(encode(CALL, 0, 3, M_IMM4) == 0xD019 && encode(TRAP, 0, 9, M_IMM4) == 0xD849);
static_assert(instrWords(0x01F3) == 2 && instrWords(0x01A9) == 1 && instrWords(0x3007) == 1);
static_assert(jrTarget(200, encodeJR(C_ALWAYS, -64)) == 74 && jrTarget(0, encodeJR(C_ALWAYS, 63)) == 128);
static_assert(valid(W_STI) && !valid(encode(ST, 0, 0, M_REG)) && !valid(encodeMisc(MISC_WFI, 1)));
static_assert(encodeMisc(MISC_NEG, 2) == 0xB100 && W_IRET == 0xB018 && W_WFI == 0xB020);
static_assert(check(encode(MISC, 0, 5, M_REG)).reason == Reason::ReservedSubcode &&
              check(encode(MISC, 3, MISC_NOT, M_IND)).reason == Reason::NonZeroField && valid(encodeMisc(MISC_BSX, 15)));
static_assert(valid(encode(MUL, 1, IP, M_POSTINC)) && valid(encode(DIVU, 0, 5, M_IMM4)) && !valid(encode(23, 0, 0, 0)));
static_assert(miscByName("wfi") == MISC_WFI && opByName("neg") == -1 && opByName("REMU") == REMU);
static_assert(check(encode(LD, 0, FL, M_IND)).validity == Validity::Unpredictable);
static_assert(check(encode(ST, 0, IP, M_PREDEC)).validity == Validity::Unpredictable);
static_assert(check(encode(JXX, 0, 3, M_PREDEC)).validity == Validity::Reserved);
static_assert(regByName("flags") == FL && regByName("r15") == 15 && regByName("R16") == -1 && regByName("R05") == -1);
static_assert(condByName("je") == C_Z && condByName("JAE") == C_NC && opByName("ld") == LD && opByName("JR") == -1);
static_assert(matchPseudo(encodePush(5)).kind == Pseudo::PUSH && matchPseudo(encodePush(5)).reg == 5);

}  // namespace cpu16

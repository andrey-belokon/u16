// cpu16asm_tests.cpp - assembler tests.
// Build (from the project root): g++ -std=c++17 -O2 -Itools -o build/cpu16asm_tests tests/cpu16asm_tests.cpp
//   (cpu16_isa.hpp, cpu16.cpp, cpu16asm.cpp must lie next to it)
// Run: ./cpu16asm_tests
//
// What is checked:
//   1. the reference encodings from the assembler reference (sect. 14);
//   2. the examples from the reference: they assemble and give the expected result in the emulator;
//   3. form selection (sect. 10): the limits of JR and the short CALL, cascading lengthening, #expr;
//   4. alignment and label binding (sect. 9);
//   5. diagnostics: every error category, the line number, several errors at a time;
//   6. .include, local labels;
//   7. .reserve and the check of the CX16 executable header (--exec), the example examples/calc_exec.asm.

#define main cpu16_emulator_main
#include "cpu16.cpp"
#undef main
#define main cpu16asm_main
#include "cpu16asm.cpp"
#undef main

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, what)                                                              \
    do {                                                                               \
        if (cond) g_pass++;                                                            \
        else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } \
    } while (0)

// Assemble the text; on an error print the diagnostics.
struct Built {
    cpu16asm::Assembler as;
    bool ok = false;
};
static void build(Built& b, const std::string& src, bool expectOk = true) {
    b.ok = b.as.assembleText("test.asm", src);
    // on a failure only the diagnostics are printed (in English), without the source text
    if (expectOk && !b.ok) std::printf("--- unexpected errors:\n%s", b.as.formatDiags().c_str());
}

static uint16_t wordAt(const cpu16asm::Assembler& as, int a) {
    return (uint16_t)(as.image[a] | (as.image[a + 1] << 8));
}

// The processor for the tests: output to port $FF20 is collected into a string.
struct TestCpu : cpu16::Cpu {
    std::string out;
    void write8(uint16_t a, uint8_t v) override {
        if (a == 0xFF20) { out.push_back((char)v); return; }
        mem[a] = v;
    }
};

static void load(TestCpu& c, const cpu16asm::Assembler& as) {
    for (int a = 0; a < 65536; a++)
        if (as.owner[a] >= 0) c.mem[a] = as.image[a];
}

static int64_t sym(cpu16asm::Assembler& as, const std::string& n) {
    int64_t v = -1;
    if (!as.symbolValue(n, v)) std::printf("no symbol %s\n", n.c_str());
    return v;
}

// ---------------------------------------------------------------------------
// 1. Reference encodings (the reference, sect. 14)
// ---------------------------------------------------------------------------
static void testGolden() {
    struct G { const char* src; int at; std::vector<uint16_t> words; };
    const std::vector<G> cases = {
        {"NOP", 0, {0x0000}},
        {"LD R1, R2", 0, {0x0090}},
        {"LD R3, #5", 0, {0x01A9}},
        {"LD R3, #$1234", 0, {0x01F3, 0x1234}},
        {"ADD R0, [R1++]", 0, {0x400B}},
        {"ST [R2+4], R0", 0, {0x2016, 0x0004}},
        {"STB [$FF20], R1", 0, {0x2885, 0xFF20}},
        {"XCHB [lock], R1\n.org $40\nlock: .byte 0", 0, {0x1885, 0x0040}},
        {"PUSH R0", 0, {0x206C}},
        {"POP R0", 0, {0x006B}},
        {"RET", 0, {0x0760}},
        {"SHL R0, #4", 0, {0x9021}},
        {"ROR R2, #8", 0, {0xA941}},
        {"NEG R2", 0, {0xB100}},
        {"JMP t\nNOP\nNOP\nNOP\nt: NOP", 0, {0x3003}},
        {"NOP\nt: NOP\nJNZ t", 4, {0x317E}},
        {"CALL t\nNOP\nNOP\nNOP\nt: NOP", 0, {0xD019}},
        {"JZ far\n.org $400\nfar: NOP", 0, {0xC8F7, 0x03FC}},
        {"JMP #$1234", 0, {0xC873, 0x1234}},
        {"JMP [R0+tbl]\n.org $80\ntbl: .word 0", 0, {0xC806, 0x0080}},
        {"CALL far\n.org $400\nfar: NOP", 0, {0xD077, 0x03FC}},
        {"LEA R0, IP+$10", 0, {0x0077, 0x0010}},
        {"STI", 0, {0x6FC1}},
        {"CLI", 0, {0x67B9}},
        {"AND FLAGS, #$FFF7", 0, {0x67F3, 0xFFF7}},
        {"STC", 0, {0x6F91}},
        {"CLC", 0, {0x67E9}},
        {"CMC", 0, {0x7791}},
        {"TEST FLAGS, #$08", 0, {0x8FC1}},
        {"TRAP #9", 0, {0xD849}},
        {"IRET", 0, {0xB018}},
        {"WFI", 0, {0xB020}},
        // revision 6: the combined MISC opcode and unsigned multiplication/division
        {"NOT R1\nBSX R3\nNEG FLAGS", 0, {0xB088, 0xB190, 0xB780}},
        {"MUL R0, R1", 0, {0xE008}},
        {"MULHU R2, #5", 0, {0xE929}},
        {"DIVU R3, #1000", 0, {0xF1F3, 0x03E8}},
        {"REMU R4, [R5+2]", 0, {0xFA2E, 0x0002}},
        {"mul r0, r1", 0, {0xE008}},
        // strings are recoded into the machine encoding ("CPU16 Machine", sect. 5.1)
        {".ascii \"Ёё═→\"", 0, {0xA3B3, 0x1AA0}},
        {".byte 'Я', 'a', '\\xD0', '☺'", 0, {0x61DF, 0x01D0}},
        // synonyms and letter case
        {"ld r1, r2", 0, {0x0090}},
        {"MOV R1, R2", 0, {0x0090}},
        {"JE t\nt: NOP", 0, {0x3080}},
        {"PUSH LR\nPOP IP", 0, {0x266C, 0x076B}},
        {"INC R3\nDEC R3", 0, {0x4189, 0x5189}},
        {"LEA R1, [R2+4]\nLEA R1, R2-4", 0, {0x0097, 0x0004, 0x0097, 0xFFFC}},
    };
    for (const auto& g : cases) {
        Built b;
        build(b, g.src);
        bool ok = b.ok;
        for (size_t i = 0; ok && i < g.words.size(); i++) ok = wordAt(b.as, g.at + 2 * (int)i) == g.words[i];
        if (!ok && b.ok) {
            std::printf("  expected:");
            for (auto w : g.words) std::printf(" %04X", w);
            std::printf("  got:");
            for (size_t i = 0; i < g.words.size(); i++) std::printf(" %04X", wordAt(b.as, g.at + 2 * (int)i));
            std::printf("\n");
        }
        CHECK(ok, g.src);
    }
}

// ---------------------------------------------------------------------------
// 2. Examples from the reference (sect. 13) with a check in the emulator
// ---------------------------------------------------------------------------
static void testExamples() {
    {   // 13.1 printing a string
        Built b;
        build(b, R"(
        .equ    CONSOLE, $FF20
        .org    0
start:  LD      SP, #$FF00
        LD      R0, #msg
@next:  LDZX    R1, [R0++]
        CMP     R1, #0
        JZ      @done
        STB     [CONSOLE], R1
        JMP     @next
@done:  WFI
msg:    .asciz  "Hello!\n"
)");
        TestCpu c; load(c, b.as); c.run(10000);
        CHECK(b.ok && c.out == "Hello!\n", "13.1 print string");
    }
    {   // 13.2 the sum of an array
        Built b;
        build(b, R"(
        LD      R0, #table       ; pointer
        LD      R1, #5           ; counter
        LD      R2, #0           ; sum
loop:   ADD     R2, [R0++]       ; sum += *p++ (step 2)
        DEC     R1
        JNZ     loop
        WFI
table:  .word   10, 20, 30, 40, 50
)");
        TestCpu c; load(c, b.as); c.run(10000);
        CHECK(b.ok && c.r[2] == 150, "13.2 array sum");
    }
    {   // 13.3 subroutines
        Built b;
        build(b, R"(
        LD      SP, #$FF00
        LD      R0, #7
        CALL    twice_plus_one
        WFI

twice_plus_one:
        PUSH    LR               ; nested call: save LR
        ADD     R0, R0
        CALL    inc_r0
        POP     LR
        RET

inc_r0: INC     R0
        RET                      ; leaf routine: LR is not saved
)");
        TestCpu c; load(c, b.as); c.run(10000);
        CHECK(b.ok && c.r[0] == 15 && c.r[cpu16::SP] == 0xFF00, "13.3 subroutines");
    }
    {   // 13.4 32-bit addition
        Built b;
        build(b, R"(
        LD      R0, #$FFFF
        LD      R1, #1
        LD      R2, #1
        LD      R3, #2
        ; R1:R0 += R3:R2
        ADD     R0, R2
        ADC     R1, R3
        WFI
)");
        TestCpu c; load(c, b.as); c.run(10000);
        CHECK(b.ok && c.r[0] == 0 && c.r[1] == 4, "13.4 32-bit addition");
    }
    {   // 13.5 comparisons: unsigned and signed differ for $FFFF and 1
        Built b;
        build(b, R"(
        LD      R0, #$FFFF
        LD      R1, #1
        CMP     R0, R1
        JB      below            ; unsigned 65535 < 1 - false
        LD      R5, #1
below:  CMP     R0, R1
        JL      less             ; signed -1 < 1 - true
        LD      R6, #1
less:   WFI
)");
        TestCpu c; load(c, b.as); c.run(10000);
        CHECK(b.ok && c.r[5] == 1 && c.r[6] == 0, "13.5 comparisons");
    }
    {   // 13.6 copying a string
        Built b;
        build(b, R"(
        LD      SP, #$FF00
        LD      R0, #src
        LD      R1, #dst
        CALL    copy
        WFI
; R0 = source, R1 = destination
copy:   LDZX    R2, [R0++]
        STB     [R1++], R2
        CMP     R2, #0
        JNZ     copy
        RET
src:    .asciz  "abc"
dst:    .space  8, $FF
)");
        TestCpu c; load(c, b.as); c.run(10000);
        const int64_t d = sym(b.as, "dst");
        CHECK(b.ok && d > 0 && std::string((char*)&c.mem[d], 4) == std::string("abc\0", 4) && c.mem[d + 4] == 0xFF,
              "13.6 string copy");
    }
    {   // 13.7 a jump table
        Built b;
        build(b, R"(
        LD      R0, #2
        ADD     R0, R0           ; index * 2
        JMP     [R0+table]       ; the address is taken from word table[R0]
table:  .word   case0, case1, case2
case0:  LD      R5, #10
        WFI
case1:  LD      R5, #11
        WFI
case2:  LD      R5, #12
        WFI
)");
        TestCpu c; load(c, b.as); c.run(10000);
        CHECK(b.ok && c.r[5] == 12, "13.7 jump table");
    }
    {   // 13.8 a spinlock
        Built b;
        build(b, R"(
        LD      SP, #$FF00
        CALL    acquire
        LDZX    R7, [lock]       ; 1 after acquiring
        CALL    release
        LDZX    R8, [lock]       ; 0 after releasing
        WFI
acquire:
        LD      R1, #1
@try:   XCHB    [lock], R1       ; R1 = the old value, lock = 1
        CMP     R1, #0
        JNZ     @try
        RET

release:
        LD      R1, #0
        STB     [lock], R1
        RET

lock:   .byte   0
)");
        TestCpu c; load(c, b.as); c.run(10000);
        CHECK(b.ok && c.r[7] == 1 && c.r[8] == 0, "13.8 spinlock");
    }
    {   // 13.9 flag constants (assembly and encodings)
        Built b;
        build(b, R"(
        .equ    IF, $08
        SHL     R0, #4
        ROR     R2, #8           ; swap the bytes
        OR      FLAGS, #IF       ; same as STI (1 word)
        AND     FLAGS, #$0F & ~IF ; same as CLI: 1 word, side effect OF=0
        AND     FLAGS, #~IF      ; CLI without the side effect: ~$08 = $FFF7, 2 words

        PUSH    FLAGS            ; critical section: save IF and OF
        CLI
        ; ...
        POP     FLAGS            ; restore everything, including OF
)");
        CHECK(b.ok && wordAt(b.as, 4) == cpu16::W_STI && wordAt(b.as, 6) == cpu16::W_CLI &&
              wordAt(b.as, 8) == 0x67F3 && wordAt(b.as, 10) == 0xFFF7, "13.9 flag constants");
    }
    {   // 13.10 interrupts
        Built b;
        build(b, R"(
        .org    0
        LD      SP, #$FF00
        STI
idle:   WFI
        JMP     idle

irq0:   PUSH    R0               ; the handler must save the registers itself
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
        POP     R0
        IRET                     ; restores the flags and IP
ticks:  .word   0
svc:    IRET                     ; handler of the software call TRAP #9

        .org    $FF00            ; vector table: 16 words
        .word   irq0, 0, 0, 0, 0, 0, 0, 0     ; vectors 0..7: external IRQs
        .word   0, svc, 0, 0, 0, 0, 0, 0      ; vector 8 ERRCMD, 9..15 software
)");
        TestCpu c; load(c, b.as);
        c.run(1000);
        c.raiseIrq(0); c.run(1000);
        c.raiseIrq(0); c.run(1000);
        const int64_t t = sym(b.as, "ticks");
        CHECK(b.ok && t > 0 && c.read16((uint16_t)t) == 2 && c.r[cpu16::SP] == 0xFF00, "13.10 interrupts");
    }
    {   // 13.13 printing a number in decimal (DIVU, REMU), boundary values
        for (const char* num : {"40000", "0", "7", "65535"}) {
            Built b;
            build(b, std::string("        .equ    CONSOLE, $FF20\n        LD      SP, #$FF00\n        LD      R0, #") + num + R"(
        CALL    print_dec
        WFI
print_dec:
        LD      R2, #0
@div:   LD      R1, R0
        REMU    R1, #10
        DIVU    R0, #10
        ADD     R1, #'0'
        PUSH    R1
        INC     R2
        CMP     R0, #0
        JNZ     @div
@out:   POP     R1
        STB     [CONSOLE], R1
        DEC     R2
        JNZ     @out
        RET
)");
            TestCpu c; load(c, b.as); c.run(10000);
            CHECK(b.ok && c.out == num, "13.13 decimal print");
        }
    }
    {   // 13.11 relative addressing
        Built b;
        build(b, R"(
        LEA     R0, IP+(msg-$-4) ; R0 = the address of msg; $ is the address of this instruction, +4 its length
        LD      R1, [IP+(val-$-4)]
        WFI
msg:    .asciz  "xy"             ; 3 bytes: the counter becomes odd
val:    .word   $BEEF            ; aligned automatically, val is even
)");
        TestCpu c; load(c, b.as); c.run(100);
        CHECK(b.ok && c.r[0] == sym(b.as, "msg") && c.r[1] == 0xBEEF && sym(b.as, "val") == 14,
              "13.11 IP-relative addressing");
    }
}

// ---------------------------------------------------------------------------
// 3. Form selection
// ---------------------------------------------------------------------------
static std::string nops(int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += "NOP\n";
    return s;
}

static void testRelaxation() {
    {   Built b; build(b, "JMP t\n" + nops(63) + "t: NOP");
        CHECK(b.ok && wordAt(b.as, 0) == 0x303F, "JR forward +63 words: short form"); }
    {   Built b; build(b, "JMP t\n" + nops(64) + "t: NOP");
        CHECK(b.ok && wordAt(b.as, 0) == 0xC877 && wordAt(b.as, 2) == 0x0080, "JR forward +64: long form"); }
    {   Built b; build(b, "t: NOP\n" + nops(62) + "JMP t");
        CHECK(b.ok && wordAt(b.as, 126) == 0x3040, "JR backward -64 words: short form"); }
    {   Built b; build(b, "t: NOP\n" + nops(63) + "JMP t");
        CHECK(b.ok && wordAt(b.as, 128) == 0xC877, "JR backward -65: long form"); }
    {   Built b; build(b, "CALL t\n" + nops(7) + "t: NOP");
        CHECK(b.ok && wordAt(b.as, 0) == 0xD039, "CALL +7: short form"); }
    {   Built b; build(b, "CALL t\n" + nops(8) + "t: NOP");
        CHECK(b.ok && wordAt(b.as, 0) == 0xD077, "CALL +8: long form"); }
    {   Built b; build(b, nops(1) + "t: NOP\n" + nops(6) + "CALL t");
        CHECK(b.ok && wordAt(b.as, 16) == 0xD041, "CALL -8: short form"); }
    {   // a cascade: lengthening an inner jump pushes the outer one beyond ±64
        Built b; build(b, "JMP end\nJMP far\n" + nops(62) + "end: NOP\n.org $1000\nfar: NOP");
        CHECK(b.ok && wordAt(b.as, 0) == 0xC877 && wordAt(b.as, 2) == 0x0080 && wordAt(b.as, 4) == 0xC877,
              "cascading branch growth");
        TestCpu c; load(c, b.as); c.run(1);
        CHECK(c.r[cpu16::IP] == 132 && sym(b.as, "end") == 132, "cascade: branch lands on end");
    }
    {   Built b; build(b, "LD R0, #15\nLD R1, #16\nLD R2, #-1\nWFI");
        TestCpu c; load(c, b.as); c.run(10);
        CHECK(b.ok && wordAt(b.as, 0) == 0x0079 && wordAt(b.as, 2) == 0x00F3 && wordAt(b.as, 4) == 0x0010 &&
              wordAt(b.as, 8) == 0xFFFF && c.r[0] == 15 && c.r[1] == 16 && c.r[2] == 0xFFFF, "#expr: 15, 16, -1"); }
    {   Built b; build(b, "LD R0, #val\n.equ val, 7");
        CHECK(b.ok && wordAt(b.as, 0) == 0x0039, "#expr with forward .equ reference: short form"); }
    {   Built b; build(b, "LD R0, #lbl\nlbl: NOP");
        CHECK(b.ok && wordAt(b.as, 0) == 0x0011 && sym(b.as, "lbl") == 2, "#expr with forward label: self-consistent short form"); }
    {   Built b; build(b, "LD R0, #lbl\n" + nops(7) + "lbl: NOP");
        CHECK(b.ok && wordAt(b.as, 0) == 0x0073 && wordAt(b.as, 2) == 18, "#expr with label > 15: long form"); }
    {   // a program with forward and backward jumps executes correctly
        Built b; build(b, "LD R0, #10\nLD R1, #0\nloop: ADD R1, R0\nDEC R0\nJNZ loop\nJMP out\n" + nops(100) + "out: WFI");
        TestCpu c; load(c, b.as); c.run(1000);
        CHECK(b.ok && c.r[1] == 55, "loop with backward JR and long forward branch");
    }
}

// ---------------------------------------------------------------------------
// 4. Alignment and labels (sect. 9)
// ---------------------------------------------------------------------------
static void testAlignment() {
    {
        Built b;
        build(b, R"(
        .org    $0000
        .byte   1, 2, 3         ; addresses 0, 1, 2; the counter is now 3 (odd)
tbl:    .word   $1234, tbl      ; a fill byte at address 3, tbl = 4
msg:    .asciz  "Hi\n"          ; 4 bytes (addresses 8..11), no alignment
buf:    .space  15              ; addresses 12..26, the counter is 27 (odd)
        NOP                     ; a fill byte at address 27, NOP at address 28
)");
        CHECK(b.ok && sym(b.as, "tbl") == 4 && sym(b.as, "msg") == 8 && sym(b.as, "buf") == 12, "section 9: label addresses");
        CHECK(b.as.owner[3] >= 0 && b.as.image[3] == 0 && wordAt(b.as, 4) == 0x1234 && wordAt(b.as, 6) == 4,
              "section 9: padding byte before .word");
        CHECK(b.as.owner[27] >= 0 && b.as.owner[28] >= 0 && b.as.maxAddr == 29, "section 9: NOP at address 28");
    }
    {   // a label on a separate line points to the instruction, not to the padding byte
        Built b; build(b, ".byte 1\nfunc:\n\n.equ X, 5\n    NOP");
        CHECK(b.ok && sym(b.as, "func") == 2, "label on its own line binds after alignment");
    }
    {   // a label before .align and .org gets the current counter
        Built b; build(b, ".byte 1\nA: .align 4\nB: .org $10\nC: NOP");
        CHECK(b.ok && sym(b.as, "A") == 1 && sym(b.as, "B") == 4 && sym(b.as, "C") == 0x10, "labels before .align and .org");
    }
    {   // $ in an instruction after alignment
        Built b; build(b, ".byte 1\nLEA R0, IP+($-2)");
        CHECK(b.ok && wordAt(b.as, 4) == 0, "$ equals the address after alignment");
    }
    {   // a label at the end of the file
        Built b; build(b, "NOP\nend:");
        CHECK(b.ok && sym(b.as, "end") == 2, "label at end of file");
    }
    {   // .space with fill, .align with fill, string bytes with an escape
        Built b; build(b, ".space 3, $AA\n.align 8, $55\n.ascii \"\\x41\\t\\0\", \"B\"\n.byte -1, 'z', lo($1234), hi($1234)");
        CHECK(b.ok && b.as.image[0] == 0xAA && b.as.image[3] == 0x55 && b.as.image[7] == 0x55 && b.as.image[8] == 0x41 &&
              b.as.image[9] == 9 && b.as.image[10] == 0 && b.as.image[11] == 'B' && b.as.image[12] == 0xFF &&
              b.as.image[13] == 'z' && b.as.image[14] == 0x34 && b.as.image[15] == 0x12, "data: .space, .align, escapes, lo/hi");
    }
    {   // expressions: precedence and operators
        Built b; build(b, ".word 2+3*4, (2+3)*4, 1<<4|1, $F0&$3C^1, -7/2, 0b101, %11, 0x1F, ~0, 5--3, -16>>2, 3-2-1");
        const uint16_t exp[] = {14, 20, 17, 0x31, (uint16_t)-3, 5, 3, 31, 0xFFFF, 8, 0xFFFC, 0};
        bool ok = b.ok;
        for (int i = 0; i < 12 && ok; i++) ok = wordAt(b.as, 2 * i) == exp[i];
        CHECK(ok, "expressions: precedence, number bases, unary operators");
    }
}

// ---------------------------------------------------------------------------
// 5. Diagnostics (sect. 12)
// ---------------------------------------------------------------------------
static void testErrors() {
    struct E { const char* src; const char* msg; int line; };
    const std::vector<E> cases = {
        {"LDD R0, R1", "unknown mnemonic 'LDD'", 1},
        {"ADD R0", "wrong number of operands", 1},
        {"NOP\nJMP nowhere", "undefined symbol 'nowhere'", 2},
        {"a: NOP\na: NOP", "duplicate definition of 'a'", 2},
        {".byte 300", "does not fit in a byte", 1},
        {".word 70000", "does not fit in a word", 1},
        {"SHL R0, #16", "shift count must be 0..15", 1},
        {"LDZX R0, #300", "byte load immediate", 1},
        {"JMP 101", "branch target is odd", 1},
        {"JMP t\n.byte 0\nt: .byte 0", "branch target is odd", 1},
        {".org 0\nNOP\n.org 0\nNOP", "overlapping output at address $0000", 4},
        {".org lbl\nlbl: NOP", ".org cannot depend on labels", 1},
        {".space $\n", ".space cannot depend", 1},
        {".byte 1/0", "division by zero", 1},
        {".equ A, B\n.equ B, A\nLD R0, #A", "circular definition", 1},
        {"start: NOP\n@w: NOP\nother: JMP @w", "undefined symbol '@w'", 3},
        {"STB [SP++], R0", "spec 6.3.5", 1},
        {"LD R0, [IP++]", "[IP++] is not allowed", 1},
        {"LD R0, [--IP]", "spec 6.3.2", 1},
        {"LD R0, [FLAGS]", "FLAGS cannot be used", 1},
        {"ST [IP], R0", "spec 6.3.4", 1},
        {"JMP [R0++]", "spec 6.3.6", 1},
        {"JMP [--R0]", "spec 6.2.3", 1},
        {"LDZX R0, R1+4", "spec 6.2.3", 1},
        {"ST R0, R1", "first operand of ST must be a memory address", 1},
        {"ST [R0], 5", "second operand of ST must be a register", 1},
        {"LD R0, 5", "has no # or brackets", 1},
        {"TRAP #16", "TRAP vector number", 1},
        {"TRAP [R0]", "TRAP only accepts", 1},
        {"LD: NOP", "is reserved", 1},
        {".equ SP, 1", "is reserved", 1},
        {"JMP R0++", "must be in brackets", 1},
        {".foo 1", "unknown directive", 1},
        {"LD R0, #'ab'", "unterminated character literal", 1},
        {".org $FFFF\nNOP", "exceeds 64 KB", 2},
        {"LD R0, #(1", "missing ')'", 1},
        {"ADD R0 R1", "expected ','", 1},
        {"NOP R0", "wrong number of operands", 1},
        {".ascii \"abc", "unterminated string", 1},
        {"LD R0, #1 << 40", "shift count out of range 0..31", 1},
        {"LD R0, #SP", "register 'SP' is not allowed in an expression", 1},
        {".word %102", "malformed number", 1},
        {"JR t\nt: NOP", "unknown mnemonic 'JR'", 1},
        {".word 1.5", "malformed number", 1},                          // a dot in a name, not in a number
        {".word $FF.x", "malformed number", 1},
        {".word.x 1", "expected expression", 1},                     // directive names contain no dots
        {"LD R0, #.x", "expected expression", 1},                     // a directive begins with a dot
        {"a.b: NOP\na.b: NOP", "duplicate definition of 'a.b'", 2},
        {"end.: NOP", "invalid name 'end.'", 1},                       // a dot at the end of a name
        {"JMP a..b", "invalid name 'a..b'", 1},                         // two dots in a row
        {"x: NOP\n@l.: NOP", "invalid name '@l.'", 2},
        {".equ k.., 1", "invalid name 'k..'", 1},
        {"main.loop: NOP\n@w: NOP\nmain.end: JMP @w", "undefined symbol '@w'", 3},   // a name with a dot is a global label
        {"MISC R0", "unknown mnemonic 'MISC'", 1},
        {".ascii \"5€\"", "character '€' is not in the CPU16 character set", 1},
        {".byte '€'", "character '€' is not in the CPU16 character set", 1},
        {"NEG R0, R1", "wrong number of operands: NEG expects 1, got 2", 1},
        {"WFI R0", "wrong number of operands: WFI expects 0, got 1", 1},
        {"NOT #5", "operand of NOT must be a register", 1},
        {"MUL R0", "wrong number of operands: MUL expects 2, got 1", 1},
        {"DIVU R0, 10", "has no # or brackets", 1},
        {"MULHU R0, [FLAGS]", "FLAGS cannot be used", 1},
        {"NEG: NOP", "is reserved", 1},
        {".include \"no_such_file.asm\"", "cannot open file", 1},
        {".reserve 4\n.org 2\n.byte 1", "overlapping output at address $0002 (reserved by line 1)", 3},
        {".byte 1, 2\n.org 1\n.reserve 2", "reserved area overlaps output at address $0001 (already used by line 1)", 3},
        {".reserve 4\n.org 3\n.reserve 2", "reserved area overlaps output at address $0003 (reserved by line 1)", 3},
        {".org $FFF0\n.reserve 17", "reserved area exceeds 64 KB address space", 2},
        {".reserve lbl\nlbl: NOP", ".reserve cannot depend on labels", 1},
        {".reserve -1", ".reserve: size out of range 0..65536", 1},
        {".reserve 4, 0", "too many operands", 1},
        {".line x", ".line expects a quoted file name and a line number", 1},
        {".line 5", ".line N without a file name needs an earlier .line \"file\", N in this file", 1},
        {".line \"a.b16\", 3\n.line\n.line 5", ".line N without a file name needs", 3},
        {".line \"a.b16\"", ".line expects a quoted file name and a line number", 1},
        {".line \"a.b16\", x", ".line: line number must be a number", 1},
        {".line \"a.b16\", 0", ".line: line number out of range 1..1000000", 1},
        {".line \"a.b16\", 1000001", ".line: line number out of range 1..1000000", 1},
        {".line \"\", 1", ".line: file name is empty", 1},
        {".line \"a.b16\", 1, 2", "too many operands", 1},
        {".line \"k.b16\", 50\nNOP\n; comment\n\nJMP nowhere", "undefined symbol 'nowhere'", 50},   // the .line location does not change
        {".line \"k.b16\", 50\nNOP\n.line 52\nJMP nowhere", "undefined symbol 'nowhere'", 52},          // .line N - the same file
        {".line \"k.b16\", 50\nNOP\n.line\nJMP nowhere", "undefined symbol 'nowhere'", 4},  // .line without arguments
        {"LD R0, #\"s\"", "strings are only allowed in .ascii, .asciz, .include and .line", 1},
    };
    for (const auto& e : cases) {
        Built b;
        build(b, e.src, false);
        bool found = false;
        for (const auto& d : b.as.diags)
            if (d.msg.find(e.msg) != std::string::npos && d.line == e.line) found = true;
        if (!found) std::printf("  expected '%s' on line %d, got:\n%s", e.msg, e.line, b.as.formatDiags().c_str());
        CHECK(!b.ok && found, e.src);
    }
    {   // several errors in one run
        Built b; build(b, "LDD\nADD R0\nNOP\n.foo", false);
        CHECK(!b.ok && b.as.errorCount() == 3, "all parse errors are reported in one run");
    }
    {   // the message format
        Built b; build(b, "NOP\nJMP nowhere", false);
        CHECK(b.as.formatDiags() == "test.asm:2: error: undefined symbol 'nowhere'\n", "format file:line: error: message");
    }
}

// ---------------------------------------------------------------------------
// 6. .include and local labels
// ---------------------------------------------------------------------------
static void testIncludeAndLocals() {
    {
        Built b;
        build(b, "a: NOP\n@x: NOP\nJMP @x\nb: NOP\n@x: NOP\nJMP @x");
        CHECK(b.ok && sym(b.as, "a@x") == 2 && sym(b.as, "b@x") == 8 && wordAt(b.as, 4) == 0x307E && wordAt(b.as, 10) == 0x307E,
              "same-named local labels in different scopes");
    }
    {
        { std::ofstream f("/tmp/cpu16asm_t_inc.asm"); f << "inc_val: .word $ABCD\n"; }
        { std::ofstream f("/tmp/cpu16asm_t_main.asm"); f << "LD R0, [inc_val]\nWFI\n.include \"cpu16asm_t_inc.asm\"\n"; }
        cpu16asm::Assembler as;
        const bool ok = as.assembleFile("/tmp/cpu16asm_t_main.asm");
        TestCpu c; load(c, as); c.run(10);
        CHECK(ok && c.r[0] == 0xABCD, ".include relative to the including file directory");
    }
    {
        { std::ofstream f("/tmp/cpu16asm_t_rec.asm"); f << ".include \"cpu16asm_t_rec.asm\"\n"; }
        cpu16asm::Assembler as;
        const bool ok = as.assembleFile("/tmp/cpu16asm_t_rec.asm");
        bool found = false;
        for (const auto& d : as.diags) if (d.msg.find("nesting too deep") != std::string::npos) found = true;
        CHECK(!ok && found, ".include: depth limit 16");
    }
    {   // a dot inside a name: labels, constants, local labels, forward references; names with a dot
        // are not reserved, even if they begin with a register name or a mnemonic
        Built b;
        build(b, ".equ con.data, $FF20\n.equ R1.x, 5\n.equ ld.b, 2\nmain.start: LD R0, #R1.x + ld.b\n@lp.1: SUB R0, #1\n"
                 "JNZ @lp.1\nJMP main.end\nmain.end: STB [con.data], R0\nbuf.x__.y: .word main.start, buf.x__.y\n");
        CHECK(b.ok && sym(b.as, "con.data") == 0xFF20 && sym(b.as, "R1.x") == 5 && sym(b.as, "main.start@lp.1") == 2 &&
              sym(b.as, "main.end") == 8 && sym(b.as, "buf.x__.y") == 12 && wordAt(b.as, 12) == 0 && wordAt(b.as, 14) == 12 &&
              wordAt(b.as, 0) == 0x0039,                                    // LD R0, #7
              "dots inside names of labels, constants and local labels");
        const std::string m = b.as.memoryMap();
        CHECK(m.find("  code  main.end  ") != std::string::npos && m.find("    @lp.1") != std::string::npos &&
              m.find("$FF20  con.data") != std::string::npos, "dotted names in the memory map");
    }
    {   // the memory map: a summary, files, objects with a type and size, local labels, alignment
        // on a separate line, a section without a label, an .org skip, a reserve, zero-size labels, constants
        Built b;
        build(b, "        .equ    SIZE, 4\n"
                 "        .equ    BIG, 100000\n"
                 "        NOP                     ; no label\n"
                 "main:   LD      R0, #SIZE\n"
                 "@lp:    DEC     R0\n"
                 "        JNZ     @lp\n"
                 "@out:   RET\n"
                 "msg:    .asciz  \"hi\"\n"
                 "tbl:    .word   1, 2\n"
                 "        .align  8\n"
                 "        .org    $40\n"
                 "alias:\n"
                 "code2:  NOP\n"
                 "        .byte   7\n"
                 "end:\n"
                 "buf:    .reserve 5\n"
                 "tail:\n"
                 "");
        const std::string m = b.as.memoryMap();
        const std::string want = "; CPU16 memory map: test.asm\n"
            "\n"
            "== Summary ==\n"
            "occupied  $0000..$0047\n"
            "file      $0000..$0042   67 bytes\n"
            "code          12 bytes\n"
            "data           8 bytes\n"
            "bss            5 bytes   .reserve\n"
            "padding        7 bytes   alignment\n"
            "gaps          40 bytes   unused addresses inside the file\n"
            "total         32 bytes   code + data + bss + padding\n"
            "\n"
            "== By source file ==\n"
            "  code    data     bss     pad   total  file\n"
            "    12       8       5       7      32  test.asm\n"
            "\n"
            "== Objects by address ==\n"
            "start  end      size  type       name        source\n"
            "$0000  $0001       2  code       (no label)  test.asm:3\n"
            "$0002  $0009       8  code       main        test.asm:4\n"
            "$0004  $0007       4  code         @lp       test.asm:5\n"
            "$0008  $0009       2  code         @out      test.asm:7\n"
            "$000A  $000C       3  data       msg         test.asm:8\n"
            "$000D  $000D       1  pad        (padding)   test.asm:9\n"
            "$000E  $0011       4  data       tbl         test.asm:9\n"
            "$0012  $0017       6  pad        (padding)   test.asm:10\n"
            "$0040              0  -          alias       test.asm:12\n"
            "$0040  $0042       3  code+data  code2       test.asm:13\n"
            "$0043              0  -          end         test.asm:15\n"
            "$0043  $0047       5  bss        buf         test.asm:16\n"
            "$0048              0  -          tail        test.asm:17\n"
            "\n"
            "== Objects by size ==\n"
            "  size   share  type       name        start\n"
            "     8   25.0%  code       main        $0002\n"
            "     5   15.6%  bss        buf         $0043\n"
            "     4   12.5%  data       tbl         $000E\n"
            "     3    9.4%  data       msg         $000A\n"
            "     3    9.4%  code+data  code2       $0040\n"
            "     2    6.3%  code       (no label)  $0000\n"
            "\n"
            "== Constants ==\n"
            "value   name  source\n"
            "$0004   SIZE  test.asm:1\n"
            "100000  BIG   test.asm:2\n"
            "";
        if (m != want) std::printf("%s", m.c_str());
        CHECK(b.ok && m == want, "memory map: exact text");
        Built bad; build(bad, "x: NOP\nJMP nowhere", false);
        CHECK(!bad.ok && bad.as.memoryMap().empty(), "memory map: empty after errors");
    }
    {   // .line: the location is kept for all following lines until the next .line; .line N - the same
        // file; messages - "location (text line): error:"; a repeated definition in the same
        // source - "line N", in another - "file:line"; .line without arguments
        Built b;
        build(b, "; kernel\n        .line \"kernel.b16\", 120\nmain: NOP\n        JMP nowhere\n        .line \"lib.b16\", 7\n"
                 "main: NOP\n        .line \"kernel.b16\", 200\nx: NOP\n        .line 201\nx: NOP\n        .line\nmain: NOP\n", false);
        const std::string want =
            "lib.b16:7 (test.asm:6): error: duplicate definition of 'main' (first defined at kernel.b16:120)\n"
            "kernel.b16:201 (test.asm:10): error: duplicate definition of 'x' (first defined on line 200)\n"
            "test.asm:12: error: duplicate definition of 'main' (first defined at kernel.b16:120)\n"
            "kernel.b16:120 (test.asm:4): error: undefined symbol 'nowhere'\n";
        if (b.as.formatDiags() != want) std::printf("%s", b.as.formatDiags().c_str());
        CHECK(!b.ok && b.as.formatDiags() == want && b.as.diags[3].asmFile == "test.asm" && b.as.diags[3].asmLine == 4 &&
              b.as.diags[2].asmLine == 0, ".line: fixed place, .line N, diagnostics with both places");
        cpu16asm::Assembler n;
        n.ignoreLine = true;
        n.assembleText("test.asm", ".line \"k.b16\", 5\nNOP\n.line 6\nJMP nowhere\n");
        cpu16asm::Assembler n2;
        n2.ignoreLine = true;
        n2.assembleText("test.asm", ".line \"k.b16\", 5\nNOP\n.line 0\n");
        CHECK(n.formatDiags() == "test.asm:4: error: undefined symbol 'nowhere'\n" &&
              n2.formatDiags() == "test.asm:3: error: .line: line number out of range 1..1000000\n",
              "--no-line: .line is checked but does not change places");
    }
    {   // the listing: the location column by .line (empty for .line lines, for their own locations and for continuations)
        Built b;
        build(b, "; gen\n.line \"k.b16\", 9\nf: LD R0, #1\n.word 1, 2, 3, 4, 5\n.line\nNOP\n");
        const std::string want =
            "                                      ; gen\n"
            "                                      .line \"k.b16\", 9\n"
            "0000  0009                   k.b16:9  f: LD R0, #1\n"
            "0002  0001 0002 0003 0004    k.b16:9  .word 1, 2, 3, 4, 5\n"
            "000A  0005                            \n"
            "                                      .line\n"
            "000C  0000                            NOP\n";
        if (b.as.listing() != want) std::printf("%s", b.as.listing().c_str());
        CHECK(b.ok && b.as.listing() == want, ".line: place column in the listing");
    }
    {   // .line works to the end of its file: in an included file - its own locations, after return -
        // again the location by .line of the including file; the memory map - locations by .line
        { std::ofstream f("/tmp/cpu16asm_t_line_inc.asm"); f << "inc_fn: NOP\n"; }
        { std::ofstream f("/tmp/cpu16asm_t_line.asm");
          f << ".line \"app.b16\", 10\nmain: NOP\n.include \"cpu16asm_t_line_inc.asm\"\nafter: .word 1\n"; }
        cpu16asm::Assembler as;
        const bool ok = as.assembleFile("/tmp/cpu16asm_t_line.asm");
        const std::string m = as.memoryMap();
        CHECK(ok && m.find("$0000  $0001       2  code  main    app.b16:10\n") != std::string::npos &&
              m.find("$0002  $0003       2  code  inc_fn  /tmp/cpu16asm_t_line_inc.asm:1\n") != std::string::npos &&
              m.find("$0004  $0005       2  data  after   app.b16:10\n") != std::string::npos &&
              m.find("     2       2       0       0       4  app.b16\n     2       0       0       0       2  /tmp/cpu16asm_t_line_inc.asm\n") !=
                  std::string::npos,
              ".line: scope is the current file; memory map uses .line places");
        if (!ok || m.find("after   app.b16:10") == std::string::npos) std::printf("%s%s", as.formatDiags().c_str(), m.c_str());
    }
    {   // the listing and the memory map are produced
        Built b; build(b, "start: LD SP, #$FF00\n.equ K, 3\nmsg: .asciz \"ok\"");
        const std::string l = b.as.listing(), m = b.as.memoryMap();
        CHECK(b.ok && l.find("0000  06F3 FF00") != std::string::npos && l.find("= $0003") != std::string::npos &&
              m.find("$0004  $0006       3  data  msg    ") != std::string::npos && m.find("$0003  K ") != std::string::npos,
              "listing and memory map");
    }
}

// ---------------------------------------------------------------------------
// 7. .reserve and the CX16 executable file header
// ---------------------------------------------------------------------------
// The example program from the executable file format (sect. 9.4), verbatim.
static const char* EXEC_EXAMPLE = R"(
        .equ    CON_DATA, $FF20

        .org    0
        JMP     #start
        .ascii  "CX16"
        .word   1, 0                    ; version, flags
        .word   image_end, bss_end
        .word   512                     ; min_free
        .word   $2000                   ; max_top: one page is enough
        .word   0, 0, 0, 0              ; reserved

start:  LD      R2, R1                  ; R2 = argv
@arg:   LD      R3, [R2++]              ; the next pointer
        CMP     R3, #0
        JZ      @done
@ch:    LDZX    R4, [R3++]
        CMP     R4, #0
        JZ      @eol
        STB     [CON_DATA], R4
        JMP     @ch
@eol:   LD      R4, #10
        STB     [CON_DATA], R4
        JMP     @arg
@done:  LD      R0, #0                  ; exit code
        RET                             ; LR = the exit routine

image_end:
bss_end:
        .align  2
)";

static std::vector<std::string> execErrors(cpu16asm::Assembler& as) {
    return as.checkExec();
}
static bool hasMsg(const std::vector<std::string>& v, const std::string& m) {
    for (const auto& s : v) if (s.find(m) != std::string::npos) return true;
    return false;
}

// Run the image on the Machine from cpu16.cpp with input in; return the teletype output.
// monitor: simulating a launch by the monitor (sect. 8 of the format, revision 2): bss is zeroed, the PIB with "CXPB",
// SP = top, R0 = argc, R1 = argv, LR is the exit routine at $E000, FLAGS = $08 (IF = 1).
// The vectors belong to the "monitor": all lead to a handler in page 7 that counts entries
// and clobbers memory below its own frame (sect. 8.3: data below SP is not preserved). IRQ0
// is raised before every seventh instruction of the program, starting with the first. Execution ends on
// entry into the exit routine (IP = $E000).
static const char* MONITOR_STUB = R"(
        .org    $E000
exit:   CLI
@h:     WFI
        JMP     @h
        .org    $E010
irq:    PUSH    R0
        LD      R0, [irq_count]
        INC     R0
        ST      [irq_count], R0
        LD      R0, #$DEAD
        PUSH    R0                      ; below the interrupt frame - garbage
        POP     R0
        POP     R0
        IRET
irq_count: .word 0
)";
static uint16_t monitorIrqs(::Machine& m) {
    cpu16asm::Assembler as;
    as.assembleText("monitor.asm", MONITOR_STUB);
    int64_t a = 0;
    as.symbolValue("irq_count", a);
    return m.read16((uint16_t)a);
}
static std::string runMachine(const std::vector<uint8_t>& bin, const std::string& in, bool monitor, ::Machine& m) {
    using namespace cpu16;
    m.readStdin = false; m.printTty = false;
    std::vector<uint8_t> out;
    m.ttyCapture = &out;
    for (size_t a = 0; a < bin.size(); a++) m.mem[a] = bin[a];
    for (unsigned char ch : in) m.dev.pushKey(ch, 0);
    if (!monitor) {
        m.run(5000000);
        return std::string(out.begin(), out.end());
    }
    const uint16_t bssEnd = (uint16_t)(bin[0x0E] | (bin[0x0F] << 8)), argv = (uint16_t)((bssEnd + 1) & ~1);
    const char* pib = "CXPB";
    for (int k = 0; k < 4; k++) m.mem[4 + k] = (uint8_t)pib[k];
    m.write16(argv, (uint16_t)(argv + 4)); m.write16(argv + 2, 0);
    const char* name = "calc.x";
    for (int k = 0; k <= 6; k++) m.mem[argv + 4 + k] = (uint8_t)name[k];
    cpu16asm::Assembler stub;
    const bool ok = stub.assembleText("monitor.asm", MONITOR_STUB);
    if (!ok) std::printf("%s", stub.formatDiags().c_str());
    for (unsigned a = 0xE000; a < 0xE100; a++) if (stub.owner[a] >= 0) m.mem[a] = stub.image[a];
    int64_t irq = 0;
    stub.symbolValue("irq", irq);
    for (unsigned n = 0; n < 16; n++) m.write16((uint16_t)(VEC_BASE + 2 * n), (uint16_t)irq);
    m.r[0] = 1; m.r[1] = argv; m.r[SP] = 0x2000; m.r[LR] = 0xE000; m.r[FL] = FIF;
    unsigned long progSteps = 0;              // steps in the program code (the handler does not count)
    for (unsigned long n = 0; n < 5000000 && m.r[IP] != 0xE000; n++) {
        if (m.r[IP] < 0xE000 && progSteps++ % 7 == 0) m.raiseIrq(0);
        if (m.step() == decltype(m.step())::Idle) break;
    }
    return std::string(out.begin(), out.end());
}

static void testReserveAndExec() {
    {   // .reserve: labels, the image length, zeros inside the image regardless of fill, occupied addresses
        Built b; build(b, "NOP\nbuf: .reserve 4\n.byte 7\ntail: .reserve 100\nend:");
        const std::vector<uint8_t> bin = b.as.binary(0, 0xAA);
        CHECK(b.ok && sym(b.as, "buf") == 2 && sym(b.as, "tail") == 7 && sym(b.as, "end") == 107, ".reserve: label values");
        CHECK(b.as.minAddr == 0 && b.as.maxAddr == 6 && bin.size() == 7, ".reserve at the end does not extend the image");
        CHECK(bin[2] == 0 && bin[5] == 0 && bin[6] == 7 && b.as.owner[2] >= 0 && b.as.owner[106] >= 0 && b.as.owner[107] < 0,
              ".reserve inside the image: zeros regardless of --fill, addresses occupied");
        CHECK(b.as.listing().find("0002  (4 bytes reserved)") != std::string::npos, ".reserve in the listing");
    }
    {   // .reserve does not align: a label after an odd counter is odd; an instruction after it is aligned
        Built b; build(b, ".byte 1\nv: .reserve 1\nNOP");
        CHECK(b.ok && sym(b.as, "v") == 1 && wordAt(b.as, 2) == 0 && b.as.owner[2] >= 0, ".reserve without alignment");
    }
    {   // only a reserve - an empty image
        Built b; build(b, ".reserve 10\nx:");
        CHECK(b.ok && b.as.maxAddr < 0 && sym(b.as, "x") == 10, ".reserve only: empty program");
    }
    {   // example 9.4 of the format: the header is correct, image_end and bss_end are the counter before .align
        Built b; build(b, EXEC_EXAMPLE);
        CHECK(b.ok && execErrors(b.as).empty() && wordAt(b.as, 0) == 0xC873 && wordAt(b.as, 2) == 0x1C,
              "exec example 9.4 passes the header check");
    }
    {   // each checked condition of sect. 3.3 separately
        auto hdr = [](const std::string& jmp, const std::string& magic, const std::string& ver, const std::string& ie,
                      const std::string& be, const std::string& tail) {
            return ".org 0\n" + jmp + "\n.ascii " + magic + "\n.word " + ver + ", 0\n.word " + ie + ", " + be +
                   "\n.word 512, 0\n.word 0, 0, 0, 0\nstart: RET\n" + tail;
        };
        const std::string J = "JMP #start", M = "\"CX16\"", T = "image_end:\nbuf: .reserve 10\nbss_end:\n";
        struct X { std::string src; const char* msg; };
        const std::vector<X> cases = {
            {hdr("JMP [start]", M, "1", "image_end", "bss_end", T), "word at $0000 is $C805, expected $C873"},
            {hdr(J, "\"CX61\"", "1", "image_end", "bss_end", T), "bytes $0004..$0007 must be \"CX16\""},
            {hdr(J, M, "2", "image_end", "bss_end", T), "version is 2, expected 1"},
            {hdr(J, M, "1", "$1A", "bss_end", T), "image_end $001A is below $001C"},
            {hdr(J, M, "1", "image_end", "image_end - 1", T), "bss_end $001D is below image_end $001E"},
            {hdr(J, M, "1", "image_end", "$E001", T), "bss_end $E001 is above $E000"},
            {hdr("JMP #start + 1", M, "1", "image_end", "bss_end", T), "start $001D is odd"},
            {hdr("JMP #image_end", M, "1", "image_end", "bss_end", T), "start $001E is outside the image"},
            {hdr("JMP #2", M, "1", "image_end", "bss_end", T), "start $0002 is outside the image"},
            {hdr(J, M, "1", "image_end + 2", "bss_end", T), "image_end $0020 exceeds the file length (30 bytes)"},
            {"JMP #4\n.ascii \"CX16\"", "shorter than the header"},
        };
        for (const auto& x : cases) {
            Built b; build(b, x.src);
            const auto e = execErrors(b.as);
            if (!hasMsg(e, x.msg)) { std::printf("  expected '%s', got:\n", x.msg); for (const auto& m : e) std::printf("    %s\n", m.c_str()); }
            CHECK(b.ok && hasMsg(e, x.msg), x.msg);
        }
        {   // not checked: flags, reserve, min_free
            Built b; build(b, ".org 0\nJMP #start\n.ascii \"CX16\"\n.word 1, 5\n.word image_end, bss_end\n.word 3, 0\n"
                              ".word 1, 2, 3, 4\nstart: RET\nimage_end:\nbss_end:\n");
            CHECK(b.ok && execErrors(b.as).empty(), "exec: flags, reserved words and min_free are not checked");
        }
        {   // output from the source after image_end is an error (data right after image_end)
            Built b; build(b, hdr(J, M, "1", "image_end", "bss_end", "image_end:\nbss_end:\n.byte 1, 2, 3\n"));
            const auto e = execErrors(b.as);
            CHECK(b.ok && hasMsg(e, "output above image_end $001E: 3 byte(s)") && hasMsg(e, "first output byte at $001E"),
                  "exec: output right after image_end is rejected");
        }
        {   // a module after bss: a reserve in the middle of the file is written as zeros, the first emitted byte is after the reserve
            Built b; build(b, hdr(J, M, "1", "image_end", "bss_end",
                                  "image_end:\nbuf: .reserve 10\nbss_end:\nmod: RET\n"));
            const auto e = execErrors(b.as);
            CHECK(b.ok && hasMsg(e, "output above image_end $001E") && hasMsg(e, "first output byte at $0028"),
                  "exec: code after bss_end is rejected and its address is reported");
        }
        {   // a reserve after image_end with no output after it is allowed (this is bss)
            Built b; build(b, hdr(J, M, "1", "image_end", "bss_end", "image_end:\nbuf: .reserve 10\nbss_end:\n"));
            CHECK(b.ok && execErrors(b.as).empty(), "exec: .reserve after image_end is allowed");
        }
        {   // bss_end = $E000 is allowed
            Built c; build(c, hdr(J, M, "1", "image_end", "$E000", T));
            CHECK(c.ok && execErrors(c.as).empty(), "exec: bss_end = $E000 is allowed");
        }
    }
    {   // examples/calc_exec.asm: the header is correct, the buffer is not in the file; the output matches calc.asm
        // on a bare machine and when simulating a monitor launch (exit by RET with code 0)
        cpu16asm::Assembler ae, ac;
        const bool okE = ae.assembleFile("examples/calc_exec.asm"), okC = ac.assembleFile("examples/calc.asm");
        if (!okE) std::printf("%s", ae.formatDiags().c_str());
        int64_t ie = -1, be = -1, ln = -1;
        ae.symbolValue("image_end", ie); ae.symbolValue("bss_end", be); ae.symbolValue("line", ln);
        const std::vector<uint8_t> binE = ae.binary(0, 0), binC = ac.binary(0, 0);
        CHECK(okE && okC && execErrors(ae).empty() && (int64_t)binE.size() == ie && ln >= ie && be == ln + 40,
              "calc_exec.asm: header valid, line buffer in bss");
        const std::string in = "1234 * 567\n7 - 9\n5 / 0\nq\n";
        ::Machine m1, m2, m3;
        const std::string outC = runMachine(binC, in, false, m1);
        const std::string outBare = runMachine(binE, in, false, m2);
        const std::string outMon = runMachine(binE, in, true, m3);
        CHECK(outC.find("= 699678 ($000AAD1E)") != std::string::npos && outC.find("bye") != std::string::npos, "calc.asm output");
        CHECK(outBare == outC && m2.waiting, "calc_exec.asm on a bare machine: same output, stops in WFI");
        CHECK(outMon == outC && m3.r[cpu16::IP] == 0xE000 && m3.r[0] == 0 && m3.r[cpu16::SP] == 0x2000 &&
              m3.read16(0xFF00 + 16) == m3.read16(0xFF00) && m3.read16(0xFF00) >= 0xE000 && monitorIrqs(m3) > 100,
              "calc_exec.asm under the monitor (IF = 1, IRQ0 every 7 instructions): same output, RET to the exit procedure "
              "with code 0, vectors untouched");
    }
    {   // docs/skeleton_exec.asm: a bare machine - no arguments; the monitor - argc/argv, exit by RET with code 0
        cpu16asm::Assembler as;
        const bool ok = as.assembleFile("docs/skeleton_exec.asm");
        if (!ok) std::printf("%s", as.formatDiags().c_str());
        const std::vector<uint8_t> bin = as.binary(0, 0);
        ::Machine m1, m2;
        const std::string bare = runMachine(bin, "", false, m1), mon = runMachine(bin, "", true, m2);
        CHECK(ok && execErrors(as).empty() && bare == "CX16 skeleton\nstarted without the monitor: no arguments\n" && m1.waiting,
              "skeleton_exec.asm on a bare machine");
        CHECK(mon == "CX16 skeleton\nargc = 1\ncalc.x\n" && m2.r[cpu16::IP] == 0xE000 && m2.r[0] == 0 && m2.r[cpu16::SP] == 0x2000 &&
              monitorIrqs(m2) > 10,
              "skeleton_exec.asm under the monitor (IF = 1, IRQ0 every 7 instructions): arguments, RET to the exit procedure");
    }
}

int main() {
    testGolden();
    testExamples();
    testRelaxation();
    testAlignment();
    testErrors();
    testIncludeAndLocals();
    testReserveAndExec();
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

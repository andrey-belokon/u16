// monrun - run a CX16 image in the CPU16 emulator on a bare machine or with a simulated monitor.
//
//   monrun [--monitor] [--garbage] [--strict-align] [--screen] [--arg STRING]... image.bin
//
// --monitor  simulate a launch by the monitor ("CPU16 Monitor: Executable File Format and Program Startup", sect. 8,
//            revision 2): the process memory $0000..$DFFF is zeroed, the PIB with "CXPB", the argument block
//            with bss_end', R0 = argc, R1 = argv, SP = top = $E000, LR is the exit routine at $E000,
//            FLAGS = $08 (IF = 1). All vectors lead to the monitor handler in page 7, which
//            counts entries and clobbers memory below its own frame. IRQ0 is raised before every
//            seventh instruction of the program, starting with the first. Exit is entry into the exit routine;
//            the exit code (R0) is printed to stderr as the line "exit N".
//            A system call (TRAP #9, the number is the word after the instruction; "System Calls", sect. 3)
//            is executed partially, just enough for universal programs to work
//            (lib/io.b16): READ from descriptor 0 - keyboard input (stdin), a line up to '\n'
//            inclusive; WRITE to descriptors 1 and 2 - teletype output (stdout); EXIT. The other
//            calls return NOSYS. A call is executed whole, without ticks.
// --garbage  bare machine: before the image is loaded all memory is filled with the byte $A5
//            (the memory state after reset is undefined).
// --screen   after the run print a screen snapshot (like cpu16 --screen).
// --arg      a command-line argument (argv[1], argv[2] ...); argv[0] = "prog.x".
// Keyboard input comes from stdin, teletype output goes to stdout.
//
// Build: c++ -std=c++17 -O2 -I<cpu16>/tools -o monrun monrun.cpp
#define main cpu16_emulator_main
#include "cpu16.cpp"
#undef main
#define main cpu16asm_main
#include "cpu16asm.cpp"
#undef main
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
        PUSH    R0
        POP     R0
        POP     R0
        IRET
irq_count: .word 0
        .org    $E040
sys:    IRET                    ; TRAP #9: monrun itself executes the call (syscall), then IRET
)";

// A system call on entry into the TRAP #9 handler: [SP] - FLAGS, [SP+2] - the address of the number word.
// Return is past the number word; the result is in R0 (error) and R1. 1 - the program terminated (EXIT).
static bool syscall(::Machine& m) {
    using namespace cpu16;
    const uint16_t ret = m.read16((uint16_t)(m.r[SP] + 2));
    const uint16_t no = m.read16(ret);
    m.write16((uint16_t)(m.r[SP] + 2), (uint16_t)(ret + 2));
    const uint16_t h = m.r[0], buf = m.r[1], n = m.r[2];
    enum { OK = 0, NOSYS = 1, BADF = 13 };
    switch (no) {
    case 0x00:                                  // EXIT: R0 = code
        m.r[IP] = 0xE000;
        return true;
    case 0x12: {                                // READ
        if (h != 0) { m.r[0] = BADF; return false; }
        uint16_t k = 0;
        while (k < n) {
            uint8_t c = m.read8(PORT_DATA);     // a keyboard event from stdin
            if (c == 0) {
                if (m.dev.keys.empty()) break;  // input ended
                continue;                       // an event without a character
            }
            m.at((uint16_t)(buf + k)) = c;
            k++;
            if (c == '\n') break;
        }
        m.r[0] = OK; m.r[1] = k;
        return false;
    }
    case 0x13:                                  // WRITE
        if (h != 1 && h != 2) { m.r[0] = BADF; return false; }
        for (uint16_t k = 0; k < n; k++) m.write8(PORT_DATA, m.at((uint16_t)(buf + k)));
        m.r[0] = OK; m.r[1] = n;
        return false;
    default:
        m.r[0] = NOSYS;
        return false;
    }
}

int main(int argc, char** argv) {
    using namespace cpu16;
    bool monitor = false, garbage = false, strict = false, screen = false;
    std::vector<std::string> args{"prog.x"};
    std::string path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--monitor") monitor = true;
        else if (a == "--garbage") garbage = true;
        else if (a == "--strict-align") strict = true;
        else if (a == "--screen") screen = true;
        else if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else path = a;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "monrun: cannot open %s\n", path.c_str()); return 2; }
    std::vector<uint8_t> bin((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // The monitor loads only the bytes up to image_end from the file, the memory above stays zero.
    if (monitor && bin.size() >= 0x10 && std::memcmp(&bin[4], "CX16", 4) == 0) {
        const size_t imageEnd = (size_t)(bin[0x0C] | (bin[0x0D] << 8));
        if (imageEnd < bin.size()) bin.resize(imageEnd);
    }
    static ::Machine m;
    m.strictAlign = strict;
    if (garbage && !monitor) for (unsigned a = 0; a < 0x10000; a++) m.mem[a] = 0xA5;
    for (size_t a = 0; a < bin.size(); a++) m.mem[a] = bin[a];
    if (!monitor) {
        // as in cpu16: up to WFI without requests; if the processor can still wake up (the timer,
        // the disk), the emulator waits for the nearest device event
        unsigned long n = 0, maxSteps = 20000000;
        for (;;) {
            n += runUntilIdle(m, maxSteps - n);
            if (n >= maxSteps || !m.canWake()) break;
            const int64_t wait = m.nextEvent() - m.dev.clock();
            if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds(wait));
            m.poll();
        }
        std::fflush(stdout);
        if (screen) std::fputs(m.screenText().c_str(), stdout);
        if (strict) std::fprintf(stderr, "strict-align: alignment violations: %lu\n", m.alignViolations);
        return 0;
    }
    const uint16_t bssEnd = (uint16_t)(bin[0x0E] | (bin[0x0F] << 8));
    const uint16_t argvAddr = (uint16_t)((bssEnd + 1) & ~1);
    uint16_t p = (uint16_t)(argvAddr + 2 * (args.size() + 1));
    for (size_t k = 0; k < args.size(); k++) {
        m.write16((uint16_t)(argvAddr + 2 * k), p);
        for (char c : args[k]) m.mem[p++] = (uint8_t)c;
        m.mem[p++] = 0;
    }
    m.write16((uint16_t)(argvAddr + 2 * args.size()), 0);
    const char* pib = "CXPB";
    for (int k = 0; k < 4; k++) m.mem[4 + k] = (uint8_t)pib[k];
    cpu16asm::Assembler stub;
    if (!stub.assembleText("monitor.asm", MONITOR_STUB)) { std::printf("%s", stub.formatDiags().c_str()); return 2; }
    for (unsigned a = 0xE000; a < 0xE100; a++) if (stub.owner[a] >= 0) m.mem[a] = stub.image[a];
    int64_t irq = 0;
    stub.symbolValue("irq", irq);
    for (unsigned n = 0; n < 16; n++) m.write16((uint16_t)(VEC_BASE + 2 * n), (uint16_t)irq);
    int64_t sys = 0;
    stub.symbolValue("sys", sys);
    m.write16((uint16_t)(VEC_BASE + 2 * 9), (uint16_t)sys);
    m.r[0] = (uint16_t)args.size(); m.r[1] = argvAddr; m.r[SP] = 0xE000; m.r[LR] = 0xE000; m.r[FL] = FIF;
    unsigned long progSteps = 0, n = 0;
    for (; n < 20000000 && m.r[IP] != 0xE000; n++) {
        if (m.r[IP] == (uint16_t)sys && syscall(m)) break;
        if (m.r[IP] < 0xE000 && progSteps++ % 7 == 0) m.raiseIrq(0);
        if (m.step() == decltype(m.step())::Idle) break;
    }
    std::fflush(stdout);
    if (m.r[IP] != 0xE000) { std::fprintf(stderr, "monrun: program did not return to the monitor\n"); return 1; }
    std::fprintf(stderr, "exit %d\n", (int16_t)m.r[0]);
    if (strict) std::fprintf(stderr, "strict-align: alignment violations: %lu\n", m.alignViolations);
    return 0;
}

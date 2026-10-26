// An emulator of the 16-bit processor (specification, revision 6).
// ERRCMD is chosen for constrained unpredictable encodings (sect. 6.3); modified code is executed immediately (sect. 11).
// Build: make tools  or  g++ -std=c++17 -O2 -Wall -o cpu16 cpu16.cpp
//         (cpu16_isa.hpp and cpu16_machine.hpp must lie next to it)
// Run: ./cpu16               - demo (prints a string through port $FF20)
//         ./cpu16 --selftest    - built-in tests
//         ./cpu16 [-t] [--strict-align] [--screen] [-n steps] [-l load_address_hex]
//                 [--disk N=file[:ro]]... [--rtc DD.MM.YYYYTHH:MM:SS] image.bin
//   --strict-align: report word accesses and jumps to an odd address.
//   The machine follows the document "CPU16 Machine: Memory and Devices", revision 5 (cpu16_machine.hpp):
//   2 MB of RAM and a memory manager ($FF40..$FF48; off after reset, PAGE0..PAGE7 = 0),
//   console-display-keyboard on ports $FF20..$FF28, a 50 Hz timer on IRQ0 ($FF29),
//   a real-time clock ($FF2A..$FF2E), a disk controller ($FF30..$FF3B, IRQ1). The window $FF20..$FFFF is not translated: an address without a
//   device reads as 0, a write to it is ignored.
//   The image (at most 64 KB) is loaded at the address -l (0 by default); the memory manager is off after
//   reset, so this is both the logical and the physical address.
//   Teletype-mode output goes to stdout (UTF-8), input comes from stdin (UTF-8 is transcoded
//   to the machine encoding, the terminal's arrows and function keys - to scancodes).
//   --screen: on completion print the contents of the 80x25 screen.
//   --disk N=file[:ro]: attach the disk image N (0..7); may be repeated. The file size is a multiple of 512;
//   :ro, or a file that cannot be opened for writing, means write protection. Disk commands are executed immediately.
//   --rtc DD.MM.YYYYTHH:MM:SS: the initial clock value (24-hour notation, 2025..2088) instead of the
//   host's local time; seconds are counted from the start. Needed for reproducible runs.
//   The clock runs on the host's monotonic clock; without --rtc the initial value is the host's local time
//   (outside 2025..2088 - 1 January 2025 0:00:00), the seconds change together with the host's seconds.
//   Input from a file: ./cpu16 prog.bin < input.txt
//   Stop: the processor is in WFI, there are no requests, the timer is off and there is no disk command that
//   will raise IRQ1.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#if defined(_WIN32)
#include <conio.h>
#include <io.h>
// The Shift state for keys without a character (user32.dll); windows.h is not included: its macros
// (min, max, small, ...) interfere with the rest of the code.
extern "C" __declspec(dllimport) short __stdcall GetKeyState(int vk);
#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#endif
static const int VK_SHIFT_KEY = 0x10;
#else
#include <poll.h>
#include <unistd.h>
#endif

#include <chrono>
#include <ctime>
#include <thread>

#include "cpu16_isa.hpp"
#include "cpu16_machine.hpp"

// Forced inlining of the hot-loop helpers: without it, when the translation unit grows GCC stops inlining
// read16/write16/cond into exec (self-tests, devices), and speed drops by a third.
#if defined(_MSC_VER)
#define CPU16_HOT __forceinline
#define CPU16_HOT_LAMBDA
#else
#define CPU16_HOT inline __attribute__((always_inline))
#define CPU16_HOT_LAMBDA __attribute__((always_inline))
#endif

namespace cpu16 {

class Cpu {
public:
    enum class Status { Ok, Idle };

    static constexpr uint16_t REG_MASK[16] = {
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFE, 0x001F};
    enum : uint8_t { D_BAD = 1, D_BYTE = 2, D_DISP = 4 };

    uint16_t r[16];
    // Physical memory of 2 MB ("CPU16 Machine", sect. 7.6). mem is its start: while the memory manager is
    // off, mem[a] is the byte at the logical address a.
    std::vector<uint8_t> physMem = std::vector<uint8_t>(PHYS_SIZE, 0);
    uint8_t* const mem = physMem.data();
    uint8_t pending = 0;   // bits of pending requests IRQ0..IRQ7 (edge-triggered: cleared on entry)
    uint8_t levels = 0;    // level-triggered requests (IRQ1 of the disk controller): held as long as the device holds them
    bool waiting = false;  // the WFI state
    const uint8_t* dec;    // a predecode table indexed by the 16-bit instruction word

    // The memory manager (sect. 7): the registers and the pointers to the frames of the eight pages.
    uint8_t mmuPage[PAGE_COUNT] = {0};
    uint8_t mmuCtrl = 0;
    uint8_t* pageBase[PAGE_COUNT];

    Cpu() { dec = decodeTable(); resetMmu(); reset(); }
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;
    virtual ~Cpu() {}

    // Memory manager reset: off, PAGE0..PAGE7 = 0 (not defined by the specification).
    void resetMmu() { std::memset(mmuPage, 0, sizeof mmuPage); mmuCtrl = 0; remap(); }
    void remap() {
        for (unsigned p = 0; p < PAGE_COUNT; p++)
            pageBase[p] = mem + ((mmuCtrl & MMU_EN) ? (uint32_t)mmuPage[p] : p) * PAGE_SIZE;
    }
    // The physical address of a logical one (21 bits).
    uint32_t physAddr(uint16_t a) const {
        return (uint32_t)(pageBase[a >> 13] - mem) + (a & (PAGE_SIZE - 1));
    }
    // A memory byte at a logical address (through the memory manager, without devices).
    CPU16_HOT uint8_t& at(uint16_t a) { return pageBase[a >> 13][a & (PAGE_SIZE - 1)]; }

    // The slow bus path: the device window $FF20-$FFFF (addresses >= ioBase). It is selected by
    // the logical address before the memory manager translation. An address without a device reads
    // as 0, a write to it is ignored. Overridden in the derived classes that attach devices.
    virtual uint8_t read8(uint16_t) { return 0; }
    virtual void write8(uint16_t, uint8_t) {}

    // The fast path: ordinary memory without virtual calls.
    uint16_t ioBase = IO_BASE;  // addresses >= ioBase are served through read8/write8

    CPU16_HOT uint8_t rd8(uint16_t a) { return a < ioBase ? at(a) : read8(a); }
    CPU16_HOT void wr8(uint16_t a, uint8_t v) { if (a < ioBase) at(a) = v; else write8(a, v); }

    // The word is aligned and does not cross a page boundary (sect. 7.1).
    CPU16_HOT uint16_t read16(uint16_t a) {
        a &= 0xFFFE;
        if (a < ioBase) {
            const uint8_t* p = &at(a);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            uint16_t v; std::memcpy(&v, p, 2); return v;
#else
            return (uint16_t)(p[0] | (p[1] << 8));
#endif
        }
        return read16io(a);
    }
    // A word from the device area: by default two byte accesses. A device may
    // override this (reading a word at $FF20 is a single keyboard access).
    virtual uint16_t read16io(uint16_t a) { return (uint16_t)(read8(a) | (read8((uint16_t)(a + 1)) << 8)); }
    CPU16_HOT void write16(uint16_t a, uint16_t v) {
        a &= 0xFFFE;
        if (a < ioBase) {
            uint8_t* p = &at(a);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            std::memcpy(p, &v, 2);
#else
            p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
#endif
            return;
        }
        write8(a, (uint8_t)v);
        write8((uint16_t)(a + 1), (uint8_t)(v >> 8));
    }

    void reset() {
        std::memset(r, 0, sizeof r);  // all registers, including SP and FLAGS, are 0
        pending = 0;
        waiting = false;
    }

    void raiseIrq(unsigned n) { pending |= (uint8_t)(1u << (n & 7)); }

    // A register write that takes the special registers into account (IP: bit 0 = 0, FLAGS: bits 5..15 = 0)
    inline void setReg(unsigned n, uint16_t v) { r[n] = v & REG_MASK[n]; }

    // Access variants for executing instructions: with S=true they register odd addresses.
    // Execution is instantiated twice (S=false/true), so without --strict-align there are no checks.
    template <bool S> CPU16_HOT uint16_t rd16(uint16_t a) {
        if (S && (a & 1)) alignNote("word read", a);
        return read16(a);
    }
    template <bool S> CPU16_HOT void wr16(uint16_t a, uint16_t v) {
        if (S && (a & 1)) alignNote("word write", a);
        write16(a, v);
    }
    template <bool S> CPU16_HOT void setR(unsigned n, uint16_t v) {
        if (S && n == IP && (v & 1)) alignNote("jump", v);
        setReg(n, v);
    }

    // Debug alignment check (--strict-align). The processor behavior does not change:
    // bit 0 of a word address is still ignored, violations are only registered.
    bool strictAlign = false;
    FILE* alignLog = stderr;          // nullptr: just count
    unsigned long alignViolations = 0;
    uint16_t curIp = 0;               // the address of the current instruction (for messages)

    void alignNote(const char* what, uint16_t a) {
        if (!strictAlign) return;
        ++alignViolations;
        if (!alignLog) return;
        if (alignViolations <= 20)
            std::fprintf(alignLog, "strict-align: %s at odd address $%04X (instruction at $%04X)\n",
                         what, a, curIp);
        else if (alignViolations == 21)
            std::fprintf(alignLog, "strict-align: further messages suppressed\n");
    }

    // Device polling (the timer): once per 256 step calls, the same way in step() and run().
    // The JS engine does exactly the same, so they can be compared on an artificial clock.
    uint32_t pollCounter = 0;
    virtual void poll() {}
    inline void pollTick() { if (__builtin_expect((++pollCounter & 255) == 0, 0)) poll(); }

    // Execute one instruction (or accept an interrupt)
    Status step() { pollTick(); return stepCore(); }
    Status stepCore() {
        const uint8_t req = pending | levels;
        if (req | waiting) {
            if (waiting) {
                if (!req) return Status::Idle;
                waiting = false;  // a request wakes the processor even with IF=0
            }
            if (req && (r[FL] & FIF)) {
                unsigned n = 0;
                while (!(req & (1u << n))) n++;  // a lower number means a higher priority
                pending &= (uint8_t)~(1u << n);   // a level-triggered request stays as long as the device holds it
                if (strictAlign) enter<true>(n, r[IP]); else enter<false>(n, r[IP]);
                return Status::Ok;
            }
        }
        if (strictAlign) exec<true>(); else exec<false>();
        return Status::Ok;
    }

    // Batch run: a fast loop without per-instruction overhead.
    // Returns the number of executed instructions; stops at WFI with no requests.
    unsigned long run(unsigned long maxSteps) {
        return strictAlign ? runT<true>(maxSteps) : runT<false>(maxSteps);
    }
    template <bool S> unsigned long runT(unsigned long maxSteps) {
        unsigned long n = 0;
        while (n < maxSteps) {
            pollTick();
            if (__builtin_expect(pending | levels | waiting, 0)) {
                if (stepCore() == Status::Idle) break;
            } else {
                exec<S>();
            }
            n++;
        }
        return n;
    }

    // Checking the validity of an instruction by its first word (sect. 6 of the specification)
    // Encoding validity: a common function from cpu16_isa.hpp. For constrained unpredictable encodings
    // the emulator chooses ERRCMD, so only Validity::Valid is accepted.
    static bool valid(unsigned op, unsigned r1, unsigned r2, unsigned mod) {
        return cpu16::valid(op, r1, r2, mod);
    }

private:
    // Predecoding: validity (sect. 6), the presence of disp and byte-ness are computed
    // once for all 65536 words, after that each instruction needs a single table access.
    static const uint8_t* decodeTable() {
        static const uint8_t* t = [] {
            uint8_t* p = new uint8_t[65536];
            for (unsigned w = 0; w < 65536; w++) {
                const unsigned op = w >> 11, r1 = (w >> 7) & 15, r2 = (w >> 3) & 15, mod = w & 7;
                uint8_t d = 0;
                if (!valid(op, r1, r2, mod)) d |= D_BAD;
                if (op == LDZX || op == LDSX || op == XCHB || op == STB) d |= D_BYTE;
                if (mod >= 5 && op != JR) d |= D_DISP;
                p[w] = d;
            }
            return p;
        }();
        return t;
    }

    // The entry sequence: IRQ, TRAP, ERRCMD
    template <bool S> void enter(unsigned n, uint16_t savedIp) {
        r[SP] -= 2; wr16<S>(r[SP], savedIp);
        r[SP] -= 2; wr16<S>(r[SP], r[FL]);
        r[FL] &= (uint16_t)~FIF;
        setR<S>(IP, rd16<S>((uint16_t)(VEC_BASE + 2 * (n & 15))));
    }

    static uint16_t zs(uint16_t res) {
        return (uint16_t)((res == 0 ? ZF : 0) | ((res & 0x8000) ? SF : 0));
    }

    CPU16_HOT bool cond(unsigned c) const {
        const uint16_t f = r[FL];
        const bool z = f & ZF, cf = f & CF, s = f & SF, o = f & OF;
        switch (c) {
        case 0: return true;
        case 1: return z;
        case 2: return !z;
        case 3: return cf;
        case 4: return !cf;
        case 5: return !cf && !z;
        case 6: return cf || z;
        case 7: return s;
        case 8: return !s;
        case 9: return o;
        case 10: return !o;
        case 11: return s != o;
        case 12: return s == o;
        case 13: return !z && s == o;
        case 14: return z || s != o;
        default: return false;
        }
    }

    template <bool S> void exec() {
        const uint16_t start = r[IP];
        if (S) curIp = start;
        const uint16_t w = rd16<S>(start);
        r[IP] = (uint16_t)(start + 2);

        const unsigned op = w >> 11, r1 = (w >> 7) & 15, r2 = (w >> 3) & 15, mod = w & 7;

        // An invalid instruction: before any side effects, the instruction address is saved
        const uint8_t d = dec[w];
        if (__builtin_expect(d & D_BAD, 0)) { enter<S>(VEC_ERRCMD, start); return; }

        switch (op) {
        case MISC:   // the combined opcode: the operation is in the R2 field (the validity of the subcode has already been checked)
            switch (r2) {
            case MISC_NEG: {
                const uint16_t a = r[r1], res = (uint16_t)(0 - a);
                commit(r1, res, (uint16_t)(zs(res) | (a != 0 ? CF : 0) | (a == 0x8000 ? OF : 0)), true);
                return;
            }
            case MISC_NOT: setR<S>(r1, (uint16_t)~r[r1]); return;
            case MISC_BSX: setR<S>(r1, (uint16_t)(int16_t)(int8_t)(r[r1] & 0xFF)); return;
            case MISC_IRET: {
                const uint16_t f = rd16<S>(r[SP]); r[SP] += 2;
                const uint16_t ip = rd16<S>(r[SP]); r[SP] += 2;
                setR<S>(FL, f);
                setR<S>(IP, ip);
                return;
            }
            default: waiting = true; return;   // MISC_WFI
            }
        case JR:  // a short conditional jump: a 7-bit offset (the R2:MOD fields), in words
            if (cond(r1)) setR<S>(IP, (uint16_t)(r[IP] + 2 * ((int8_t)((w & 0x7F) << 1) >> 1)));
            return;
        default: break;
        }

        const bool byteOp = d & D_BYTE;
        const unsigned step = byteOp ? 1 : 2;

        // The second word of the instruction
        uint16_t disp = 0;
        if (d & D_DISP) { disp = rd16<S>(r[IP]); r[IP] = (uint16_t)(r[IP] + 2); }
        // Now IP equals the address of the next instruction.

        const uint16_t a = r[r1];  // the R1 source is fixed before R2 is modified

        bool isMem = false;
        uint16_t ea = 0, val = 0;
        switch (mod) {
        case 0: val = r[r2]; break;
        case 1: val = (uint16_t)r2; break;
        case 2: ea = r[r2]; isMem = true; break;
        case 3:
            if (r2 == IP) { val = rd16<S>(r[IP]); r[IP] = (uint16_t)(r[IP] + 2); }
            else { ea = r[r2]; setR<S>(r2, (uint16_t)(ea + step)); isMem = true; }
            break;
        case 4: setR<S>(r2, (uint16_t)(r[r2] - step)); ea = r[r2]; isMem = true; break;
        case 5: ea = disp; isMem = true; break;
        case 6: ea = (uint16_t)(r[r2] + disp); isMem = true; break;
        case 7: val = (uint16_t)(r[r2] + disp); break;
        }

        auto load = [&]() CPU16_HOT_LAMBDA -> uint16_t {
            if (!isMem) return val;
            return byteOp ? rd8(ea) : rd16<S>(ea);
        };

        switch (op) {
        case LD: setR<S>(r1, load()); return;
        case LDZX: setR<S>(r1, (uint16_t)(load() & 0xFF)); return;
        case LDSX: setR<S>(r1, (uint16_t)(int16_t)(int8_t)(load() & 0xFF)); return;
        case ST: wr16<S>(ea, a); return;
        case STB: wr8(ea, (uint8_t)a); return;
        case XCHB: {
            const uint8_t old = rd8(ea);  // the bus is seized, the instruction is indivisible
            wr8(ea, (uint8_t)a);
            setR<S>(r1, old);
            return;
        }
        case ADD: case ADC: {
            const uint16_t b = load();
            const uint32_t sum = (uint32_t)a + b + (op == ADC ? (r[FL] & CF ? 1 : 0) : 0);
            const uint16_t res = (uint16_t)sum;
            uint16_t f = zs(res);
            if (sum >> 16) f |= CF;
            if (~(a ^ b) & (a ^ res) & 0x8000) f |= OF;
            commit(r1, res, f, true);
            return;
        }
        case SUB: case SBC: case CMP: {
            const uint16_t b = load();
            const uint32_t bw = (uint32_t)b + (op == SBC ? (r[FL] & CF ? 1 : 0) : 0);
            const uint16_t res = (uint16_t)(a - bw);
            uint16_t f = zs(res);
            if ((uint32_t)a < bw) f |= CF;  // CF = borrow
            if ((a ^ b) & (a ^ res) & 0x8000) f |= OF;
            commit(r1, res, f, op != CMP);
            return;
        }
        case AND: case OR: case XOR: case TEST: {
            const uint16_t b = load();
            const uint16_t res = (op == OR) ? (a | b) : (op == XOR) ? (a ^ b) : (a & b);
            commit(r1, res, zs(res), op != TEST);
            return;
        }
        case SHL: case SHR: case SAR: case ROR: {
            const unsigned n = load() & 15;
            if (n == 0) return;  // a shift by 0: nothing changes
            uint16_t res = 0, cf = 0;
            if (op == SHL) { res = (uint16_t)(a << n); cf = (a >> (16 - n)) & 1; }
            else if (op == SHR) { res = (uint16_t)(a >> n); cf = (a >> (n - 1)) & 1; }
            else if (op == SAR) { res = (uint16_t)((int16_t)a >> n); cf = (a >> (n - 1)) & 1; }
            else { res = (uint16_t)((a >> n) | (a << (16 - n))); cf = (res >> 15) & 1; }
            commit(r1, res, (uint16_t)(zs(res) | (cf ? CF : 0)), true);
            return;
        }
        case MUL: case MULHU: {   // unsigned 16x16 = 32
            const uint32_t p = (uint32_t)a * load();
            const uint16_t res = (uint16_t)(op == MUL ? p : p >> 16);
            // MUL: CF = OF = 1 if the high word is not 0 (the result did not fit); MULHU: CF = OF = 0
            commit(r1, res, (uint16_t)(zs(res) | (op == MUL && (p >> 16) ? CF | OF : 0)), true);
            return;
        }
        case DIVU: case REMU: {   // unsigned 16/16
            const uint16_t b = load();
            // division by 0: no exception; the quotient is $FFFF, the remainder = the dividend (as with restoring division), OF = 1
            const uint16_t res = b == 0 ? (op == DIVU ? 0xFFFF : a) : (op == DIVU ? a / b : a % b);
            commit(r1, res, (uint16_t)(zs(res) | (b == 0 ? OF : 0)), true);
            return;
        }
        case JXX: case CALL: case TRAP: {
            if (op == TRAP) { enter<S>(load() & 15, r[IP]); return; }
            const uint16_t target = (mod == 1)
                ? (uint16_t)(r[IP] + 2 * (int)((int8_t)(r2 << 4) >> 4))  // a signed offset in words
                : load();
            if (op == CALL) { r[LR] = r[IP]; setR<S>(IP, target); }
            else if (cond(r1)) setR<S>(IP, target);
            return;
        }
        }
    }

    // Writing the result and the ALU flags. With R1=R15 the flags are not updated automatically.
    void commit(unsigned r1, uint16_t res, uint16_t f, bool write) {
        if (write) setReg(r1, res);
        if (write && r1 == FL) return;
        r[FL] = (uint16_t)((r[FL] & FIF) | f);
    }
};

}  // namespace cpu16

// ---------------------------------------------------------------------------
// A machine with a console
// ---------------------------------------------------------------------------
using namespace cpu16;

// A key press in the Windows console (_getwch) -> bytes of the terminal stream, which TerminalKeys parses.
// The console delivers Enter as '\r' (Ctrl+Enter as '\n'), and the arrows, Home, End, Ins, Del and F1-F12
// with the prefix 0 or 0xE0 and a key code. Enter becomes '\n' (sect. 2.6: Enter is $0A), Ctrl+Enter becomes
// the sequence CSI 13;5u (an event with character 0 and scancode $1C), the keys with a prefix become the same
// ESC sequences as on a terminal, characters become UTF-8.
// An empty string is a key that does not exist on the machine.
static std::string winConsoleKey(unsigned ch, unsigned ext, bool shift = false) {
    if (ch == 0 || ch == 0xE0) {
        // Ctrl and Shift with keys that have no character: the console's own codes -> scancode and MODS -> \e[...;m
        static const struct { unsigned code; uint8_t scan, mods; } MODKEYS[] = {
            {0x73, 0x4B, MOD_CTRL}, {0x74, 0x4D, MOD_CTRL}, {0x8D, 0x48, MOD_CTRL}, {0x91, 0x50, MOD_CTRL},
            {0x77, 0x47, MOD_CTRL}, {0x75, 0x4F, MOD_CTRL}, {0x84, 0x49, MOD_CTRL}, {0x76, 0x51, MOD_CTRL},
            {0x92, 0x52, MOD_CTRL}, {0x93, 0x53, MOD_CTRL}, {0x87, 0x57, MOD_SHIFT}, {0x88, 0x58, MOD_SHIFT},
            {0x89, 0x57, MOD_CTRL}, {0x8A, 0x58, MOD_CTRL}, {0x8B, 0x57, MOD_ALT}, {0x8C, 0x58, MOD_ALT}};
        uint8_t scan = 0, mods = 0;
        for (const auto& k : MODKEYS) if (k.code == ext) { scan = k.scan; mods = k.mods; }
        if (!scan && ext >= 0x54 && ext <= 0x5D) { scan = (uint8_t)(0x3B + ext - 0x54); mods = MOD_SHIFT; }   // Shift+F1..F10
        if (!scan && ext >= 0x5E && ext <= 0x67) { scan = (uint8_t)(0x3B + ext - 0x5E); mods = MOD_CTRL; }    // Ctrl+F1..F10
        if (!scan && ext >= 0x68 && ext <= 0x71) { scan = (uint8_t)(0x3B + ext - 0x68); mods = MOD_ALT; }     // Alt+F1..F10
        if (scan) {
            if (shift) mods |= MOD_SHIFT;
            return "\x1b" + termSequence(scan, mods);
        }
        // arrows and others with Shift: the code is the same, Shift is taken from the key state (GetKeyState)
        if (shift && ext != 0x85 && ext != 0x86) {
            static const uint8_t PLAIN[] = {0x48, 0x50, 0x4D, 0x4B, 0x47, 0x4F, 0x49, 0x51, 0x52, 0x53,
                                            0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44};
            for (uint8_t p : PLAIN) if (p == ext) return "\x1b" + termSequence(p, MOD_SHIFT);
        }
        switch (ext) {
        case 0x48: return "\x1b[A";  case 0x50: return "\x1b[B";  case 0x4D: return "\x1b[C";  case 0x4B: return "\x1b[D";
        case 0x47: return "\x1b[H";  case 0x4F: return "\x1b[F";  case 0x49: return "\x1b[5~"; case 0x51: return "\x1b[6~";
        case 0x52: return "\x1b[2~"; case 0x53: return "\x1b[3~";
        case 0x3B: return "\x1bOP";  case 0x3C: return "\x1bOQ";  case 0x3D: return "\x1bOR";  case 0x3E: return "\x1bOS";
        case 0x3F: return "\x1b[15~"; case 0x40: return "\x1b[17~"; case 0x41: return "\x1b[18~"; case 0x42: return "\x1b[19~";
        case 0x43: return "\x1b[20~"; case 0x44: return "\x1b[21~"; case 0x85: return "\x1b[23~"; case 0x86: return "\x1b[24~";
        default: return "";
        }
    }
    if (ch == '\r') return "\n";
    if (ch == '\n') return "\x1b[13;5u";     // Ctrl+Enter
    std::string s;
    if (ch < 0x80) s += (char)ch;
    else if (ch < 0x800) { s += (char)(0xC0 | (ch >> 6)); s += (char)(0x80 | (ch & 0x3F)); }
    else if (ch < 0xD800 || ch > 0xDFFF) { s += (char)(0xE0 | (ch >> 12)); s += (char)(0x80 | ((ch >> 6) & 0x3F)); s += (char)(0x80 | (ch & 0x3F)); }
    return s;   // halves of surrogate pairs (characters outside the BMP) do not occur in the machine encoding anyway
}

// The next byte of stdin without waiting: -1 if there is no input now; -2 - end of input.
// From a terminal a line becomes available after Enter (the ordinary line-by-line mode).
// In the Windows console keys arrive at once, one at a time (see winConsoleKey).
static bool consoleKeys() {
#if defined(_WIN32)
    return _isatty(0) != 0;
#else
    return false;
#endif
}
static int pollStdin() {
    static bool eof = false;
    if (eof) return -2;
#if defined(_WIN32)
    if (_isatty(0)) {
        static std::string pend;
        while (pend.empty()) {
            if (!_kbhit()) return -1;
            const unsigned ch = (unsigned)_getwch();
            const unsigned ext = (ch == 0 || ch == 0xE0) ? (unsigned)_getwch() : 0;
            pend = winConsoleKey(ch, ext, (GetKeyState(VK_SHIFT_KEY) & 0x8000) != 0);
        }
        const int b = (unsigned char)pend[0];
        pend.erase(0, 1);
        return b;
    }
    const int c = std::getchar();
    if (c == EOF) { eof = true; return -2; }
    return c;
#else
    struct pollfd p = {0, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0) return -1;
    unsigned char c;
    if (read(0, &c, 1) != 1) { eof = true; return -2; }
    return c;
#endif
}

class TerminalKeys {
public:
    std::function<int()> source = pollStdin;
    // Sequences arrive whole (the Windows console): a lone ESC at the end of the buffer is
    // the Esc key, there is no need to wait for a continuation.
    bool wholeSequences = consoleKeys();
    void pump(ConsoleDevice& dev) {
        bool eof = false;
        for (int k = 0; k < 4096; k++) {
            const int c = source();
            if (c == -1) break;
            if (c == -2) { eof = true; break; }
            buf.push_back((char)c);
        }
        parse(dev, eof);
    }
private:
    std::string buf;
    void parse(ConsoleDevice& dev, bool eof) {
        size_t i = 0;
        while (i < buf.size()) {
            if (buf[i] == 0x1B) {
                const std::string rest = buf.substr(i + 1);
                bool matched = false, prefix = false;
                for (const auto& t : TERM_SEQUENCES) {
                    const std::string seq = t.name;
                    if (rest.compare(0, seq.size(), seq) == 0) {
                        dev.pushKey(0, t.scan, t.scan == 0x1C ? MOD_CTRL : 0);     // \e[...u, \e[27;5;13~ - Ctrl+Enter
                        i += 1 + seq.size(); matched = true; break;
                    }
                    if (!rest.empty() && seq.compare(0, rest.size(), rest) == 0) prefix = true;
                }
                if (matched) continue;
                uint8_t msc = 0, mm = 0;
                bool mpre = false;
                if (const size_t n = termModified(rest, msc, mm, mpre)) { dev.pushKey(0, msc, mm); i += 1 + n; continue; }
                if (mpre) prefix = true;
                if ((rest.empty() || prefix) && !eof && !wholeSequences && rest.size() < TERM_SEQ_MAX) break;   // waiting for a continuation
                dev.pushKey(0x1B, 0x01);                                                              // a lone Esc
                i++;
                continue;
            }
            const unsigned char c0 = (unsigned char)buf[i];
            const size_t need = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : (c0 >> 3) == 30 ? 4 : 1;
            if (i + need > buf.size() && !eof) break;                                // the character has not arrived in full yet
            size_t j = i;
            const long cp = utf8Decode(buf, j);
            i = j;
            if (cp < 0) continue;
            const int code = charsetFind((char32_t)cp);
            if (code > 0) dev.pushKey((uint8_t)code, 0);
        }
        buf.erase(0, i);
    }
};

// A disk image in a file (the --disk option): a flat file of sectors, sector n at offset 512*n.
class FileDisk : public DiskImage {
public:
    // Open the file; on an error - a null pointer and the text in err.
    static std::shared_ptr<FileDisk> open(const std::string& path, bool wantRo, std::string& err) {
        bool ro = wantRo;
        FILE* f = ro ? nullptr : std::fopen(path.c_str(), "r+b");
        if (!f) { f = std::fopen(path.c_str(), "rb"); ro = true; }   // cannot be opened for writing - read only
        if (!f) { err = "cannot open " + path; return nullptr; }
        if (seek(f, 0, SEEK_END) != 0) { std::fclose(f); err = "cannot read " + path; return nullptr; }
        const long long size = tell(f);
        if (size < 0 || size % SECTOR_SIZE) {
            std::fclose(f);
            err = path + ": size " + std::to_string(size) + " is not a multiple of 512";
            return nullptr;
        }
        if (size / SECTOR_SIZE > 0xFFFFFFFFLL) { std::fclose(f); err = path + ": more than 2^32 sectors"; return nullptr; }
        auto d = std::make_shared<FileDisk>();
        d->f = f;
        d->n = (uint32_t)(size / SECTOR_SIZE);
        d->ro = ro;
        d->lbl = diskLabelFromName(path);
        return d;
    }
    ~FileDisk() override { if (f) { flush(); std::fclose(f); } }
    uint32_t sectors() const override { return n; }
    bool read(uint32_t lba, uint8_t* buf) override {
        return lba < n && seek(f, (long long)lba * SECTOR_SIZE, SEEK_SET) == 0 && std::fread(buf, 1, SECTOR_SIZE, f) == SECTOR_SIZE;
    }
    bool write(uint32_t lba, const uint8_t* buf) override {
        return lba < n && seek(f, (long long)lba * SECTOR_SIZE, SEEK_SET) == 0 && std::fwrite(buf, 1, SECTOR_SIZE, f) == SECTOR_SIZE;
    }
    bool flush() override {
        if (ro) return true;
        if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
        return _commit(_fileno(f)) == 0;
#else
        return fsync(fileno(f)) == 0;
#endif
    }
    bool readOnly() const override { return ro; }
    std::string label() const override { return lbl; }
private:
    FILE* f = nullptr;
    uint32_t n = 0;
    bool ro = false;
    std::string lbl;
    static int seek(FILE* f, long long off, int whence) {
#if defined(_WIN32)
        return _fseeki64(f, off, whence);
#else
        return fseeko(f, (off_t)off, whence);
#endif
    }
    static long long tell(FILE* f) {
#if defined(_WIN32)
        return _ftelli64(f);
#else
        return (long long)ftello(f);
#endif
    }
};

// The host's local time for the machine clock (sect. 9.1): the seconds from 1 January 2025 and how many
// microseconds have passed since the start of the host's current second. Outside 2025-2088 - 1 January 2025 0:00:00.
static uint32_t hostRtcSeconds(int64_t& intoSecondUs) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    const int64_t us = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        now - std::chrono::system_clock::from_time_t(tt)).count();
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    uint32_t s = 0;
    const RtcCivil c{(unsigned)(tmv.tm_year + 1900), (unsigned)(tmv.tm_mon + 1), (unsigned)tmv.tm_mday,
                     (unsigned)tmv.tm_hour, (unsigned)tmv.tm_min, (unsigned)(tmv.tm_sec > 59 ? 59 : tmv.tm_sec)};
    if (tmv.tm_year + 1900 < 0 || !rtcFromCivil(c, s)) { intoSecondUs = 0; return 0; }
    intoSecondUs = us >= 0 && us < RTC_SECOND_US ? us : 0;
    return s;
}

// The machine: the processor, console-display-keyboard-timer ($FF20-$FF29, MODS $FF3C, SCRD $FF50), a real-time clock
// ($FF2A-$FF2E), the disk controller ($FF30-$FF3B), the memory manager registers ($FF40-$FF48).
// The rest of the device window is empty: a read gives 0, a write is ignored.
class Machine : public Cpu {
public:
    ConsoleDevice dev;
    RtcDevice rtc;
    DiskController disk;
    TerminalKeys term;
    bool readStdin = true;      // false - the keyboard events are supplied by the owner itself (tests)
    bool printTty = true;       // print the teletype output to stdout
    std::vector<uint8_t>* ttyCapture = nullptr;   // tests: collect the teletype output here

    Machine() {
        const auto now = [] {
            return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        dev.clock = now;
        disk.clock = now;
        rtc.clock = now;
        setRtcHostTime();
        // DMA: logical addresses, translated by the memory manager like processor accesses.
        // The buffer is always in $0000-$FF1F (BADADDR), so DMA never touches the device window.
        disk.dmaRead = [this](uint16_t a) { return at(a); };
        disk.dmaWrite = [this](uint16_t a, uint8_t v) { at(a) = v; };
    }
    uint8_t read8(uint16_t a) override {
        if (ConsoleDevice::isPort(a)) { if (a <= PORT_SCAN) feed(); return dev.read8(a); }
        if (a == PORT_MODS) return dev.mods;
        if (a == PORT_SCRD) return (uint8_t)dev.readCell(false);
        if (a == PORT_SCRD + 1) return (uint8_t)(dev.readCell(false) >> 8);
        if (RtcDevice::isPort(a)) return rtc.read8(a);
        if (DiskController::isPort(a)) return disk.read8(a);
        if (a >= MMU_FIRST && a <= MMU_LAST) return a == MMU_CTRL ? mmuCtrl : mmuPage[a - MMU_PAGE0];
        return 0;
    }
    uint16_t read16io(uint16_t a) override {
        if (a == PORT_DATA) { feed(); return dev.read16(a); }
        if (a == PORT_SCRD) return dev.readCell(true);
        return Cpu::read16io(a);
    }
    void write8(uint16_t a, uint8_t v) override {
        if (RtcDevice::isPort(a)) { rtc.write8(a, v); return; }
        if (DiskController::isPort(a)) { disk.write8(a, v); syncIrq(); return; }
        if (a >= MMU_FIRST && a <= MMU_LAST) {
            if (a == MMU_CTRL) mmuCtrl = v & MMU_EN; else mmuPage[a - MMU_PAGE0] = v;
            remap();
            return;
        }
        if (!ConsoleDevice::isPort(a)) return;
        dev.write8(a, v);
        if (!dev.ttyOut.empty()) {
            if (ttyCapture) ttyCapture->insert(ttyCapture->end(), dev.ttyOut.begin(), dev.ttyOut.end());
            if (printTty) {
                std::string out;
                for (uint8_t b : dev.ttyOut) out += charsetToUtf8(b);
                std::fwrite(out.data(), 1, out.size(), stdout);
                std::fflush(stdout);
            }
            dev.ttyOut.clear();
        }
    }
    // Device polling: a timer tick (IRQ0), the progress of a disk command, the IRQ1 line.
    void poll() override {
        if (dev.pollTimer()) pending |= 1;
        disk.poll();
        syncIrq();
    }
    void syncIrq() { levels = disk.irq() ? 2 : 0; }
    // Whether anything can wake the processor in WFI (sect. 3): the timer or a disk command that will raise IRQ1.
    bool canWake() const { return dev.timerOn || disk.willIrq(); }
    // The nearest device event by the clock (for sleeping in WFI).
    int64_t nextEvent() const {
        int64_t t = INT64_MAX;
        if (dev.timerOn) t = dev.nextTick;
        if (disk.willIrq() && disk.nextAt < t) t = disk.nextAt;
        return t;
    }
    std::string screenText() const {
        std::string out;
        for (unsigned r = 0; r < SCREEN_ROWS; r++) {
            std::string line;
            for (unsigned c = 0; c < SCREEN_COLS; c++) {
                const char32_t cp = CHARSET[dev.chars[r * SCREEN_COLS + c]];
                line += cp ? charsetToUtf8Glyph(cp) : std::string(" ");
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line + "\n";
        }
        return out;
    }
    void feedForTest() { term.pump(dev); }
    // The clock is the host's local time; the machine's seconds change together with the host's seconds.
    void setRtcHostTime() {
        int64_t into = 0;
        const uint32_t s = hostRtcSeconds(into);
        rtc.set(s, rtc.clock() - into);
    }
private:
    void feed() { if (readStdin && dev.keys.empty()) term.pump(dev); }
    static std::string charsetToUtf8Glyph(char32_t cp) {
        std::string s;
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        return s;
    }
};

struct Asm {
    Cpu& c;
    uint16_t pc;
    void w(uint16_t x) { c.write16(pc, x); pc = (uint16_t)(pc + 2); }
    void i(unsigned op, unsigned r1, unsigned r2, unsigned mod) {
        w((uint16_t)(op << 11 | r1 << 7 | r2 << 3 | mod));
    }
    void i(unsigned op, unsigned r1, unsigned r2, unsigned mod, uint16_t disp) { i(op, r1, r2, mod); w(disp); }
    void imm(unsigned op, unsigned r1, uint16_t v) { i(op, r1, IP, 3); w(v); }  // op R1, #v
    void jr(unsigned cond, int offWords) { w((uint16_t)(JR << 11 | cond << 7 | (offWords & 0x7F))); }
};

static void trace(const Cpu& c) {
    const uint16_t ip = c.r[IP];
    const uint32_t pa = c.physAddr(ip);   // the instruction word through the memory manager (does not cross a page)
    const uint16_t w = (uint16_t)(c.mem[pa] | (c.mem[pa + 1] << 8));
    std::printf("%04X: %04X %-5s R1=%2u R2=%2u M=%u |", ip, w, (opInfo(w >> 11).name ? opInfo(w >> 11).name : "?"),
                (w >> 7) & 15, (w >> 3) & 15, w & 7);
    for (int k = 0; k < 16; k++) std::printf(" %04X", c.r[k]);
    std::printf("\n");
}

static unsigned long runUntilIdle(Cpu& c, unsigned long maxSteps, bool tr = false) {
    if (!tr) return c.run(maxSteps);
    unsigned long n = 0;
    while (n < maxSteps) {
        if (tr) trace(c);
        if (c.step() == Cpu::Status::Idle) break;
        n++;
    }
    return n;
}

static int selftest() {
    int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

    { // ADD: carry and zero
        Machine c; Asm a{c, 0};
        a.imm(LD, 0, 0xFFFF); a.i(ADD, 0, 1, 1); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[0] == 0 && (c.r[FL] & (ZF | CF)) == (ZF | CF) && !(c.r[FL] & OF));
    }
    { // CMP: borrow, sign
        Machine c; Asm a{c, 0};
        a.imm(LD, 0, 3); a.i(CMP, 0, 5, 1); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[0] == 3 && (c.r[FL] & (ZF | CF | SF)) == (CF | SF));
    }
    { // signed overflow: $7FFF + 1
        Machine c; Asm a{c, 0};
        a.imm(LD, 0, 0x7FFF); a.i(ADD, 0, 1, 1); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK((c.r[FL] & (OF | SF)) == (OF | SF));
    }
    { // PUSH/POP, PUSH SP stores the old value, POP SP loads from the stack
        Machine c; Asm a{c, 0};
        a.imm(LD, SP, 0xFF00); a.imm(LD, 1, 0x1234);
        a.i(ST, 1, SP, 4);      // PUSH R1
        a.i(LD, 2, SP, 3);      // POP R2
        a.i(ST, SP, SP, 4);     // PUSH SP
        a.i(LD, SP, SP, 3);     // POP SP
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[2] == 0x1234);
        CHECK(c.r[SP] == 0xFF00);
        CHECK(c.read16(0xFEFE) == 0xFF00);
    }
    { // CALL / RET
        Machine c; Asm a{c, 0};
        a.imm(LD, SP, 0xFF00); a.imm(CALL, 0, 0x0100); a.w(W_WFI);
        Asm b{c, 0x0100};
        b.imm(LD, 3, 7); b.i(LD, IP, LR, 0);
        runUntilIdle(c, 100);
        CHECK(c.r[3] == 7 && c.r[LR] == 8 && c.r[IP] == 0x0A);
    }
    { // TRAP / IRET, restoring IF
        Machine c; Asm a{c, 0};
        c.write16(VEC_BASE + 2 * 9, 0x0200);
        a.imm(LD, SP, 0xFF00); a.imm(OR, FL, FIF);   // the long form of STI (the test addresses do not change)
        a.i(TRAP, 0, 9, 1); a.imm(LD, 5, 1); a.w(W_WFI);
        Asm h{c, 0x0200};
        h.imm(LD, 4, 2); h.w(W_IRET);
        runUntilIdle(c, 100);
        CHECK(c.r[4] == 2 && c.r[5] == 1 && (c.r[FL] & FIF) && c.r[SP] == 0xFF00);
    }
    { // ERRCMD: ST with MOD=0, the instruction address is saved, IF is cleared
        Machine c; Asm a{c, 0};
        c.write16(VEC_BASE + 2 * 8, 0x0300);
        a.imm(LD, SP, 0xFF00); a.imm(OR, FL, FIF);   // the long form of STI (the test addresses do not change)
        a.i(ST, 1, 2, 0);  // invalid, address 8
        Asm h{c, 0x0300}; h.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.read16(0xFEFE) == 8);
        CHECK(c.read16(0xFEFC) & FIF);
        CHECK(!(c.r[FL] & FIF));
        CHECK(c.r[IP] == 0x0302);
    }
    { // shifts: CF = the last bit shifted out, a shift by 0 leaves the flags alone
        Machine c; Asm a{c, 0};
        a.imm(LD, 0, 0x8001); a.i(SHL, 0, 1, 1);
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[0] == 2 && (c.r[FL] & CF));
        Machine d; Asm b{d, 0};
        b.imm(LD, 0, 2); b.i(SHR, 0, 2, 1); b.i(SHL, 0, 0, 1); b.w(W_WFI);
        runUntilIdle(d, 100);
        CHECK(d.r[0] == 0 && (d.r[FL] & (CF | ZF)) == (CF | ZF));
    }
    { // SAR preserves the sign, ROR
        Machine c; Asm a{c, 0};
        a.imm(LD, 0, 0x8000); a.i(SAR, 0, 3, 1);
        a.imm(LD, 1, 0x0003); a.i(ROR, 1, 1, 1);
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[0] == 0xF000 && c.r[1] == 0x8001);
    }
    { // XCHB
        Machine c; Asm a{c, 0};
        c.mem[0x500] = 0xAB;
        a.imm(LD, 0, 0x0500); a.imm(LD, 1, 0x12CD); a.i(XCHB, 1, 0, 2); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[1] == 0x00AB && c.mem[0x500] == 0xCD);
    }
    { // R1==R2: the result write wins over the auto-increment
        Machine c; Asm a{c, 0};
        c.write16(0x600, 0xBEEF);
        a.imm(LD, 0, 0x0600); a.i(LD, 0, 0, 3); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[0] == 0xBEEF);
    }
    { // LDSX/LDZX, a byte at an odd address, little-endian
        Machine c; Asm a{c, 0};
        c.write16(0x700, 0x80FF);
        a.imm(LD, 0, 0x0701); a.i(LDSX, 1, 0, 2); a.i(LDZX, 2, 0, 2); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[1] == 0xFF80 && c.r[2] == 0x0080);
    }
    { // a short signed forward jump
        Machine c; Asm a{c, 0};
        a.i(JXX, 0, 1, 1); a.i(ADD, 7, 1, 1); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[7] == 0);
    }
    { // conditional jumps after CMP (unsigned and signed)
        Machine c; Asm a{c, 0};
        a.imm(LD, 0, 0xFFFF); a.i(CMP, 0, 1, 1);       // 65535 vs 1: JA is true, JL is false (-1 < 1: true)
        a.i(JXX, 5, 1, 1); a.i(ADD, 6, 1, 1);          // JA skips R6++
        a.i(JXX, 11, 1, 1); a.i(ADD, 7, 1, 1);         // JL skips R7++
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[6] == 0 && c.r[7] == 0);
    }
    { // an external interrupt
        Machine c; Asm a{c, 0};
        c.write16(VEC_BASE, 0x0400);
        a.imm(LD, SP, 0xFF00); a.imm(OR, FL, FIF);   // the long form of STI (the test addresses do not change)
        a.w(W_WFI); a.w(W_WFI);
        Asm h{c, 0x0400}; h.i(ADD, 8, 1, 1); h.w(W_IRET);
        runUntilIdle(c, 100);
        c.raiseIrq(0);
        runUntilIdle(c, 100);
        CHECK(c.r[8] == 1 && c.r[IP] == 0x000C && (c.r[FL] & FIF));
    }
    { // a write to R15 does not update the ALU flags, bits 5..15 are cleared
        Machine c; Asm a{c, 0};
        a.imm(LD, FL, 0xFFFF); a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[FL] == 0x1F);
    }
    { // [IP+disp] and LEA relative to IP
        Machine c; Asm a{c, 0};
        a.i(LD, 0, IP, 7, 0x0010);   // LEA R0, IP+$10: IP = 4 after disp
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[0] == 0x0014);
    }
    { // revision 4: one-word operations with FLAGS and their side effects (sect. 2.2)
        Machine c; Asm a{c, 0};
        a.imm(LD, 0, 0x7FFF); a.i(ADD, 0, 1, 1);    // OF=1, SF=1
        a.i(OR, FL, FIF, 1);                        // STI
        a.i(OR, FL, CF, 1);                         // STC
        a.i(XOR, FL, CF, 1);                        // CMC
        a.i(OR, FL, CF, 1);                         // STC
        a.i(AND, FL, 13, 1);                        // CLC: CF=0, as a side effect OF=0
        a.imm(LD, 0, 0x7FFF); a.i(ADD, 0, 1, 1);    // OF=1, SF=1 again (IF is preserved)
        a.imm(AND, FL, 0xFFF7);                     // CLI with no side effect
        a.i(TEST, FL, FIF, 1);                      // IF check: ZF=1, CF=SF=OF=0
        a.imm(LD, 0, 0x7FFF); a.i(ADD, 0, 1, 1);    // OF=1, SF=1
        a.i(OR, FL, FIF, 1);                        // STI
        a.i(AND, FL, 7, 1);                         // CLI: IF=0, as a side effect OF=0
        a.w(W_WFI);
        CHECK(c.read16(4) == 0x4009 && c.read16(6) == 0x6FC1 && c.read16(8) == 0x6F91);
        CHECK(c.read16(10) == 0x7791 && c.read16(14) == 0x67E9 && c.read16(36) == 0x67B9);
        auto st = [&](int n) { while (n--) c.step(); return c.r[FL]; };
        CHECK(st(2) == (OF | SF));
        CHECK(st(1) == (OF | SF | FIF));            // STI: only IF
        CHECK(st(1) == (OF | SF | FIF | CF));       // STC
        CHECK(st(1) == (OF | SF | FIF));            // CMC
        CHECK(st(1) == (OF | SF | FIF | CF));       // STC
        CHECK(st(1) == (SF | FIF));                 // CLC: CF=0 and OF=0
        CHECK(st(2) == (OF | SF | FIF));
        CHECK(st(1) == (OF | SF));                  // long CLI: OF is preserved
        CHECK(st(1) == ZF);                         // TEST FLAGS,#IF with IF=0
        CHECK(st(2) == (OF | SF));
        CHECK(st(1) == (OF | SF | FIF));            // STI
        CHECK(st(1) == SF);                         // CLI: IF=0 and OF=0
    }
    { // revision 5: JR forward by +63 words
        Machine c; Asm a{c, 0};
        a.jr(0, 63);                                  // 0: the target is 2 + 126 = 128
        Asm b{c, 128}; b.i(ADD, 7, 1, 1); b.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.read16(0) == 0x303F && c.r[7] == 1 && c.r[IP] == 132);
    }
    { // JR back by -64 words
        Machine c; Asm a{c, 0};
        a.imm(JXX, 0, 200);                           // JMP #200
        Asm b{c, 200}; b.jr(0, -64);                  // the target is 202 - 128 = 74
        Asm t{c, 74}; t.i(ADD, 7, 2, 1); t.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.read16(200) == 0x3040 && c.r[7] == 2);
    }
    { // a conditional JR: a false condition does not jump, a true one jumps
        Machine c; Asm a{c, 0};
        a.i(CMP, 0, 1, 1); a.jr(1, 1); a.i(ADD, 7, 1, 1);   // R0=0: ZF=0, JZ does not fire
        a.i(LD, 0, 1, 1); a.i(CMP, 0, 1, 1); a.jr(1, 1); a.i(ADD, 7, 4, 1);  // ZF=1: skipped
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[7] == 1);
    }
    { // revision 5: required-zero fields and JR with condition 1111 are reserved encodings
        CHECK(!Cpu::valid(MISC, 0, MISC_NEG, 1) && !Cpu::valid(MISC, 0, MISC_NOT, 2) && !Cpu::valid(MISC, 3, 5, 0));
        CHECK(!Cpu::valid(MISC, 1, MISC_IRET, 0) && !Cpu::valid(MISC, 0, MISC_WFI, 1) && !Cpu::valid(MISC, 2, MISC_WFI, 0));
        CHECK(!Cpu::valid(CALL, 1, 0, 0) && !Cpu::valid(TRAP, 2, 9, 1));
        CHECK(!Cpu::valid(LD, 0, 3, 5) && !Cpu::valid(ST, 1, 4, 5) && !Cpu::valid(JXX, 0, 1, 5));
        CHECK(!Cpu::valid(JR, 15, 0, 0));
        CHECK(Cpu::valid(MISC, 5, MISC_NEG, 0) && Cpu::valid(MISC, 0, MISC_IRET, 0) && Cpu::valid(CALL, 0, 3, 0));
        CHECK(Cpu::valid(LD, 0, 0, 5) && Cpu::valid(JR, 14, 15, 7));
        Machine c; Asm a{c, 0};                       // and an end-to-end check: WFI with R1=1 gives ERRCMD
        c.write16(VEC_BASE + 2 * 8, 0x0300);
        a.imm(LD, SP, 0xFF00); a.w(encodeMisc(MISC_WFI, 1));
        Asm h{c, 0x0300}; h.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[IP] == 0x0302 && c.read16(0xFEFE) == 4);
    }
    { // --strict-align: violations are counted, behavior does not change
        Machine c; Asm a{c, 0};
        c.strictAlign = true; c.alignLog = nullptr;
        a.imm(LD, 0, 0x0101); a.i(LD, 1, 0, 2);      // a word read at $0101: the word at $0100 is read
        a.i(JXX, 0, 0, 0);                            // JMP R0: a jump to the odd $0101 -> $0100
        c.write16(0x100, W_WFI);                      // there is a WFI at $0100
        runUntilIdle(c, 100);
        CHECK(c.r[1] == W_WFI && c.r[IP] == 0x0102 && c.alignViolations == 2);
    }
    { // port $FF20: the byte is the character, the word is the character and scancode of one event, $FF21 is the scancode
        Machine c; Asm a{c, 0};
        c.readStdin = false; c.printTty = false;
        c.dev.pushKey('A', 0x1E); c.dev.pushKey('B', 0x30); c.dev.pushKey(0, 0x48); c.dev.pushKey('x', 0x2D);
        a.i(LDZX, 1, 0, 5, PORT_DATA);   // 'A'
        a.i(LD, 2, 0, 5, PORT_DATA);     // word: 'B' | $30 << 8
        a.i(LDZX, 3, 0, 5, PORT_SCAN);   // scancode $48 (the up arrow)
        a.i(LDZX, 4, 0, 5, PORT_DATA);   // 'x'
        a.i(LDZX, 5, 0, 5, PORT_DATA);   // no events: 0
        a.i(LDZX, 6, 0, 5, PORT_COLS);   // 80
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[1] == 'A' && c.r[2] == (0x3000 | 'B') && c.r[3] == 0x48 && c.r[4] == 'x' && c.r[5] == 0 && c.r[6] == 80);
    }
    { // display: teletype with scrolling, coordinate mode, attributes, commands
        ConsoleDevice d;
        for (int i = 0; i < 26; i++) { d.write8(PORT_DATA, (uint8_t)('a' + i % 26)); d.write8(PORT_DATA, '\n'); }
        CHECK(d.chars[0] == 'c' && d.chars[23 * 80] == 'z' && d.row == 24 && d.col == 0);   // 26 lines with line feeds: two scrolls
        d.write8(PORT_CTRL, 0); d.write8(PORT_COL, 79); d.write8(PORT_ROW, 24);
        d.write8(PORT_DATA, '\n');                           // in coordinate mode - just a character
        CHECK(d.chars[1999] == '\n' && d.col == 0 && d.row == 0 && d.usedDisplay);
        d.write8(PORT_ATTR, 0x1E); d.write8(PORT_CMD, CMD_CLS);
        CHECK(d.chars[500] == ' ' && d.attrs[500] == 0x1E && d.col == 0 && d.row == 0);
        d.write8(PORT_DATA, 'Q'); d.write8(PORT_CTRL, CTRL_ATTRMODE); d.write8(PORT_COL, 0); d.write8(PORT_DATA, 0x87);
        CHECK(d.chars[0] == 'Q' && d.attrs[0] == 0x87 && d.col == 1);
        d.write8(PORT_COL, 200); d.write8(PORT_ROW, 200);
        CHECK(d.col == 79 && d.row == 24);
        d.write8(PORT_CTRL, CTRL_TTY); d.write8(PORT_COL, 78); d.write8(PORT_DATA, 9);   // a tab past the edge: a new line
        CHECK(d.col == 0 && d.row == 24 && d.chars[0] != 'Q');
    }
    { // timer: 50 Hz on IRQ0, ticks do not accumulate, it stands still when frozen
        Machine c; Asm a{c, 0};
        int64_t now = 0;
        c.dev.clock = [&] { return now; };
        c.readStdin = false;
        a.imm(LD, 0, 1); a.i(STB, 0, 0, 5, PORT_TIMER);
        a.w(W_WFI);
        runUntilIdle(c, 10);
        CHECK(c.dev.timerOn && c.waiting && !c.pending);
        now = 19999; c.poll(); CHECK(!c.pending);
        now = 20000; c.poll(); CHECK(c.pending == 1);
        c.pending = 0; now = 200000; c.poll(); CHECK(c.pending == 1 && c.dev.nextTick == 220000);   // catches up to "now"
        c.pending = 0; c.dev.clockFrozen = true; now = 900000; c.poll(); CHECK(!c.pending);
        c.dev.resumeClock(); CHECK(c.dev.nextTick == 920000);
    }
    { // terminal input: UTF-8 to the machine encoding, sequences to scancodes
        Machine c;
        std::string in = "Aя\x1b[A\x1bOP\xe2\x95\x90\x1b";
        size_t pos = 0;
        c.term.source = [&]() { return pos < in.size() ? (int)(uint8_t)in[pos++] : -2; };
        c.feedForTest();
        std::vector<std::pair<int, int>> got;
        for (const auto& e : c.dev.keys) got.push_back({e.ch, e.scan});
        CHECK((got == std::vector<std::pair<int, int>>{{'A', 0}, {0xFF, 0}, {0, 0x48}, {0, 0x3B}, {0xA0, 0}, {0x1B, 0x01}}));
    }
    { // ERRCMD on a reserved opcode and on Jxx with code 1111
        Machine c; Asm a{c, 0};
        c.write16(VEC_BASE + 2 * 8, 0x0300);
        a.imm(LD, SP, 0xFF00); a.i(JXX, 15, 0, 0);
        Asm h{c, 0x0300}; h.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[IP] == 0x0302 && c.read16(0xFEFE) == 4);
    }
    { // MODS $FF3C: the modifiers of the last event taken; $FF3D - 0; a write is ignored
        Machine c; Asm a{c, 0};
        c.readStdin = false; c.printTty = false;
        c.dev.pushKey(0, 0x48, MOD_SHIFT); c.dev.pushKey('a', 0x1E); c.dev.pushKey(0, 0x3E, MOD_SHIFT | MOD_CTRL | MOD_ALT | 8);
        a.i(LDZX, 1, 0, 5, PORT_MODS);   // nothing taken yet: 0
        a.i(LD, 2, 0, 5, PORT_DATA);     // Shift+arrow
        a.i(LD, 3, 0, 5, PORT_MODS);     // word: MODS | 0 << 8
        a.i(LDZX, 4, 0, 5, PORT_SCAN);   // 'a' without modifiers (scancode $1E)
        a.i(LDZX, 5, 0, 5, PORT_MODS);
        a.i(LDZX, 6, 0, 5, PORT_DATA);   // Shift+Ctrl+Alt+F4: the extra bit is dropped
        a.i(LDZX, 7, 0, 5, PORT_MODS);
        a.i(LDZX, 8, 0, 5, PORT_DATA);   // no events: MODS = 0
        a.i(LDZX, 9, 0, 5, PORT_MODS);
        a.w(W_WFI);
        c.wr8(PORT_MODS, 0x55);
        runUntilIdle(c, 100);
        CHECK(c.r[1] == 0 && c.r[2] == 0x4800 && c.r[3] == MOD_SHIFT && c.r[4] == 0x1E && c.r[5] == 0 &&
              c.r[6] == 0 && c.r[7] == 7 && c.r[8] == 0 && c.r[9] == 0 && c.rd8(0xFF3D) == 0);
    }
    { // SCRD (revision 9): reading the screen; a word read advances the position like coordinate mode, a byte read does not
        Machine c; c.readStdin = false; c.printTty = false;
        c.wr8(PORT_CTRL, 0); c.wr8(PORT_ATTR, 0x1E); c.wr8(PORT_COL, 79); c.wr8(PORT_ROW, 24);
        c.wr8(PORT_DATA, 'Z');                                          // (79, 24) -> position (0, 0)
        c.wr8(PORT_COL, 79); c.wr8(PORT_ROW, 24);
        CHECK(c.rd8(PORT_SCRD) == 'Z' && c.rd8(PORT_SCRD + 1) == 0x1E && c.rd8(PORT_COL) == 79);
        CHECK(c.read16io(PORT_SCRD) == 0x1E5A && c.rd8(PORT_COL) == 0 && c.rd8(PORT_ROW) == 0);
        CHECK(c.read16io(PORT_SCRD) == 0x0720 && c.rd8(PORT_COL) == 1);  // after reset: a space, $07
        c.wr8(PORT_SCRD, 0x41);                                         // a write does nothing
        CHECK(c.rd8(PORT_SCRD) == ' ' && c.mem[0xFF50] == 0);
    }
    { // the device window $FF20-$FFFF: an address without a device reads as 0, a write is ignored,
      // with any mapping of page 7; the vectors $FF00-$FF1F are ordinary memory
        Machine c; c.readStdin = false; c.printTty = false;
        c.mem[0xFF2F] = 0x55; c.mem[0xFFFE] = 0x66;
        CHECK(c.rd8(0xFF2F) == 0 && c.read16(0xFFFE) == 0 && c.rd8(0xFF3E) == 0 && c.rd8(0xFF49) == 0);
        c.wr8(0xFF50, 0x77); c.write16(0xFFF0, 0x1234); c.wr8(0xFF2F, 0x12);
        CHECK(c.mem[0xFF50] == 0 && c.mem[0xFFF0] == 0 && c.mem[0xFF2F] == 0x55);
        c.wr8(RTC_TIME, 0x34);                                         // $FF2A is a clock register, not memory
        CHECK(c.rd8(RTC_TIME) == 0x34 && c.mem[0xFF2A] == 0);
        c.write16(0xFF1E, 0xBEEF);
        CHECK(c.read16(0xFF1E) == 0xBEEF && c.mem[0xFF1E] == 0xEF);
        CHECK(c.rd8(PORT_COLS) == 80);
    }
    { // memory manager: registers, mapping, vectors via PAGE7, the device window is not translated
        Machine c; c.readStdin = false; c.printTty = false;
        CHECK(c.rd8(MMU_CTRL) == 0 && c.rd8(MMU_PAGE0 + 3) == 0);
        Asm a{c, 0};
        a.imm(LD, 0, 9 << 8 | 0);  a.i(ST, 0, 0, 5, MMU_PAGE0 + 0);   // PAGE0 = 0, PAGE1 = 9
        a.imm(LD, 0, 3 << 8 | 2);  a.i(ST, 0, 0, 5, MMU_PAGE0 + 2);
        a.imm(LD, 0, 5 << 8 | 4);  a.i(ST, 0, 0, 5, MMU_PAGE0 + 4);
        a.imm(LD, 0, 20 << 8 | 6); a.i(ST, 0, 0, 5, MMU_PAGE0 + 6);   // PAGE6 = 6, PAGE7 = 20
        a.imm(LD, 0, MMU_EN); a.i(STB, 0, 0, 5, MMU_CTRL);
        a.imm(LD, 1, 0xABCD); a.i(ST, 1, 0, 5, 0x2010);               // page 1 -> frame 9
        a.i(ST, 1, 0, 5, 0xFF02);                                      // vector 1 -> frame 20
        a.i(LD, 2, 0, 5, MMU_PAGE0 + 6);                               // word PAGE6, PAGE7
        a.i(LDZX, 3, 0, 5, PORT_COLS);                                 // the devices are visible with any PAGE7
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.mmuCtrl == MMU_EN && c.read16(0x2010) == 0xABCD);
        CHECK(c.mem[9 * 0x2000 + 0x10] == 0xCD && c.mem[9 * 0x2000 + 0x11] == 0xAB && c.mem[0x2010] == 0);
        CHECK(c.mem[20 * 0x2000 + 0x1F02] == 0xCD && c.mem[0xFF02] == 0);
        CHECK(c.r[2] == (20 << 8 | 6) && c.r[3] == 80 && c.physAddr(0xC010) == 6 * 0x2000 + 0x10);
        c.wr8(MMU_CTRL, 0xFF);                                         // bits 1..7 - 0
        CHECK(c.rd8(MMU_CTRL) == MMU_EN);
        c.wr8(MMU_CTRL, 0);                                            // turning off: physical = logical
        CHECK(c.read16(0x2010) == 0 && c.rd8(MMU_PAGE0 + 1) == 9 && c.physAddr(0x2010) == 0x2010);
    }
    { // self-mapping of code: enabling with PAGE0 = 0 continues execution, changing PAGE in another page
        Machine c; c.readStdin = false; c.printTty = false;
        c.mem[3 * 0x2000 + 0x100] = 0x5A;                              // frame 3 at offset $100
        Asm a{c, 0};
        a.imm(LD, 0, 3 << 8 | 0); a.i(ST, 0, 0, 5, MMU_PAGE0);        // PAGE0 = 0, PAGE1 = 3
        a.imm(LD, 0, 1); a.i(STB, 0, 0, 5, MMU_CTRL);
        a.i(LDZX, 4, 0, 5, 0x2100);
        a.w(W_WFI);
        runUntilIdle(c, 100);
        CHECK(c.r[4] == 0x5A);
    }
    { // disk controller: INFO, WRITE, READ via DMA, advancing of registers, STATUS, errors in order
        Machine c; c.readStdin = false; c.printTty = false;
        auto d0 = std::make_shared<MemoryDisk>(10, false, "sys");
        auto d1 = std::make_shared<MemoryDisk>(4, true, "ro");
        for (size_t i = 0; i < d0->data.size(); i++) d0->data[i] = (uint8_t)(i * 7 + (i >> 9));
        c.disk.attach(0, d0); c.disk.attach(1, d1);
        CHECK(c.rd8(DK_DRIVES) == 3 && c.rd8(DK_STATUS) == DKS_PRESENT);
        auto cmd = [&](uint8_t disk, uint8_t count, uint16_t addr, uint32_t lba, uint8_t op) {
            c.write16(DK_DISK, (uint16_t)(count << 8 | disk));
            c.write16(DK_ADDR, addr); c.write16(DK_LBA_LO, (uint16_t)lba); c.write16(DK_LBA_HI, (uint16_t)(lba >> 16));
            c.wr8(DK_CMD, op);
            return c.rd8(DK_ERROR);
        };
        CHECK(cmd(0, 0, 0x1000, 0, DKC_INFO) == DKE_OK && c.read16(0x1000) == 10 && c.rd8(0x1004) == DKI_SMALL &&
              c.rd8(0x1010) == 's' && c.rd8(0x1012) == 's' && c.rd8(0x1013) == 0);
        CHECK(c.rd8(DK_STATUS) == (DKS_DONE | DKS_PRESENT) && c.rd8(DK_ADDR) == 0x00 && c.rd8(DK_ADDR + 1) == 0x10);
        CHECK(cmd(0, 2, 0x2000, 3, DKC_READ) == DKE_OK && c.rd8(0x2000) == d0->data[3 * 512] &&
              c.rd8(0x23FF) == d0->data[5 * 512 - 1] && c.read16(DK_ADDR) == 0x2400 && c.read16(DK_LBA_LO) == 5 &&
              c.rd8(DK_COUNT) == 0);
        for (int i = 0; i < 512; i++) c.wr8((uint16_t)(0x3000 + i), (uint8_t)(255 - i));
        CHECK(cmd(0, 1, 0x3000, 9, DKC_WRITE) == DKE_OK && d0->data[9 * 512] == 255 && d0->data[9 * 512 + 511] == 0);
        CHECK(cmd(0, 2, 0x3000, 9, DKC_READ) == DKE_RANGE && c.read16(DK_LBA_LO) == 9 && c.rd8(DK_COUNT) == 2);
        CHECK(cmd(0, 1, 0xFE00, 0, DKC_READ) == DKE_BADADDR && cmd(0, 1, 0xFD20, 0, DKC_READ) == DKE_OK);
        CHECK(cmd(0, 1, 0xFF00, 0, DKC_INFO) == DKE_OK && cmd(0, 1, 0xFF01, 0, DKC_INFO) == DKE_BADADDR);
        CHECK(cmd(1, 1, 0x3000, 0, DKC_WRITE) == DKE_READONLY && (c.rd8(DK_STATUS) & (DKS_RO | DKS_ERR)) == (DKS_RO | DKS_ERR));
        CHECK(cmd(1, 9, 0xFF00, 0, DKC_WRITE) == DKE_READONLY);          // READONLY before RANGE and BADADDR
        CHECK(cmd(1, 9, 0xFF00, 0, DKC_READ) == DKE_RANGE);              // RANGE before BADADDR
        CHECK(cmd(5, 0, 0, 0, DKC_READ) == DKE_NODISK && cmd(5, 1, 0, 0, 9) == DKE_BADCMD);
        CHECK(cmd(1, 0, 0xFF00, 99, DKC_WRITE) == DKE_OK);               // COUNT = 0: success before READONLY
        CHECK(cmd(0, 0, 0, 0, DKC_FLUSH) == DKE_OK);
        c.wr8(DK_CMD, DKC_ACK);
        CHECK(c.rd8(DK_STATUS) == DKS_PRESENT && c.rd8(DK_ERROR) == 0);
        cmd(5, 1, 0, 0, DKC_READ); c.wr8(DK_CMD, DKC_ACK);              // ACK does not touch ERR and ERROR
        CHECK(c.rd8(DK_STATUS) == DKS_ERR && c.rd8(DK_ERROR) == DKE_NODISK && c.read16(DK_STATUS) == (DKE_NODISK << 8 | DKS_ERR));
        c.disk.detach(1);
        CHECK(c.rd8(DK_DRIVES) == 1);
    }
    { // DMA through the memory manager: a buffer at a logical address lands in the frame of the page
        Machine c; c.readStdin = false; c.printTty = false;
        auto d = std::make_shared<MemoryDisk>(2);
        d->data[0] = 0x11; d->data[511] = 0x22;
        c.disk.attach(0, d);
        c.mmuPage[4] = 77; c.mmuCtrl = MMU_EN; c.remap();
        for (unsigned p = 0; p < 8; p++) if (p != 4) c.mmuPage[p] = (uint8_t)p;
        c.remap();
        c.write16(DK_ADDR, 0x8000); c.wr8(DK_COUNT, 1); c.wr8(DK_CMD, DKC_READ);
        CHECK(c.rd8(DK_ERROR) == 0 && c.mem[77 * 0x2000] == 0x11 && c.mem[77 * 0x2000 + 511] == 0x22 && c.mem[0x8000] == 0);
    }
    { // level-triggered IRQ1: held until acknowledged with ACK; IRQ0 has priority
        Machine c; c.readStdin = false; c.printTty = false;
        c.disk.attach(0, std::make_shared<MemoryDisk>(1));
        c.write16(VEC_BASE + 2, 0x0200);
        Asm a{c, 0};
        a.imm(LD, SP, 0xFF00);
        a.imm(LD, 0, 1); a.i(STB, 0, 0, 5, DK_CTRL);   // IEN
        a.imm(LD, 0, DKC_FLUSH); a.i(STB, 0, 0, 5, DK_CMD);
        a.i(OR, FL, FIF, 1);                            // STI: the handler is entered
        a.w(W_WFI);
        Asm h{c, 0x0200};
        h.i(ADD, 7, 1, 1);                              // entry counter
        h.i(CMP, 7, 3, 1); h.jr(2, 2);                  // JNZ: the third entry acknowledges
        h.imm(LD, 0, DKC_ACK); h.i(STB, 0, 0, 5, DK_CMD);
        h.w(W_IRET);
        runUntilIdle(c, 1000);
        CHECK(c.r[7] == 3 && c.levels == 0 && c.waiting && !c.disk.done);
        c.disk.write8(DK_CMD, DKC_FLUSH); c.syncIrq(); c.raiseIrq(0);
        CHECK(c.levels == 2 && c.pending == 1);
    }
    { // commands with a delay: BUSY is visible, parameters are not written while BUSY, a pause stops the transfer,
      // disconnecting a disk during a command is an IO error
        Machine c; c.readStdin = false; c.printTty = false;
        int64_t now = 0;
        c.disk.clock = [&] { return now; };
        c.disk.sectorDelayUs = 1000;
        auto d = std::make_shared<MemoryDisk>(8);
        d->data[512] = 0x42;
        c.disk.attach(0, d);
        c.wr8(DK_COUNT, 3); c.wr8(DK_CMD, DKC_READ);
        c.wr8(DK_COUNT, 9); c.wr8(DK_DISK, 4);
        CHECK(c.rd8(DK_STATUS) == (DKS_BUSY | DKS_PRESENT) && c.rd8(DK_COUNT) == 3 && c.canWake() == false);
        c.wr8(DK_CTRL, 1);
        CHECK(c.canWake() && c.nextEvent() == 1000);
        now = 999; c.poll(); CHECK(c.rd8(DK_COUNT) == 3);
        now = 1000; c.poll(); CHECK(c.rd8(DK_COUNT) == 2 && c.read16(DK_ADDR) == 0x200);
        c.disk.clockFrozen = true; now = 50000; c.poll(); CHECK(c.rd8(DK_COUNT) == 2);
        c.disk.resumeClock(); CHECK(c.disk.nextAt == 51000);
        now = 51000; c.poll(); CHECK(c.rd8(DK_COUNT) == 1 && c.mem[0x200] == 0x42 && c.levels == 0);
        c.disk.detach(0); c.syncIrq();
        CHECK(c.rd8(DK_ERROR) == DKE_IO && c.rd8(DK_COUNT) == 1 && c.levels == 2 && !c.canWake());
    }
    { // disk label from the file name: without the directory, 15 characters, foreign characters - '?'
        CHECK(diskLabelFromName("/tmp/dir/data.img") == "data.img");
        CHECK(diskLabelFromName("C:\\images\\Диск_€.img") == std::string("\xc4\xe8\xf1\xea_?.img"));
        CHECK(diskLabelFromName("abcdefghijklmnopqrst") == "abcdefghijklmno");
    }
    { // clock: format, validity, conversions between seconds <-> registers
        const auto pack = [](unsigned y, unsigned mo, unsigned d, unsigned h24, unsigned mi, unsigned se, uint16_t& t, uint16_t& dt) {
            t = (uint16_t)((h24 % 12) << 12 | mi << 6 | se); dt = (uint16_t)((y - 2025) << 10 | mo << 6 | d << 1 | (h24 >= 12));
        };
        uint16_t t, d;
        CHECK(RTC_DAYS == 23376 && RTC_PERIOD == 2019686400u);
        CHECK(rtcDayNumber(63, 12, 31) + 1 == RTC_DAYS && rtcDayNumber(3, 3, 1) == 3 * 365 + 31 + 29);
        CHECK(rtcValidDate(3 << 10 | 2 << 6 | 29 << 1) && !rtcValidDate(2 << 10 | 2 << 6 | 29 << 1));  // 29.02.2028, 2027
        CHECK(!rtcValidDate(1 << 1) && !rtcValidDate(13 << 6 | 1 << 1) && !rtcValidDate(1 << 6) && !rtcValidDate(4 << 6 | 31 << 1));
        CHECK(rtcValidDate(63 << 10 | 12 << 6 | 31 << 1 | 1) && rtcValidDate(63 << 10 | 2 << 6 | 29 << 1));  // 2088 is a leap year
        CHECK(rtcValidTime(11 << 12 | 59 << 6 | 59) && !rtcValidTime(12 << 12) && !rtcValidTime(60 << 6) && !rtcValidTime(60));
        CHECK(!rtcValid(0, 0));                                       // 0 - "time unknown", month 0
        rtcFromSeconds(0, t, d); CHECK(t == 0 && d == (1 << 6 | 1 << 1));
        rtcFromSeconds(RTC_PERIOD - 1, t, d); CHECK(t == (11 << 12 | 59 << 6 | 59) && d == (63 << 10 | 12 << 6 | 31 << 1 | 1));
        bool roundTrip = true;
        for (uint32_t s = 0; s < RTC_PERIOD; s += 86400 * 3 + 3607) { rtcFromSeconds(s, t, d); roundTrip &= rtcValid(t, d) && rtcToSeconds(t, d) == s; }
        CHECK(roundTrip);
        pack(2026, 10, 11, 23, 59, 58, t, d); uint16_t t2, d2; rtcFromSeconds(rtcToSeconds(t, d), t2, d2); CHECK(t2 == t && d2 == d);
        const RtcCivil cv = rtcCivil(rtcToSeconds(t, d));
        CHECK(cv.year == 2026 && cv.month == 10 && cv.day == 11 && cv.hour == 23 && cv.minute == 59 && cv.second == 58);
        uint32_t s = 0;
        CHECK(rtcParse("11.10.2026T23:59:58", s) && s == rtcToSeconds(t, d));
        CHECK(rtcParse("01.01.2025T00:00:00", s) && s == 0 && rtcParse("31.12.2088T23:59:59", s) && s == RTC_PERIOD - 1);
        CHECK(rtcParse("29.02.2028T12:00:00", s) && !rtcParse("29.02.2027T12:00:00", s) && !rtcParse("31.12.2024T23:59:59", s));
        CHECK(!rtcParse("01.01.2089T00:00:00", s) && !rtcParse("31.04.2026T00:00:00", s) && !rtcParse("01.01.2026T24:00:00", s));
        CHECK(!rtcParse("2026-1-01T00:00:00", s) && !rtcParse("01.01.2026 00:00:00", s) && !rtcParse("01.01.2026T00:00:60", s));
        CHECK(!rtcParse("10.00.2026T00:00:00", s) && !rtcParse("00.01.2026T00:00:00", s) && !rtcParse("", s));
    }
    { // clock: commands, running, the fraction of a second, reset, processor accesses
        Machine c; c.readStdin = false; c.printTty = false;
        int64_t now = 5000000;
        c.rtc.clock = [&] { return now; };
        uint32_t s0 = 0;
        rtcParse("28.02.2028T23:59:58", s0);
        c.rtc.setNow(s0);
        const auto rd = [&] { c.wr8(RTC_CMD, RTCC_READ); return std::make_pair(c.read16(RTC_TIME), c.read16(RTC_DATE)); };
        const auto civil = [&](const char* text) { uint32_t v = 0; rtcParse(text, v); uint16_t t, d; rtcFromSeconds(v, t, d); return std::make_pair(t, d); };
        CHECK(rd() == civil("28.02.2028T23:59:58") && c.rtc.err == false);
        now += 1999999; CHECK(rd() == civil("28.02.2028T23:59:59"));
        now += 1; CHECK(rd() == civil("29.02.2028T00:00:00"));        // a leap February
        const auto at = [&](const char* text) { uint32_t v = 0; rtcParse(text, v); c.rtc.setNow(v); };
        at("11.10.2026T11:59:59"); now += 1000000; CHECK(rd() == civil("11.10.2026T12:00:00"));   // AM -> PM (noon)
        at("28.02.2027T23:59:59"); now += 1000000; CHECK(rd() == civil("01.03.2027T00:00:00"));
        at("31.12.2026T23:59:59"); now += 1000000; CHECK(rd() == civil("01.01.2027T00:00:00"));
        at("31.12.2088T23:59:59"); now += 1000000; CHECK(rd() == civil("01.01.2025T00:00:00"));   // the cycle
        now += 3600LL * 1000000; CHECK(rd() == civil("01.01.2025T01:00:00"));
        // WRITE: a valid value is set, ERR = 0; the fraction of a second is reset
        at("01.01.2026T00:00:00"); now += 700000;
        auto v = civil("15.06.2030T13:45:10");
        c.write16(RTC_TIME, v.first); c.write16(RTC_DATE, v.second); c.wr8(RTC_CMD, RTCC_WRITE);
        CHECK(c.rd8(RTC_STATUS) == 0 && c.read16(RTC_TIME) == v.first);   // a write does not touch the registers
        now += 999999; CHECK(rd() == civil("15.06.2030T13:45:10"));
        now += 1; CHECK(rd() == civil("15.06.2030T13:45:11"));
        // WRITE of an invalid value: the clock does not change and keeps running, ERR = 1; READ does not change ERR; an unknown code too
        c.write16(RTC_TIME, 0); c.write16(RTC_DATE, 0); c.wr8(RTC_CMD, RTCC_WRITE);
        CHECK(c.rd8(RTC_STATUS) == RTC_ERR && c.read16(RTC_TIME) == 0 && c.read16(RTC_DATE) == 0);
        now += 1000000; CHECK(rd() == civil("15.06.2030T13:45:12") && c.rd8(RTC_STATUS) == RTC_ERR);
        c.write16(RTC_TIME, 0x1234); c.wr8(RTC_CMD, 0); c.wr8(RTC_CMD, 5); c.wr8(RTC_CMD, 255);
        CHECK(c.rd8(RTC_STATUS) == RTC_ERR && c.read16(RTC_TIME) == 0x1234);
        c.write16(RTC_TIME, 12 << 12); c.wr8(RTC_CMD, RTCC_WRITE_TIME); CHECK(c.rd8(RTC_STATUS) == RTC_ERR);
        c.write16(RTC_DATE, 2 << 10 | 2 << 6 | 29 << 1); c.wr8(RTC_CMD, RTCC_WRITE_DATE); CHECK(c.rd8(RTC_STATUS) == RTC_ERR);
        CHECK(rd() == civil("15.06.2030T13:45:12"));
        // WRITE_TIME: the time of day and PM from DATE (the rest of DATE does not matter, even if invalid); the date is unchanged
        now += 300000;
        c.write16(RTC_TIME, 3 << 12 | 5 << 6 | 7); c.write16(RTC_DATE, 0x0001); c.wr8(RTC_CMD, RTCC_WRITE_TIME);
        CHECK(c.rd8(RTC_STATUS) == 0);
        now += 999999; CHECK(rd() == civil("15.06.2030T15:05:07"));
        now += 1; CHECK(rd() == civil("15.06.2030T15:05:08"));
        // WRITE_DATE: the date without PM (the time of day is unchanged), the second phase is not disturbed
        now += 400000;
        c.write16(RTC_DATE, 3 << 10 | 2 << 6 | 29 << 1 | 0); c.write16(RTC_TIME, 0xFFFF); c.wr8(RTC_CMD, RTCC_WRITE_DATE);
        CHECK(c.rd8(RTC_STATUS) == 0 && rd() == civil("29.02.2028T15:05:08"));
        now += 599999; CHECK(rd() == civil("29.02.2028T15:05:08"));
        now += 1; CHECK(rd() == civil("29.02.2028T15:05:09"));
        // word and bytes; a word write to $FF2E - the command in the low byte, the high byte goes to the free $FF2F
        c.write16(RTC_TIME, 0xA1B2); CHECK(c.rd8(RTC_TIME) == 0xB2 && c.rd8(RTC_TIME + 1) == 0xA1);
        c.wr8(RTC_DATE + 1, 0x7E); c.wr8(RTC_DATE, 0x55); CHECK(c.read16(RTC_DATE) == 0x7E55);
        c.write16(RTC_CMD, 0xFF00 | RTCC_READ); CHECK(std::make_pair(c.read16(RTC_TIME), c.read16(RTC_DATE)) == civil("29.02.2028T15:05:09"));
        c.write16(RTC_DATE, 0); c.wr8(RTC_CMD, RTCC_WRITE);
        CHECK(c.read16(RTC_STATUS) == RTC_ERR && c.rd8(0xFF2F) == 0);
        // machine reset: the registers are 0, the clock keeps running
        c.rtc.reset();
        CHECK(c.read16(RTC_TIME) == 0 && c.read16(RTC_DATE) == 0 && c.rd8(RTC_STATUS) == 0);
        now += 1000000; CHECK(rd() == civil("29.02.2028T15:05:10"));
        // program: READ and reading the registers
        Asm a{c, 0};
        a.imm(LD, 0, RTCC_READ); a.i(STB, 0, 0, 5, RTC_CMD);
        a.i(LD, 1, 0, 5, RTC_TIME); a.i(LD, 2, 0, 5, RTC_DATE); a.i(LDZX, 3, 0, 5, RTC_STATUS);
        a.w(W_WFI);
        c.reset(); c.run(100);
        CHECK(std::make_pair(c.r[1], c.r[2]) == civil("29.02.2028T15:05:10") && c.r[3] == 0);
    }
    { // clock: the initial value is the host's time, always valid
        Machine c;
        c.wr8(RTC_CMD, RTCC_READ);
        CHECK(rtcValid(c.read16(RTC_TIME), c.read16(RTC_DATE)));
        CHECK(c.rtc.anchor <= c.rtc.clock() && c.rtc.clock() - c.rtc.anchor < 2 * RTC_SECOND_US);
    }
    { // the --disk option: a file image; a size that is not a multiple of 512 is an error; the label; writing to the file
        const char* path = "/tmp/cpu16_selftest_disk.img";
        { FILE* f = std::fopen(path, "wb"); std::vector<uint8_t> z(1024, 0); z[512] = 9; std::fwrite(z.data(), 1, z.size(), f); std::fclose(f); }
        std::string err;
        auto d = FileDisk::open(path, false, err);
        CHECK(d && d->sectors() == 2 && !d->readOnly() && d->label() == "cpu16_selftest_");
        uint8_t buf[512];
        CHECK(d && d->read(1, buf) && buf[0] == 9 && !d->read(2, buf));
        buf[0] = 10;
        CHECK(d && d->write(0, buf) && d->flush());
        d.reset();
        auto r = FileDisk::open(path, true, err);
        CHECK(r && r->readOnly() && r->read(0, buf) && buf[0] == 10);
        r.reset();
        { FILE* f = std::fopen(path, "ab"); std::fputc(1, f); std::fclose(f); }
        CHECK(!FileDisk::open(path, false, err) && err.find("not a multiple of 512") != std::string::npos);
        std::remove(path);
    }
    { // Windows console: Enter ('\r') -> '\n', Ctrl+Enter ('\n') -> (0, $1C), keys with a 0/0xE0 prefix -> sequences, characters -> UTF-8
        Machine c;
        std::string in = winConsoleKey('\r', 0) + winConsoleKey(10, 0) + winConsoleKey(0xE0, 0x48) + winConsoleKey(0, 0x3B) +
                         winConsoleKey(0xE0, 0x86) + winConsoleKey(0x44F, 0) + winConsoleKey(27, 0) + winConsoleKey(0xE0, 0x99);
        size_t pos = 0;
        c.term.wholeSequences = true;
        c.term.source = [&]() { return pos < in.size() ? (int)(uint8_t)in[pos++] : -1; };
        c.feedForTest();
        std::vector<std::pair<int, int>> got;
        for (const auto& e : c.dev.keys) got.push_back({e.ch, e.scan});
        CHECK((got == std::vector<std::pair<int, int>>{{10, 0}, {0, 0x1C}, {0, 0x48}, {0, 0x3B}, {0, 0x58}, {0xFF, 0}, {0x1B, 0x01}}));
    }
    { // terminal: Ctrl+Enter as CSI 13;5u and xterm modifyOtherKeys, in parts
        Machine c;
        std::string in = "a\x1b[13;5u\x1b[27;5;13~b";
        size_t pos = 0, lim = 4;
        c.term.wholeSequences = false;
        c.term.source = [&]() { return pos < in.size() && pos < lim ? (int)(uint8_t)in[pos++] : -1; };
        c.feedForTest();
        lim = in.size();
        c.feedForTest();
        std::vector<std::pair<int, int>> got;
        for (const auto& e : c.dev.keys) got.push_back({e.ch, e.scan});
        CHECK((got == std::vector<std::pair<int, int>>{{'a', 0}, {0, 0x1C}, {0, 0x1C}, {'b', 0}}));
        CHECK(c.dev.keys[1].mods == MOD_CTRL && c.dev.keys[0].mods == 0);
    }
    { // terminal: keys with xterm modifiers, in parts; unfamiliar parameters - as before
        Machine c;
        std::string in = "\x1b[1;2A\x1b[1;5H\x1b[1;2S\x1b[15;2~\x1b[3;6~\x1b[1;3D\x1b[A";
        size_t pos = 0, lim = 3;
        c.term.wholeSequences = false;
        c.term.source = [&]() { return pos < in.size() && pos < lim ? (int)(uint8_t)in[pos++] : -1; };
        c.feedForTest();
        CHECK(c.dev.keys.empty());                  // \e[1 is the beginning of a sequence: wait
        lim = in.size();
        c.feedForTest();
        std::vector<std::vector<int>> got;
        for (const auto& e : c.dev.keys) got.push_back({e.ch, e.scan, e.mods});
        CHECK((got == std::vector<std::vector<int>>{{0, 0x48, MOD_SHIFT}, {0, 0x47, MOD_CTRL}, {0, 0x3E, MOD_SHIFT},
                                                    {0, 0x3F, MOD_SHIFT}, {0, 0x53, MOD_SHIFT | MOD_CTRL}, {0, 0x4B, MOD_ALT},
                                                    {0, 0x48, 0}}));
        CHECK(termSequence(0x48, MOD_SHIFT) == "[1;2A" && termSequence(0x3E, MOD_SHIFT) == "[1;2S" &&
              termSequence(0x3F, MOD_CTRL) == "[15;5~" && termSequence(0x47, 0) == "[H");
    }
    { // Windows console: Shift+F4, Ctrl+Home, Shift+arrow (by GetKeyState) -> xterm sequences
        Machine c;
        std::string in = winConsoleKey(0, 0x57) + winConsoleKey(0xE0, 0x77) + winConsoleKey(0xE0, 0x50, true) +
                         winConsoleKey(0xE0, 0x73, true);
        size_t pos = 0;
        c.term.wholeSequences = true;
        c.term.source = [&]() { return pos < in.size() ? (int)(uint8_t)in[pos++] : -1; };
        c.feedForTest();
        std::vector<std::vector<int>> got;
        for (const auto& e : c.dev.keys) got.push_back({e.scan, e.mods});
        CHECK((got == std::vector<std::vector<int>>{{0x3E, MOD_SHIFT}, {0x47, MOD_CTRL}, {0x50, MOD_SHIFT},
                                                    {0x4B, MOD_SHIFT | MOD_CTRL}}));
    }
#undef CHECK
    std::printf(fails ? "Tests: %d failed\n" : "All tests passed\n", fails);
    return fails ? 1 : 0;
}

static void demo(Machine& c) {
    // Prints a string from memory at address $0100 to port $FF20
    const char* msg = "Hello from CPU16!\n";
    for (size_t k = 0; k <= std::strlen(msg); k++) c.mem[0x100 + k] = (uint8_t)msg[k];
    Asm a{c, 0};
    a.imm(LD, SP, 0xFF00);       // 0
    a.imm(LD, 0, 0x0100);        // 2   R0 = pointer
    a.i(LDZX, 1, 0, 3);          // 4   loop: R1 = [R0++]
    a.i(CMP, 1, 0, 1);           // 5
    a.i(JXX, 1, 3, 1);           // 6   JZ done (+3 words)
    a.i(STB, 1, 0, 5, 0xFF20);   // 7   output a character
    a.i(JXX, 0, (unsigned)(-6) & 15, 1);  // 9   JMP loop (-6 words)
    a.w(W_WFI);           // 10  done
}

int main(int argc, char** argv) {
    bool tr = false, haveImage = false, strictAlign = false, showScreen = false;
    unsigned long maxSteps = 10000000UL;
    unsigned loadAddr = 0;
    const char* path = nullptr;
    struct DiskArg { unsigned n; std::string file; bool ro; };
    std::vector<DiskArg> diskArgs;
    bool haveRtc = false;
    uint32_t rtcStart = 0;

    for (int k = 1; k < argc; k++) {
        std::string s = argv[k];
        if (s == "--selftest") return selftest();
        else if (s == "-t") tr = true;
        else if (s == "--strict-align") strictAlign = true;
        else if (s == "--screen") showScreen = true;
        else if (s == "-n" && k + 1 < argc) maxSteps = std::strtoul(argv[++k], nullptr, 10);
        else if (s == "-l" && k + 1 < argc) loadAddr = (unsigned)std::strtoul(argv[++k], nullptr, 16) & 0xFFFF;
        else if (s == "--disk" && k + 1 < argc) {
            // N=file[:ro]; the path may contain ':' (C:\...), so only the suffix ":ro" is stripped
            std::string a = argv[++k];
            if (a.size() < 3 || a[0] < '0' || a[0] > '7' || a[1] != '=') {
                std::fprintf(stderr, "--disk %s: expected N=file[:ro] with N = 0..7\n", a.c_str());
                return 1;
            }
            DiskArg d{(unsigned)(a[0] - '0'), a.substr(2), false};
            if (d.file.size() > 3 && d.file.compare(d.file.size() - 3, 3, ":ro") == 0) { d.ro = true; d.file.resize(d.file.size() - 3); }
            for (const auto& o : diskArgs)
                if (o.n == d.n) { std::fprintf(stderr, "--disk: disk %u given twice\n", d.n); return 1; }
            diskArgs.push_back(d);
        }
        else if (s == "--rtc" && k + 1 < argc) {
            if (!rtcParse(argv[++k], rtcStart)) {
                std::fprintf(stderr, "--rtc %s: expected DD.MM.YYYYTHH:MM:SS, a valid date and time in 2025..2088\n", argv[k]);
                return 1;
            }
            haveRtc = true;
        }
        else { path = argv[k]; haveImage = true; }
    }

    Machine c;
    c.strictAlign = strictAlign;
    if (haveRtc) c.rtc.setNow(rtcStart);
    for (const auto& d : diskArgs) {
        std::string err;
        auto img = FileDisk::open(d.file, d.ro, err);
        if (!img) { std::fprintf(stderr, "--disk %u: %s\n", d.n, err.c_str()); return 1; }
        if (img->readOnly() && !d.ro) std::fprintf(stderr, "Disk %u: %s is read-only\n", d.n, d.file.c_str());
        c.disk.attach(d.n, img);
    }
    if (haveImage) {
        FILE* f = std::fopen(path, "rb");
        if (!f) { std::perror(path); return 1; }
        size_t n = std::fread(c.mem + loadAddr, 1, 65536 - loadAddr, f);
        std::fclose(f);
        std::fprintf(stderr, "Loaded %zu bytes at $%04X\n", n, loadAddr);
    } else {
        demo(c);
    }

    // Run until WFI with no requests. If something can still wake the processor (the timer is on or
    // a disk command that will raise IRQ1 is in progress), WFI is a wait: the emulator sleeps until the nearest
    // device event and continues.
    unsigned long n = 0;
    for (;;) {
        n += runUntilIdle(c, maxSteps - n, tr);
        if (n >= maxSteps || !c.canWake()) break;
        const int64_t wait = c.nextEvent() - c.dev.clock();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds(wait));
        c.poll();
    }
    if (showScreen) std::fputs(c.screenText().c_str(), stdout);
    std::fprintf(stderr, "Instructions executed: %lu%s\n", n, n >= maxSteps ? " (limit reached)" : "");
    if (strictAlign) std::fprintf(stderr, "strict-align: alignment violations: %lu\n", c.alignViolations);
    for (int k = 0; k < 16; k++) std::fprintf(stderr, "R%-2d=%04X%s", k, c.r[k], (k % 4 == 3) ? "\n" : "  ");
    return 0;
}

// gen_cases.cpp - reference cases for checking the JS engine against the cpu16.cpp emulator.
//
// For each case a random program is generated (mostly valid instructions
// of all classes, some words are random data, accesses to all device ports, including
// the clock, the disk controller and the memory manager, and to empty addresses of the device window),
// random vectors and initial registers. The program runs on the Machine from cpu16.cpp
// with an artificial clock (10 us per step and rare jumps of 0.4 s); the schedule of IRQ requests
// and key presses, state hashes every 100 steps, the hash of physical memory, the screen,
// device registers, the clock and disks at the end, and the teletype output are recorded. The initial clock value is
// random, often a few seconds before noon, midnight, the end of a month or the end of 2088. The disks are the same in C++ and JS
// (the contents come from the diskByte formula), the disk commands are instant or 1 ms per sector.
// test_engine.js repeats the same in the JS engine and compares.
//
// Build (from the project root): g++ -std=c++17 -O2 -Itools -o build/gen_cases debugger/tests/gen_cases.cpp
// Run: ./gen_cases [cases] [steps] > cases.jsonl

#define main cpu16_emulator_main
#include "cpu16.cpp"
#undef main

#include <cstdio>
#include <string>
#include <vector>

static uint64_t rs;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

// FNV-1a over 16-bit values; in JS: h = Math.imul(h ^ v, 16777619) >>> 0.
static uint32_t fnv(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

// The contents of the test disks: byte i of disk n (the same in test_engine.js).
static uint8_t diskByte(unsigned n, uint32_t i) { return (uint8_t)(((i * 31u) ^ ((i >> 9) * 7u)) + n * 17u); }
struct TestDisk { unsigned n; uint32_t sectors; bool ro; };
static const TestDisk TEST_DISKS[] = {{0, 64, false}, {1, 16, true}, {3, 8, false}};

static std::string b64(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '='; o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

int main(int argc, char** argv) {
    using namespace cpu16;
    const int cases = argc > 1 ? std::atoi(argv[1]) : 500;
    const int steps = argc > 2 ? std::atoi(argv[2]) : 3000;
    static const unsigned ops[] = {ADD, ADD, ADC, ADC, SUB, SUB, SBC, SBC, AND, OR, XOR, CMP, CMP, TEST, SHL, SHR, SAR, ROR,
                                   MISC, MISC, MISC, JXX, JXX, JR, JR, CALL, TRAP, LD, LD, LD, LDZX, LDSX,
                                   ST, ST, STB, XCHB, MUL, MUL, MULHU, MULHU, DIVU, DIVU, REMU, REMU};
    static const unsigned mods[] = {0, 0, 1, 1, 2, 3, 3, 4, 5, 5, 6, 7};
    for (int cs = 1; cs <= cases; cs++) {
        rs = 0x9E3779B97F4A7C15ULL * (uint64_t)cs + 777;
        for (int k = 0; k < 5; k++) rnd();
        Machine c;
        int64_t clockNow = 0;
        std::vector<uint8_t> tty;
        c.readStdin = false; c.printTty = false; c.ttyCapture = &tty;
        c.dev.clock = [&] { return clockNow; };
        c.disk.clock = [&] { return clockNow; };
        c.rtc.clock = [&] { return clockNow; };
        // the clock: the value and the second's phase; often near boundaries where carries trigger
        static const char* edges[] = {"11.10.2026T11:59:58", "11.10.2026T23:59:57", "28.02.2027T23:59:58", "28.02.2028T23:59:58",
                                      "29.02.2028T23:59:59", "30.04.2030T23:59:58", "31.12.2041T23:59:56", "31.12.2088T23:59:57"};
        uint32_t rtcStart = rnd() % RTC_PERIOD;
        if (rnd() % 3) rtcParse(edges[rnd() % 8], rtcStart);
        const int64_t rtcAnchor = -(int64_t)(rnd() % 1000000);
        c.rtc.set(rtcStart, rtcAnchor);
        // values for the clock registers: valid ones, ones near the validity limits and random ones
        const auto rtcTimeVal = [&]() -> uint16_t {
            switch (rnd() % 4) {
            case 0: return (uint16_t)rnd();
            case 1: return (uint16_t)((rnd() % 13) << 12 | (rnd() % 61) << 6 | rnd() % 61);
            default: return (uint16_t)((rnd() % 12) << 12 | (rnd() % 60) << 6 | rnd() % 60);
            }
        };
        const auto rtcDateVal = [&]() -> uint16_t {
            switch (rnd() % 4) {
            case 0: return (uint16_t)rnd();
            case 1: return (uint16_t)((rnd() % 64) << 10 | (rnd() % 14) << 6 | (28 + rnd() % 4) << 1 | (rnd() & 1));
            default: { uint16_t t, d; rtcFromSeconds(rnd() % RTC_PERIOD, t, d); return d; }
            }
        };
        const int64_t delay = (cs % 2) ? 0 : 1000;
        c.disk.sectorDelayUs = delay;
        std::shared_ptr<MemoryDisk> disks[DISK_COUNT];
        for (const auto& t : TEST_DISKS) {
            auto d = std::make_shared<MemoryDisk>(t.sectors, t.ro, "d" + std::to_string(t.n));
            for (uint32_t i = 0; i < d->data.size(); i++) d->data[i] = diskByte(t.n, i);
            disks[t.n] = d;
            c.disk.attach(t.n, d);
        }
        for (unsigned a = 0; a < 0x2000; a += 2) {
            uint16_t w;
            if (a + 6 < 0x2000 && rnd() % 10 == 0) {
                // printing an "interesting" byte or a mode change: LD Rk, #value / STB [port], Rk
                static const uint8_t vals[] = {9, 9, 10, 13, 8, 8, 0, 'A', 0xC0, 0x1A, 0, 1, 2, 3, 4, 5, 6, 7};
                static const uint16_t ports[] = {PORT_DATA, PORT_DATA, PORT_DATA, PORT_DATA, PORT_CTRL, PORT_CMD, PORT_COL, PORT_TIMER};
                const unsigned k = rnd() % 12;
                const uint16_t port = ports[rnd() % 8];
                const uint8_t v = port == PORT_TIMER ? 1 : vals[rnd() % sizeof vals];
                if (rnd() % 3 == 0) {   // enable interrupts so that timer ticks are accepted
                    c.write16((uint16_t)a, encode(OR, FL, FIF, M_IMM4));
                    c.write16((uint16_t)(a + 2), W_NOP);
                    a += 2;
                    continue;
                }
                c.write16((uint16_t)a, encode(LD, k, IP, M_POSTINC)); c.write16((uint16_t)(a + 2), v);
                c.write16((uint16_t)(a + 4), encode(STB, k, 0, M_ABS)); c.write16((uint16_t)(a + 6), port);
                a += 6;
                continue;
            }
            if (a + 14 < 0x2000 && rnd() % 60 == 0) {
                // a ready clock setting: the value into TIME or DATE, then the write command (or READ)
                const unsigned k = rnd() % 12;
                const bool date = rnd() % 2;
                const uint16_t v = date ? rtcDateVal() : rtcTimeVal();
                const uint16_t op = (uint16_t)(rnd() % 4 ? (date ? (rnd() % 2 ? RTCC_WRITE_DATE : RTCC_WRITE) : (rnd() % 2 ? RTCC_WRITE_TIME : RTCC_WRITE)) : RTCC_READ);
                c.write16((uint16_t)a, encode(LD, k, IP, M_POSTINC)); c.write16((uint16_t)(a + 2), v);
                c.write16((uint16_t)(a + 4), encode(ST, k, 0, M_ABS)); c.write16((uint16_t)(a + 6), date ? RTC_DATE : RTC_TIME);
                c.write16((uint16_t)(a + 8), encode(LD, k, IP, M_POSTINC)); c.write16((uint16_t)(a + 10), op);
                c.write16((uint16_t)(a + 12), encode(STB, k, 0, M_ABS)); c.write16((uint16_t)(a + 14), RTC_CMD);
                a += 14;
                continue;
            }
            if (a + 6 < 0x2000 && rnd() % 48 == 0) {
                // the clock: a write of TIME or DATE as a word (sometimes a byte), a command, reading the registers
                const unsigned k = rnd() % 12, which = rnd() % 8;
                uint16_t port, v = 0;
                unsigned op = ST;
                switch (which) {
                case 0: port = RTC_TIME; v = rtcTimeVal(); break;
                case 1: port = RTC_DATE; v = rtcDateVal(); break;
                case 2: port = (uint16_t)(RTC_TIME + rnd() % 4); v = (uint16_t)rnd(); op = STB; break;
                case 3: case 4: case 5:
                    port = RTC_CMD; op = STB;
                    v = (uint16_t)(rnd() % 6 ? 1 + rnd() % 4 : (rnd() % 2 ? 0 : 5 + rnd() % 251));
                    break;
                default: port = rnd() % 2 ? RTC_TIME : RTC_DATE; op = LD; break;
                }
                if (op == LD) {
                    c.write16((uint16_t)a, encode(rnd() % 2 ? LD : LDZX, k, 0, M_ABS)); c.write16((uint16_t)(a + 2), port);
                    a += 2;
                    continue;
                }
                c.write16((uint16_t)a, encode(LD, k, IP, M_POSTINC)); c.write16((uint16_t)(a + 2), v);
                c.write16((uint16_t)(a + 4), encode(op, k, 0, M_ABS)); c.write16((uint16_t)(a + 6), port);
                a += 6;
                continue;
            }
            if (a + 14 < 0x2000 && rnd() % 40 == 0) {
                // a ready disk command: COUNT and DISK as one word, then the command code
                const unsigned k = rnd() % 12;
                const uint16_t dc = (uint16_t)((1 + rnd() % 3) << 8 | (rnd() % 3 ? 0 : rnd() % 4));
                const uint16_t op = (uint16_t)(rnd() % 5 ? 1 + rnd() % 2 : 3 + rnd() % 2);
                c.write16((uint16_t)a, encode(LD, k, IP, M_POSTINC)); c.write16((uint16_t)(a + 2), dc);
                c.write16((uint16_t)(a + 4), encode(ST, k, 0, M_ABS)); c.write16((uint16_t)(a + 6), DK_DISK);
                c.write16((uint16_t)(a + 8), encode(LD, k, IP, M_POSTINC)); c.write16((uint16_t)(a + 10), op);
                c.write16((uint16_t)(a + 12), encode(STB, k, 0, M_ABS)); c.write16((uint16_t)(a + 14), DK_CMD);
                a += 14;
                continue;
            }
            if (a + 6 < 0x2000 && rnd() % 14 == 0) {
                // the disk controller and the memory manager: plausible register values
                const unsigned k = rnd() % 12, which = rnd() % 16;
                uint16_t port, v;
                bool word = false;
                switch (which) {
                case 0: case 1: port = DK_ADDR; v = (uint16_t)(rnd() % 4 ? (rnd() % 0x40) * 0x100 : 0xFE00 + (rnd() % 3) * 0x100); word = true; break;
                case 2: port = DK_LBA_LO; v = (uint16_t)(rnd() % 4 ? rnd() % 6 : rnd() % 70); word = true; break;
                case 3: port = DK_LBA_HI; v = (uint16_t)(rnd() % 16 ? 0 : 1); word = true; break;
                case 4: port = DK_DISK; v = (uint16_t)(rnd() % 3 ? 0 : rnd() % 8); break;
                case 5: port = DK_COUNT; v = (uint16_t)(rnd() % 5); break;
                case 6: port = DK_CTRL; v = (uint16_t)(rnd() % 2); break;
                case 7: case 8: case 9: port = DK_CMD; v = (uint16_t)(rnd() % 4 ? 1 + rnd() % 4 : rnd() % 7); break;
                case 10: port = DK_DISK; v = (uint16_t)((1 + rnd() % 2) << 8 | (rnd() % 2 ? 0 : rnd() % 4)); word = true; break;
                case 11: case 12: case 13: port = (uint16_t)(MMU_PAGE0 + rnd() % 8); v = (uint16_t)(rnd() % 3 ? (port - MMU_PAGE0) : rnd() % 12); break;
                case 14: port = (uint16_t)(MMU_PAGE0 + 2 * (rnd() % 4)); v = (uint16_t)((rnd() % 12) << 8 | (rnd() % 12)); word = true; break;
                default: port = MMU_CTRL; v = (uint16_t)(rnd() % 3 == 0); break;
                }
                c.write16((uint16_t)a, encode(LD, k, IP, M_POSTINC)); c.write16((uint16_t)(a + 2), v);
                c.write16((uint16_t)(a + 4), encode(word ? ST : STB, k, 0, M_ABS)); c.write16((uint16_t)(a + 6), port);
                a += 6;
                continue;
            }
            if (a + 2 < 0x2000 && rnd() % 8 == 0) {
                // accesses to device ports (and SCRD: reading the screen with a position shift) and empty window addresses: a read as byte and word,
                // a write as byte and word, an exchange
                static const unsigned portOps[] = {LDZX, LDZX, LDSX, LD, XCHB, ADD, STB, STB, STB, ST};
                const unsigned pk = rnd() % 8;
                const uint16_t port = (uint16_t)(pk < 3 ? 0xFF20 + (rnd() % 3 ? rnd() % 10 : rnd() % 16)
                                                 : pk == 3 ? DK_FIRST + rnd() % 14 : pk == 4 ? MMU_FIRST + rnd() % 10
                                                 : pk == 5 ? PORT_MODS + rnd() % 2 : pk == 6 ? PORT_SCRD + rnd() % 2
                                                 : 0xFF00 + rnd() % 0x100);
                c.write16((uint16_t)a, encode(portOps[rnd() % 10], rnd() % 12, 0, M_ABS));
                c.write16((uint16_t)(a + 2), port);
                a += 2;
                continue;
            }
            if (rnd() % 10 < 7) {
                const unsigned op = ops[rnd() % (sizeof ops / sizeof *ops)];
                unsigned r1 = rnd() % 16, r2 = rnd() % 16, mod = mods[rnd() % 12];
                if (rnd() % 6 == 0) r1 = 15;
                if (rnd() % 6 == 0) r2 = 15;
                if (op == JXX || op == JR) r1 = (rnd() % 8 == 0) ? 15 : rnd() % 15;
                if ((op == CALL || op == TRAP) && rnd() % 4) r1 = 0;
                if (op == MISC) {   // the subcode in R2: mostly valid 0..4, sometimes reserved
                    r2 = rnd() % 8;
                    if (rnd() % 4) mod = 0;
                    if (r2 >= MISC_IRET && rnd() % 4) r1 = 0;
                }
                if ((op == DIVU || op == REMU) && mod == M_IMM4 && rnd() % 3 == 0) r2 = 0;   // division by 0
                if (mod == M_ABS && rnd() % 4) r2 = 0;
                w = (op == JR) ? encodeJR(r1, (int)(rnd() % 128) - 64) : encode(op, r1, r2, mod);
            } else {
                w = (uint16_t)rnd();
            }
            c.write16((uint16_t)a, w);
        }
        uint16_t vec[16];
        for (int n = 0; n < 16; n++) { vec[n] = (uint16_t)((rnd() % 0x1000) * 2); c.write16((uint16_t)(VEC_BASE + 2 * n), vec[n]); }
        uint16_t init[16];
        for (int k = 0; k < 12; k++) c.setReg(k, (rnd() & 1) ? (uint16_t)rnd() : (uint16_t)(rnd() % 0x2000));
        c.setReg(SP, (uint16_t)(0x1000 + (rnd() % 0x400) * 2));
        c.setReg(LR, (uint16_t)(rnd() % 0x2000));
        c.setReg(FL, (uint16_t)(rnd() & 0x1F));
        for (int k = 0; k < 16; k++) init[k] = c.r[k];
        std::vector<uint8_t> image(c.mem, c.mem + 0x2000);
        // memory bytes under the port: reading the port must not return them (only input or 0)
        const uint8_t port0 = (uint8_t)rnd(), port1 = (uint8_t)rnd();
        c.mem[0xFF20] = port0; c.mem[0xFF21] = port1;
        const size_t inLen = rnd() % 60;
        std::string keys;   // initial events: triples of bytes (character, scancode, modifiers)
        for (size_t k = 0; k < inLen; k++) {
            const uint8_t ch = (uint8_t)rnd(), sc = (uint8_t)(rnd() % 4 ? rnd() % 0x59 : 0), md = (uint8_t)(rnd() % 3 ? 0 : rnd() & 15);
            c.dev.pushKey(ch, sc, md); keys += (char)ch; keys += (char)sc; keys += (char)md;
        }

        std::string sched, checks, ksched, jumps;
        uint32_t h = 2166136261u;
        for (int s = 0; s < steps; s++) {
            if (rnd() % 64 == 0) {
                const unsigned n = rnd() & 7;
                c.raiseIrq(n);
                sched += (sched.empty() ? "" : ",") + std::string("[") + std::to_string(s) + "," + std::to_string(n) + "]";
            }
            if (rnd() % 40 == 0) {   // a key press during execution
                const uint8_t ch = (uint8_t)rnd(), sc = (uint8_t)(rnd() % 0x59), md = (uint8_t)(rnd() & 7);
                c.dev.pushKey(ch, sc, md);
                ksched += (ksched.empty() ? "" : ",") + std::string("[") + std::to_string(s) + "," + std::to_string(ch) + "," +
                          std::to_string(sc) + "," + std::to_string(md) + "]";
            }
            clockNow += 10;                              // an artificial clock: 10 us per step
            if (rnd() % 300 == 0) {                      // and rare host "hiccups" of 0.4 s
                clockNow += 400000;
                jumps += (jumps.empty() ? "" : ",") + std::to_string(s);
            }
            const auto st = c.step();
            for (int k = 0; k < 16; k++) h = fnv(h, c.r[k]);
            h = fnv(h, c.pending);
            h = fnv(h, c.levels);
            h = fnv(h, c.waiting ? 1 : 0);
            if (st == Cpu::Status::Idle) c.raiseIrq(0);   // the same rule is repeated by JS
            if ((s + 1) % 100 == 0) checks += (checks.empty() ? "" : ",") + std::to_string(h);
        }
        // physical memory: nonzero frames (the frame number and contents)
        uint32_t hm = 2166136261u;
        for (uint32_t f = 0; f < FRAME_COUNT; f++) {
            const uint8_t* p = c.mem + f * PAGE_SIZE;
            bool any = false;
            for (uint32_t i = 0; i < PAGE_SIZE && !any; i++) any = p[i] != 0;
            if (!any) continue;
            hm = fnv(hm, f);
            for (uint32_t i = 0; i < PAGE_SIZE; i++) hm = fnv(hm, p[i]);
        }
        for (const auto& t : TEST_DISKS)
            for (uint8_t b : disks[t.n]->data) hm = fnv(hm, b);
        uint32_t hs = 2166136261u;   // the screen and device state
        for (unsigned k = 0; k < SCREEN_CELLS; k++) { hs = fnv(hs, c.dev.chars[k]); hs = fnv(hs, c.dev.attrs[k]); }
        for (uint32_t v : {(uint32_t)c.dev.col, (uint32_t)c.dev.row, (uint32_t)c.dev.ctrl, (uint32_t)c.dev.attr,
                           (uint32_t)c.dev.timerOn, (uint32_t)c.dev.keys.size(), (uint32_t)c.dev.usedDisplay, (uint32_t)c.dev.mods}) hs = fnv(hs, v);
        for (uint32_t v : {(uint32_t)c.disk.status(), (uint32_t)c.disk.error, (uint32_t)c.disk.disk, (uint32_t)c.disk.count,
                           (uint32_t)c.disk.addr, c.disk.lba, (uint32_t)c.disk.ctrl, (uint32_t)c.mmuCtrl}) hs = fnv(hs, v);
        for (unsigned p = 0; p < PAGE_COUNT; p++) hs = fnv(hs, c.mmuPage[p]);
        const uint32_t rtcEnd = c.rtc.value();
        for (uint32_t v : {(uint32_t)c.rtc.time, (uint32_t)c.rtc.date, (uint32_t)c.rtc.err, rtcEnd & 0xFFFF, rtcEnd >> 16}) hs = fnv(hs, v);
        std::string out;
        for (uint8_t b : tty) { char x[4]; std::snprintf(x, sizeof x, "%02x", b); out += x; }
        std::printf("{\"id\":%d,\"steps\":%d,\"image\":\"%s\",\"vec\":[", cs, steps, b64(image.data(), image.size()).c_str());
        for (int n = 0; n < 16; n++) std::printf("%s%u", n ? "," : "", vec[n]);
        std::printf("],\"regs\":[");
        for (int k = 0; k < 16; k++) std::printf("%s%u", k ? "," : "", init[k]);
        std::printf("],\"irq\":[%s],\"keysched\":[%s],\"jumps\":[%s],\"checks\":[%s],\"mem\":%u,\"screen\":%u,\"out\":\"%s\",\"keys\":\"%s\",\"port\":[%u,%u],\"delay\":%lld,\"rtc\":[%u,%lld]}\n",
                    sched.c_str(), ksched.c_str(), jumps.c_str(), checks.c_str(), hm, hs, out.c_str(),
                    b64((const uint8_t*)keys.data(), keys.size()).c_str(), port0, port1, (long long)delay, rtcStart, (long long)rtcAnchor);
    }
    return 0;
}

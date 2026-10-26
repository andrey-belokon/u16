// kmon - the prototype B test bench: running the monitor image in the CPU16 emulator with measurements.
//
//   kmon monitor.bin --map monitor.map --disk disk.img [--disk d1.img | --disk-ro d1.img ...] [keys]
//
//   --tick-every N   instead of the real-time timer: IRQ0 before every N-th instruction,
//                    while the machine timer is on (0 - no ticks). Waiting in WFI without requests
//                    gives a tick at once (time jumps), these ticks are counted separately.
//   --tick-first     IRQ0 before the first instruction of each process (IP = 0 with the MMU on).
//   --tick-in-wait   IRQ0 before every STI in the kernel (SP on the kernel stack): a tick in the wait window, at the
//                    deepest point of the stack.
//   --script file    a keyboard script (the format is below).
//   --max-steps N    the limit on the number of steps (60 million by default).
//   --profile FILE   write to FILE the number of executed instructions by address: lines "address count"
//                    (the logical address of the current process; match to functions by the memory map).
//   --strict-align   report unaligned accesses.
//
// The script: lines of the form
//     after "text" steps N key CODE SCAN [MOD] a key (hexadecimal numbers; MOD is MODS:
//                                              1 Shift, 2 Ctrl, 4 Alt, "The Machine", sect. 2.6)
//     after "text" idle  N ctrlc             Ctrl+C
//     after "text" steps N detach D          disconnect disk D
//     after "text" steps N type "string"     a string of characters (\n - Enter, \b - Backspace,
//                                             \xHH - character HH; the scancode is 0, for Enter $1C, for \b $0E)
//     screen "text" steps N ...              wait for text on the screen (coordinate output too)
//     after "text" steps N dump              print the screen (dumpa - with attributes too, hex)
//   after "text": wait until the console output (after the previous match) contains the text;
//   steps N / idle N: then wait N processor steps / N ticks received while waiting in WFI.
//
// The console output goes to stdout, the measurement totals to stderr as one line "KMON {json}".
//
// Build: c++ -std=c++17 -O2 -w -I<cpu16>/tools -o kmon kmon.cpp
#define main cpu16_emulator_main
#include "cpu16.cpp"
#undef main
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace cpu16;

namespace {

constexpr uint16_t WORD_IRET = 0xB018;
constexpr uint16_t KF_ADDR = 0xEDE0;       // the kernel entry frame
constexpr uint16_t STACK_LO = 0xE800;      // the kernel stack: $E800..KF-1
constexpr uint8_t  PAT_STACK = 0xA5;
constexpr uint8_t  PAT_CANARY = 0x5A;
constexpr uint16_t CANARY_LO = 0xFE00, CANARY_HI = 0xFE00;   // $FE00-$FEFF the page 7 reserve

struct KeyAction {
    std::string after;
    bool byIdle = false;
    unsigned long delay = 0;
    bool ctrlc = false;
    uint8_t ch = 0, scan = 0, mods = 0;               // key CH SC [MODS]: MODS - bits 1 Shift, 2 Ctrl, 4 Alt
    std::vector<std::pair<uint8_t, uint8_t>> keys;   // type: several events at once
    int detach = -1;                                  // detach N: disconnect disk N
    bool onScreen = false;                            // screen "text": wait for text on the screen
    int dump = 0;                                     // dump: 1 - screen characters, 2 - attributes too
};

// \n, \b, \t, \xHH in a script string; UTF-8 Cyrillic goes into the machine encoding
std::vector<std::pair<uint8_t, uint8_t>> parseTyped(const std::string& t) {
    std::vector<std::pair<uint8_t, uint8_t>> v;
    for (size_t i = 0; i < t.size(); i++) {
        unsigned char c = t[i];
        if (c == '\\' && i + 1 < t.size()) {
            const char n = t[++i];
            if (n == 'n') { v.push_back({0x0A, 0x1C}); continue; }
            if (n == 'b') { v.push_back({0x08, 0x0E}); continue; }
            if (n == 't') { v.push_back({0x09, 0x0F}); continue; }
            if (n == 'x' && i + 2 < t.size()) { v.push_back({(uint8_t)std::stoul(t.substr(i + 1, 2), nullptr, 16), 0}); i += 2; continue; }
            v.push_back({(uint8_t)n, 0}); continue;
        }
        v.push_back({c, 0});
    }
    return v;
}

struct Hist {          // a histogram: value -> how many times
    std::map<unsigned, unsigned> h;
    void add(unsigned v) { h[v]++; }
    std::string json() const {
        std::string s = "{";
        bool first = true;
        for (auto& kv : h) {
            if (!first) s += ",";
            first = false;
            s += "\"" + std::to_string(kv.first) + "\":" + std::to_string(kv.second);
        }
        return s + "}";
    }
};

std::map<std::string, unsigned> loadMap(const std::string& path) {
    std::map<std::string, unsigned> syms;
    std::ifstream f(path);
    std::string line;
    static const std::regex re(R"(^\$([0-9A-Fa-f]{4})\s+(?:\$[0-9A-Fa-f]{4}\s+)?\d+\s+\S+\s+(\S+)\s)");
    static const std::regex rc(R"(^\$([0-9A-Fa-f]{4})\s+(\S+)\s+\S+:\d+\s*$)");
    bool inConst = false;
    while (std::getline(f, line)) {
        std::smatch m;
        if (line.rfind("== Constants ==", 0) == 0) { inConst = true; continue; }
        if (inConst) { if (std::regex_search(line, m, rc)) syms[m[2]] = std::stoul(m[1], nullptr, 16); }
        else if (std::regex_search(line, m, re)) syms[m[2]] = std::stoul(m[1], nullptr, 16);
    }
    return syms;
}

std::vector<KeyAction> loadScript(const std::string& path) {
    std::vector<KeyAction> v;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto q1 = line.find('"');
        const auto q2 = line.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        KeyAction a;
        a.onScreen = line.rfind("screen", 0) == 0;
        a.after = line.substr(q1 + 1, q2 - q1 - 1);
        for (size_t p = 0; (p = a.after.find("\\n", p)) != std::string::npos;) a.after.replace(p, 2, "\n");
        std::istringstream ss(line.substr(q2 + 1));
        std::string kind, what;
        ss >> kind >> a.delay >> what;
        a.byIdle = kind == "idle";
        if (what == "detach") { ss >> std::dec >> a.detach; }
        else if (what == "dump") { a.dump = 1; }
        else if (what == "dumpa") { a.dump = 2; }
        else if (what == "type") {
            const std::string rest = line.substr(q2 + 1);
            const auto r1 = rest.find('"'), r2 = rest.rfind('"');
            if (r1 != std::string::npos && r2 > r1) a.keys = parseTyped(rest.substr(r1 + 1, r2 - r1 - 1));
        } else if (what == "ctrlc") { a.ctrlc = true; a.ch = 3; a.scan = 0x2E; }
        else {
            unsigned c = 0, s = 0, m = 0;
            ss >> std::hex >> c >> s;
            if (!(ss >> m)) m = 0;
            a.ch = (uint8_t)c; a.scan = (uint8_t)s; a.mods = (uint8_t)m;
        }
        v.push_back(a);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    std::string imgPath, mapPath, scriptPath;
    std::vector<std::pair<std::string, bool>> diskPaths;     // disks 0, 1, ... in order; true - protected
    unsigned long tickEvery = 0, maxSteps = 60000000;
    const char* profilePath = nullptr;
    std::vector<unsigned long> profile;
    bool tickFirst = false, strict = false, tickInWait = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--map" && i + 1 < argc) mapPath = argv[++i];
        else if (a == "--disk" && i + 1 < argc) diskPaths.push_back({argv[++i], false});
        else if (a == "--disk-ro" && i + 1 < argc) diskPaths.push_back({argv[++i], true});
        else if (a == "--tick-every" && i + 1 < argc) tickEvery = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--tick-first") tickFirst = true;
        else if (a == "--tick-in-wait") tickInWait = true;
        else if (a == "--script" && i + 1 < argc) scriptPath = argv[++i];
        else if (a == "--max-steps" && i + 1 < argc) maxSteps = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--strict-align") strict = true;
        else if (a == "--profile" && i + 1 < argc) profilePath = argv[++i];
        else imgPath = a;
    }
    std::ifstream fi(imgPath, std::ios::binary);
    if (!fi) { std::fprintf(stderr, "kmon: cannot open %s\n", imgPath.c_str()); return 2; }
    std::vector<uint8_t> bin((std::istreambuf_iterator<char>(fi)), std::istreambuf_iterator<char>());
    auto sym = loadMap(mapPath);
    auto S = [&](const char* n) -> unsigned {
        auto it = sym.find(n);
        if (it == sym.end()) { std::fprintf(stderr, "kmon: symbol %s not in map\n", n); std::exit(2); }
        return it->second;
    };
    const unsigned aTrap = S("trap9"), aDisp = S("trap9_disp"), aResume = S("resume"), aResumeIret = S("resume_iret");
    const unsigned aIrq0 = S("irq0"), aIrq0Hook = S("irq0_hook"), aIrq1 = S("irq1"), aErr8 = S("err8");
    const unsigned aNfree = S("nfree"), aExitProc = S("exit_proc");
    const unsigned aResEnd = S("resident_end");

    static ::Machine m;
    m.strictAlign = strict;
    m.readStdin = false;
    m.printTty = false;
    std::vector<uint8_t> tty;
    std::string dumps;
    m.ttyCapture = &tty;
    m.dev.clockFrozen = true;       // ticks - only by the bench's schedule
    for (size_t k = 0; k < diskPaths.size(); k++) {
        std::string err;
        auto img = FileDisk::open(diskPaths[k].first, diskPaths[k].second, err);
        if (!img) { std::fprintf(stderr, "kmon: disk: %s\n", err.c_str()); return 2; }
        m.disk.attach((unsigned)k, img);
    }
    for (size_t a = 0; a < bin.size(); a++) m.mem[a] = bin[a];
    // samples for checking: the kernel stack and the page 7 reserve (frame 7 = physical $E000..$FFFF)
    for (unsigned a = STACK_LO; a < KF_ADDR; a++) m.mem[a] = PAT_STACK;
    for (unsigned a = CANARY_LO; a < CANARY_HI + 0x100; a++) m.mem[a] = PAT_CANARY;

    auto script = loadScript(scriptPath);
    size_t scriptIdx = 0, ttyScanned = 0;
    bool armed = false;
    unsigned long armedAt = 0, idleAtArm = 0;
    unsigned long screenPoll = 0;   // screen checks: an iteration counter, it runs while waiting in WFI too

    unsigned long steps = 0, sinceTick = 0, idleTicks = 0, injected = 0;
    // tick measurements
    struct Tk { bool active = false; unsigned n = 0; bool proc = false; } tk;
    std::map<std::string, Hist> tkStats;
    bool vecSaved = false, firedAtZero = false;
    uint16_t vec0[16] = {0};

    // call measurements
    struct Sc { bool active = false; unsigned entry = 0, disp = 0, handler = 0, exit = 0, other = 0, nr = 0; bool dirty = false; } sc;
    struct ScStat { Hist total, entry, disp, handler, exit; unsigned n = 0, dirty = 0; };
    std::map<unsigned, ScStat> scStats;
    unsigned irqEntries = 0;
    bool timedOut = false;

    for (;;) {
        if (steps >= maxSteps) { timedOut = true; break; }
        const bool mmuOn = m.mmuCtrl & MMU_EN;
        const uint16_t ip = m.r[IP];

        // vectors after loading: a snapshot at the first entry into a process
        if (mmuOn && !vecSaved && ip == 0) {
            for (int k = 0; k < 16; k++) vec0[k] = m.read16((uint16_t)(0xFF00 + 2 * k));
            vecSaved = true;
        }
        // the tick schedule
        if (m.dev.timerOn) {
            bool fire = false;
            // the interval is counted in instructions outside the tick handler, otherwise ticks come more often than the handler
            // lets the program run
            if (tickEvery && !tk.active && ++sinceTick >= tickEvery) fire = true;
            if (tickFirst && mmuOn && ip == 0 && !firedAtZero) { fire = true; firedAtZero = true; }
            if (fire) { m.raiseIrq(0); sinceTick = 0; injected++; }
        }
        if (tickInWait && m.dev.timerOn && m.r[SP] >= 0xE000 && m.read16(ip) == 0x6FC1 && !(m.r[FL] & FIF)) {
            if (m.waiting) idleTicks++;       // a tick instead of waiting: for the script this is an idle tick
            m.raiseIrq(0); injected++;
        }
        // the keyboard script
        if (scriptIdx < script.size()) {
            const KeyAction& a = script[scriptIdx];
            if (!armed && a.onScreen) {
                if (++screenPoll % 512 == 0 && m.screenText().find(a.after) != std::string::npos) { armed = true; armedAt = steps; idleAtArm = idleTicks; }
            } else if (!armed) {
                const std::string text((const char*)tty.data(), tty.size());
                const auto pos = text.find(a.after, ttyScanned);
                if (pos != std::string::npos) { armed = true; armedAt = steps; idleAtArm = idleTicks; ttyScanned = pos + a.after.size(); }
            } else if (a.byIdle ? idleTicks - idleAtArm >= a.delay : steps - armedAt >= a.delay) {
                if (a.dump) {
                    dumps += "=== SCREEN @" + std::to_string(steps) + " ===\n" + m.screenText();
                    if (a.dump == 2) {
                        static const char* hx = "0123456789ABCDEF";
                        dumps += "--- attrs (high nibble/low nibble per row pair) ---\n";
                        for (unsigned r = 0; r < 25; r++) {
                            std::string h1, h2;
                            for (unsigned c = 0; c < 80; c++) { uint8_t at = m.dev.attrs[r * 80 + c]; h1 += hx[at >> 4]; h2 += hx[at & 15]; }
                            dumps += h1 + "\n" + h2 + "\n";
                        }
                    }
                    dumps += "=== END ===\n";
                }
                else if (a.detach >= 0) m.disk.detach((unsigned)a.detach);
                else if (!a.keys.empty()) for (auto& k : a.keys) m.dev.pushKey(k.first, k.second);
                else m.dev.pushKey(a.ch, a.scan, a.mods);
                armed = false; scriptIdx++;
            }
        }

        // instrumentation: before executing an instruction
        const uint16_t word = m.read16(ip);
        if (ip == aTrap) { sc = Sc(); sc.active = true; sc.entry = 1; /* the TRAP instruction itself */ }
        if (ip == aIrq0 && !tk.active) { tk = Tk(); tk.active = true; tk.proc = m.r[SP] < 0xE000; }
        if (ip == aIrq0 || ip == aIrq1 || ip == aErr8) irqEntries++;
        const bool inIrq0 = tk.active;
        if (sc.active) {
            if (ip == aDisp) sc.nr = m.r[3];
            if (inIrq0 || ip == aIrq1) sc.dirty = true;          // a tick or a disk in the middle of a call
            if (!inIrq0) {
                if (ip < 0xE000) sc.handler++;                       // kernel code in B16
                else if (ip >= aTrap && ip < aDisp) sc.entry++;
                else if (ip >= aDisp && ip < aResume) sc.disp++;
                else if (ip >= aResume && ip <= aResumeIret) sc.exit++;
                else sc.other++;
            }
        }
        if (tk.active) tk.n++;

        if (profilePath) { if (profile.empty()) profile.assign(65536, 0); profile[ip]++; }
        const auto st = m.step();
        steps++;
        if (ip == 0 && m.r[IP] != aIrq0) firedAtZero = false;   // the instruction at address 0 has been executed
        if (st == Cpu::Status::Idle) {
            steps--;
            if (m.dev.timerOn) { m.raiseIrq(0); idleTicks++; sinceTick = 0; continue; }
            break;                                              // halt
        }
        // after execution
        if (tk.active && (word == WORD_IRET || ip == aIrq0Hook)) {
            const char* end = ip == aIrq0Hook ? "hook" : "iret";
            tkStats[std::string(tk.proc ? "proc" : "kernel") + "_" + end].add(tk.n);
            tk.active = false;
        }
        if (sc.active && ip == aResumeIret) {
            auto& s = scStats[sc.nr];
            if (sc.dirty) s.dirty++;
            else {
                s.n++;
                s.total.add(sc.entry + sc.disp + sc.handler + sc.exit + sc.other);
                s.entry.add(sc.entry); s.disp.add(sc.disp); s.handler.add(sc.handler); s.exit.add(sc.exit);
            }
            sc.active = false;
        }
    }

    // ---- totals ---------------------------------------------------------------------
    if (profilePath && !profile.empty()) {
        FILE* pf = std::fopen(profilePath, "w");
        if (pf) {
            for (unsigned a = 0; a < 65536; a++) if (profile[a]) std::fprintf(pf, "%04X %lu\n", a, profile[a]);
            std::fclose(pf);
        }
    }
    std::fwrite(tty.data(), 1, tty.size(), stdout);
    std::fputs(dumps.c_str(), stdout);
    std::fflush(stdout);
    if (strict) std::fprintf(stderr, "strict-align: alignment violations: %lu\n", m.alignViolations);

    unsigned low = KF_ADDR;
    for (unsigned a = STACK_LO; a < KF_ADDR; a++) if (m.mem[a] != PAT_STACK) { low = a; break; }
    bool canaryOk = true;
    for (unsigned a = CANARY_LO; a < CANARY_HI + 0x100; a++) if (m.mem[a] != PAT_CANARY) { canaryOk = false; break; }
    bool vecOk = vecSaved;
    for (int k = 0; k < 16 && vecSaved; k++) if (m.read16((uint16_t)(0xFF00 + 2 * k)) != vec0[k]) vecOk = false;
    // the root's exit code: the line "exit $XXXX" in the output
    std::string text((const char*)tty.data(), tty.size());
    int exitCode = -1;
    const std::string mark = "shell exited, code $";     // the monitor's message about the shell exiting
    const auto ep = text.rfind(mark);
    if (ep != std::string::npos) exitCode = (int)std::strtol(text.c_str() + ep + mark.size(), nullptr, 16);

    std::ostringstream js;
    js << "{\"steps\":" << steps << ",\"idle_ticks\":" << idleTicks << ",\"injected_ticks\":" << injected
       << ",\"ip\":" << m.r[IP] << ",\"sp\":" << m.r[SP] << ",\"flags\":" << m.r[FL] << ",\"timeout\":" << (timedOut ? "true" : "false")
       << ",\"exit\":" << exitCode
       << ",\"stack_low\":" << (low < KF_ADDR ? (int)low : -1)
       << ",\"stack_depth\":" << (KF_ADDR - low)
       << ",\"canary_ok\":" << (canaryOk ? "true" : "false")
       << ",\"vectors_ok\":" << (vecOk ? "true" : "false")
       << ",\"nfree\":" << m.read16((uint16_t)aNfree)
       << ",\"ticks\":" << m.read16((uint16_t)(S("RES") + S("ResData.ticks")))
       << ",\"resident_end\":" << aResEnd
       << ",\"syscalls\":{";
    bool first = true;
    for (auto& kv : scStats) {
        if (!first) js << ",";
        first = false;
        js << "\"" << kv.first << "\":{\"n\":" << kv.second.n << ",\"dirty\":" << kv.second.dirty
           << ",\"total\":" << kv.second.total.json() << ",\"entry\":" << kv.second.entry.json()
           << ",\"disp\":" << kv.second.disp.json() << ",\"handler\":" << kv.second.handler.json()
           << ",\"exit\":" << kv.second.exit.json() << "}";
    }
    js << "},\"tick\":{";
    first = true;
    for (auto& kv : tkStats) {
        if (!first) js << ",";
        first = false;
        js << "\"" << kv.first << "\":" << kv.second.json();
    }
    js << "}}";
    std::fprintf(stderr, "KMON %s\n", js.str().c_str());
    return 0;
}

// cpu16_machine.hpp - the CPU16 machine: devices and encoding ("CPU16 Machine: Memory and Devices",
// revision 5).
//
// A common header for the emulator, the assembler and the JS table generator: port addresses, the palette,
// the character encoding, scancodes, the model of the console-display-keyboard-timer device,
// the disk controller (sect. 6), the memory manager constants (sect. 7; the manager itself is in Cpu,
// because it sits on the path of every memory access) and the real-time clock (sect. 8).
// There is a single source of truth: the debugger's JS engine takes its tables from here (gen_isa_js.cpp)
// and repeats the device model; the match is checked by comparison on random programs.

#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cpu16 {

// ============================================================================
// Ports (sect. 2-4 of the document)
// ============================================================================
constexpr uint16_t PORT_DATA = 0xFF20, PORT_SCAN = 0xFF21, PORT_CTRL = 0xFF22, PORT_ATTR = 0xFF23,
                   PORT_COL = 0xFF24, PORT_ROW = 0xFF25, PORT_CMD = 0xFF26, PORT_COLS = 0xFF27,
                   PORT_ROWS = 0xFF28, PORT_TIMER = 0xFF29;
constexpr uint16_t PORT_FIRST = 0xFF20, PORT_LAST = 0xFF29;
// MODS (sect. 2.6, revision 8): the modifiers of the last keyboard event taken. A console
// register, but outside its range PORT_FIRST..PORT_LAST: $FF2A-$FF3B are already occupied by the clock and the disk.
constexpr uint16_t PORT_MODS = 0xFF3C;
// SCRD (sect. 2.2, revision 9): reading the screen. The word at $FF50 is the cell at the position (character | attribute << 8),
// the position advances as in coordinate mode; the byte $FF50 is the character, $FF51 is the attribute, with no advance.
constexpr uint16_t PORT_SCRD = 0xFF50;
enum ModBits : uint8_t { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_MASK = 7 };

// The device window (sect. 1, 7.5): the whole range $FF20-$FFFF belongs to the device bus and is selected by
// the logical address before the memory manager translation. Reading an address with no device
// gives 0, writing to it is ignored. The vector table $FF00-$FF1F is ordinary memory.
constexpr uint16_t IO_BASE = 0xFF20;

constexpr unsigned SCREEN_COLS = 80, SCREEN_ROWS = 25, SCREEN_CELLS = SCREEN_COLS * SCREEN_ROWS;
enum CtrlBits : uint8_t { CTRL_TTY = 1, CTRL_CURSOR = 2, CTRL_ATTRMODE = 4, CTRL_MASK = 7 };
enum Commands : uint8_t { CMD_CLS = 1, CMD_CLEOL = 2, CMD_CLEOS = 3, CMD_SCROLL_UP = 4, CMD_SCROLL_DOWN = 5 };
constexpr uint8_t ATTR_DEFAULT = 0x07, CTRL_DEFAULT = CTRL_TTY | CTRL_CURSOR;
constexpr unsigned KEY_QUEUE_MAX = 256;
constexpr int64_t TIMER_PERIOD_US = 20000;   // 50 Hz

// The disk controller (sect. 6)
constexpr uint16_t DK_STATUS = 0xFF30, DK_CMD = 0xFF30, DK_ERROR = 0xFF31, DK_DISK = 0xFF32, DK_COUNT = 0xFF33,
                   DK_ADDR = 0xFF34, DK_LBA_LO = 0xFF36, DK_LBA_HI = 0xFF38, DK_CTRL = 0xFF3A, DK_DRIVES = 0xFF3B;
constexpr uint16_t DK_FIRST = 0xFF30, DK_LAST = 0xFF3B;
enum DiskStatusBits : uint8_t { DKS_BUSY = 1, DKS_ERR = 2, DKS_DONE = 4, DKS_PRESENT = 8, DKS_RO = 16 };
enum DiskCommands : uint8_t { DKC_ACK = 0, DKC_READ = 1, DKC_WRITE = 2, DKC_INFO = 3, DKC_FLUSH = 4 };
enum DiskErrors : uint8_t { DKE_OK = 0, DKE_NODISK = 1, DKE_BADCMD = 2, DKE_RANGE = 3, DKE_BADADDR = 4, DKE_READONLY = 5, DKE_IO = 6 };
enum DiskInfoFlags : uint8_t { DKI_RO = 1, DKI_REMOVABLE = 2, DKI_SMALL = 4 };
constexpr unsigned SECTOR_SIZE = 512, DISK_COUNT = 8, DISK_LABEL_MAX = 15, DISK_INFO_SIZE = 32;
constexpr uint16_t DMA_LIMIT = 0xFF1F;            // the whole DMA buffer is within $0000..$FF1F
constexpr unsigned DISK_IRQ = 1;                  // the IRQ1 request is level-triggered
constexpr uint8_t DK_IEN = 1;                     // CTRL: IRQ1 on command completion

// The real-time clock (sect. 8)
constexpr uint16_t RTC_TIME = 0xFF2A, RTC_DATE = 0xFF2C, RTC_STATUS = 0xFF2E, RTC_CMD = 0xFF2E;
constexpr uint16_t RTC_FIRST = 0xFF2A, RTC_LAST = 0xFF2E;          // $FF2F is free
enum RtcCommands : uint8_t { RTCC_READ = 1, RTCC_WRITE = 2, RTCC_WRITE_TIME = 3, RTCC_WRITE_DATE = 4 };
constexpr uint8_t RTC_ERR = 1;                                     // STATUS, bit 0
constexpr unsigned RTC_BASE_YEAR = 2025, RTC_YEARS = 64;           // 2025..2088
constexpr uint32_t RTC_DAY = 86400;
constexpr uint32_t RTC_DAYS = RTC_YEARS * 365 + RTC_YEARS / 4;     // leap years are 2028, 2032, ..., 2088
constexpr uint32_t RTC_PERIOD = RTC_DAYS * RTC_DAY;               // seconds in the clock cycle, < 2^31
constexpr int64_t RTC_SECOND_US = 1000000;

// The memory manager (sect. 7)
constexpr uint16_t MMU_PAGE0 = 0xFF40, MMU_CTRL = 0xFF48, MMU_FIRST = 0xFF40, MMU_LAST = 0xFF48;
constexpr uint8_t MMU_EN = 1;
constexpr unsigned PAGE_BITS = 13;
constexpr uint32_t PAGE_SIZE = 1u << PAGE_BITS, PAGE_COUNT = 8, FRAME_COUNT = 256, PHYS_SIZE = FRAME_COUNT * PAGE_SIZE;   // 2 MB

// The palette (RGB): the index is the color number in the attribute.
inline constexpr uint32_t PALETTE[8] = {0x000000, 0x0037DA, 0x13A10E, 0x3A96DD, 0xC50F1F, 0xC19C00, 0x767676, 0xCCCCCC};

// ============================================================================
// The encoding: CP1251 with the block $80-$BF taken from KOI8-R, glyphs in $01-$1F and $7F (sect. 5).
// An element is a Unicode code point; 0 - no image (code $00).
// ============================================================================
inline constexpr char32_t CHARSET[256] = {
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

// The character code in the machine encoding for a Unicode code point; -1 if there is no such character.
// ASCII $00-$7F maps to itself (control codes too).
inline int charsetFind(char32_t cp) {
    if (cp < 0x80) return (int)cp;
    for (int i = 0x80; i < 256; i++) if (CHARSET[i] == cp) return i;
    for (int i = 1; i < 0x20; i++) if (CHARSET[i] == cp) return i;
    return CHARSET[0x7F] == cp ? 0x7F : -1;
}

// A machine-encoding character as UTF-8 (for the terminal): the control codes $08 $09 $0A $0D - as is.
inline std::string charsetToUtf8(uint8_t b) {
    if (b == 0x08 || b == 0x09 || b == 0x0A || b == 0x0D) return std::string(1, (char)b);
    const char32_t cp = CHARSET[b];
    if (!cp) return "";
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    return s;
}

// Parsing one UTF-8 character: s[i..] -> a code point, i is advanced; -1 - an invalid sequence.
inline long utf8Decode(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) { i++; return -1; }
    long cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k <= n; k++) {
        const unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) { i++; return -1; }
        cp = (cp << 6) | (d & 0x3F);
    }
    i += n + 1;
    return cp;
}

// ============================================================================
// Scancodes (set 1): key names in the style of KeyboardEvent.code (for the web debugger)
// and terminal sequences (for the command-line emulator).
// ============================================================================
struct NamedScan { const char* name; uint8_t scan; };
inline constexpr NamedScan WEB_SCANCODES[] = {
    {"Escape", 0x01}, {"Digit1", 0x02}, {"Digit2", 0x03}, {"Digit3", 0x04}, {"Digit4", 0x05}, {"Digit5", 0x06},
    {"Digit6", 0x07}, {"Digit7", 0x08}, {"Digit8", 0x09}, {"Digit9", 0x0A}, {"Digit0", 0x0B}, {"Minus", 0x0C},
    {"Equal", 0x0D}, {"Backspace", 0x0E}, {"Tab", 0x0F}, {"KeyQ", 0x10}, {"KeyW", 0x11}, {"KeyE", 0x12},
    {"KeyR", 0x13}, {"KeyT", 0x14}, {"KeyY", 0x15}, {"KeyU", 0x16}, {"KeyI", 0x17}, {"KeyO", 0x18}, {"KeyP", 0x19},
    {"BracketLeft", 0x1A}, {"BracketRight", 0x1B}, {"Enter", 0x1C}, {"KeyA", 0x1E}, {"KeyS", 0x1F}, {"KeyD", 0x20},
    {"KeyF", 0x21}, {"KeyG", 0x22}, {"KeyH", 0x23}, {"KeyJ", 0x24}, {"KeyK", 0x25}, {"KeyL", 0x26},
    {"Semicolon", 0x27}, {"Quote", 0x28}, {"Backquote", 0x29}, {"Backslash", 0x2B}, {"KeyZ", 0x2C}, {"KeyX", 0x2D},
    {"KeyC", 0x2E}, {"KeyV", 0x2F}, {"KeyB", 0x30}, {"KeyN", 0x31}, {"KeyM", 0x32}, {"Comma", 0x33},
    {"Period", 0x34}, {"Slash", 0x35}, {"Space", 0x39}, {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E},
    {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57},
    {"F12", 0x58}, {"Home", 0x47}, {"ArrowUp", 0x48}, {"PageUp", 0x49}, {"ArrowLeft", 0x4B}, {"ArrowRight", 0x4D},
    {"End", 0x4F}, {"ArrowDown", 0x50}, {"PageDown", 0x51}, {"Insert", 0x52}, {"Delete", 0x53},
    {"NumpadEnter", 0x1C}};
// Terminal sequences after ESC.
inline constexpr NamedScan TERM_SEQUENCES[] = {
    {"[A", 0x48}, {"[B", 0x50}, {"[C", 0x4D}, {"[D", 0x4B}, {"[H", 0x47}, {"[F", 0x4F},
    {"OA", 0x48}, {"OB", 0x50}, {"OC", 0x4D}, {"OD", 0x4B}, {"OH", 0x47}, {"OF", 0x4F},
    {"[1~", 0x47}, {"[2~", 0x52}, {"[3~", 0x53}, {"[4~", 0x4F}, {"[5~", 0x49}, {"[6~", 0x51},
    {"OP", 0x3B}, {"OQ", 0x3C}, {"OR", 0x3D}, {"OS", 0x3E}, {"[15~", 0x3F}, {"[17~", 0x40}, {"[18~", 0x41},
    {"[19~", 0x42}, {"[20~", 0x43}, {"[21~", 0x44}, {"[23~", 0x57}, {"[24~", 0x58},
    // Ctrl+Enter (the machine, sect. 2.6: character 0, scancode Enter): CSI u and xterm modifyOtherKeys
    {"[13;5u", 0x1C}, {"[27;5;13~", 0x1C}};
// The longest sequence after ESC: we do not wait for a continuation longer than that.
inline constexpr size_t TERM_SEQ_MAX = 9;

// A key with modifiers in the xterm style: \e[1;mX instead of \e[X or \eOX, \e[n;m~ instead of \e[n~,
// where m is 1 + (Shift 1, Alt 2, Ctrl 4). Parsing rest (after ESC): the length of the sequence or 0,
// the scancode and the MODS bits; 0 with prefix = true - the beginning of such a sequence, wait for the continuation.
inline size_t termModified(const std::string& rest, uint8_t& scan, uint8_t& mods, bool& prefix) {
    prefix = false;
    if (rest.empty() || rest[0] != '[') return 0;
    size_t i = 1;
    unsigned p1 = 0, m = 0;
    bool d1 = false, dm = false;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') { p1 = p1 * 10 + (rest[i] - '0'); i++; d1 = true; }
    if (i == rest.size()) { prefix = true; return 0; }
    if (rest[i] != ';' || !d1) return 0;
    i++;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') { m = m * 10 + (rest[i] - '0'); i++; dm = true; }
    if (i == rest.size()) { prefix = true; return 0; }
    if (!dm || m < 1 || m > 16) return 0;
    const char fin = rest[i];
    std::string base;
    if (fin == '~') base = "[" + std::to_string(p1) + "~";
    else if (p1 == 1 && (fin == 'P' || fin == 'Q' || fin == 'R' || fin == 'S')) base = std::string("O") + fin;
    else if (p1 == 1) base = std::string("[") + fin;
    else return 0;
    for (const auto& t : TERM_SEQUENCES)
        if (base == t.name) {
            const unsigned x = m - 1;
            scan = t.scan;
            mods = (uint8_t)(((x & 1) ? MOD_SHIFT : 0) | ((x & 2) ? MOD_ALT : 0) | ((x & 4) ? MOD_CTRL : 0));
            return i + 1;
        }
    return 0;
}

// The reverse: the sequence (after ESC) for a scancode and MODS; empty - the scancode is not in the table.
inline std::string termSequence(uint8_t scan, uint8_t mods) {
    for (const auto& t : TERM_SEQUENCES) {
        if (t.scan != scan || scan == 0x1C) continue;
        const std::string n = t.name;
        if (!mods) return n;
        const unsigned m = 1 + ((mods & MOD_SHIFT) ? 1 : 0) + ((mods & MOD_ALT) ? 2 : 0) + ((mods & MOD_CTRL) ? 4 : 0);
        if (n.size() == 2) return "[1;" + std::to_string(m) + n[1];                       // [X, OX
        return n.substr(0, n.size() - 1) + ";" + std::to_string(m) + "~";                   // [n~
    }
    return "";
}

// ============================================================================
// Device: console, display, keyboard, timer
// ============================================================================
struct KeyEvent { uint8_t ch, scan, mods; };

class ConsoleDevice {
public:
    uint8_t chars[SCREEN_CELLS], attrs[SCREEN_CELLS];
    unsigned col = 0, row = 0;
    uint8_t ctrl = CTRL_DEFAULT, attr = ATTR_DEFAULT;
    std::deque<KeyEvent> keys;
    uint8_t mods = 0;                    // MODS: the modifiers of the last event taken
    std::vector<uint8_t> ttyOut;         // bytes output in teletype mode (for the host)
    bool usedDisplay = false;            // the program used the display features
    unsigned long screenVersion = 0;     // grows on every change of the screen (for redrawing)
    // the real-time timer; clock() - microseconds
    bool timerOn = false;
    bool clockFrozen = false;            // the debugger is paused: the timer stands still
    int64_t nextTick = 0;
    std::function<int64_t()> clock = [] { return (int64_t)0; };

    ConsoleDevice() { reset(); }

    void reset() {
        for (unsigned i = 0; i < SCREEN_CELLS; i++) { chars[i] = ' '; attrs[i] = ATTR_DEFAULT; }
        col = row = 0;
        ctrl = CTRL_DEFAULT; attr = ATTR_DEFAULT;
        keys.clear(); ttyOut.clear(); mods = 0;
        usedDisplay = false; timerOn = false;
        screenVersion++;
    }
    static bool isPort(uint16_t a) { return a >= PORT_FIRST && a <= PORT_LAST; }

    void pushKey(uint8_t ch, uint8_t scan, uint8_t m = 0) {
        if (keys.size() < KEY_QUEUE_MAX) keys.push_back({ch, scan, (uint8_t)(m & MOD_MASK)});
    }

    uint8_t read8(uint16_t a) {
        switch (a) {
        case PORT_DATA: { const KeyEvent e = pop(); return e.ch; }
        case PORT_SCAN: { const KeyEvent e = pop(); return e.scan; }
        case PORT_CTRL: return ctrl;
        case PORT_ATTR: return attr;
        case PORT_COL: return (uint8_t)col;
        case PORT_ROW: return (uint8_t)row;
        case PORT_COLS: return SCREEN_COLS;
        case PORT_ROWS: return SCREEN_ROWS;
        case PORT_TIMER: return timerOn ? 1 : 0;
        default: return 0;   // CMD
        }
    }
    // A word at $FF20 is a single access: the character and scancode of one event.
    uint16_t read16(uint16_t a) {
        if (a == PORT_DATA) { const KeyEvent e = pop(); return (uint16_t)(e.ch | (e.scan << 8)); }
        return (uint16_t)(read8(a) | (read8((uint16_t)(a + 1)) << 8));
    }
    void write8(uint16_t a, uint8_t v) {
        switch (a) {
        case PORT_DATA: output(v); break;
        case PORT_CTRL: ctrl = v & CTRL_MASK; if (ctrl != CTRL_DEFAULT) usedDisplay = true; screenVersion++; break;
        case PORT_ATTR: attr = v; usedDisplay = true; break;
        case PORT_COL: col = v < SCREEN_COLS ? v : SCREEN_COLS - 1; usedDisplay = true; screenVersion++; break;
        case PORT_ROW: row = v < SCREEN_ROWS ? v : SCREEN_ROWS - 1; usedDisplay = true; screenVersion++; break;
        case PORT_CMD: command(v); break;
        case PORT_TIMER: {
            const bool on = v & 1;
            if (on && !timerOn) nextTick = clock() + TIMER_PERIOD_US;
            timerOn = on;
            break;
        }
        default: break;      // SCAN, COLS, ROWS - read only
        }
    }
    // SCRD: the cell at the position; advance - advance the position (a word read)
    uint16_t readCell(bool advance) {
        const unsigned i = row * SCREEN_COLS + col;
        const uint16_t v = (uint16_t)(chars[i] | (attrs[i] << 8));
        if (advance) advanceRaw();
        return v;
    }
    // Whether it is time to raise IRQ0. Ticks do not accumulate: a lagging timer catches up to "now".
    bool pollTimer() {
        if (!timerOn || clockFrozen) return false;
        const int64_t now = clock();
        if (now < nextTick) return false;
        nextTick += TIMER_PERIOD_US;
        if (nextTick <= now) nextTick = now + TIMER_PERIOD_US;
        return true;
    }
    // The debugger resumes execution after a pause: counting starts anew, missed ticks do not accumulate.
    void resumeClock() { clockFrozen = false; nextTick = clock() + TIMER_PERIOD_US; }

private:
    // An event is taken whole; MODS is its modifiers (an empty queue: event 0, MODS = 0).
    KeyEvent pop() {
        if (keys.empty()) { mods = 0; return {0, 0, 0}; }
        const KeyEvent e = keys.front();
        keys.pop_front();
        mods = e.mods;
        return e;
    }
    void put(uint8_t ch, uint8_t a) { const unsigned i = row * SCREEN_COLS + col; chars[i] = ch; attrs[i] = a; }
    void clearRange(unsigned from, unsigned to) { for (unsigned i = from; i < to; i++) { chars[i] = ' '; attrs[i] = attr; } }
    void scrollUp() {
        std::memmove(chars, chars + SCREEN_COLS, SCREEN_CELLS - SCREEN_COLS);
        std::memmove(attrs, attrs + SCREEN_COLS, SCREEN_CELLS - SCREEN_COLS);
        clearRange(SCREEN_CELLS - SCREEN_COLS, SCREEN_CELLS);
    }
    void scrollDown() {
        std::memmove(chars + SCREEN_COLS, chars, SCREEN_CELLS - SCREEN_COLS);
        std::memmove(attrs + SCREEN_COLS, attrs, SCREEN_CELLS - SCREEN_COLS);
        clearRange(0, SCREEN_COLS);
    }
    void newlineTty() { if (++row == SCREEN_ROWS) { scrollUp(); row = SCREEN_ROWS - 1; } }
    // advance of the position in coordinate and attribute modes: without scrolling
    void advanceRaw() { if (++col == SCREEN_COLS) { col = 0; if (++row == SCREEN_ROWS) row = 0; } }

    void output(uint8_t v) {
        screenVersion++;
        if (ctrl & CTRL_ATTRMODE) { attrs[row * SCREEN_COLS + col] = v; advanceRaw(); usedDisplay = true; return; }
        if (!(ctrl & CTRL_TTY)) { put(v, attr); advanceRaw(); usedDisplay = true; return; }
        if (v == 0) return;
        ttyOut.push_back(v);
        switch (v) {
        case 0x0A: col = 0; newlineTty(); break;
        case 0x0D: col = 0; break;
        case 0x08: if (col) col--; break;
        case 0x09: col = (col / 8 + 1) * 8; if (col >= SCREEN_COLS) { col = 0; newlineTty(); } break;
        default:
            put(v, attr);
            if (++col == SCREEN_COLS) { col = 0; newlineTty(); }
        }
    }
    void command(uint8_t v) {
        const unsigned pos = row * SCREEN_COLS + col;
        switch (v) {
        case CMD_CLS: clearRange(0, SCREEN_CELLS); col = row = 0; break;
        case CMD_CLEOL: clearRange(pos, (row + 1) * SCREEN_COLS); break;
        case CMD_CLEOS: clearRange(pos, SCREEN_CELLS); break;
        case CMD_SCROLL_UP: scrollUp(); break;
        case CMD_SCROLL_DOWN: scrollDown(); break;
        default: return;
        }
        usedDisplay = true;
        screenVersion++;
    }
};

// ============================================================================
// The disk controller (sect. 6): up to 8 disks, a 512-byte sector, DMA, level-triggered IRQ1.
// ============================================================================

// A disk image. Implementations: MemoryDisk (below; tests, the web debugger) and a file (cpu16.cpp).
class DiskImage {
public:
    virtual ~DiskImage() {}
    virtual uint32_t sectors() const = 0;
    virtual bool readOnly() const = 0;
    virtual bool removable() const { return false; }
    virtual std::string label() const = 0;                    // bytes in the machine encoding, up to 15
    virtual bool read(uint32_t lba, uint8_t* buf) = 0;         // 512 bytes; false - a media error
    virtual bool write(uint32_t lba, const uint8_t* buf) = 0;
    virtual bool flush() { return true; }
};

class MemoryDisk : public DiskImage {
public:
    std::vector<uint8_t> data;
    bool ro = false, rem = false;
    std::string lbl;
    MemoryDisk(uint32_t nSectors, bool readOnly = false, const std::string& label = "")
        : data((size_t)nSectors * SECTOR_SIZE, 0), ro(readOnly), lbl(label) {}
    uint32_t sectors() const override { return (uint32_t)(data.size() / SECTOR_SIZE); }
    bool readOnly() const override { return ro; }
    bool removable() const override { return rem; }
    std::string label() const override { return lbl; }
    bool read(uint32_t lba, uint8_t* buf) override {
        if (lba >= sectors()) return false;
        std::memcpy(buf, &data[(size_t)lba * SECTOR_SIZE], SECTOR_SIZE);
        return true;
    }
    bool write(uint32_t lba, const uint8_t* buf) override {
        if (lba >= sectors()) return false;
        std::memcpy(&data[(size_t)lba * SECTOR_SIZE], buf, SECTOR_SIZE);
        return true;
    }
};

// A disk label from the file name (sect. 8.1): the name without the directory, at most 15 characters,
// characters outside the machine encoding (and control ones) are replaced with '?'.
inline std::string diskLabelFromName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string out;
    size_t i = 0;
    while (i < name.size() && out.size() < DISK_LABEL_MAX) {
        const long cp = utf8Decode(name, i);
        const int code = cp < 0 ? -1 : charsetFind((char32_t)cp);
        out += code >= 0x20 && code != 0x7F ? (char)code : '?';
    }
    return out;
}

class DiskController {
public:
    std::shared_ptr<DiskImage> disks[DISK_COUNT];
    uint8_t error = 0, disk = 0, count = 0, ctrl = 0;
    uint16_t addr = 0;
    uint32_t lba = 0;
    bool busy = false, done = false;
    uint8_t cmd = 0;                     // the command being executed
    // Command time: a delay per sector (INFO and FLUSH - one delay); 0 - instant, on a write to CMD.
    int64_t sectorDelayUs = 0;
    bool clockFrozen = false;            // the debugger is paused: the transfer stands still
    int64_t nextAt = 0;
    std::function<int64_t()> clock = [] { return (int64_t)0; };
    // DMA: a logical address, translated by the memory manager like a processor access.
    std::function<uint8_t(uint16_t)> dmaRead = [](uint16_t) { return (uint8_t)0; };
    std::function<void(uint16_t, uint8_t)> dmaWrite = [](uint16_t, uint8_t) {};

    // Machine reset: the registers, except DRIVES, are 0; the disks are not disconnected.
    void reset() { error = disk = count = ctrl = 0; addr = 0; lba = 0; busy = done = false; cmd = 0; }
    static bool isPort(uint16_t a) { return a >= DK_FIRST && a <= DK_LAST; }

    uint8_t drives() const {
        uint8_t m = 0;
        for (unsigned n = 0; n < DISK_COUNT; n++) if (disks[n]) m |= (uint8_t)(1u << n);
        return m;
    }
    uint8_t status() const {
        const DiskImage* d = disks[disk].get();
        return (uint8_t)((busy ? DKS_BUSY : 0) | (error ? DKS_ERR : 0) | (done ? DKS_DONE : 0) |
                         (d ? DKS_PRESENT : 0) | (d && d->readOnly() ? DKS_RO : 0));
    }
    bool irq() const { return done && (ctrl & DK_IEN); }          // the IRQ1 line
    bool willIrq() const { return busy && (ctrl & DK_IEN); }      // a command that will raise IRQ1 is being executed

    uint8_t read8(uint16_t a) const {
        switch (a) {
        case DK_STATUS: return status();
        case DK_ERROR: return error;
        case DK_DISK: return disk;
        case DK_COUNT: return count;
        case DK_ADDR: return (uint8_t)addr;
        case DK_ADDR + 1: return (uint8_t)(addr >> 8);
        case DK_LBA_LO: return (uint8_t)lba;
        case DK_LBA_LO + 1: return (uint8_t)(lba >> 8);
        case DK_LBA_HI: return (uint8_t)(lba >> 16);
        case DK_LBA_HI + 1: return (uint8_t)(lba >> 24);
        case DK_CTRL: return ctrl;
        case DK_DRIVES: return drives();
        default: return 0;
        }
    }
    void write8(uint16_t a, uint8_t v) {
        if (a == DK_CTRL) { ctrl = v & DK_IEN; return; }
        if (busy) return;                // while BUSY = 1, commands and parameters are not accepted
        switch (a) {
        case DK_CMD: start(v); break;
        case DK_DISK: disk = v & 7; break;
        case DK_COUNT: count = v; break;
        case DK_ADDR: addr = (uint16_t)((addr & 0xFF00) | v); break;
        case DK_ADDR + 1: addr = (uint16_t)((addr & 0x00FF) | (v << 8)); break;
        case DK_LBA_LO: lba = (lba & 0xFFFFFF00u) | v; break;
        case DK_LBA_LO + 1: lba = (lba & 0xFFFF00FFu) | ((uint32_t)v << 8); break;
        case DK_LBA_HI: lba = (lba & 0xFF00FFFFu) | ((uint32_t)v << 16); break;
        case DK_LBA_HI + 1: lba = (lba & 0x00FFFFFFu) | ((uint32_t)v << 24); break;
        default: break;                  // ERROR, DRIVES - read only
        }
    }

    void attach(unsigned n, std::shared_ptr<DiskImage> d) { disks[n & 7] = std::move(d); }
    // Disconnecting during a command with this disk ends it with an IO error.
    void detach(unsigned n) {
        disks[n & 7].reset();
        if (busy && disk == (n & 7)) finish(DKE_IO);
    }

    // Advance the transfer by the clock (called when the devices are polled).
    void poll() {
        if (!busy || clockFrozen) return;
        if (sectorDelayUs <= 0) { while (unit()) {} return; }
        const int64_t now = clock();
        while (busy && now >= nextAt) if (unit()) nextAt += sectorDelayUs;
    }
    // The debugger resumes execution after a pause: counting of the current sector starts anew.
    void resumeClock() { clockFrozen = false; nextAt = clock() + sectorDelayUs; }

private:
    void finish(uint8_t err) { busy = false; done = true; error = err; }

    // Checks in the order BADCMD, NODISK, (COUNT = 0 - success), READONLY, RANGE, BADADDR.
    void start(uint8_t c) {
        if (c == DKC_ACK) { done = false; return; }
        busy = true; done = false; error = DKE_OK; cmd = c;
        if (c > DKC_FLUSH) { finish(DKE_BADCMD); return; }
        DiskImage* d = disks[disk].get();
        if (!d) { finish(DKE_NODISK); return; }
        if (c == DKC_READ || c == DKC_WRITE) {
            if (count == 0) { finish(DKE_OK); return; }
            if (c == DKC_WRITE && d->readOnly()) { finish(DKE_READONLY); return; }
            if ((uint64_t)lba + count > d->sectors()) { finish(DKE_RANGE); return; }
            if ((uint32_t)addr + SECTOR_SIZE * count - 1 > DMA_LIMIT) { finish(DKE_BADADDR); return; }
        } else if (c == DKC_INFO) {
            if ((uint32_t)addr + DISK_INFO_SIZE - 1 > DMA_LIMIT) { finish(DKE_BADADDR); return; }
        }
        if (sectorDelayUs <= 0) { while (unit()) {} return; }
        nextAt = clock() + sectorDelayUs;
    }

    // One unit of transfer: a READ/WRITE sector, INFO or FLUSH as a whole. true - there is more work.
    bool unit() {
        if (!busy) return false;
        DiskImage* d = disks[disk].get();
        if (!d) { finish(DKE_IO); return false; }
        uint8_t buf[SECTOR_SIZE];
        switch (cmd) {
        case DKC_READ:
            if (!d->read(lba, buf)) { finish(DKE_IO); return false; }
            for (unsigned i = 0; i < SECTOR_SIZE; i++) dmaWrite((uint16_t)(addr + i), buf[i]);
            break;
        case DKC_WRITE:
            for (unsigned i = 0; i < SECTOR_SIZE; i++) buf[i] = dmaRead((uint16_t)(addr + i));
            if (!d->write(lba, buf)) { finish(DKE_IO); return false; }
            break;
        case DKC_INFO: {
            uint8_t info[DISK_INFO_SIZE] = {0};
            const uint32_t n = d->sectors();
            for (int k = 0; k < 4; k++) info[k] = (uint8_t)(n >> (8 * k));
            info[4] = (uint8_t)((d->readOnly() ? DKI_RO : 0) | (d->removable() ? DKI_REMOVABLE : 0) | (n <= 0x10000 ? DKI_SMALL : 0));
            const std::string l = d->label();
            for (size_t k = 0; k < l.size() && k < DISK_LABEL_MAX; k++) info[16 + k] = (uint8_t)l[k];
            for (unsigned i = 0; i < DISK_INFO_SIZE; i++) dmaWrite((uint16_t)(addr + i), info[i]);
            finish(DKE_OK);
            return false;
        }
        default:   // FLUSH
            finish(d->flush() ? DKE_OK : DKE_IO);
            return false;
        }
        lba++;
        addr = (uint16_t)(addr + SECTOR_SIZE);
        if (--count == 0) { finish(DKE_OK); return false; }
        return true;
    }
};


// ============================================================================
// The real-time clock (sect. 8)
// ============================================================================
// The value is an FS16 timestamp: TIME = hour 0..11 (bits 15-12), minute (11-6), second (5-0);
// DATE = year - 2025 (15-10), month (9-6), day (5-1), PM (0). Internally the clock counts seconds
// from 1 January 2025 AM 0:00:00 (0..RTC_PERIOD - 1); this way carries, month lengths and the
// 2088 -> 2025 wrap-around come out by themselves.

// A year field 0..63 is a leap year if 2025 + y is divisible by 4 (the rule is exact in the range 2025-2088).
inline bool rtcLeap(unsigned y) { return y % 4 == 3; }
inline unsigned rtcDaysInMonth(unsigned y, unsigned m) {
    static const uint8_t len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && rtcLeap(y) ? 29 : len[m - 1];
}
inline bool rtcValidTime(uint16_t t) { return (t >> 12) <= 11 && ((t >> 6) & 63) <= 59 && (t & 63) <= 59; }
inline bool rtcValidDate(uint16_t d) {
    const unsigned y = d >> 10, m = (d >> 6) & 15, day = (d >> 1) & 31;
    return m >= 1 && m <= 12 && day >= 1 && day <= rtcDaysInMonth(y, m);
}
inline bool rtcValid(uint16_t t, uint16_t d) { return rtcValidTime(t) && rtcValidDate(d); }

// The day number (from 1 January 2025) by year, month and day; the arguments are valid.
inline uint32_t rtcDayNumber(unsigned y, unsigned m, unsigned day) {
    uint32_t n = y * 365 + y / 4;                       // leap years before year y: 2028 (y = 3), ...
    for (unsigned k = 1; k < m; k++) n += rtcDaysInMonth(y, k);
    return n + day - 1;
}
// Seconds from the registers; the value is valid.
inline uint32_t rtcToSeconds(uint16_t t, uint16_t d) {
    const unsigned h24 = (t >> 12) + 12 * (d & 1);
    return rtcDayNumber(d >> 10, (d >> 6) & 15, (d >> 1) & 31) * RTC_DAY + h24 * 3600 + ((t >> 6) & 63) * 60 + (t & 63);
}
// Registers from seconds 0..RTC_PERIOD - 1.
inline void rtcFromSeconds(uint32_t s, uint16_t& t, uint16_t& d) {
    uint32_t days = s / RTC_DAY;
    const uint32_t tod = s % RTC_DAY;
    unsigned y = 0, m = 1;
    while (days >= (rtcLeap(y) ? 366u : 365u)) { days -= rtcLeap(y) ? 366 : 365; y++; }
    while (days >= rtcDaysInMonth(y, m)) { days -= rtcDaysInMonth(y, m); m++; }
    const unsigned h24 = tod / 3600;
    t = (uint16_t)((h24 % 12) << 12 | (tod / 60 % 60) << 6 | tod % 60);
    d = (uint16_t)(y << 10 | m << 6 | (days + 1) << 1 | (h24 >= 12 ? 1 : 0));
}

// Date and time in ordinary notation (the full year, hour 0..23).
struct RtcCivil { unsigned year, month, day, hour, minute, second; };
inline RtcCivil rtcCivil(uint32_t s) {
    uint16_t t, d;
    rtcFromSeconds(s, t, d);
    return {RTC_BASE_YEAR + (d >> 10), (unsigned)(d >> 6) & 15, (unsigned)(d >> 1) & 31,
            (unsigned)(t >> 12) + 12 * (d & 1), (unsigned)(t >> 6) & 63, (unsigned)t & 63};
}
// Seconds from ordinary notation; false - the date or time is invalid, or the year is outside 2025-2088.
inline bool rtcFromCivil(const RtcCivil& c, uint32_t& s) {
    if (c.year < RTC_BASE_YEAR || c.year >= RTC_BASE_YEAR + RTC_YEARS || c.month < 1 || c.month > 12) return false;
    const unsigned y = c.year - RTC_BASE_YEAR;
    if (c.day < 1 || c.day > rtcDaysInMonth(y, c.month) || c.hour > 23 || c.minute > 59 || c.second > 59) return false;
    s = rtcDayNumber(y, c.month, c.day) * RTC_DAY + c.hour * 3600 + c.minute * 60 + c.second;
    return true;
}
// Parsing "DD.MM.YYYYTHH:MM:SS" (the --rtc option, sect. 9.2): exactly this format, 24-hour notation.
inline bool rtcParse(const std::string& str, uint32_t& s) {
    static const char pattern[] = "DD.DD.DDDDTDD:DD:DD";
    if (str.size() != sizeof pattern - 1) return false;
    for (size_t i = 0; i < str.size(); i++)
        if (pattern[i] == 'D' ? (str[i] < '0' || str[i] > '9') : str[i] != pattern[i]) return false;
    const auto num = [&](size_t at, size_t n) { unsigned v = 0; for (size_t i = 0; i < n; i++) v = v * 10 + (unsigned)(str[at + i] - '0'); return v; };
    return rtcFromCivil({num(6, 4), num(3, 2), num(0, 2), num(11, 2), num(14, 2), num(17, 2)}, s);
}

// Device: the exchange registers TIME, DATE, the ERR bit and the clock itself. The clock is the seconds base at the moment
// anchor (microseconds of the monotonic clock clock()); it runs while the debugger is paused and raises no interrupts.
class RtcDevice {
public:
    uint16_t time = 0, date = 0;
    bool err = false;
    uint32_t base = 0;
    int64_t anchor = 0;
    std::function<int64_t()> clock = [] { return (int64_t)0; };

    // Machine reset: the registers are 0, the value and the running of the clock do not change.
    void reset() { time = date = 0; err = false; }
    static bool isPort(uint16_t a) { return a >= RTC_FIRST && a <= RTC_LAST; }

    uint32_t valueAt(int64_t now) const {
        const int64_t el = now - anchor;
        const int64_t sec = el >= 0 ? el / RTC_SECOND_US : -((-el + RTC_SECOND_US - 1) / RTC_SECOND_US);
        return (uint32_t)((((int64_t)base + sec) % RTC_PERIOD + RTC_PERIOD) % RTC_PERIOD);
    }
    uint32_t value() const { return valueAt(clock()); }
    // Set the value s (0..RTC_PERIOD - 1) so that the next second comes at anchorUs + 1 s.
    void set(uint32_t s, int64_t anchorUs) { base = s % RTC_PERIOD; anchor = anchorUs; }
    void setNow(uint32_t s) { set(s, clock()); }

    uint8_t read8(uint16_t a) const {
        switch (a) {
        case RTC_TIME: return (uint8_t)time;
        case RTC_TIME + 1: return (uint8_t)(time >> 8);
        case RTC_DATE: return (uint8_t)date;
        case RTC_DATE + 1: return (uint8_t)(date >> 8);
        case RTC_STATUS: return err ? RTC_ERR : 0;
        default: return 0;
        }
    }
    void write8(uint16_t a, uint8_t v) {
        switch (a) {
        case RTC_TIME: time = (uint16_t)((time & 0xFF00) | v); break;
        case RTC_TIME + 1: time = (uint16_t)((time & 0x00FF) | (v << 8)); break;
        case RTC_DATE: date = (uint16_t)((date & 0xFF00) | v); break;
        case RTC_DATE + 1: date = (uint16_t)((date & 0x00FF) | (v << 8)); break;
        case RTC_CMD: command(v); break;
        default: break;
        }
    }

private:
    void command(uint8_t c) {
        const int64_t now = clock();
        const uint32_t v = valueAt(now);
        switch (c) {
        case RTCC_READ: rtcFromSeconds(v, time, date); break;
        case RTCC_WRITE:                 // the whole value; the fraction of a second is reset
            if (!(err = !rtcValid(time, date))) set(rtcToSeconds(time, date), now);
            break;
        case RTCC_WRITE_TIME:            // the time of day from TIME and PM; the date is unchanged; the fraction of a second is reset
            if (!(err = !rtcValidTime(time))) {
                uint16_t t, d;
                rtcFromSeconds(v, t, d);
                set(rtcToSeconds(time, (uint16_t)((d & ~1u) | (date & 1))), now);
            }
            break;
        case RTCC_WRITE_DATE:            // the date from DATE without PM; the time of day and the second phase are unchanged
            if (!(err = !rtcValidDate(date))) {
                const uint32_t nv = rtcDayNumber(date >> 10, (date >> 6) & 15, (date >> 1) & 31) * RTC_DAY + v % RTC_DAY;
                base = (uint32_t)(((int64_t)base + (int64_t)nv - (int64_t)v) % RTC_PERIOD + RTC_PERIOD) % RTC_PERIOD;
            }
            break;
        default: break;                  // 0, 5-255: nothing
        }
    }
};

static_assert(CHARSET[0xA3] == U'ё' && CHARSET[0xB3] == U'Ё' && CHARSET[0xC0] == U'А' && CHARSET[0xFF] == U'я');
static_assert(CHARSET[0x80] == U'─' && CHARSET[0xA0] == U'═' && CHARSET[0x1A] == U'→' && CHARSET[0x41] == U'A');

}  // namespace cpu16

// asm_oracle.cpp - the reference for checking the JS assembler: runs a corpus through cpu16asm.cpp.
//
// Input (stdin), one line per case, fields separated by tabs:
//   id  name  source(base64)  files
// where files is empty or a list "path=base64" separated by ';' (then the case is assembled
// through assembleFile(name) after writing the files to disk; otherwise assembleText(name, source)).
// Output (stdout), one line per case:
//   id  ok  diagnostics(base64)  minAddr  maxAddr  image_hash  listing(base64)  map(base64)  exec(base64)
// where exec is the CX16 header check messages (--exec) for the image from address 0, one line per message
// (empty if assembly failed or the header is correct).
//
// Build (from the project root): g++ -std=c++17 -O2 -Itools -o build/asm_oracle debugger/tests/asm_oracle.cpp

#define main cpu16asm_main
#include "cpu16asm.cpp"
#undef main

#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

static std::string b64enc(const std::string& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < d.size(); i += 3) {
        const unsigned v = ((uint8_t)d[i] << 16) | ((i + 1 < d.size() ? (uint8_t)d[i + 1] : 0) << 8) | (i + 2 < d.size() ? (uint8_t)d[i + 2] : 0);
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += i + 1 < d.size() ? t[(v >> 6) & 63] : '='; o += i + 2 < d.size() ? t[v & 63] : '=';
    }
    return o;
}
static std::string b64dec(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string o;
    unsigned acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((char)((acc >> bits) & 0xFF)); }
    }
    return o;
}
static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> r;
    size_t p = 0;
    for (;;) {
        const size_t e = s.find(sep, p);
        r.push_back(s.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return r;
}
static void mkdirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const auto f = split(line, '\t');
        const std::string id = f[0], name = f[1], src = b64dec(f[2]);
        cpu16asm::Assembler as;
        bool ok;
        if (f.size() > 3 && !f[3].empty()) {
            for (const auto& kv : split(f[3], ';')) {
                const size_t eq = kv.find('=');
                const std::string path = kv.substr(0, eq);
                mkdirs(path);
                std::ofstream(path, std::ios::binary) << b64dec(kv.substr(eq + 1));
            }
            ok = as.assembleFile(name);
        } else {
            ok = as.assembleText(name, src);
        }
        uint32_t h = 2166136261u;
        for (int a = 0; a < 65536; a++)
            if (as.owner[a] >= 0) { h = (h ^ (uint32_t)a) * 16777619u; h = (h ^ as.image[a]) * 16777619u; }
        std::string exec;
        if (ok) for (const auto& m : as.checkExec()) exec += m + "\n";
        std::cout << id << '\t' << (ok ? 1 : 0) << '\t' << b64enc(as.formatDiags()) << '\t' << as.minAddr << '\t'
                  << as.maxAddr << '\t' << h << '\t' << b64enc(as.listing()) << '\t' << b64enc(as.memoryMap()) << '\t'
                  << b64enc(exec) << '\n';
    }
    return 0;
}

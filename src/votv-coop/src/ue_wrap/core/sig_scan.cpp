#include "ue_wrap/core/sig_scan.h"

#include <windows.h>

#include <vector>

namespace ue_wrap {
namespace {

// A parsed pattern byte: either a concrete value or a wildcard.
struct PatByte {
    uint8_t value;
    bool wild;
};

std::vector<PatByte> ParsePattern(const char* pattern) {
    std::vector<PatByte> out;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (*p == '?') {
            out.push_back({0, true});
            ++p;
            if (*p == '?') ++p;  // accept "??" or "?"
            continue;
        }
        // Parse one hex byte (two nibbles).
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(p[0]);
        const int lo = (hi >= 0) ? nibble(p[1]) : -1;
        if (hi < 0 || lo < 0) break;  // malformed; stop
        out.push_back({static_cast<uint8_t>((hi << 4) | lo), false});
        p += 2;
    }
    return out;
}

}  // namespace

void MainModuleRange(uintptr_t& base, size_t& size) {
    HMODULE h = ::GetModuleHandleW(nullptr);
    base = reinterpret_cast<uintptr_t>(h);
    size = 0;
    if (!h) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    size = nt->OptionalHeader.SizeOfImage;
}

uintptr_t FindPatternIn(uintptr_t base, size_t size, const char* pattern) {
    const std::vector<PatByte> pat = ParsePattern(pattern);
    if (pat.empty() || size < pat.size()) return 0;

    const auto* bytes = reinterpret_cast<const uint8_t*>(base);
    const size_t last = size - pat.size();
    const size_t n = pat.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            if (!pat[j].wild && bytes[i + j] != pat[j].value) break;
        }
        if (j == n) return base + i;
    }
    return 0;
}

uintptr_t FindPattern(const char* pattern) {
    uintptr_t base = 0;
    size_t size = 0;
    MainModuleRange(base, size);
    if (!base || !size) return 0;
    return FindPatternIn(base, size, pattern);
}

}  // namespace ue_wrap

// coop/player/hand_history.cpp -- see coop/player/hand_history.h.

#include "coop/player/hand_history.h"

#include "coop/player/players_registry.h"

#include <cstdio>
#include <cwchar>

namespace coop::hand_history {
namespace {

struct Held {
    std::wstring cls;
    uint64_t fromMs = 0;
    uint64_t toMs = 0;  // 0 while the class is still held
};

struct Ring {
    Held entry[kRing];
    int newest = -1;  // index of the newest entry; -1 when empty
    int count = 0;
};

Ring g_rings[coop::players::kMaxPeers];

bool SameClass(const std::wstring& a, const wchar_t* b) {
    return ::_wcsicmp(a.c_str(), b) == 0;
}

}  // namespace

void Note(uint8_t slot, bool has, const std::wstring& cls, uint64_t nowMs) {
    if (slot >= coop::players::kMaxPeers) return;
    Ring& r = g_rings[slot];
    Held* open = (r.newest >= 0 && r.entry[r.newest].toMs == 0) ? &r.entry[r.newest] : nullptr;
    if (open && (!has || !SameClass(open->cls, cls.c_str()))) {
        open->toMs = nowMs;
        open = nullptr;
    }
    if (!has || open) return;
    r.newest = (r.newest + 1) % kRing;
    if (r.count < kRing) ++r.count;
    r.entry[r.newest] = Held{cls, nowMs, 0};
}

bool HeldWithinMatching(uint8_t slot, ClassMatch match, const wchar_t* want, uint64_t windowMs, uint64_t nowMs) {
    if (slot >= coop::players::kMaxPeers || !match || !want) return false;
    const Ring& r = g_rings[slot];
    for (int i = 0; i < r.count; ++i) {
        const Held& h = r.entry[i];
        if (!match(h.cls, want)) continue;
        if (h.toMs == 0 || h.toMs + windowMs >= nowMs) return true;
    }
    return false;
}

bool HeldWithin(uint8_t slot, const wchar_t* cls, uint64_t windowMs, uint64_t nowMs) {
    return HeldWithinMatching(slot, &SameClass, cls, windowMs, nowMs);
}

std::string Describe(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return "slot out of range";
    const Ring& r = g_rings[slot];
    std::string out;
    for (int k = 0; k < r.count; ++k) {
        const Held& h = r.entry[(r.newest - r.count + 1 + k + kRing) % kRing];
        char buf[48];
        std::snprintf(buf, sizeof(buf), "[%llu..%llu] ", static_cast<unsigned long long>(h.fromMs),
                      static_cast<unsigned long long>(h.toMs));
        out += std::string(h.cls.begin(), h.cls.end()) + buf;
    }
    return out.empty() ? "empty" : out;
}

void Clear(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return;
    g_rings[slot] = Ring{};
}

}  // namespace coop::hand_history

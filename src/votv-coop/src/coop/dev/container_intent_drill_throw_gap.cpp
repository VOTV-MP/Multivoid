// coop/dev/container_intent_drill_throw_gap.cpp -- the instants of a throw's flight, said by one peer and set against
// another peer's trace: see container_intent_drill_throw.h. The thrower says where its copy was at kAtCount instants
// after the press (wall clock, the same machine on loopback); the host sets its own copy against them, a joiner says its
// own and the host sets its copy against those, and the client of a host throw sets its mirror against the host's. The
// same two functions serve all of them.

#include "container_intent_drill_throw.h"
#include "container_route_drill_detail.h"

#include "coop/dev/drill_chat.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::dev::container_intent_drill::detail {

namespace DC = coop::dev::drill_chat;
using ue_wrap::FVector;

namespace {
unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }
}  // namespace

// The `n` comma-separated integers after `key` (with its '=') in `text`; false when the key is absent or the list short.
bool IntsAfter(const std::string& text, const char* key, int* out, int n) {
    const size_t at = text.find(key);
    if (at == std::string::npos) return false;
    const char* p = text.c_str() + at + std::strlen(key);
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = static_cast<int>(std::strtol(p, &end, 10));
        if (end == p) return false;
        p = end;
        if (i + 1 < n) {
            if (*p != ',') return false;
            ++p;
        }
    }
    return true;
}

void SayInstants(const Track& t, uint64_t pressMs, const char* who, int n) {
    char say[160];
    for (int k = 1; k <= kAtCount; ++k) {
        const uint64_t want = pressMs + static_cast<uint64_t>(k) * kAtEveryMs;
        const Sample* best = nullptr;
        for (const Sample& s : t.samples) {
            const uint64_t d = s.t > want ? s.t - want : want - s.t;
            if (d <= kAtSlackMs && (!best || d < (best->t > want ? best->t - want : want - best->t))) best = &s;
        }
        if (!best) continue;
        std::snprintf(say, sizeof(say), "%sk=%d t=%llu p=%d,%d,%d", ThrowLine(who, "at", n, true).c_str(), k, U(best->t),
                      static_cast<int>(best->pos.X), static_cast<int>(best->pos.Y), static_cast<int>(best->pos.Z));
        DC::Say(say);
    }
}

Gap JudgeInstants(const Track& t, const char* who, int n) {
    Gap g;
    for (int k = 1; k <= kAtCount; ++k) {
        char key[24];
        std::snprintf(key, sizeof(key), "k=%d ", k);
        std::string rest;
        if (!DC::LiveRestOf(ThrowLine(who, "at", n, true) + key, &rest)) continue;
        ++g.said;
        int p[3] = {};
        const size_t at = rest.find("t=");
        if (at == std::string::npos || !IntsAfter(rest, "p=", p, 3)) continue;
        const uint64_t want = std::strtoull(rest.c_str() + at + 2, nullptr, 10);
        const Sample* best = nullptr;
        for (const Sample& s : t.samples) {
            const uint64_t d = s.t > want ? s.t - want : want - s.t;
            if (d <= kAtSlackMs && (!best || d < (best->t > want ? best->t - want : want - best->t))) best = &s;
        }
        if (!best) continue;
        ++g.matched;
        const float gap = Dist3(best->pos, FVector{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])});
        if (gap > g.worst) { g.worst = gap; g.worstK = k; }
    }
    return g;
}

}  // namespace coop::dev::container_intent_drill::detail

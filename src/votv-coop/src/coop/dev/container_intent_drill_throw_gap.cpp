// coop/dev/container_intent_drill_throw_gap.cpp -- the instants of a throw's flight, said by one peer and set against
// another peer's trace: see container_intent_drill_throw.h. The thrower says where its copy was at kAtCount instants
// after the press (wall clock, the same machine on loopback); the host sets its own copy against them, a joiner says its
// own and the host sets its copy against those, and the client of a host throw sets its mirror against the host's. The
// same functions serve all of them. The set travels as a few lines (the chat line is short) and is collected part by part.

#include "container_intent_drill_throw.h"
#include "container_route_drill_detail.h"

#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// The sample of `t` nearest the wall-clock instant `want`, within kAtSlackMs; null with none.
static const Sample* SampleAt(const Track& t, uint64_t want) {
    const Sample* best = nullptr;
    for (const Sample& s : t.samples) {
        const uint64_t d = s.t > want ? s.t - want : want - s.t;
        if (d <= kAtSlackMs && (!best || d < (best->t > want ? best->t - want : want - best->t))) best = &s;
    }
    return best;
}

// The instants leave as lines of at most the chat's own cap (ShapeChatLine cuts a longer line, chat_sync.cpp), as few as that
// allows; every line names its place in the set and the press, so the host needs any one part to place its own.
void SayInstants(const Track& t, uint64_t pressMs, const char* who, int n) {
    constexpr size_t kCap = sizeof(coop::net::ChatMessagePayload::text);
    std::vector<std::string> tuples;
    char one[64];
    for (int k = 1; k <= kAtCount; ++k) {
        const Sample* best = SampleAt(t, pressMs + static_cast<uint64_t>(k) * kAtEveryMs);
        if (!best) continue;
        std::snprintf(one, sizeof(one), "%d,%lld,%d,%d,%d;", k, static_cast<long long>(best->t) - static_cast<long long>(pressMs),
                      static_cast<int>(best->pos.X), static_cast<int>(best->pos.Y), static_cast<int>(best->pos.Z));
        tuples.emplace_back(one);
    }
    const std::string head = ThrowLine(who, "inst", n, true);
    char press[40];
    std::snprintf(press, sizeof(press), "press=%llu ", U(pressMs));
    const size_t room = kCap - head.size() - std::strlen("part=9/9 ") - std::strlen(press);
    std::vector<std::string> parts(1);
    for (const std::string& tp : tuples) {
        if (!parts.back().empty() && parts.back().size() + tp.size() > room) parts.emplace_back();
        parts.back() += tp;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        char mid[24];
        std::snprintf(mid, sizeof(mid), "part=%zu/%zu ", i + 1, parts.size());
        DC::Say(head + mid + press + parts[i]);
    }
}

void InstantsCollect(Instants& in, const char* who, int n) {
    std::vector<std::string> rows;
    DC::LiveRestsOf(ThrowLine(who, "inst", n, true), &rows);
    for (const std::string& r : rows) {
        int i = 0, m = 0;
        unsigned long long press = 0;
        if (std::sscanf(r.c_str(), "part=%d/%d press=%llu", &i, &m, &press) != 3 || i < 1 || m < i || m > Instants::kMaxParts) continue;
        if (in.m != 0 && in.m != m) continue;
        in.m = m;
        in.pressMs = press;
        in.seen |= 1u << (i - 1);
        const size_t sp = r.find(' ', r.find("press="));
        const char* q = sp == std::string::npos ? "" : r.c_str() + sp + 1;
        for (;;) {
            int k = 0, x = 0, y = 0, z = 0, used = 0;
            long long dt = 0;
            if (std::sscanf(q, "%d,%lld,%d,%d,%d;%n", &k, &dt, &x, &y, &z, &used) != 5 || used <= 0) break;
            q += used;
            if (k < 1 || k > kAtCount) continue;
            in.at[k] = {true, dt, {x, y, z}};
        }
    }
}

int InstantsHeld(const Instants& in) {
    int held = 0;
    for (int i = 0; i < in.m; ++i)
        if (in.seen & (1u << i)) ++held;
    return held;
}

bool InstantsComplete(const Instants& in) { return in.m > 0 && InstantsHeld(in) == in.m; }

Gap JudgeInstants(const Track& t, const Instants& in) {
    Gap g;
    for (int k = 1; k <= kAtCount; ++k) {
        if (!in.at[k].have) continue;
        ++g.said;
        const Sample* best = SampleAt(t, static_cast<uint64_t>(static_cast<long long>(in.pressMs) + in.at[k].dtMs));
        if (!best) continue;
        ++g.matched;
        const float gap = Dist3(best->pos, FVector{static_cast<float>(in.at[k].p[0]), static_cast<float>(in.at[k].p[1]),
                                                   static_cast<float>(in.at[k].p[2])});
        if (gap > g.worst) { g.worst = gap; g.worstK = k; }
    }
    return g;
}

int EarlySpeed(const Track& t, uint64_t pressMs) {
    float best = 0.f;
    for (const Sample& s : t.samples)
        if (s.t >= pressMs && s.t - pressMs <= kEarlyMs && s.speed > best) best = s.speed;
    return static_cast<int>(best);
}

}  // namespace coop::dev::container_intent_drill::detail

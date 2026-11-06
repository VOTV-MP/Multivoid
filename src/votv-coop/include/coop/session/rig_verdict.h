// coop/session/rig_verdict.h -- the one log line a drill ends on, and a test rig stops on.
//
// A rig that waits for a drill's own wording ends only when the operator typed the right marker, and a drill that
// cannot run sat idle to the run's budget. So every drill says its end in one shape. Every other log line may be
// reworded; this one is a contract with tools outside the tree:
//   rig: VERDICT <tag> PASS|FAIL|INVALID[ -- <reason>]
//   <tag>     the drill's name, [a-z0-9_-]+. One judge per drill says PASS (by convention the host); any peer
//             may say FAIL or INVALID.
//   PASS      the drill measured what it was built for and it held.
//   FAIL      it measured a failure (a red arm's expected end).
//   INVALID   it could not measure (a precondition missing, no route, a peer's signal never came): neither
//             green nor red.
// A tag is said once per process life; a second Say for it is dropped and logged in a line that does not carry
// the contract's prefix.

#pragma once

#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace coop::rig_verdict {

enum class Outcome { Pass, Fail, Invalid };

namespace detail {

inline constexpr int kMaxTags = 32;
inline constexpr int kReasonChars = 384;

// The tags already said, as FNV-1a hashes (0 = a free slot). A slot is claimed by compare-exchange, so Say needs no
// lock, takes no heap and is safe from any thread: a drill's host and client halves run on the game thread, and a
// helper thread (peer_signal's wait) may say INVALID.
inline std::atomic<uint32_t> g_said[kMaxTags]{};

inline uint32_t HashOf(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 16777619u;
    return h ? h : 1u;
}

// True when `tag` is new, and now claimed. A full table cannot tell a repeat from a first Say, so it lets the line
// through: a missing verdict would idle the run, a repeated one is dropped by the rig's first-line rule.
inline bool Claim(const char* tag) {
    const uint32_t h = HashOf(tag);
    for (int i = 0; i < kMaxTags; ++i) {
        uint32_t seen = g_said[i].load(std::memory_order_acquire);
        if (seen == 0 && g_said[i].compare_exchange_strong(seen, h, std::memory_order_acq_rel)) return true;
        if (seen == h) return false;
    }
    return true;
}

}  // namespace detail

// `reason` is printf-style and may be null (a bare verdict). A line break in it becomes a space.
inline void Say(const char* tag, Outcome outcome, const char* fmt = nullptr, ...) {
    if (!detail::Claim(tag)) {
        UE_LOGW("rig: a second verdict for '%s' dropped", tag);
        return;
    }
    char reason[detail::kReasonChars] = {};
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(reason, sizeof(reason), fmt, args);
        va_end(args);
        for (char* c = reason; *c; ++c)
            if (*c == '\n' || *c == '\r') *c = ' ';
    }
    const char* word = outcome == Outcome::Pass ? "PASS" : outcome == Outcome::Fail ? "FAIL" : "INVALID";
    const char* dash = reason[0] ? " -- " : "";
    if (outcome == Outcome::Pass) UE_LOGI("rig: VERDICT %s %s%s%s", tag, word, dash, reason);
    else UE_LOGW("rig: VERDICT %s %s%s%s", tag, word, dash, reason);
}

}  // namespace coop::rig_verdict

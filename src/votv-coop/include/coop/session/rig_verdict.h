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
// A tag is said once per connection life: a second Say is dropped (logged without the contract's prefix) until
// Rearm, which DisconnectAll calls (a client's session end, a host's last client leaving). Say maps a tag into
// [a-z0-9_-] (upper case lowered, other bytes '_', empty "untagged", 47 bytes at most): every counted line parses.

#pragma once

#include "ue_wrap/core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace coop::rig_verdict {

enum class Outcome { Pass, Fail, Invalid };

namespace detail {

inline constexpr int kMaxTags = 32;
inline constexpr int kTagChars = 48;
inline constexpr int kReasonChars = 384;

// The tags already said, as text (a free slot starts with a NUL). A mutex, because a drill's host and client halves
// run on the game thread but a helper thread (peer_signal's wait) may say INVALID, and a slot is a string that no
// single atomic claims; a drill says its verdict once, so the lock is never contended in a way that costs.
struct Latch {
    std::mutex lock;
    char said[kMaxTags][kTagChars] = {};
};
inline Latch g_latch;

// `out` holds the tag in [a-z0-9_-]+, at most kTagChars - 1 bytes.
inline void Sanitize(const char* tag, char (&out)[kTagChars]) {
    int n = 0;
    for (; tag && *tag && n < kTagChars - 1; ++tag, ++n) {
        const char c = *tag;
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        const bool ok =
            (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '_' || lower == '-';
        out[n] = ok ? lower : '_';
    }
    if (n == 0) {
        std::snprintf(out, kTagChars, "untagged");
        return;
    }
    out[n] = '\0';
}

// True when `tag` is new, and now claimed. A full table cannot tell a repeat from a first Say, so it lets the line
// through: a missing verdict would idle the run, and a repeat is only one more line for the rig to read.
inline bool Claim(const char* tag) {
    std::lock_guard<std::mutex> hold(g_latch.lock);
    for (int i = 0; i < kMaxTags; ++i) {
        if (g_latch.said[i][0] == '\0') {
            std::snprintf(g_latch.said[i], kTagChars, "%s", tag);
            return true;
        }
        if (std::strcmp(g_latch.said[i], tag) == 0) return false;
    }
    return true;
}

}  // namespace detail

// From DisconnectAll (a client's session end, or a host's last client leaving): every tag may be said again.
inline void Rearm() {
    std::lock_guard<std::mutex> hold(detail::g_latch.lock);
    for (auto& slot : detail::g_latch.said) slot[0] = '\0';
}

// `reason` is printf-style and may be null (a bare verdict). A line break in it becomes a space.
inline void Say(const char* rawTag, Outcome outcome, const char* fmt = nullptr, ...) {
    char tag[detail::kTagChars];
    detail::Sanitize(rawTag, tag);
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

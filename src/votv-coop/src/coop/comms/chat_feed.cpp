#include "coop/comms/chat_feed.h"

#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace coop::chat_feed {

namespace {

// A line lives kTtlMs, fading over the last kFadeMs. Matches the old hud_feed 10 s
// feel with a soft fade-out tail so a line doesn't pop off abruptly. kFadeInMs is
// the arrival ramp (2026-07-04, the chat-imgui-samp fade-in): a new line eases in
// instead of popping -- short enough to feel instant, long enough to read as motion.
constexpr uint64_t kTtlMs    = 11000;
constexpr uint64_t kFadeMs   = 1500;
constexpr uint64_t kFadeInMs = 220;

struct Entry {
    std::string text;
    uint64_t    bornMs = 0;
    uint8_t     nickLen = 0;
    uint8_t     slot = 0;
    uint8_t     action = 0;  // 1 = peer-action line (HUD draws the predicate yellow)
};

// Lines queued by PushDelayed, promoted to g_lines by Tick once dueMs is reached. Game-thread only.
struct Pending {
    std::string text;
    uint64_t    dueMs = 0;
};
std::deque<Pending> g_pending;

// g_lines is GAME-THREAD ONLY (Push/Tick/Reset all run there) -- no lock needed
// between them. The published POD snapshot (g_pub) is what the render thread reads,
// guarded by g_mu; g_count is a lock-free HasAny() fast-path.
std::deque<Entry> g_lines;
std::mutex        g_mu;
Snapshot          g_pub;
std::atomic<int>  g_count{0};

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// (ToUtf8 moved to namespace scope below -- exported via chat_feed.h, 2026-07-10 dedupe.)

// ---- resurrection probe (user 2026-07-04: a long-gone line sometimes REAPPEARS
// for ~0.5 s and fades out again). Static analysis proves the store can't do it
// (per-entry alpha rises only during the 220 ms arrival ramp and is monotone-
// decreasing after; bornMs monotone; expiry pops the oldest; the host relay
// excludes the origin) -- so the mechanism is either a duplicate re-push of the
// same TEXT or something outside this file. PERMANENT cheap logging (a few
// lines/min at most) so the NEXT sighting's log names the path: every push,
// every expiry AND every overflow drop is logged; a push whose text matches a
// line that expired/dropped <60 s ago is flagged [feed RESURRECT]; and Republish
// cross-checks the published snapshot against the previous one for an impossible
// alpha RISE on the SAME entry (bornMs identity) in its fade-out tail
// ([feed ALPHA-JUMP] = the "can't happen" detector -- entry-keyed + tail-gated
// so the arrival ramp and a coexisting same-text line can never false-fire it).
struct Expired {
    char     text[64] = {};
    uint64_t atMs = 0;
};
Expired g_expired[8];   // small ring of recently expired lines (game thread)
int     g_expiredNext = 0;

void NoteExpired(const std::string& text, uint64_t now) {
    Expired& x = g_expired[g_expiredNext];
    g_expiredNext = (g_expiredNext + 1) % 8;
    std::snprintf(x.text, sizeof(x.text), "%s", text.c_str());
    x.atMs = now;
}

// Cap g_lines at kMaxLines, logging + expiry-ring-noting every overflow drop so
// the probe is not blind to the 7th-line path (audit note 2026-07-04).
void TrimOverflow(const char* via) {
    while (g_lines.size() > static_cast<size_t>(kMaxLines)) {
        const Entry& f = g_lines.front();
        UE_LOGI("feed: drop-overflow via=%s text=\"%.40s\"", via, f.text.c_str());
        NoteExpired(f.text, NowMs());
        g_lines.pop_front();
    }
}

void ProbeOnPush(const char* via, const Entry& e, size_t linesNow) {
    UE_LOGI("feed: push via=%s slot=%u nickLen=%u lines=%zu text=\"%.40s\"",
            via, static_cast<unsigned>(e.slot), static_cast<unsigned>(e.nickLen),
            linesNow, e.text.c_str());
    for (const Expired& x : g_expired) {
        if (!x.text[0] || x.atMs == 0) continue;
        if (e.bornMs - x.atMs > 60000) continue;
        if (std::strncmp(x.text, e.text.c_str(), sizeof(x.text) - 1) == 0) {
            UE_LOGW("feed: RESURRECT -- same text re-pushed %.1f s after it expired "
                    "(via=%s) text=\"%.40s\"",
                    static_cast<double>(e.bornMs - x.atMs) / 1000.0, via, e.text.c_str());
        }
    }
}

float FadeAlpha(uint64_t ageMs) {
    if (ageMs >= kTtlMs) return 0.f;
    float a = 1.f;
    if (ageMs < kFadeInMs)  // arrival ramp (ease-out: fast start, soft settle)
        a = static_cast<float>(ageMs) / static_cast<float>(kFadeInMs);
    if (ageMs > kTtlMs - kFadeMs)
        a = static_cast<float>(kTtlMs - ageMs) / static_cast<float>(kFadeMs);
    return a;
}

// Rebuild the published snapshot from g_lines (recomputing each line's fade), then
// store it for the render thread. Game thread.
void Republish() {
    Snapshot s;
    const uint64_t now = NowMs();
    for (const auto& e : g_lines) {
        if (s.count >= kMaxLines) break;
        Line& l = s.lines[s.count];
        std::snprintf(l.text, sizeof(l.text), "%s", e.text.c_str());
        l.alpha   = FadeAlpha(now - e.bornMs);
        l.bornMs  = e.bornMs;
        l.nickLen = e.nickLen;
        l.slot    = e.slot;
        l.action  = e.action;
        ++s.count;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        // Resurrection probe: the SAME entry (bornMs identity) rising in alpha
        // while it sits in its fade-out TAIL is impossible by this store's math
        // (the only legitimate rise is the 220 ms arrival ramp, excluded by the
        // tail gate; a coexisting same-text line has a different bornMs). If it
        // ever logs, the mechanism is inside this file after all -- and the log
        // carries the numbers to prove where.
        for (int n = 0; n < s.count; ++n) {
            if (now - s.lines[n].bornMs <= kTtlMs - kFadeMs) continue;  // not in the tail
            for (int o = 0; o < g_pub.count; ++o) {
                if (s.lines[n].bornMs == g_pub.lines[o].bornMs &&
                    g_pub.lines[o].alpha < 0.5f &&
                    s.lines[n].alpha > g_pub.lines[o].alpha + 0.25f) {
                    UE_LOGW("feed: ALPHA-JUMP -- \"%.40s\" (born=%llu) published alpha %.2f -> %.2f",
                            s.lines[n].text,
                            static_cast<unsigned long long>(s.lines[n].bornMs),
                            g_pub.lines[o].alpha, s.lines[n].alpha);
                }
            }
        }
        g_pub = s;
    }
    g_count.store(s.count, std::memory_order_relaxed);
}

}  // namespace

// UTF-8-encode a wide string (UTF-16 on Windows, surrogate pairs included).
// Replaced ToAscii 2026-07-04: the feed carries UTF-8 now that the overlay font
// has Cyrillic glyphs -- a Russian nick in "X joined the game" renders as-is.
std::string ToUtf8(const std::wstring& w) {
    std::string s;
    s.reserve(w.size() * 2);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size() &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x20 && cp != 0x09) continue;  // strip control chars
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return s;
}

void Push(const std::wstring& line) {
    Entry e;
    e.text = ToUtf8(line);
    e.bornMs = NowMs();
    ProbeOnPush("event", e, g_lines.size() + 1);
    g_lines.push_back(std::move(e));
    TrimOverflow("event");
    Republish();
}

void PushChat(const std::string& utf8Line, uint8_t nickByteLen, uint8_t slot) {
    Entry e;
    e.text = utf8Line;
    if (e.text.size() >= sizeof(Line{}.text)) e.text.resize(sizeof(Line{}.text) - 1);
    e.bornMs  = NowMs();
    e.nickLen = (nickByteLen <= e.text.size()) ? nickByteLen : 0;
    e.slot    = slot;
    ProbeOnPush("chat", e, g_lines.size() + 1);
    g_lines.push_back(std::move(e));
    TrimOverflow("chat");
    Republish();
}

void PushAction(const std::string& utf8Line, uint8_t nickByteLen, uint8_t slot) {
    Entry e;
    e.text = utf8Line;
    if (e.text.size() >= sizeof(Line{}.text)) e.text.resize(sizeof(Line{}.text) - 1);
    e.bornMs  = NowMs();
    e.nickLen = (nickByteLen <= e.text.size()) ? nickByteLen : 0;
    e.slot    = slot;
    e.action  = 1;
    ProbeOnPush("action", e, g_lines.size() + 1);
    g_lines.push_back(std::move(e));
    TrimOverflow("action");
    Republish();
}

void PushDelayed(const std::wstring& line, uint64_t delayMs) {
    Pending p;
    p.text  = ToUtf8(line);
    p.dueMs = NowMs() + delayMs;
    g_pending.push_back(std::move(p));
}

void Tick() {
    const uint64_t now = NowMs();
    // Promote any delayed lines whose time has come (born NOW so their TTL/fade starts here).
    bool promoted = false;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now >= it->dueMs) {
            Entry e; e.text = std::move(it->text); e.bornMs = now;
            ProbeOnPush("delayed", e, g_lines.size() + 1);
            g_lines.push_back(std::move(e));
            it = g_pending.erase(it);
            promoted = true;
        } else {
            ++it;
        }
    }
    if (promoted) TrimOverflow("delayed");
    if (g_lines.empty()) return;  // cheap idle path (nothing live; future-dated pending re-checked next Tick)
    while (!g_lines.empty() && now - g_lines.front().bornMs >= kTtlMs) {
        const Entry& f = g_lines.front();
        UE_LOGI("feed: expire age=%.1fs text=\"%.40s\"",
                static_cast<double>(now - f.bornMs) / 1000.0, f.text.c_str());
        NoteExpired(f.text, now);
        g_lines.pop_front();
    }
    Republish();  // promote-fresh + expire-old + refresh the fade alphas on the survivors
}

void GetSnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_pub;
}

bool HasAny() {
    return g_count.load(std::memory_order_relaxed) > 0;
}

void Reset() {
    g_lines.clear();
    g_pending.clear();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pub = Snapshot{};
    }
    g_count.store(0, std::memory_order_relaxed);
}

}  // namespace coop::chat_feed

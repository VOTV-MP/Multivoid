#include "coop/comms/chat_feed.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
        l.nickLen = e.nickLen;
        l.slot    = e.slot;
        ++s.count;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pub = s;
    }
    g_count.store(s.count, std::memory_order_relaxed);
}

}  // namespace

void Push(const std::wstring& line) {
    Entry e;
    e.text = ToUtf8(line);
    e.bornMs = NowMs();
    g_lines.push_back(std::move(e));
    while (g_lines.size() > static_cast<size_t>(kMaxLines)) g_lines.pop_front();
    Republish();
}

void PushChat(const std::string& utf8Line, uint8_t nickByteLen, uint8_t slot) {
    Entry e;
    e.text = utf8Line;
    if (e.text.size() >= sizeof(Line{}.text)) e.text.resize(sizeof(Line{}.text) - 1);
    e.bornMs  = NowMs();
    e.nickLen = (nickByteLen <= e.text.size()) ? nickByteLen : 0;
    e.slot    = slot;
    g_lines.push_back(std::move(e));
    while (g_lines.size() > static_cast<size_t>(kMaxLines)) g_lines.pop_front();
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
            g_lines.push_back(std::move(e));
            it = g_pending.erase(it);
            promoted = true;
        } else {
            ++it;
        }
    }
    if (promoted)
        while (g_lines.size() > static_cast<size_t>(kMaxLines)) g_lines.pop_front();
    if (g_lines.empty()) return;  // cheap idle path (nothing live; future-dated pending re-checked next Tick)
    while (!g_lines.empty() && now - g_lines.front().bornMs >= kTtlMs) g_lines.pop_front();
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

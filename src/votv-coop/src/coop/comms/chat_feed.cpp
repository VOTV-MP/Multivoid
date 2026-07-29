#include "coop/comms/chat_feed.h"

#include "coop/config/config.h"
#include "coop/text/utf8_codec.h"

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

// A LIVE line lives kTtlMs, fading over the last kFadeMs. Matches the old hud_feed
// 10 s feel with a soft fade-out tail so a line doesn't pop off abruptly. kFadeInMs
// is the arrival ramp (2026-07-04, the chat-imgui-samp fade-in): a new line eases in
// instead of popping -- short enough to feel instant, long enough to read as motion.
constexpr uint64_t kTtlMs    = 11000;
constexpr uint64_t kFadeMs   = 1500;
constexpr uint64_t kFadeInMs = 220;

struct Entry {
    std::string text;
    uint64_t    key = 0;               // total order + identity (chat_feed.h)
    uint64_t    bornMs = 0;            // wall birth (fade clock base) -- NOT identity
    uint64_t    bornSuspendedMs = 0;   // the suspension accumulator AT BIRTH (see below)
    uint32_t    nickArgb = 0;
    uint8_t     nickLen = 0;
    uint8_t     action = 0;
    Keep        keep = Keep::Transient;
};

// Lines queued by PushDelayed, promoted into the live tier by Tick once dueMs is
// reached. dueMs is WALL clock on purpose: the delay is about when the line should
// APPEAR, not about how long it then lives. Game-thread only.
struct Pending {
    std::string text;
    uint64_t    dueMs = 0;
    Keep        keep = Keep::Transient;
};
std::deque<Pending> g_pending;

std::mutex       g_mu;
Snapshot         g_pub;
std::atomic<int> g_count{0};

// --- the reveal state (the ONE place that answers "is the history on screen").
// Written from whichever thread closed the chat surface -- the WndProc ESC path,
// the render-thread Enter-submit, the SEH unlatch -- so it is ATOMICS ONLY. It
// never touches the line store, which stays game-thread-only.
std::atomic<bool>     g_chatOpen{false};
std::atomic<uint64_t> g_closeAtMs{0};
std::atomic<bool>     g_retentionFrozen{false};

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// --- the SUSPENDED TTL clock (game thread).
//
// While the reveal is up the TTL must not advance, or a player reading history
// watches it expire under them. The naive form -- stamping bornMs forward -- is not
// available: the open/close edge arrives on three different threads while the store
// is game-thread-only. So wall time keeps running and we accumulate how much of it
// was SUSPENDED, then subtract.
//
// The subtraction is the trap. A line born DURING a reveal has `now - bornMs` near
// zero while the accumulator is large, so a global "total suspended" would underflow
// an unsigned age straight past kTtlMs and pop the message one tick after it arrived
// -- the one behaviour the user called non-negotiable. Each entry therefore snapshots
// the accumulator at BIRTH and is aged against the suspension accrued SINCE, which is
// bounded above by the wall time since birth. Non-negative by construction.
//
// The accumulator only ever advances forward, and only on the game thread, at the top
// of every entry point that reads it. A reveal edge landing between two ticks is
// attributed to the next slice -- an error bounded by one tick (~16 ms).
uint64_t g_suspendedMs = 0;
uint64_t g_lastAdvanceMs = 0;
bool     g_revealLatched = false;

bool RevealActiveNow() {
    if (g_chatOpen.load(std::memory_order_relaxed)) return true;
    const uint64_t closedAt = g_closeAtMs.load(std::memory_order_relaxed);
    return closedAt != 0 && NowMs() - closedAt < kRevealMs;
}

void AdvanceSuspension(uint64_t now) {
    if (g_lastAdvanceMs == 0) g_lastAdvanceMs = now;
    if (now > g_lastAdvanceMs) {
        if (g_revealLatched) g_suspendedMs += now - g_lastAdvanceMs;
        g_lastAdvanceMs = now;
    }
    // Sampled AFTER accounting the slice that just elapsed, so the latch always
    // describes a slice that is already closed.
    g_revealLatched = RevealActiveNow();
}

uint64_t EffectiveAgeMs(const Entry& e, uint64_t now) {
    const int64_t wall = static_cast<int64_t>(now - e.bornMs);
    const int64_t held = static_cast<int64_t>(g_suspendedMs - e.bornSuspendedMs);
    const int64_t age  = wall - held;
    // The birth snapshot makes `held <= wall` structural. The clamp is not a fix for
    // a known case: it is here so that if the invariant were ever broken the line
    // ages NORMALLY rather than expiring instantly, which is the exact failure this
    // whole mechanism exists to prevent.
    return age > 0 ? static_cast<uint64_t>(age) : 0;
}

// A composed line is up to 285 bytes -- kNickMaxBytes (80) + ": " + the 203-byte wire
// text -- against a 256-byte Line, so it CAN overflow and the header once claimed it
// could not. It must be cut on a CHARACTER boundary: the raw resize() that used to do
// this here, and the snprintf that publishes it, both cut on a BYTE, which splits a
// multi-byte sequence and puts ill-formed UTF-8 on the exact surface the receive
// boundary was hardened to keep it off. Cut ONCE, here at birth, so the per-tick
// publish never has to truncate anything.
void SetText(Entry& e, const std::string& utf8) {
    e.text = coop::text::CapUtf8Bytes(utf8, sizeof(Line{}.text) - 1);
}

// The total order (chat_feed.h). ONE key, two producers. A wire row sits at
// (its host lineSeq, 0). A locally-authored row -- a join line, a peer action, this
// peer's own notices -- sits at (the newest host line applied so far, ++tiebreak), so
// it lands immediately after the last thing that was actually said and before the next.
// Without that base, "Connecting to <host>'s game..." would sort in front of a joiner's
// entire seeded history, because a local counter starting at 1 is below every lineSeq.
uint32_t g_wireBase = 0;
uint32_t g_tiebreak = 0;
uint64_t NextKey() {
    return (static_cast<uint64_t>(g_wireBase) << 32) | ++g_tiebreak;
}
uint64_t WireKey(uint32_t lineSeq) { return static_cast<uint64_t>(lineSeq) << 32; }

// DEV INJECTION -- the must-FAIL control for the retained tier. A feature that has
// only ever been shown WORKING passes by construction; with this set, Retire drops
// instead of retaining, so drill D-L must go RED.
bool NoRetain() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_CHAT_NO_RETAIN") == "1";
    return v;
}

// ---- resurrection probe (user 2026-07-04: a long-gone line sometimes REAPPEARS
// for ~0.5 s and fades out again). Static analysis proves the store can't do it
// (per-entry alpha rises only during the 220 ms arrival ramp and is monotone-
// decreasing after) -- so the mechanism is either a duplicate re-push of the same
// TEXT or something outside this file. PERMANENT cheap logging so the NEXT
// sighting's log names the path: every push and every DESTRUCTION is logged; a push
// whose text matches a line destroyed <60 s ago is flagged [feed RESURRECT]; and
// Republish cross-checks the published STORE alpha against the previous one for an
// impossible RISE on the SAME entry in its fade-out tail ([feed ALPHA-JUMP] = the
// "can't happen" detector).
//
// 2026-07-29: the probe now keys on `key`, not bornMs. Tick() hoists `now` outside
// its promotion loop, so two lines promoted in one tick carry the SAME bornMs and
// the old pairing could compare the wrong two entries -- the comment asserting that
// uniqueness was simply false. And a RETIRED line is no longer noted as expired: it
// still exists, so a legitimate re-push must not read as a resurrection.
struct Expired {
    char     text[64] = {};
    uint64_t atMs = 0;
};
Expired g_expired[8];
int     g_expiredNext = 0;

void NoteDestroyed(const std::string& text, uint64_t now) {
    Expired& x = g_expired[g_expiredNext];
    g_expiredNext = (g_expiredNext + 1) % 8;
    std::snprintf(x.text, sizeof(x.text), "%s", text.c_str());
    x.atMs = now;
}

void ProbeOnPush(const char* via, const Entry& e, size_t linesNow) {
    UE_LOGI("feed: push via=%s keep=%s nickLen=%u lines=%zu text=\"%.40s\"",
            via, e.keep == Keep::History ? "history" : "transient",
            static_cast<unsigned>(e.nickLen), linesNow, e.text.c_str());
    for (const Expired& x : g_expired) {
        if (!x.text[0] || x.atMs == 0) continue;
        if (e.bornMs - x.atMs > 60000) continue;
        if (std::strncmp(x.text, e.text.c_str(), sizeof(x.text) - 1) == 0) {
            UE_LOGW("feed: RESURRECT -- same text re-pushed %.1f s after it was destroyed "
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

// ---- the store.
//
// An owning type, not two bare deques: the invariant that matters -- RETIRE IS THE
// ONLY PATH OUT OF THE LIVE SET -- is not enforceable by a comment over a container
// whose clear()/erase()/pop_back() all still compile. Both live exits (overflow and
// expiry) route through the same transition, which is also why the 7th message no
// longer makes the oldest line vanish at full opacity: it is not destroyed any more,
// so its fade is the natural one.
class Store {
public:
    const std::deque<Entry>& live() const { return live_; }
    const std::deque<Entry>& retained() const { return retained_; }
    bool retainedDirty() const { return retainedDirty_; }
    void clearRetainedDirty() { retainedDirty_ = false; }

    void Birth(Entry&& e, const char* via) {
        ProbeOnPush(via, e, live_.size() + 1);
        live_.push_back(std::move(e));
        while (live_.size() > static_cast<size_t>(kMaxLines)) Retire(via, NowMs());
    }

    // Seed a row DIRECTLY into the retained tier, bypassing live -- the join seed. The
    // seed arrives oldest-first so in practice every one of them appends at the back.
    void Seed(Entry&& e) {
        InsertRetained(std::move(e));
        CapRetained();
    }

    // Expire everything past its (suspended) TTL. Ages are monotone across the deque
    // -- keys and birth stamps are both assigned in order -- so the front is oldest.
    void RetireExpired(uint64_t now) {
        while (!live_.empty() && EffectiveAgeMs(live_.front(), now) >= kTtlMs)
            Retire("expire", now);
    }

    void ClearAll() {
        live_.clear();
        retained_.clear();
        retainedDirty_ = true;
    }

private:
    // THE ONLY PATH OUT OF THE LIVE SET. A History line moves to the retained tier;
    // a Transient one is destroyed and noted for the resurrection probe.
    void Retire(const char* via, uint64_t now) {
        Entry& f = live_.front();
        const bool keep = (f.keep == Keep::History) && !NoRetain();
        UE_LOGI("feed: retire via=%s %s age=%.1fs text=\"%.40s\"", via,
                keep ? "-> history" : "(destroyed)",
                static_cast<double>(EffectiveAgeMs(f, now)) / 1000.0, f.text.c_str());
        if (keep) {
            InsertRetained(std::move(f));
        } else {
            NoteDestroyed(f.text, now);
        }
        live_.pop_front();
        CapRetained();
    }

    // THE ONE INSERTION DISCIPLINE for the retained tier (2026-07-29). Both entrances --
    // the join seed and live retirement -- come through here, because a deque built by
    // two different rules is not ordered by either.
    //
    // This used to be a sorted insert in Seed and a plain push_back in Retire. Under the
    // shipped design every retained row happened to be a wire row, so the two could not
    // disagree; they disagree the moment a LOCALLY-authored History row retires after a
    // seed has applied. AnnounceJoinerOnce is exactly that row: it fires at puppet spawn
    // and RACES the seed, so with g_wireBase still 0 it keys at (0<<32)|n while seeded
    // rows key at >= 1<<32, and a push_back would append it BEHIND all of them. That
    // makes chat_feed.h's documented "ascending by key" FALSE, and chat_view's pin
    // anchor search (`key >= g_anchorKey`) relies on it.
    void InsertRetained(Entry&& e) {
        const uint64_t k = e.key;
        auto at = retained_.end();
        while (at != retained_.begin() && (at - 1)->key > k) --at;
        retained_.insert(at, std::move(e));
        retainedDirty_ = true;
    }

    void CapRetained() {
        // Paging back through history freezes eviction, or the rows you are reading
        // vanish as new ones arrive. A hard ceiling at 2x still wins: an unbounded
        // store is not a scroll feature.
        const bool frozen = g_retentionFrozen.load(std::memory_order_relaxed);
        const size_t cap = static_cast<size_t>(frozen ? kMaxHeldLines : kMaxRetained);
        while (retained_.size() > cap) {
            if (frozen) {
                UE_LOGW("feed: retained ceiling hit while paged back (%zu > %zu) -- "
                        "evicting the oldest history line", retained_.size(), cap);
            }
            NoteDestroyed(retained_.front().text, NowMs());
            retained_.pop_front();
            retainedDirty_ = true;
        }
    }

    std::deque<Entry> live_;
    std::deque<Entry> retained_;
    bool              retainedDirty_ = true;
};

Store g_store;

// Published-state bookkeeping (guarded by g_mu, written on the game thread).
int  g_pubRetained = 0;      // rows currently occupying the snapshot's history prefix
bool g_pubRevealing = false;

// The previous publish's LIVE alphas, for the ALPHA-JUMP probe. Bounded by the live
// cap -- the probe never walks the history, whose store alpha is a constant 0 and
// therefore cannot jump.
struct PrevAlpha { uint64_t key; float alpha; };
PrevAlpha g_prevLive[kMaxLines];
int       g_prevLiveCount = 0;

void FillLine(Line& l, const Entry& e, float alpha) {
    std::snprintf(l.text, sizeof(l.text), "%s", e.text.c_str());
    l.alpha    = alpha;
    l.key      = e.key;
    l.nickArgb = e.nickArgb;
    l.nickLen  = e.nickLen;
    l.action   = e.action;
}

// Rebuild the published snapshot, then store it for the render thread. Game thread.
//
// The history prefix is rewritten only when it CHANGED or when the reveal opened or
// closed; the per-tick work is the <= 6 live rows it always was. A retained row's
// store alpha is a constant 0 -- there is nothing to recompute -- and publishing 100
// of them at 60 Hz would be ~28 KB of copying per tick for no new information.
void Republish(uint64_t now) {
    const bool reveal = RevealActiveNow();
    std::lock_guard<std::mutex> lk(g_mu);

    if (reveal != g_pubRevealing || g_store.retainedDirty()) {
        g_pubRetained = 0;
        if (reveal) {
            // Publish the WHOLE held tier. This used to stop at kMaxRetained while the
            // store was allowed to hold kMaxRetained * kRetentionFreezeFactor, and the
            // walk starts at the FRONT (oldest) -- so a reader paged back past the base
            // ceiling had the rows that arrived DURING the freeze, the newest ones,
            // silently outside the published window. The array is now sized to the same
            // derived ceiling, so the bound below can never truncate; it is a guard, not
            // a policy, and the live rows below always fit because
            // kMaxSnapshotLines == kMaxLines + kMaxHeldLines.
            for (const Entry& e : g_store.retained()) {
                if (g_pubRetained >= kMaxHeldLines) break;
                FillLine(g_pub.lines[g_pubRetained++], e, 0.f);
            }
        }
        g_pubRevealing = reveal;
        g_store.clearRetainedDirty();
    }

    int n = g_pubRetained;
    PrevAlpha nowLive[kMaxLines];
    int nowLiveCount = 0;
    for (const Entry& e : g_store.live()) {
        if (n >= kMaxSnapshotLines) break;
        const uint64_t age = EffectiveAgeMs(e, now);
        const float a = FadeAlpha(age);
        FillLine(g_pub.lines[n], e, a);
        // The SAME entry rising in alpha while it sits in its fade-out TAIL is
        // impossible by this store's math (the only legitimate rise is the arrival
        // ramp, excluded by the tail gate). If it ever logs, the mechanism is inside
        // this file after all -- and the log carries the numbers to prove where.
        if (age > kTtlMs - kFadeMs) {
            for (int p = 0; p < g_prevLiveCount; ++p) {
                if (g_prevLive[p].key == e.key && g_prevLive[p].alpha < 0.5f &&
                    a > g_prevLive[p].alpha + 0.25f) {
                    UE_LOGW("feed: ALPHA-JUMP -- \"%.40s\" (key=%llu) published alpha %.2f -> %.2f",
                            g_pub.lines[n].text, static_cast<unsigned long long>(e.key),
                            g_prevLive[p].alpha, a);
                }
            }
        }
        if (nowLiveCount < kMaxLines) nowLive[nowLiveCount++] = {e.key, a};
        ++n;
    }

    std::memcpy(g_prevLive, nowLive, sizeof(PrevAlpha) * static_cast<size_t>(nowLiveCount));
    g_prevLiveCount = nowLiveCount;

    g_pub.count     = n;
    g_pub.liveCount = n - g_pubRetained;
    ++g_pub.gen;
    g_count.store(g_pub.liveCount, std::memory_order_relaxed);
}

}  // namespace

// UTF-8-encode a wide string. RULE 2, 2026-07-28: the body is GONE -- this is a
// two-line forward to coop::text::ToUtf8, the one owner. The hand-rolled copy
// that lived here had quietly DIVERGED from it: it emitted a 3-byte CESU-8
// sequence for an unpaired surrogate where the codec drops it, so ill-formed
// UTF-8 could reach the chat wire. The kept name is the only thing worth
// keeping (chat_feed.h exports it and peer_action_feed calls it).
std::string ToUtf8(const std::wstring& w) { return coop::text::ToUtf8(w); }

void Push(const std::wstring& line, Keep keep) {
    const uint64_t now = NowMs();
    AdvanceSuspension(now);
    Entry e;
    SetText(e, ToUtf8(line));
    e.key = NextKey();
    e.bornMs = now;
    e.bornSuspendedMs = g_suspendedMs;
    e.keep = keep;
    g_store.Birth(std::move(e), "event");
    Republish(now);
}

void PushWireChat(const std::string& utf8Line, uint8_t nickByteLen, uint32_t nickArgb,
                  uint32_t lineSeq, bool seeded) {
    const uint64_t now = NowMs();
    AdvanceSuspension(now);
    Entry e;
    SetText(e, utf8Line);
    e.key = WireKey(lineSeq);
    e.bornMs = now;
    e.bornSuspendedMs = g_suspendedMs;
    e.nickLen  = (nickByteLen <= e.text.size()) ? nickByteLen : 0;
    e.nickArgb = nickArgb;
    e.keep     = Keep::History;
    // The base advances for BOTH tiers: a seeded row is still the newest thing this
    // peer knows was said, so a local notice pushed after the seed must sort after it.
    if (lineSeq > g_wireBase) g_wireBase = lineSeq;
    if (seeded) {
        g_store.Seed(std::move(e));
    } else {
        g_store.Birth(std::move(e), "chat");
    }
    Republish(now);
}

void PushAction(const std::string& utf8Line, uint8_t nickByteLen, uint32_t nickArgb) {
    const uint64_t now = NowMs();
    AdvanceSuspension(now);
    Entry e;
    SetText(e, utf8Line);
    e.key = NextKey();
    e.bornMs = now;
    e.bornSuspendedMs = g_suspendedMs;
    e.nickLen  = (nickByteLen <= e.text.size()) ? nickByteLen : 0;
    e.nickArgb = nickArgb;
    e.action   = 1;
    e.keep     = Keep::History;
    g_store.Birth(std::move(e), "action");
    Republish(now);
}

void PushDelayed(const std::wstring& line, uint64_t delayMs, Keep keep) {
    Pending p;
    p.text  = ToUtf8(line);
    p.dueMs = NowMs() + delayMs;
    p.keep  = keep;
    g_pending.push_back(std::move(p));
}

void Tick() {
    const uint64_t now = NowMs();
    AdvanceSuspension(now);

    // Promote any delayed lines whose time has come (born NOW, so their TTL and fade
    // start here -- and so their key sorts after everything already published).
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now >= it->dueMs) {
            Entry e;
            SetText(e, it->text);
            e.key = NextKey();
            e.bornMs = now;
            e.bornSuspendedMs = g_suspendedMs;
            e.keep = it->keep;
            g_store.Birth(std::move(e), "delayed");
            it = g_pending.erase(it);
        } else {
            ++it;
        }
    }

    g_store.RetireExpired(now);

    // Cheap idle path: nothing live, no history to (un)publish, reveal state
    // unchanged -> the published snapshot is already correct.
    if (g_store.live().empty() && RevealActiveNow() == g_pubRevealing &&
        !g_store.retainedDirty())
        return;
    Republish(now);
}

bool GetSnapshotIfNewer(Snapshot& out, uint32_t& gen) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pub.gen == gen) return false;
    gen           = g_pub.gen;
    out.count     = g_pub.count;
    out.liveCount = g_pub.liveCount;
    out.gen       = g_pub.gen;
    if (g_pub.count > 0)
        std::memcpy(out.lines, g_pub.lines, sizeof(Line) * static_cast<size_t>(g_pub.count));
    return true;
}

bool HasAny() {
    return g_count.load(std::memory_order_relaxed) > 0;
}

void SetChatOpen(bool open) {
    const bool was = g_chatOpen.exchange(open, std::memory_order_relaxed);
    if (was && !open) g_closeAtMs.store(NowMs(), std::memory_order_relaxed);
    if (!was && open) g_closeAtMs.store(0, std::memory_order_relaxed);
}

bool RevealActive() { return RevealActiveNow(); }

void SetRetentionFrozen(bool frozen) {
    g_retentionFrozen.store(frozen, std::memory_order_relaxed);
}

void Reset() {
    g_store.ClearAll();
    g_pending.clear();
    g_suspendedMs = 0;
    g_lastAdvanceMs = 0;
    g_revealLatched = false;
    g_tiebreak = 0;
    g_wireBase = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pub.count = 0;
        g_pub.liveCount = 0;
        ++g_pub.gen;
        g_pubRetained = 0;
        g_pubRevealing = false;
        g_prevLiveCount = 0;
    }
    g_count.store(0, std::memory_order_relaxed);
}

}  // namespace coop::chat_feed

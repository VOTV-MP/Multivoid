// coop/comms/chat_rate.cpp -- see coop/comms/chat_rate.h.

#include "coop/comms/chat_rate.h"

#include "coop/player/roster_ledger.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cmath>

namespace coop::chat_rate {
namespace {

// Source's convar defaults (player.cpp:98-101), constants here: a server-tunable limit is the server settings' to add.
constexpr float    kTier1Bucket        = 4.0f;
constexpr float    kTier1SecondsPerMsg = 3.0f;
constexpr float    kTier2Bucket        = 30.0f;
constexpr float    kTier2SecondsPerMsg = 10.0f;
constexpr uint64_t kTalkIntervalMs     = 660;  // CanPlayerTalk's talk_interval, 0.66 s (player.cpp:8811)

coop::roster_ledger::PerSlotState<TalkBudget> g_budget;

}  // namespace

TalkVerdict Attempt(TalkBudget& b, uint64_t nowMs) {
    // CanPlayerTalk (player.cpp:8809-8818): the interval, then the bucket, both on every attempt.
    const bool intervalOk = b.lastTalkMs == 0 || b.lastTalkMs + kTalkIntervalMs < nowMs;

    // ArePlayerTalkMessagesAvailable (player.cpp:8771-8804), in its order and in float. Two divergences: the clock is
    // the host's steady milliseconds, not the map's float curtime, so the elapsed time is the integer difference turned
    // into seconds and the floats stay small however long the machine has run; and 0 stands for never (Source starts
    // both times at 0, :613-614), which on a clock counted from the machine's boot gives a slot's first attempt full
    // tiers, as Source gives a first say once its map has run 200 s; a younger map's first say meets less of tier 2.
    const float elapsed = static_cast<float>(nowMs > b.lastAttemptMs ? nowMs - b.lastAttemptMs : 0) / 1000.0f;
    b.lastAttemptMs = nowMs;
    // Tier 1, the short term: every attempt spends one, the counter may fall to -2.5, and a debt slows the refill.
    const float secondsPerMsg1 = kTier1SecondsPerMsg - std::min(0.0f, b.tier1);
    const float gained1 = std::max(0.0f, elapsed / secondsPerMsg1);
    b.tier1 = std::max(-2.5f, std::min(kTier1Bucket, b.tier1 + gained1) - 1.0f);
    // Tier 2, the long term: gained and spent only while tier 1 is above 0.
    if (b.tier1 > 0.0f) {
        const float gained2 = std::max(0.0f, elapsed / kTier2SecondsPerMsg);
        b.tier2 = std::max(0.0f, std::min(kTier2Bucket, b.tier2 + gained2) - 1.0f);
    }
    const bool bucketOk = b.tier1 > 1.0f && b.tier2 > 1.0f;

    if (!intervalOk) return TalkVerdict::Interval;
    if (!bucketOk) return TalkVerdict::Bucket;
    b.lastTalkMs = nowMs;  // NotePlayerTalked, after Host_Say (client.cpp:850-851)
    return TalkVerdict::Talk;
}

TalkVerdict AttemptLine(uint8_t slot, uint64_t nowMs) { return Attempt(g_budget[slot], nowMs); }

bool RunChatRateSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("chat_rate selftest FAIL: %s", what);
    };
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    using V = TalkVerdict;
    // Far from zero: a first attempt needs 200 s of clock for tier 2's full refill.
    constexpr uint64_t t0 = 1000000000ull;

    {
        TalkBudget b{};
        check(b.lastAttemptMs == 0 && b.lastTalkMs == 0 && b.tier1 == 1.0f && b.tier2 == 10.0f,
              "a value-initialised budget is Source's fresh player");
        check(Attempt(b, t0) == V::Talk && near(b.tier1, 3.0f) && near(b.tier2, 29.0f),
              "a fresh budget's first line talks, leaving tier 1 at 3 and tier 2 at 29");
    }
    TalkBudget afterTen;
    {
        TalkBudget b;
        const V first = Attempt(b, t0);
        const V second = Attempt(b, t0);
        for (int i = 2; i < 10; ++i) Attempt(b, t0);
        check(first == V::Talk && second == V::Interval && near(b.tier1, -2.5f) && near(b.tier2, 27.0f),
              "ten lines at one instant: the first talks, the second meets the interval, tier 1 -2.5 and tier 2 27");
        afterTen = b;
    }
    {
        TalkBudget b = afterTen;
        check(Attempt(b, t0 + 24000) == V::Bucket && near(b.tier1, 0.8636f),
              "after the ten, a line 24 s later is refused by the slowed refill");
        b = afterTen;
        check(Attempt(b, t0 + 25000) == V::Talk, "after the ten, a line 25 s later talks");
    }
    {
        TalkBudget b;
        Attempt(b, t0);
        const V at660 = Attempt(b, t0 + 660);
        check(at660 == V::Interval && Attempt(b, t0 + 670) == V::Talk,
              "a line 0.66 s after a talk meets the interval, one 0.67 s after talks");
    }
    {
        TalkBudget b;
        const bool ok = Attempt(b, t0) == V::Talk && Attempt(b, t0 + 900) == V::Talk &&
                        Attempt(b, t0 + 1800) == V::Talk && Attempt(b, t0 + 2700) == V::Bucket;
        check(ok, "four lines 0.9 s apart: three talk, the fourth is refused by tier 1");
    }
    {
        TalkBudget b;
        int talked = 0;
        V v = V::Talk;
        for (uint64_t t = t0; talked < 100 && (v = Attempt(b, t)) == V::Talk; t += 3100) ++talked;
        check(talked == 41 && v == V::Bucket && b.tier1 > 1.0f,
              "lines 3.1 s apart: 41 talk, then tier 2 refuses with tier 1 above 1");
    }
    {
        TalkBudget b;
        for (uint64_t i = 0; i < 100; ++i) Attempt(b, t0 + i * 3100);
        const bool paced = near(b.tier1, 3.0f) && near(b.tier2, 0.0f);
        for (int i = 0; i < 20; ++i) Attempt(b, t0 + 100 * 3100ull);
        check(paced && near(b.tier1, -2.5f) && near(b.tier2, 0.0f),
              "a paced run floors tier 2 at 0, then a burst floors tier 1 at -2.5");
    }

    if (pass == total) {
        UE_LOGI("chat_rate selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("chat_rate selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::chat_rate

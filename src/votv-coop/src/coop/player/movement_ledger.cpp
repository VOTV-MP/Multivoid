// coop/player/movement_ledger.cpp -- the arithmetic behind coop/player/movement_ledger.h. A peer
// earns the right to have moved by the passage of real time, at no more than the game's own top
// speed. Two caps do two jobs: a bank of real time (kSkewBankMaxMs) lets the sender's clock
// supply the shape of an interval, so a network clump costs nothing; a cap on the carry, the
// credit left standing from earlier samples (kUnearnedJumpCm), bounds what an unexplained jump
// can spend. Neither loosens the sustained rate, since credit is only created by elapsed time.
// The cap sits on the carry, not on carry plus earnings: this interval's own earnings are
// spendable in full, or no sample could ever cover more than 50 cm and the ATV at speed_turbo
// (53 cm between two 60 Hz poses) would read untrusted. One accounting path: an unstamped pose
// earns no time and is charged its step like any other, so it cannot move the anchor for free.

#include "coop/player/movement_ledger.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace coop::movement_ledger {

namespace E = ue_wrap::engine;

namespace {

constexpr int      kSlots           = static_cast<int>(coop::players::kMaxPeers);
constexpr float    kMaxDebtCm       = kMaxTravelSpeedCmS * kMaxDebtSeconds;
// The summary period: frequent enough that a short act still produces a line, rare enough not
// to cost frames.
constexpr uint64_t kSummaryPeriodMs = 10000;

// Monotonic milliseconds. The row keeps a count rather than a time_point so the selftest can
// supply every input to the arithmetic, the receiver's clock included.
uint64_t NowMs() {
    static const std::chrono::steady_clock::time_point kEpoch = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - kEpoch).count());
}

struct Row {
    // The occupancy generation the row was anchored under; a change means the slot's occupant was
    // replaced and everything below belongs to somebody else.
    uint32_t gen = 0;
    bool     armed = false;
    uint64_t anchorAtMs = 0;      // when THIS occupant's record began; the summary prints its age

    // Position accounting.
    ue_wrap::FVector lastPos{};
    float    creditCm = 0.f;      // ALWAYS within [-kMaxDebtCm, kUnearnedJumpCm]; see the cap note
    bool     trusted = false;

    // Clocks: lastStateMs is the origin's 24-bit stamp, lastRecvMs is ours.
    uint32_t lastStateMs = 0;
    uint64_t lastRecvMs = 0;
    uint32_t skewBankMs = 0;      // real time earned and not yet spent as claimed elapsed

    // Diagnosis latches. Every condition below is authored by the peer being judged, and the logger
    // flushes each non-info line synchronously on the net thread, so a 60 Hz stream must never
    // turn a condition into a flood. discReported is per summary window: a peer alternating a hop
    // with a standing sample would otherwise earn a trusted-to-untrusted edge every other pose.
    // The window's full count still rides the summary's disc field.
    bool     unstampedReported = false;
    bool     nonFiniteReported = false;
    bool     discReported = false;

    // Summary accumulators, reset when the summary prints and on every re-anchor; per occupant,
    // not per slot. The count is load-bearing: without it a row that saw no poses and one that saw
    // poses that did not move print the same line.
    uint64_t samples = 0;
    uint64_t discontinuities = 0;
    float    maxStepCm = 0.f;
    uint32_t minDtMs = UINT32_MAX;
    uint32_t maxDtMs = 0;         // the largest REAL receive gap -- the field number that decides
                                  // whether a per-packet earn cap is affordable (see the header)
    uint32_t maxUseMs = 0;        // the largest interval any ONE packet was credited for
    uint32_t maxBankMs = 0;       // peak unspent banked real time
    float    minCreditCm = 0.f;   // the residual-slack MINIMUM -- the number that says if the
                                  // un-earned-jump cap is too small to survive real jitter
    float    maxImpliedSpeed = 0.f;
    float    lastWireVsActorCm = -1.f;  // game-thread sample; -1 = never taken
};

std::mutex g_mu;
Row        g_rows[kSlots];
// Two threads touch this: Tick reads and re-arms it on the game thread, and OnSessionStart
// re-bases it on the harness bring-up thread; the two overlap on a stop/start cycle. Relaxed
// is enough: it guards no other data, and the read-modify-write is game-thread-only.
std::atomic<uint64_t> g_nextSummaryMs{0};

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Start or restart a row at `pos` under `gen`, granting nothing: arriving is not evidence of
// having travelled. A first pose has no predecessor and is admitted.
void AnchorRow(Row& r, uint32_t gen, const ue_wrap::FVector& pos,
               uint32_t stateMs, uint64_t nowMs) {
    r.gen = gen;
    r.armed = true;
    r.anchorAtMs = nowMs;
    r.lastPos = pos;
    r.creditCm = 0.f;
    r.trusted = true;      // principle 8: a first pose has no predecessor and must be admitted
    r.lastStateMs = stateMs;
    r.lastRecvMs = nowMs;
    r.skewBankMs = 0;
    r.unstampedReported = false;
    r.nonFiniteReported = false;
    r.discReported = false;
    // The accumulators are per occupant. Slots recycle lowest-free, so a slot goes from one person
    // to the next with no absence between, and leaving these standing would report the old
    // occupant's worst step under the new occupant's slot number.
    r.samples = 1;
    r.discontinuities = 0;
    r.maxStepCm = 0.f;
    r.minDtMs = UINT32_MAX;
    r.maxDtMs = 0;
    r.maxUseMs = 0;
    r.maxBankMs = 0;
    r.minCreditCm = 0.f;
    r.maxImpliedSpeed = 0.f;
    r.lastWireVsActorCm = -1.f;
}

// What one accepted pose did to a row, returned rather than logged in place so the logging
// happens outside the lock and the selftest asserts on the arithmetic, not on log text.
struct Outcome {
    bool     anchored = false;        // first pose on this row, or a replaced occupant
    bool     unstampedEdge = false;   // FIRST unstamped pose on this row (latched thereafter)
    bool     nonFiniteEdge = false;   // FIRST non-finite pose on this row (latched thereafter)
    bool     discontinuity = false;   // the trusted -> untrusted edge, latched per summary window
    bool     trusted = false;
    float    stepCm = 0.f;
    float    creditCm = 0.f;
    uint32_t dtSenderMs = 0;
    uint32_t dtRecvMs = 0;
    uint32_t useMs = 0;
};

// The arithmetic, with every input explicit: the row, the occupancy generation, the sender's
// stamp and the receiver's clock. OnClientPose is this plus the session reads and the logging;
// the selftest is this over its own row, so it drives the shipped function and cannot touch
// production state. The caller owns the lock.
Outcome ApplyPose(Row& r, uint32_t gen, const ue_wrap::FVector& pos,
                  uint32_t stateTimeMs24, uint64_t nowMs) {
    Outcome o;

    // Defended, not assumed: ValidatePose rejects non-finite components upstream, but a NaN
    // reaching the arithmetic would be silent and permanent (every comparison is false, so creditCm
    // stores NaN for the rest of the session). A named refusal instead.
    if (!std::isfinite(pos.X) || !std::isfinite(pos.Y) || !std::isfinite(pos.Z)) {
        r.trusted = false;
        if (!r.nonFiniteReported) { r.nonFiniteReported = true; o.nonFiniteEdge = true; }
        o.creditCm = r.creditCm;
        return o;
    }

    if (!r.armed || r.gen != gen) {
        AnchorRow(r, gen, pos, stateTimeMs24, nowMs);
        o.anchored = true;
        o.trusted = true;
        return o;
    }

    // Unstamped means no time information, so no time is earned, and the pose then goes down the
    // same path as every other; an early return here would move the anchor for free.
    const bool stamped = (stateTimeMs24 != 0);
    if (!stamped && !r.unstampedReported) { r.unstampedReported = true; o.unstampedEdge = true; }

    const uint32_t dtR = static_cast<uint32_t>(nowMs - r.lastRecvMs);

    uint32_t useMs = 0;
    if (stamped) {
        if (dtR >= kClockRebaseGapMs) {
            // A gap long enough to make the 24-bit stamp ambiguous. Only the clock re-bases: the
            // anchor, the credit and the path survive, so silence earns nothing.
            r.skewBankMs = 0;
        } else {
            // The sender's stamp supplies the shape of the interval; real time supplies the
            // ceiling. An inflated or rewound stamp claims at most what the bank holds.
            const uint32_t dtS = coop::net::ElapsedMs24(r.lastStateMs, stateTimeMs24);
            uint64_t bank = static_cast<uint64_t>(r.skewBankMs) + dtR;
            if (bank > kSkewBankMaxMs) bank = kSkewBankMaxMs;
            useMs = (dtS < bank) ? dtS : static_cast<uint32_t>(bank);
            r.skewBankMs = static_cast<uint32_t>(bank - useMs);
            o.dtSenderMs = dtS;
        }
    }
    o.dtRecvMs = dtR;
    o.useMs = useMs;

    const float step = Dist3(r.lastPos, pos);

    // The carry is what was not earned in this interval, so the carry is what the jump cap bounds;
    // this interval's own earnings are spendable in full. Every store below keeps creditCm inside
    // the cap, so this clamp is the invariant written down once.
    float carry = r.creditCm;
    if (carry > kUnearnedJumpCm) carry = kUnearnedJumpCm;
    const float earned = kMaxTravelSpeedCmS * (static_cast<float>(useMs) / 1000.f);

    float credit = carry + earned - step;
    if (credit > kUnearnedJumpCm) credit = kUnearnedJumpCm;   // the standing budget, bounded
    if (credit < -kMaxDebtCm)     credit = -kMaxDebtCm;       // the debt, bounded

    const bool wasTrusted = r.trusted;
    r.creditCm = credit;
    r.trusted = stamped && (credit >= 0.f);
    r.lastPos = pos;
    if (stamped) r.lastStateMs = stateTimeMs24;               // an unstamped packet is not a clock
    r.lastRecvMs = nowMs;

    ++r.samples;
    if (step > r.maxStepCm) r.maxStepCm = step;
    if (dtR < r.minDtMs) r.minDtMs = dtR;
    if (dtR > r.maxDtMs) r.maxDtMs = dtR;
    if (useMs > r.maxUseMs) r.maxUseMs = useMs;
    if (r.skewBankMs > r.maxBankMs) r.maxBankMs = r.skewBankMs;
    if (credit < r.minCreditCm) r.minCreditCm = credit;
    if (useMs > 0) {
        const float implied = step / (static_cast<float>(useMs) / 1000.f);
        if (implied > r.maxImpliedSpeed) r.maxImpliedSpeed = implied;
    }
    if (wasTrusted && !r.trusted) {
        ++r.discontinuities;
        if (!r.discReported) { r.discReported = true; o.discontinuity = true; }
    }

    o.trusted = r.trusted;
    o.stepCm = step;
    o.creditCm = credit;
    return o;
}

// The selftest: every session start, unconditionally, in microseconds, over its own row. Not
// env-gated, because a wrong verdict here is silent, and the branches below (an unstamped
// sender, a recycled slot, a 24-bit wrap, an inflated clock, a drained receive queue, the debt
// floor) are unreachable from a two-peer LAN smoke. It drives the production function, not a
// copy of its arithmetic.

// Drive `n` samples of steady motion at `speedCmS` with `dtMs` between poses from a fresh
// anchor, both clocks advancing together: the honest case, every row of which must stay
// trusted.
Outcome SimSteady(Row& r, float speedCmS, uint32_t dtMs, int n) {
    uint64_t now = 100000;                 // arbitrary; only differences matter
    uint32_t st = 12345;
    ue_wrap::FVector p{0.f, 0.f, 0.f};
    Outcome o = ApplyPose(r, 1, p, st, now);
    for (int i = 0; i < n; ++i) {
        now += dtMs;
        st = (st + dtMs) & 0x00FFFFFFu;
        if (st == 0) st = 1;
        p.X += speedCmS * (static_cast<float>(dtMs) / 1000.f);
        o = ApplyPose(r, 1, p, st, now);
    }
    return o;
}

bool RunSelfTest() {
    bool pass = true;
    int  checks = 0;
    Row  r;                                // OUR row -- production state is never touched

    auto check = [&](const char* name, bool ok, const char* detail) {
        ++checks;
        if (!ok) { pass = false; UE_LOGE("movement_ledger selftest: FAIL %s -- %s", name, detail); }
    };
    auto fresh = [&r] { r = Row{}; };

    // 1. The first pose of a peer never seen before is admitted: a joining peer has no predecessor.
    fresh();
    {
        const Outcome o = ApplyPose(r, 7, ue_wrap::FVector{1000.f, 2000.f, 300.f}, 500, 1000);
        check("anchor-admits-first-pose", o.anchored && o.trusted, "a joining peer was not admitted");
    }

    // 2. A slot recycled to a new occupant re-anchors and is admitted however far the position
    //    moved: the previous occupant's anchor is not this peer's history.
    {
        const Outcome o = ApplyPose(r, 8, ue_wrap::FVector{-400000.f, 400000.f, 0.f}, 600, 1016);
        check("recycle-reanchors", o.anchored && o.trusted,
              "a replaced occupant was judged against the previous occupant's anchor");
    }

    // 3. An unstamped pose is refused trust, and the report latches.
    {
        const Outcome a = ApplyPose(r, 8, ue_wrap::FVector{-400000.f, 400000.f, 0.f}, 0, 1032);
        const Outcome b = ApplyPose(r, 8, ue_wrap::FVector{-400000.f, 400000.f, 0.f}, 0, 1048);
        check("unstamped-untrusted", !a.trusted && !b.trusted, "an unstamped pose was believed");
        check("unstamped-latched", a.unstampedEdge && !b.unstampedEdge,
              "the unstamped warning is not latched -- it would print once per packet");
    }

    // 4. The two-packet relocation: an unstamped hop must cost its full distance in debt, and a
    //    stamped pose at the same spot must not clear it. Asking only whether the unstamped
    //    packets are untrusted passes on a broken module.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17;
        ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, 0, now);          // unstamped 220 m hop
        now += 17; st += 34;
        const Outcome after = ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);
        check("unstamped-hop-costs-debt", !after.trusted && after.creditCm < -10000.f,
              "an unstamped pose relocated the anchor for free -- two packets teleport anywhere");
    }

    // 5. Ordinary motion stays trusted at the game's own speeds: walk, sprint, the ATV's turbo, the
    //    noclip diagonal.
    fresh(); check("walk-400-at-60hz", SimSteady(r, 400.f, 17, 40).trusted,
                   "walking at defSpeed was refused");
    fresh(); check("sprint-1000-at-60hz", SimSteady(r, 1000.f, 17, 40).trusted,
                   "sprinting at the updateSpeed ceiling was refused");
    fresh(); check("atv-turbo-3200-at-60hz", SimSteady(r, 3200.f, 17, 40).trusted,
                   "the game's own ATV at speed_turbo was refused");
    fresh(); check("noclip-diag-8660-at-60hz", SimSteady(r, 8660.f, 17, 40).trusted,
                   "the game's own noclip diagonal was refused");
    // The same speeds at a sparse packet rate, which is what a lossy link looks like.
    fresh(); check("sprint-1000-at-10hz", SimSteady(r, 1000.f, 100, 20).trusted,
                   "sprinting across a 100 ms packet gap was refused");
    fresh(); check("atv-turbo-3200-at-10hz", SimSteady(r, 3200.f, 100, 20).trusted,
                   "the ATV across a 100 ms packet gap was refused");

    // 6. Sustained over-bound motion is caught, and caught quickly.
    fresh(); check("over-bound-15000-caught", !SimSteady(r, 15000.f, 17, 10).trusted,
                   "sustained motion above the speed bound was believed");
    fresh(); check("over-bound-11000-caught", !SimSteady(r, 11000.f, 17, 40).trusted,
                   "a ten-percent overspeed was believed indefinitely");

    // 7. The subdivided jump: hops small enough to pass for jitter, repeated. 11 x 1999 cm is 220 m
    //    in under 0.2 s.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ue_wrap::FVector p{0.f, 0.f, 0.f};
        ApplyPose(r, 1, p, st, now);
        bool everUntrusted = false;
        for (int i = 0; i < 11; ++i) {
            now += 17; st += 17; p.X += 1999.f;
            if (!ApplyPose(r, 1, p, st, now).trusted) everUntrusted = true;
        }
        check("subdivided-jump-caught", everUntrusted,
              "220 m in eleven sub-jitter hops passed as ordinary motion");
    }

    // 8. A single long teleport is untrusted, and its debt clears at the speed bound: not sooner
    //    (it would be free) and not later (an honest teleport punished forever).
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        const Outcome jump = ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);  // 220 m
        check("teleport-untrusted", !jump.trusted, "a 220 m single-packet jump was believed");
        // Standing still: 220 m of debt at 10 000 cm/s is 2.2 s, so trust must not return inside
        // 1.7 s and must return inside 3.4 s.
        bool recoveredEarly = false, recovered = false;
        for (int i = 0; i < 200; ++i) {                        // 200 x 17 ms = 3.4 s
            now += 17; st += 17;
            if (ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now).trusted) {
                if (i < 100) recoveredEarly = true;
                recovered = true;
                break;
            }
        }
        check("teleport-debt-not-free", !recoveredEarly, "the teleport debt cleared too cheaply");
        check("teleport-debt-clears", recovered, "an honest teleport would never regain trust");
    }

    // 9. Silence past the rebase gap earns nothing: the clock re-bases, the anchor and the credit
    //    survive.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 30000; st = (st + 30000) & 0x00FFFFFFu;         // 30 s, past kClockRebaseGapMs
        const Outcome o = ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);
        check("long-silence-earns-nothing", !o.trusted,
              "a peer bought a 220 m jump by muting its own pose stream past the rebase gap");
    }

    // 10. Silence under the rebase gap earns banked time, and this row pins the number: 7 s of
    //     silence banks kSkewBankMaxMs, 5 s, which at the bound is 500 m spendable in one packet.
    //     Inside the sustained rate (7 s legitimately buys 700 m) but invisible, since trust never
    //     falls and no discontinuity line is logged. Whether a per-packet earn cap is needed is a
    //     field question the summary's dt/use/bank maxima answer. If this row flips, the model
    //     changed.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 7000; st = (st + 7000) & 0x00FFFFFFu;           // 7 s, UNDER kClockRebaseGapMs
        const Outcome o = ApplyPose(r, 1, ue_wrap::FVector{50000.f, 0.f, 0.f}, st, now);   // 500 m
        check("sub-rebase-silence-banks-time", o.trusted && o.useMs == kSkewBankMaxMs,
              "the banked-time model changed -- re-read the header before trusting this build");
    }
    // One metre past what the bank holds is still refused, so the bound is a bound.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 7000; st = (st + 7000) & 0x00FFFFFFu;
        const Outcome over = ApplyPose(r, 1, ue_wrap::FVector{50100.f, 0.f, 0.f}, st, now);
        check("sub-rebase-silence-is-still-bounded", !over.trusted,
              "banked time was not capped at kSkewBankMaxMs -- silence buys unbounded distance");
    }

    // 11. A rewound or inflated sender clock buys nothing: the bank holds only real time.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17;
        st = (st + 5000000u) & 0x00FFFFFFu;                    // claim 5 000 s of elapsed time
        const Outcome o = ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);
        check("inflated-clock-buys-nothing", !o.trusted,
              "a peer minted credit by inflating its own clock");
    }

    // 12. The 24-bit stamp wraps every 4 h 39 min; the arithmetic must cross it without inventing
    //     time.
    fresh();
    {
        uint64_t now = 5000;
        uint32_t st = coop::net::kStateTimeMs24Period - 5;     // 5 ms before the wrap
        ue_wrap::FVector p{0.f, 0.f, 0.f};
        ApplyPose(r, 1, p, st, now);
        now += 17; st = 12;                                    // 17 ms later, wrapped past zero
        p.X += 1000.f * 0.017f;
        const Outcome o = ApplyPose(r, 1, p, st, now);
        check("wrap-crossed-cleanly", o.trusted && o.dtSenderMs == 17,
              "the 24-bit wrap read as a rewind or as 16 777 216 ms of elapsed time");
    }

    // 13. A network clump costs nothing: the receiver stalls 200 ms, then drains twelve poses whose
    //     arrival intervals are zero but whose production intervals were 17 ms each. The bank
    //     exists for this.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ue_wrap::FVector p{0.f, 0.f, 0.f};
        ApplyPose(r, 1, p, st, now);
        now += 200;                                            // the stall lands on packet 1
        bool allTrusted = true;
        for (int i = 0; i < 12; ++i) {
            st += 17;                                          // the real production interval
            p.X += 1000.f * 0.017f;                            // sprinting throughout
            if (!ApplyPose(r, 1, p, st, now).trusted) allTrusted = false;   // arrival dt = 0
        }
        check("clump-costs-nothing", allTrusted,
              "a drained receive queue read as motion -- the bank is not absorbing it");
    }

    // 14. The debt floor holds, so an absurd jump cannot buy exile beyond the stated policy.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        const Outcome o = ApplyPose(r, 1, ue_wrap::FVector{900000.f, 0.f, 0.f}, st, now);   // 9 km
        check("debt-floored", o.creditCm >= -(kMaxDebtCm + 1.f),
              "the debt floor did not hold -- recovery time is unbounded");
    }

    // 15. A recycled slot starts its measurements empty: the accumulators are per occupant.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        ApplyPose(r, 1, ue_wrap::FVector{50000.f, 0.f, 0.f}, st, now);       // 500 m: a big maxStep
        const bool dirty = (r.maxStepCm > 0.f && r.discontinuities > 0);
        now += 17; st += 17;
        ApplyPose(r, 2, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);           // NEW occupancy gen
        check("recycle-clears-accumulators",
              dirty && r.maxStepCm == 0.f && r.discontinuities == 0 &&
              r.maxImpliedSpeed == 0.f && r.lastWireVsActorCm < 0.f,
              "a recycled slot inherited the previous occupant's measurements");
    }

    // 16. The discontinuity detail line is latched per summary window while the count is not.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        const Outcome e1 = ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);
        for (int i = 0; i < 300 && !r.trusted; ++i) {          // stand still until trust returns
            now += 17; st += 17;
            ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);
        }
        now += 17; st += 17;
        const Outcome e2 = ApplyPose(r, 1, ue_wrap::FVector{44000.f, 0.f, 0.f}, st, now);
        check("discontinuity-detail-latched", e1.discontinuity && !e2.discontinuity,
              "the per-edge WARN is not latched -- a peer can author its rate on the net thread");
        check("discontinuity-count-not-latched", r.discontinuities == 2,
              "the discontinuity COUNT was latched too -- the summary would under-report");
    }

    // 17. A non-finite position is refused and cannot poison the row. ValidatePose makes it
    //     unreachable today; if it ever becomes reachable, the failure is a named refusal, not a
    //     row that reads untrusted forever.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        const float nan = std::nanf("");
        const Outcome bad = ApplyPose(r, 1, ue_wrap::FVector{nan, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        const Outcome good = ApplyPose(r, 1, ue_wrap::FVector{100.f, 0.f, 0.f}, st, now);
        check("non-finite-refused", !bad.trusted && bad.nonFiniteEdge,
              "a non-finite position was accepted");
        check("non-finite-does-not-poison", good.trusted && std::isfinite(good.creditCm),
              "a non-finite position poisoned the row permanently");
    }

    if (pass) {
        UE_LOGI("movement_ledger selftest: ALL PASS (%d checks)", checks);
    } else {
        UE_LOGE("movement_ledger selftest: FAILURES (%d checks) -- every verdict this module logs "
                "below is UNTRUSTWORTHY and the calibration transcript must not be read", checks);
    }
    return pass;
}

}  // namespace

void OnSessionStart() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (Row& r : g_rows) r = Row{};
    }
    g_nextSummaryMs.store(NowMs() + kSummaryPeriodMs, std::memory_order_relaxed);
    // Un-gated on purpose, over its own row.
    RunSelfTest();
}

void OnClientPose(coop::net::Session& session, int slot,
                  const ue_wrap::FVector& pos, uint32_t stateTimeMs24) {
    if (slot <= 0 || slot >= kSlots) return;   // slot 0 is the host; it never validates itself
    if (session.role() != coop::net::Role::Host) return;   // client-scoped: a client validates none

    // Read on this thread (the acquire load is documented any-thread), so a recycled slot's first
    // pose is never judged against the previous occupant's anchor.
    const uint32_t gen = session.peerGenerationForSlot(slot);
    const uint64_t now = NowMs();

    Outcome o;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        o = ApplyPose(g_rows[slot], gen, pos, stateTimeMs24, now);
    }

    // Logging outside the lock. An instrument that can be off must say when it is on: the arm edge
    // is announced once per row.
    if (o.anchored) {
        UE_LOGI("movement_ledger: ARMED slot=%d gen=%u anchor=(%.0f,%.0f,%.0f) -- measure-only, "
                "nothing refuses on this build",
                slot, gen, pos.X, pos.Y, pos.Z);
    }
    if (o.unstampedEdge) {
        UE_LOGW("movement_ledger: slot=%d sends UNSTAMPED poses (stateTimeMs24==0) -- the origin's "
                "clock is what bounds claimed motion, so without it this peer earns NO time and its "
                "moves are charged in full. Latched: reported once, not per packet.",
                slot);
    }
    if (o.nonFiniteEdge) {
        UE_LOGW("movement_ledger: slot=%d sent a NON-FINITE position -- refused without touching the "
                "row. ValidatePose should have caught this upstream; that it did not would be the "
                "finding, not this line. Latched: reported once, not per packet.",
                slot);
    }
    if (o.discontinuity) {
        UE_LOGW("movement_ledger: slot=%d DISCONTINUITY step=%.0f cm to=(%.0f,%.0f,%.0f) "
                "dtSender=%u ms dtRecv=%u ms credited=%u ms credit=%.0f cm (debt clears in %.1f s at "
                "%.0f cm/s). MEASURE-ONLY: nothing is refused; this line is the input record that "
                "decides whether the destination belongs in the host-derivable set. FIRST of this "
                "10 s window only -- the window's full count rides disc= in the summary.",
                slot, o.stepCm, pos.X, pos.Y, pos.Z, o.dtSenderMs, o.dtRecvMs, o.useMs, o.creditCm,
                (o.creditCm < 0.f ? -o.creditCm / kMaxTravelSpeedCmS : 0.f), kMaxTravelSpeedCmS);
    }
}

// Both readers check the occupancy generation, as the write path does: a slot recycles with no
// absence between, the generation drops to 0 the moment the old occupant disconnects, and the
// row keeps the old verdict until the new occupant's first pose reaches the net thread. A
// joining peer can send a reliable before its first pose, and a read in that window would
// answer about the new peer with a fact about the old. Fail closed: an unarmed row, a stale
// generation or a departed slot is not a body. Staleness is refused at the read, which is why
// there is no OnSessionStop.
bool PositionTrusted(coop::net::Session& session, int slot) {
    if (slot <= 0 || slot >= kSlots) return false;
    const uint32_t gen = session.peerGenerationForSlot(slot);
    if (gen == 0) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    const Row& r = g_rows[slot];
    return r.armed && r.gen == gen && r.trusted;
}

bool TryGetAcceptedPosition(coop::net::Session& session, int slot, ue_wrap::FVector& out) {
    if (slot <= 0 || slot >= kSlots) return false;
    const uint32_t gen = session.peerGenerationForSlot(slot);
    if (gen == 0) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    const Row& r = g_rows[slot];
    if (!r.armed || r.gen != gen) return false;
    out = r.lastPos;
    return true;
}

void Tick(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("movement_ledger::Tick");
    if (session.role() != coop::net::Role::Host) return;

    const uint64_t now = NowMs();
    if (now < g_nextSummaryMs.load(std::memory_order_relaxed)) return;
    g_nextSummaryMs.store(now + kSummaryPeriodMs, std::memory_order_relaxed);

    // The wire-versus-actor divergence is sampled here, inside the summary gate, once per window,
    // right before the line that consumes it. It is an engine read per slot, so it cannot run in
    // the net-thread write, and per tick it would spend dispatches every frame for a number read
    // at 0.1 Hz. It measures the gap between the value the ledger validated and the value the
    // intent authorizer consumes; coop/element/intent_authority samples the same divergence at
    // every real intent, since this sampler had produced one reading in the whole log corpus, on a
    // stationary peer.
    for (int slot = 1; slot < kSlots; ++slot) {
        ue_wrap::FVector wire{};
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_rows[slot].armed) continue;
            wire = g_rows[slot].lastPos;
        }
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
        ue_wrap::FVector actor{};
        if (!puppet || !E::TryGetActorLocation(puppet, actor)) continue;
        const float d = Dist3(wire, actor);
        std::lock_guard<std::mutex> lk(g_mu);
        g_rows[slot].lastWireVsActorCm = d;
    }

    // Unconditional, so an empty log means the instrument is dead rather than the world quiet.
    for (int slot = 1; slot < kSlots; ++slot) {
        Row snap;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            Row& r = g_rows[slot];
            if (!r.armed) continue;
            snap = r;
            r.samples = 0;
            r.discontinuities = 0;
            r.maxStepCm = 0.f;
            r.minDtMs = UINT32_MAX;
            r.maxDtMs = 0;
            r.maxUseMs = 0;
            r.maxBankMs = 0;
            r.minCreditCm = r.creditCm;
            r.maxImpliedSpeed = 0.f;
            r.discReported = false;
            // Back to never-taken: otherwise a window whose puppet read failed prints the previous
            // window's divergence with nothing marking it stale.
            r.lastWireVsActorCm = -1.f;
        }
        // dt, use and bank are calibration fields: dt is the real receive-gap distribution and use
        // is how much of one gap a single packet was credited; together they decide whether a
        // per-packet earn cap is affordable (selftest row 10).
        UE_LOGI("movement_ledger[slot %d]: n=%llu disc=%llu age=%.0fs maxStep=%.0f cm "
                "dt=[%u..%u] ms use<=%u ms bank<=%u ms minCredit=%.0f cm (cap %.0f) "
                "maxImplied=%.0f cm/s (bound %.0f) wireVsActor=%.0f cm trusted=%d",
                slot, static_cast<unsigned long long>(snap.samples),
                static_cast<unsigned long long>(snap.discontinuities),
                static_cast<double>(now - snap.anchorAtMs) / 1000.0, snap.maxStepCm,
                (snap.minDtMs == UINT32_MAX ? 0u : snap.minDtMs), snap.maxDtMs,
                snap.maxUseMs, snap.maxBankMs,
                snap.minCreditCm, kUnearnedJumpCm, snap.maxImpliedSpeed, kMaxTravelSpeedCmS,
                snap.lastWireVsActorCm, snap.trusted ? 1 : 0);
    }
}

}  // namespace coop::movement_ledger

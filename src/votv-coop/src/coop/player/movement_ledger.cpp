// coop/player/movement_ledger.cpp -- see coop/player/movement_ledger.h for WHY this exists, what it
// deliberately does not do, and the honest bound it buys. This file is the arithmetic.
//
// THE MODEL, in one paragraph. A peer earns the right to have moved by the passage of REAL time, at
// no more than the speed the game itself can move a player. Two independent caps do two jobs: a bank
// of real time (`kSkewBankMaxMs`) lets the sender's own clock supply the SHAPE of an interval, so a
// network clump costs nothing; and a cap on the CARRY -- the credit left standing from earlier
// samples -- bounds what can be spent as an unexplained jump (`kUnearnedJumpCm`). Neither cap
// loosens the sustained rate, because credit is only ever created by elapsed time.
//
// WHICH QUANTITY THE JUMP CAP APPLIES TO IS THE WHOLE DESIGN, and the first version of this file got
// it wrong. It capped `carry + earned` and only then subtracted the step, which makes the largest
// step ANY sample may take `kUnearnedJumpCm` -- 50 cm -- no matter how much time elapsed. An honest
// ATV rider at the game's own `speed_turbo` (3200 cm/s) covers 53 cm between two 60 Hz poses and was
// therefore marked untrusted on his first sample; so was any sprinter across a single 100 ms packet
// gap. The cap belongs on the CARRY, because the carry is the part that was not earned in this
// interval. Caught by writing the selftest at the bottom of this file, before the build was deployed
// and before the field run it would have poisoned -- the calibration transcript would have read
// "untrusted" for ordinary play and been believed as "the bound is too tight".
//
// THERE IS EXACTLY ONE ACCOUNTING PATH, and that is the second correction. An unstamped pose used to
// take an early return that set `trusted = false` and then MOVED THE ANCHOR anyway, leaving the
// credit untouched -- so two packets bought an unlimited relocation: one unstamped to arrive, one
// stamped at the same spot to be measured against a zero step and regain trust immediately. The
// header called that path fail-CLOSED. It was fail-closed for one packet and fail-OPEN for the
// anchor. Unstamped now means exactly what it says -- NO TIME INFORMATION, therefore no time earned
// -- and the step is charged like any other. The special case is gone rather than patched.

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
// How often the unconditional summary prints. Frequent enough that a short act in the runbook still
// produces a line, rare enough that it cannot be the thing that costs frames.
constexpr uint64_t kSummaryPeriodMs = 10000;

// Monotonic milliseconds. The row keeps a COUNT rather than a `time_point` so that every input to
// the arithmetic below -- including the receiver's own clock -- can be supplied explicitly by the
// selftest. A branch only a real 8-second network gap can reach is a branch that ships undrilled.
uint64_t NowMs() {
    static const std::chrono::steady_clock::time_point kEpoch = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - kEpoch).count());
}

struct Row {
    // Identity. The occupancy generation this row was anchored under; a change means the slot's
    // occupant was REPLACED and everything below it belongs to somebody else.
    uint32_t gen = 0;
    bool     armed = false;
    uint64_t anchorAtMs = 0;      // when THIS occupant's record began; the summary prints its age

    // Position accounting.
    ue_wrap::FVector lastPos{};
    float    creditCm = 0.f;      // ALWAYS within [-kMaxDebtCm, kUnearnedJumpCm]; see the cap note
    bool     trusted = false;

    // Clocks. `lastStateMs` is the ORIGIN's 24-bit stamp; `lastRecvMs` is ours.
    uint32_t lastStateMs = 0;
    uint64_t lastRecvMs = 0;
    uint32_t skewBankMs = 0;      // real time earned and not yet spent as claimed elapsed

    // Diagnosis latches -- a 60 Hz stream must never turn a condition into a log flood, and every
    // condition below is AUTHORED BY THE PEER BEING JUDGED. `discReported` is per SUMMARY WINDOW,
    // not per row: without it a peer alternating one 350 cm hop with one standing sample earns a
    // trusted->untrusted edge every other pose, which is ~30 UE_LOGW per second per peer -- and
    // `log.cpp:225` fflushes every non-INFO line synchronously, ON THE NET THREAD, the one that
    // services every peer's pose relay and reliable ARQ. That hands the peer this module exists to
    // bound a frame-rate knob. Nothing is lost: the window's full COUNT still rides `disc=` in the
    // summary, so the detail line becomes a sample rather than a transcript.
    bool     unstampedReported = false;
    bool     nonFiniteReported = false;
    bool     discReported = false;

    // Summary accumulators, reset when the summary prints AND on every re-anchor -- they are per
    // OCCUPANT, not per slot. The COUNT is load-bearing: without it "this row saw no poses at all"
    // and "it saw poses that did not move" print the same line, which is the ambiguity
    // `docs/LESSONS.md:1019` was written about.
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
// TWO threads touch this, which is why it is atomic and not a plain uint64_t: `Tick` reads and
// re-arms it on the GAME thread, and `OnSessionStart` re-bases it on the harness BRINGUP thread
// (`session_runtime.cpp` -- that function Posts to the game thread and Sleeps on the result, so it
// demonstrably is not the game thread itself). On a Stop()/Start() cycle the two overlap. Relaxed is
// the right ordering: it guards no other data, and the read-modify-write is game-thread-only, so
// there is no cross-thread RMW to lose.
std::atomic<uint64_t> g_nextSummaryMs{0};

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Start (or restart) a row at `pos` under `gen`, granting nothing. Used for the first sample and for
// a slot whose occupant was replaced. The peer begins with an EMPTY credit rather than a full one:
// arriving is not evidence of having travelled.
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
    // The accumulators below are PER OCCUPANT, not per slot. Slots recycle lowest-free, so a slot
    // goes person X -> person Y with no absence in between; leaving these standing makes Y's first
    // summary window report X's worst step, X's discontinuity count and X's divergence sample under
    // Y's slot number -- an attribution defect in the one artifact this build exists to produce.
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

// What one accepted pose did to a row. Returned rather than logged in place so the logging happens
// outside the lock, and so the selftest asserts on the arithmetic instead of on log text.
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

// THE ARITHMETIC, with every input the session supplies made explicit: the row, the occupancy
// generation, the sender's stamp and the receiver's clock. `OnClientPose` is this plus the two
// session reads and the logging; the selftest is this plus a table, over ITS OWN row -- so the test
// is structurally incapable of touching production state, and still drives the shipped function
// rather than a second copy of its arithmetic.
//
// THE CALLER OWNS THE LOCK. Production takes `g_mu` around it; the selftest runs on its own storage.
Outcome ApplyPose(Row& r, uint32_t gen, const ue_wrap::FVector& pos,
                  uint32_t stateTimeMs24, uint64_t nowMs) {
    Outcome o;

    // PRECONDITION, defended rather than assumed. In production `coop::net::ValidatePose` has
    // already rejected non-finite components and bounded |xyz| at 10 km, so this cannot fire -- but
    // a NaN reaching the arithmetic below is unrecoverable and SILENT: `NaN >= 0.f` is false (so the
    // row reads untrusted, which looks correct) and BOTH clamps are false too, so `creditCm` stores
    // NaN and every later sample stays NaN for the rest of the session. A permanent loss with no
    // diagnosis is worse than a named refusal, and the enforcing build gives this function callers
    // that do not sit behind ValidatePose.
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

    // NOT STAMPED means NO TIME INFORMATION, so no time is earned -- and then the pose goes down the
    // SAME path as every other one. See the file header: the early return this replaced moved the
    // anchor for free and made two packets a teleport.
    const bool stamped = (stateTimeMs24 != 0);
    if (!stamped && !r.unstampedReported) { r.unstampedReported = true; o.unstampedEdge = true; }

    const uint32_t dtR = static_cast<uint32_t>(nowMs - r.lastRecvMs);

    uint32_t useMs = 0;
    if (stamped) {
        if (dtR >= kClockRebaseGapMs) {
            // A gap long enough to make the 24-bit stamp ambiguous. Re-base the CLOCK ONLY -- the
            // position anchor, the credit and the path all survive, so silence earns nothing and
            // cannot be used as a free reset.
            r.skewBankMs = 0;
        } else {
            // The sender supplies the SHAPE of the interval; real time supplies the CEILING. An
            // inflated or rewound stamp can only ever claim what the bank actually holds.
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

    // The CARRY is what was not earned in this interval, so the carry is what the un-earned-jump cap
    // bounds. THIS interval's own earnings are spendable in full -- otherwise no sample may ever
    // cover more than 50 cm and the game's own vehicle outruns the instrument. `r.creditCm` is kept
    // inside the cap on every store below, so this read is already bounded; the clamp stays as the
    // one place the invariant is written down.
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

// --- selftest ----------------------------------------------------------------
// Runs on every session start, unconditionally, in microseconds, over ITS OWN row. It is NOT
// env-gated on purpose: this is a security primitive whose failure mode is SILENT -- a wrong verdict
// merely reads wrong -- and the branches below (an unstamped sender, a slot recycled to a new
// occupant, a 24-bit clock wrap, an inflated clock, a drained receive queue, the debt floor) are
// unreachable from a two-peer LAN smoke. `join_seed`'s selftest is the shape precedent; unlike that
// one, this drives the PRODUCTION function rather than a second copy of its arithmetic, so the test
// cannot pass while the shipped path is wrong.

// Drive `n` samples of steady motion at `speedCmS` with `dtMs` between poses, from a fresh anchor.
// Sender and receiver clocks advance together: this is the HONEST case, and every row of it must
// stay trusted or the instrument is unusable for calibration.
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

    // 1. The first pose of a peer never seen before is ADMITTED (principle 8: a joining peer has no
    //    predecessor, and refusing it would make mid-activity join an unsupported edge case).
    fresh();
    {
        const Outcome o = ApplyPose(r, 7, ue_wrap::FVector{1000.f, 2000.f, 300.f}, 500, 1000);
        check("anchor-admits-first-pose", o.anchored && o.trusted, "a joining peer was not admitted");
    }

    // 2. A slot RECYCLED to a new occupant re-anchors and is admitted even though the position moved
    //    the width of the world -- the previous occupant's anchor is not this peer's history. Slots
    //    recycle lowest-free, so a slot goes X->Y with NO absence in between.
    {
        const Outcome o = ApplyPose(r, 8, ue_wrap::FVector{-400000.f, 400000.f, 0.f}, 600, 1016);
        check("recycle-reanchors", o.anchored && o.trusted,
              "a replaced occupant was judged against the previous occupant's anchor");
    }

    // 3. An UNSTAMPED pose is refused trust, and the report LATCHES (a 60 Hz stream must not flood).
    {
        const Outcome a = ApplyPose(r, 8, ue_wrap::FVector{-400000.f, 400000.f, 0.f}, 0, 1032);
        const Outcome b = ApplyPose(r, 8, ue_wrap::FVector{-400000.f, 400000.f, 0.f}, 0, 1048);
        check("unstamped-untrusted", !a.trusted && !b.trusted, "an unstamped pose was believed");
        check("unstamped-latched", a.unstampedEdge && !b.unstampedEdge,
              "the unstamped warning is not latched -- it would print once per packet");
    }

    // 4. THE TWO-PACKET RELOCATION. An unstamped pose used to move the anchor for free, so a stamped
    //    pose at the SAME spot then measured a zero step and restored trust immediately. The
    //    unstamped hop must cost its full distance in debt, and the stamped follow-up must not clear
    //    it. This is the row the earlier selftest could not have had: it asked only whether the
    //    unstamped packets themselves were untrusted, which passes on the broken module.
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

    // 5. ORDINARY MOTION STAYS TRUSTED. Every row here failed under the first version of this file,
    //    which capped `carry + earned` and so allowed no sample to cover more than 50 cm.
    fresh(); check("walk-400-at-60hz", SimSteady(r, 400.f, 17, 40).trusted,
                   "walking at defSpeed was refused");
    fresh(); check("sprint-1000-at-60hz", SimSteady(r, 1000.f, 17, 40).trusted,
                   "sprinting at the updateSpeed ceiling was refused");
    fresh(); check("atv-turbo-3200-at-60hz", SimSteady(r, 3200.f, 17, 40).trusted,
                   "the game's own ATV at speed_turbo was refused");
    fresh(); check("noclip-diag-8660-at-60hz", SimSteady(r, 8660.f, 17, 40).trusted,
                   "the game's own noclip diagonal was refused");
    // ...and the same speeds at a sparse packet rate, which is what a lossy link looks like.
    fresh(); check("sprint-1000-at-10hz", SimSteady(r, 1000.f, 100, 20).trusted,
                   "sprinting across a 100 ms packet gap was refused");
    fresh(); check("atv-turbo-3200-at-10hz", SimSteady(r, 3200.f, 100, 20).trusted,
                   "the ATV across a 100 ms packet gap was refused");

    // 6. Sustained OVER-bound motion is caught, and caught quickly.
    fresh(); check("over-bound-15000-caught", !SimSteady(r, 15000.f, 17, 10).trusted,
                   "sustained motion above the speed bound was believed");
    fresh(); check("over-bound-11000-caught", !SimSteady(r, 11000.f, 17, 40).trusted,
                   "a ten-percent overspeed was believed indefinitely");

    // 7. THE SUBDIVIDED FREE JUMP -- the attack that killed the first design of this module: hops
    //    small enough to pass for jitter, repeated. 11 x 1999 cm is 220 m in 183 ms.
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

    // 8. A single long teleport is untrusted, and its debt clears at the speed bound -- not sooner
    //    (it would be free) and not later (an honest teleport would be punished forever).
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        const Outcome jump = ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);  // 220 m
        check("teleport-untrusted", !jump.trusted, "a 220 m single-packet jump was believed");
        // Standing still afterwards: 220 m of debt at 10 000 cm/s is ~2.2 s, so trust must NOT
        // return inside the first 1.7 s and must return inside 3.4 s.
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

    // 9. SILENCE PAST THE REBASE GAP EARNS NOTHING. The clock re-bases; the position anchor and the
    //    credit survive, so muting one's own stream for a long time is not a free teleport.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 30000; st = (st + 30000) & 0x00FFFFFFu;         // 30 s, past kClockRebaseGapMs
        const Outcome o = ApplyPose(r, 1, ue_wrap::FVector{22000.f, 0.f, 0.f}, st, now);
        check("long-silence-earns-nothing", !o.trusted,
              "a peer bought a 220 m jump by muting its own pose stream past the rebase gap");
    }

    // 10. SILENCE *UNDER* THE REBASE GAP EARNS BANKED TIME, and this row exists to PIN that number
    //     rather than to assert it is safe. 7 s of silence banks kSkewBankMaxMs = 5 s, which at the
    //     speed bound is 500 m spendable in ONE packet. That is INSIDE the sustained rate -- 7 s of
    //     real time legitimately buys 700 m, so the bank cap tightens the bound rather than loosening
    //     it -- but it is INVISIBLE, because trust never falls and no DISCONTINUITY line is logged.
    //     The silence, not the rate, is the defect, and the discontinuity record IS this build's
    //     product. Whether to add a per-packet earn cap is a FIELD question: the summary's new
    //     dt/use/bank maxima are what answer it, and the header records the question rather than
    //     guessing a constant. If this row ever flips, someone changed the model -- read the header.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 7000; st = (st + 7000) & 0x00FFFFFFu;           // 7 s, UNDER kClockRebaseGapMs
        const Outcome o = ApplyPose(r, 1, ue_wrap::FVector{50000.f, 0.f, 0.f}, st, now);   // 500 m
        check("sub-rebase-silence-banks-time", o.trusted && o.useMs == kSkewBankMaxMs,
              "the banked-time model changed -- re-read the header before trusting this build");
    }
    // ...and one metre past what the bank holds is still refused, so the bound is a bound.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 7000; st = (st + 7000) & 0x00FFFFFFu;
        const Outcome over = ApplyPose(r, 1, ue_wrap::FVector{50100.f, 0.f, 0.f}, st, now);
        check("sub-rebase-silence-is-still-bounded", !over.trusted,
              "banked time was not capped at kSkewBankMaxMs -- silence buys unbounded distance");
    }

    // 11. A REWOUND or INFLATED sender clock buys nothing: the bank holds only real time.
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

    // 12. The 24-bit stamp WRAPS every 4h39m; the arithmetic must cross it without inventing time.
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

    // 13. A NETWORK CLUMP costs nothing. The receiver stalls 200 ms, then drains twelve poses whose
    //     ARRIVAL intervals are zero but whose PRODUCTION intervals were 17 ms each. This is the one
    //     thing the bank exists for, and without it an ordinary hiccup reads as motion.
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

    // 14. The debt FLOOR holds, so an absurd jump cannot buy exile beyond the stated policy.
    fresh();
    {
        uint64_t now = 5000; uint32_t st = 100;
        ApplyPose(r, 1, ue_wrap::FVector{0.f, 0.f, 0.f}, st, now);
        now += 17; st += 17;
        const Outcome o = ApplyPose(r, 1, ue_wrap::FVector{900000.f, 0.f, 0.f}, st, now);   // 9 km
        check("debt-floored", o.creditCm >= -(kMaxDebtCm + 1.f),
              "the debt floor did not hold -- recovery time is unbounded");
    }

    // 15. A RECYCLED slot starts its measurements empty. The accumulators are per OCCUPANT: leaving
    //     them standing makes the new person's first summary window report the old person's worst
    //     step under the new person's slot number, in the one artifact this build exists to produce.
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

    // 16. The discontinuity DETAIL line is latched per summary window while the COUNT is not. The
    //     condition is authored by the peer being judged, and the detail line is a UE_LOGW, which
    //     fflushes synchronously on the net thread.
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

    // 17. A NON-FINITE position is refused and cannot poison the row. ValidatePose makes this
    //     unreachable in production today; the point is that if it ever becomes reachable, the
    //     failure is a named refusal rather than a row that reads untrusted forever with no
    //     explanation and no way to recover.
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
    // Un-gated on purpose, and it drives its OWN row. See its comment.
    RunSelfTest();
}

void OnClientPose(coop::net::Session& session, int slot,
                  const ue_wrap::FVector& pos, uint32_t stateTimeMs24) {
    if (slot <= 0 || slot >= kSlots) return;   // slot 0 is the host; it never validates itself
    if (session.role() != coop::net::Role::Host) return;   // client-scoped: a client validates none

    // Read on THIS thread (the acquire load is documented "Any thread") so a recycled slot's very
    // first pose is never judged against the previous occupant's anchor.
    const uint32_t gen = session.peerGenerationForSlot(slot);
    const uint64_t now = NowMs();

    Outcome o;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        o = ApplyPose(g_rows[slot], gen, pos, stateTimeMs24, now);
    }

    // Logging OUTSIDE the lock, always. An instrument that can be off must say when it is ON, so the
    // arm edge is announced once per row with the facts that identify it.
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

// BOTH readers take the session and check the occupancy generation, for the same reason the WRITE
// path does: a slot recycles X->Y with no absence in between, `peerGenBySlot_` drops to 0 the moment
// X disconnects, and the row keeps X's verdict at X's position until Y's first pose reaches the NET
// thread. A read landing in that window -- and a joining peer can send a reliable before its first
// pose -- would otherwise answer a question about Y with a fact about X. Fail-CLOSED: an unarmed
// row, a stale generation, or a departed slot (generation 0) is not a body, and no body means no
// reach. This is also why there is no OnSessionStop: staleness is refused at the READ, which is the
// layer that can actually see it, rather than swept at a teardown hook this tree does not have.
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

    // The wire-vs-actor divergence is sampled HERE -- inside the summary gate, once per window,
    // immediately before the line that consumes it. It is an ENGINE read (a ParamFrame plus a
    // ProcessEvent dispatch per slot), so doing it inside the net-thread write would break the
    // game-thread-only rule outright; and doing it EVERY tick, as the first version of this file
    // did, spends up to three UFunction dispatches per frame -- some 375 per second -- to produce a
    // number that is read at 0.1 Hz. It measures the gap between the value the ledger validated and
    // the value the intent authorizer currently consumes, which is the number that justifies (or
    // refutes) moving authorization onto the ledger. (It named `SenderMayReach` until 2026-08-26,
    // when that module-local check was promoted whole into `coop/element/intent_authority`; the
    // authorizer now samples this same divergence at EVERY real intent, because this 0.1 Hz sampler
    // had produced exactly ONE reading in the whole log corpus and that reading was taken on a
    // stationary peer, where the divergence is trivially zero.)
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

    // UNCONDITIONAL, so an empty log means the instrument is DEAD rather than the world being quiet.
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
            // Back to "never taken this window". Without this, a window whose puppet read FAILED
            // prints the previous window's divergence with nothing marking it stale; -1 says so.
            r.lastWireVsActorCm = -1.f;
        }
        // dt / use / bank are CALIBRATION fields, not decoration. `dt` is the real receive-gap
        // distribution and `use` is how much of one gap a single packet was credited for; together
        // they decide whether a per-packet earn cap is affordable in the enforcing build -- the open
        // question selftest row 10 pins and this build exists to answer rather than guess.
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

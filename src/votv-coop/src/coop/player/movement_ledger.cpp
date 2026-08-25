// coop/player/movement_ledger.cpp -- see coop/player/movement_ledger.h for WHY this exists, what it
// deliberately does not do, and the honest bound it buys. This file is the arithmetic.
//
// THE MODEL, in one paragraph. A peer earns the right to have moved by the passage of REAL time, at
// no more than the speed the game itself can move a player. Two independent caps do two jobs: a bank
// of real time (`kSkewBankMaxMs`) lets the sender's own clock supply the SHAPE of an interval, so a
// network clump costs nothing; and a cap on credit (`kUnearnedJumpCm`) bounds what can be spent as
// an unexplained jump, no matter how much time was banked. Neither cap loosens the sustained rate,
// because credit is only ever created by elapsed time.

#include "coop/player/movement_ledger.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace coop::movement_ledger {

namespace E = ue_wrap::engine;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int   kSlots           = static_cast<int>(coop::players::kMaxPeers);
constexpr float kMaxDebtCm       = kMaxTravelSpeedCmS * kMaxDebtSeconds;
// How often the unconditional summary prints. Frequent enough that a short act in the runbook still
// produces a line, rare enough that it cannot be the thing that costs frames.
constexpr auto  kSummaryPeriod   = std::chrono::seconds(10);

struct Row {
    // Identity. The occupancy generation this row was anchored under; a change means the slot's
    // occupant was REPLACED and everything below belongs to somebody else.
    uint32_t gen = 0;
    bool     armed = false;

    // Position accounting.
    ue_wrap::FVector lastPos{};
    float    creditCm = 0.f;      // spendable now; capped at kUnearnedJumpCm, floored at -kMaxDebtCm
    bool     trusted = false;

    // Clocks. `lastStateMs` is the ORIGIN's 24-bit stamp; `lastRecv` is ours.
    uint32_t    lastStateMs = 0;
    Clock::time_point lastRecv{};
    uint32_t    skewBankMs = 0;   // real time earned and not yet spent as claimed elapsed

    // One-shot diagnosis latches -- a 60 Hz stream must never turn a condition into a log flood.
    bool     unstampedReported = false;

    // Summary accumulators, reset each time the summary prints. The COUNT is load-bearing: without
    // it "this row saw no poses at all" and "it saw poses that did not move" print the same line,
    // which is the ambiguity `docs/LESSONS.md:1019` was written about.
    uint64_t samples = 0;
    uint64_t discontinuities = 0;
    float    maxStepCm = 0.f;
    uint32_t minDtMs = UINT32_MAX;
    uint32_t maxDtMs = 0;
    float    minCreditCm = 0.f;   // the residual-slack MINIMUM -- the number that says if the
                                  // un-earned-jump cap is too small to survive real jitter
    float    maxImpliedSpeed = 0.f;
    float    lastWireVsActorCm = -1.f;  // game-thread sample; -1 = never taken
};

std::mutex g_mu;
Row        g_rows[kSlots];
Clock::time_point g_nextSummary{};

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Start (or restart) a row at `pos` under `gen`, granting nothing. Used for the first sample and for
// a slot whose occupant was replaced. The peer begins with an EMPTY credit rather than a full one:
// arriving is not evidence of having travelled.
void AnchorLocked(Row& r, uint32_t gen, const ue_wrap::FVector& pos,
                  uint32_t stateMs, Clock::time_point now) {
    r.gen = gen;
    r.armed = true;
    r.lastPos = pos;
    r.creditCm = 0.f;
    r.trusted = true;      // principle 8: a first pose has no predecessor and must be admitted
    r.lastStateMs = stateMs;
    r.lastRecv = now;
    r.skewBankMs = 0;
    r.unstampedReported = false;
}

}  // namespace

void OnSessionStart() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (Row& r : g_rows) r = Row{};
    g_nextSummary = Clock::now() + kSummaryPeriod;
}

void OnSessionStop() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (Row& r : g_rows) r = Row{};
}

void OnClientPose(coop::net::Session& session, int slot,
                  const ue_wrap::FVector& pos, uint32_t stateTimeMs24) {
    if (slot <= 0 || slot >= kSlots) return;   // slot 0 is the host; it never validates itself
    if (session.role() != coop::net::Role::Host) return;   // client-scoped: a client validates none

    const uint32_t gen = session.peerGenerationForSlot(slot);
    const auto now = Clock::now();

    bool armedEdge = false;
    ue_wrap::FVector armedAt{};
    bool unstampedEdge = false;
    float logStep = 0.f, logCredit = 0.f;
    uint32_t logDtS = 0, logDtR = 0;
    bool discontinuity = false;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        Row& r = g_rows[slot];

        // A replaced occupant invalidates every field below it. Read on THIS thread (the acquire load
        // is documented "Any thread") so a recycled slot's very first pose is never judged against the
        // previous occupant's anchor.
        if (!r.armed || r.gen != gen) {
            AnchorLocked(r, gen, pos, stateTimeMs24, now);
            r.samples = 1;
            armedEdge = true;
            armedAt = pos;
        } else {
            // NOT STAMPED. Fail-closed with a named reason, latched: skipping would freeze the row's
            // last verdict silently, and crediting zero would emit a violation sixty times a second.
            if (stateTimeMs24 == 0) {
                r.trusted = false;
                if (!r.unstampedReported) { r.unstampedReported = true; unstampedEdge = true; }
                r.lastPos = pos;
                r.lastRecv = now;
                ++r.samples;
            } else {
                const uint32_t dtR = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - r.lastRecv).count());

                uint32_t useMs = 0;
                if (dtR >= kClockRebaseGapMs) {
                    // A gap long enough to make the 24-bit stamp ambiguous. Re-base the CLOCK ONLY --
                    // the position anchor, the credit and the path all survive, so silence earns
                    // nothing and cannot be used as a free reset.
                    r.skewBankMs = 0;
                } else {
                    // The sender supplies the SHAPE of the interval; real time supplies the CEILING.
                    // An inflated or rewound stamp can only ever claim what the bank actually holds.
                    const uint32_t dtS = coop::net::ElapsedMs24(r.lastStateMs, stateTimeMs24);
                    uint64_t bank = static_cast<uint64_t>(r.skewBankMs) + dtR;
                    if (bank > kSkewBankMaxMs) bank = kSkewBankMaxMs;
                    useMs = (dtS < bank) ? dtS : static_cast<uint32_t>(bank);
                    r.skewBankMs = static_cast<uint32_t>(bank - useMs);
                    logDtS = dtS;
                }
                logDtR = dtR;

                const float step = Dist3(r.lastPos, pos);
                float credit = r.creditCm + kMaxTravelSpeedCmS * (static_cast<float>(useMs) / 1000.f);
                if (credit > kUnearnedJumpCm) credit = kUnearnedJumpCm;
                credit -= step;
                if (credit < -kMaxDebtCm) credit = -kMaxDebtCm;

                const bool wasTrusted = r.trusted;
                r.creditCm = credit;
                r.trusted = (credit >= 0.f);
                r.lastPos = pos;
                r.lastStateMs = stateTimeMs24;
                r.lastRecv = now;

                ++r.samples;
                if (step > r.maxStepCm) r.maxStepCm = step;
                if (logDtR < r.minDtMs) r.minDtMs = logDtR;
                if (logDtR > r.maxDtMs) r.maxDtMs = logDtR;
                if (credit < r.minCreditCm) r.minCreditCm = credit;
                if (useMs > 0) {
                    const float implied = step / (static_cast<float>(useMs) / 1000.f);
                    if (implied > r.maxImpliedSpeed) r.maxImpliedSpeed = implied;
                }
                if (wasTrusted && !r.trusted) {
                    ++r.discontinuities;
                    discontinuity = true;
                    logStep = step;
                    logCredit = credit;
                }
            }
        }
    }

    // Logging OUTSIDE the lock, always. An instrument that can be off must say when it is ON, so the
    // arm edge is announced once per row with the facts that identify it.
    if (armedEdge) {
        UE_LOGI("movement_ledger: ARMED slot=%d gen=%u anchor=(%.0f,%.0f,%.0f) -- measure-only, "
                "nothing refuses on this build",
                slot, gen, armedAt.X, armedAt.Y, armedAt.Z);
    }
    if (unstampedEdge) {
        UE_LOGW("movement_ledger: slot=%d sends UNSTAMPED poses (stateTimeMs24==0) -- the origin's "
                "clock is what bounds claimed motion, so without it this peer's position cannot be "
                "believed and the row is held UNTRUSTED. Latched: reported once, not per packet.",
                slot);
    }
    if (discontinuity) {
        UE_LOGW("movement_ledger: slot=%d DISCONTINUITY step=%.0f cm to=(%.0f,%.0f,%.0f) "
                "dtSender=%u ms dtRecv=%u ms credit=%.0f cm (debt clears in %.1f s at %.0f cm/s). "
                "MEASURE-ONLY: nothing is refused; this line is the input record that decides "
                "whether the destination belongs in the host-derivable set.",
                slot, logStep, pos.X, pos.Y, pos.Z, logDtS, logDtR, logCredit,
                (logCredit < 0.f ? -logCredit / kMaxTravelSpeedCmS : 0.f), kMaxTravelSpeedCmS);
    }
}

bool PositionTrusted(int slot) {
    if (slot <= 0 || slot >= kSlots) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    const Row& r = g_rows[slot];
    return r.armed && r.trusted;      // fail-CLOSED: no sample means no body means no reach
}

bool TryGetAcceptedPosition(int slot, ue_wrap::FVector& out) {
    if (slot <= 0 || slot >= kSlots) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    const Row& r = g_rows[slot];
    if (!r.armed) return false;
    out = r.lastPos;
    return true;
}

void Tick(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("movement_ledger::Tick");
    if (session.role() != coop::net::Role::Host) return;

    const auto now = Clock::now();

    // The wire-vs-actor divergence. It is sampled HERE, on the game thread, because reading a
    // puppet's transform is an ENGINE read -- doing it inside the net-thread write would break the
    // game-thread-only rule outright. It measures the gap between the value the ledger validated and
    // the value `SenderMayReach` currently consumes, which is the number that justifies (or refutes)
    // moving authorization onto the ledger in the enforcing build.
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

    if (now < g_nextSummary) return;
    g_nextSummary = now + kSummaryPeriod;

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
            r.minCreditCm = r.creditCm;
            r.maxImpliedSpeed = 0.f;
        }
        UE_LOGI("movement_ledger[slot %d]: n=%llu disc=%llu maxStep=%.0f cm dt=[%u..%u] ms "
                "minCredit=%.0f cm (cap %.0f) maxImplied=%.0f cm/s (bound %.0f) wireVsActor=%.0f cm "
                "trusted=%d",
                slot, static_cast<unsigned long long>(snap.samples),
                static_cast<unsigned long long>(snap.discontinuities), snap.maxStepCm,
                (snap.minDtMs == UINT32_MAX ? 0u : snap.minDtMs), snap.maxDtMs,
                snap.minCreditCm, kUnearnedJumpCm, snap.maxImpliedSpeed, kMaxTravelSpeedCmS,
                snap.lastWireVsActorCm, snap.trusted ? 1 : 0);
    }
}

}  // namespace coop::movement_ledger

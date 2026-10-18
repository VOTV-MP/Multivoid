// coop/session/join_beacon.cpp -- see coop/session/join_beacon.h.

#include "coop/session/join_beacon.h"

#include "coop/config/config.h"           // ResolveFlag, the [dev] join_draining_freeze knob
#include "coop/config/config_registry.h"  // rows::join_draining_freeze
#include "coop/net/session.h"
#include "coop/session/join_progress.h"   // BeaconRenews, the client rule the selftest runs
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace coop::join_beacon {
namespace {

using coop::net::HostJoinPhase;

// Set once at boot from the harness thread, read on the game thread.
std::atomic<coop::net::Session*> g_session{nullptr};

constexpr int64_t kBeaconIntervalMs = 1000;

// The cadence is kept on ATTEMPTS and the log on SENDS, and the two are separate fields because
// the interesting second is the one where they disagree. A refused send leaves nothing behind to
// throttle against, so a cadence measured from the last SUCCESS stops throttling the moment the
// link pushes back -- which is the exact second this beacon exists for, and the send buffer is at
// the brim for the whole of a world transfer. The same is true of a phase change, whose
// "send it now" bypass stays armed until a send lands. Both became a per-tick send attempt on the
// host, tens of times a second, on the machine already carrying the transfer.
struct SlotNote {
    bool          noted = false;        // an owner worked this slot since the last Tick
    HostJoinPhase phase = HostJoinPhase::CapturingWorld;
    uint32_t      done = 0;
    uint32_t      total = 0;
    // The throttle's own state: what was last ATTEMPTED, and when.
    bool          everAttempted = false;
    HostJoinPhase attemptedPhase = HostJoinPhase::CapturingWorld;
    int64_t       lastAttemptMs = 0;
    // The log's state: what actually went out, so a line never claims a note the link refused.
    bool          everSent = false;
    HostJoinPhase sentPhase = HostJoinPhase::CapturingWorld;
};
SlotNote g_notes[coop::net::kMaxPeers];

// The Draining owner's state for a slot whose bracket has closed. Pushed by prop_snapshot, which
// owns the bracket; this module only reads the link's published delivery against it.
struct DrainState {
    int64_t  closedAtMs = 0;   // when the bracket closed (steady ms); 0 = open, nothing to say
    bool     latched = false;  // the first fresh sample found the queue drained: speak no more
    bool     frozen = false;   // [dev] join_draining_freeze took its value
    uint32_t frozenDone = 0;
};
DrainState g_drain[coop::net::kMaxPeers];

// One packet of reliable bytes: at or under it, with nothing queued behind, the slot's queue has
// drained. Each lane is FIFO and the bracket was enqueued before the drain began, so a drained
// queue means the bracket reached the joiner.
constexpr int64_t kDrainDoneBytes = 1200;

enum class DrainStep : uint8_t { Speak, Latch, Silent };

// The Draining owner's whole decision, pure. Silent: the bracket is open or the slot already
// latched. Latch: a link sample taken AFTER the close (strictly: the stamp is the sample's start,
// so a later one was read after the SnapshotComplete went in) found at most kDrainDoneBytes owed.
// Speak: everything else, which includes every sample from before the close.
DrainStep DrainingDecision(int64_t closedAtMs, bool latched, int64_t sampleMs, int64_t queueBytes) {
    if (closedAtMs <= 0 || latched) return DrainStep::Silent;
    if (sampleMs > closedAtMs && queueBytes <= kDrainDoneBytes) return DrainStep::Latch;
    return DrainStep::Speak;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* PhaseName(HostJoinPhase p) {
    switch (p) {
    case HostJoinPhase::CapturingWorld:    return "capturing the world";
    case HostJoinPhase::StreamingWorld:    return "streaming the world";
    case HostJoinPhase::SnapshotDeferred:  return "holding the bracket";
    case HostJoinPhase::StreamingSnapshot: return "streaming the bracket";
    case HostJoinPhase::Draining:          return "delivering the bracket";
    }
    return "?";
}

// Resolved once: the ini is read on every Resolve*, and this is asked on every drain pass.
bool FreezeKnob() {
    static const bool kOn =
        coop::config::ResolveFlag(coop::config_registry::rows::join_draining_freeze);
    return kOn;
}

// The slot's bracket is enqueued: say so, with the link's delivered KiB, until a fresh sample finds
// the queue drained. Runs every host pass, outside DrainChunk's world gate. Divergence from MTA
// (CGame::InitialDataStream sends a joiner's data as one stream and marks it joined at its start):
// our lanes stay, and the joiner's watchdog reads delivery progress instead.
void NoteDraining(coop::net::Session& s, int slot) {
    DrainState& d = g_drain[slot];
    // Open, or already delivered: nothing to read (the decision would say Silent anyway).
    if (d.closedAtMs <= 0 || d.latched) return;
    const auto pub = s.LinkDeliveryOf(slot);
    switch (DrainingDecision(d.closedAtMs, d.latched, pub.sampleMs, pub.queueBytes)) {
    case DrainStep::Silent: return;
    case DrainStep::Latch:
        d.latched = true;
        UE_LOGI("join_beacon: slot %d -- bracket delivered (%lld KiB, %lld B still owed)", slot,
                static_cast<long long>(pub.deliveredBytes / 1024),
                static_cast<long long>(pub.queueBytes));
        return;
    case DrainStep::Speak: break;
    }
    uint32_t kib = static_cast<uint32_t>(
        std::min<int64_t>(pub.deliveredBytes / 1024, static_cast<int64_t>(UINT32_MAX)));
    if (FreezeKnob()) {
        if (!d.frozen) {
            d.frozen = true;
            d.frozenDone = kib;
        }
        kib = d.frozenDone;
    }
    NotePhase(slot, HostJoinPhase::Draining, kib, 0);
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void NotePhase(int slot, HostJoinPhase phase, uint32_t done, uint32_t total) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    SlotNote& n = g_notes[slot];
    n.noted = true;
    n.phase = phase;
    n.done = done;
    n.total = total;
}

void NoteBracketClosed(int slot, int64_t closedAtMs) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    g_drain[slot] = DrainState{};
    g_drain[slot].closedAtMs = closedAtMs;
}

void NoteBracketReset(int slot) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    g_drain[slot] = DrainState{};
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    const int64_t now = NowMs();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        SlotNote& n = g_notes[slot];
        NoteDraining(*s, slot);
        // Consumed whether or not it is sent: the flag means "an owner worked this slot in the pass
        // just gone", and a slot nobody works falls silent on the next pass, which is the contract.
        const bool noted = n.noted;
        n.noted = false;
        if (!noted) continue;
        // A phase this slot has not tried to report yet goes out at once; everything else waits out
        // the interval, whether or not the last try landed.
        const bool newPhase = !n.everAttempted || n.phase != n.attemptedPhase;
        if (!newPhase && now - n.lastAttemptMs < kBeaconIntervalMs) continue;
        n.everAttempted = true;
        n.attemptedPhase = n.phase;
        n.lastAttemptMs = now;
        coop::net::JoinPhaseNotePayload p{};
        p.phase = static_cast<uint8_t>(n.phase);
        p.done = n.done;
        p.total = n.total;
        // The probe's path, for the probe's reason: a beacon is only worth what it says about NOW,
        // so it goes straight at the connection under the headroom reserve's exemption rather than
        // into the backlog, where a full send buffer -- the state a world transfer holds it in for
        // its whole length -- would park it behind the very stream it is reporting on. A refusal
        // here is the transport's own buffer being full, and the next second's note is a better
        // message than the one that was turned back, so a refusal is dropped rather than retried.
        if (!s->TrySendReliableToSlot(slot, coop::net::ReliableKind::JoinPhaseNote, &p,
                                      sizeof(p))) {
            continue;
        }
        if (!n.everSent || n.phase != n.sentPhase) {
            UE_LOGI("join_beacon: slot %d -- %s (%u/%u)", slot, PhaseName(n.phase), n.done,
                    n.total);
        }
        n.everSent = true;
        n.sentPhase = n.phase;
    }
}

void CancelForSlot(int slot) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    g_notes[slot] = SlotNote{};
    g_drain[slot] = DrainState{};
}

void OnDisconnect() {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_notes[slot] = SlotNote{};
        g_drain[slot] = DrainState{};
    }
}

bool RunDrainingSelftest() {
    int dPass = 0, dTotal = 0;
    auto decide = [&](DrainStep want, int64_t closed, bool latched, int64_t sample, int64_t queue,
                      const char* what) {
        ++dTotal;
        if (DrainingDecision(closed, latched, sample, queue) == want) { ++dPass; return; }
        UE_LOGE("join_beacon draining selftest FAIL: %s", what);
    };
    constexpr int64_t kClosed = 10'000;
    decide(DrainStep::Silent, 0, false, 20'000, 0, "an open bracket says nothing, whatever the sample");
    decide(DrainStep::Speak, kClosed, false, 9'900, 0, "a sample from before the close never stops it");
    decide(DrainStep::Speak, kClosed, false, kClosed, 0, "a sample stamped AT the close is not after it");
    decide(DrainStep::Speak, kClosed, false, 0, 0, "no sample yet keeps speaking");
    decide(DrainStep::Speak, kClosed, false, kClosed + 1, kDrainDoneBytes + 1,
           "a fresh sample with more than a packet owed keeps speaking");
    decide(DrainStep::Latch, kClosed, false, kClosed + 1, kDrainDoneBytes,
           "a fresh sample with exactly a packet owed latches");
    decide(DrainStep::Latch, kClosed, false, kClosed + 100, 0, "a fresh empty queue latches");
    decide(DrainStep::Silent, kClosed, true, kClosed + 100, 0, "a latched slot stays silent");
    decide(DrainStep::Silent, kClosed, true, kClosed + 100, 50'000,
           "a latched slot stays silent when the queue refills");
    const bool dOk = (dPass == dTotal);
    if (dOk) UE_LOGI("join_beacon draining selftest: PASS (%d/%d cases)", dPass, dTotal);
    else     UE_LOGE("join_beacon draining selftest: FAIL (%d/%d cases)", dPass, dTotal);

    // The joiner's renewal rule across the new phase, through the pure function the client calls.
    int rPass = 0, rTotal = 0;
    auto renews = [&](bool want, uint8_t wasPhase, uint32_t wasDone, uint8_t phase, uint32_t done,
                      const char* what) {
        ++rTotal;
        if (coop::join_progress::BeaconRenews(wasPhase, wasDone, phase, done) == want) {
            ++rPass;
            return;
        }
        UE_LOGE("join_beacon client-rule selftest FAIL: %s", what);
    };
    constexpr uint8_t kStream = static_cast<uint8_t>(HostJoinPhase::StreamingSnapshot);
    constexpr uint8_t kDrain = static_cast<uint8_t>(HostJoinPhase::Draining);
    renews(true, 0, 0, kStream, 0, "the first beacon heard renews");
    renews(true, kStream, 900, kDrain, 0, "Streaming -> Draining renews on the phase change alone");
    renews(false, kDrain, 100, kDrain, 100, "a flat Draining done does not renew");
    renews(true, kDrain, 100, kDrain, 101, "a rising Draining done renews");
    renews(false, kDrain, 100, kDrain, 99, "a falling done does not renew");
    renews(false, kStream, 900, kStream, 900, "a repeated Streaming beacon does not renew");
    renews(true, kStream, 900, 9, 0, "a phase value this build does not know renews once");
    const bool rOk = (rPass == rTotal);
    if (rOk) UE_LOGI("join_beacon client-rule selftest: PASS (%d/%d cases)", rPass, rTotal);
    else     UE_LOGE("join_beacon client-rule selftest: FAIL (%d/%d cases)", rPass, rTotal);
    return dOk && rOk;
}

}  // namespace coop::join_beacon

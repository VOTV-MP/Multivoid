// coop/session/world_ready_answer.cpp -- see coop/session/world_ready_answer.h.

#include "coop/session/world_ready_answer.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/end_reason.h"
#include "coop/net/fake_link_steps.h"  // the `wr` origin of net.fakelink_steps
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"  // kMaxPeers, Role
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_snapshot.h"  // HasSnapshotFor
#include "coop/session/player_handshake.h"  // OnClientWorldReady
#include "coop/session/subsystems.h"  // ConnectReplayForSlot

#include "ue_wrap/core/log.h"

#include <chrono>
#include <cstdint>

namespace coop::world_ready_answer {
namespace {

using Clock = std::chrono::steady_clock;

// Burst 2, NO refill: a hard cap of two re-announce replays per occupancy. A positive refill would let a
// client paced at exactly that rate (0.5/s -> a replay every 2 s) run forever, a standing cost no honest
// client incurs (honest re-announces are at most one per occupancy, 0 of 1825 archived sessions have two).
// With no refill, a flood at ANY pace empties the burst and is ended. The bucket resets to full at the
// successor's row birth (PerSlotState), so a rejoin gets a fresh two.
constexpr coop::net::IntentBudget kReplayBudget{2.0f, 0.0f};

// Per slot. PerSlotState resets it to a full IntentBucket at the successor's row birth, so a recycled
// slot starts fresh -- no manual clear beside the automatic one, and no missed-window raw clear.
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_rate;

// The net thread opened the slot's join gate at the announce's receipt, before any guard here ran,
// and the gate stays held until the announce has been consumed. A trigger that started no snapshot
// (refused, absorbed by nothing, the host not hosting) leaves the gate with no owner, and only this
// call closes it; a snapshot running, queued or deferred closes it at its own Complete.
void CloseGateIfNoSnapshot(coop::net::Session& session, int slot) {
    if (!coop::prop_snapshot::HasSnapshotFor(slot)) session.CloseJoinGate(slot);
}

// Every path that consumes an announce ends here, once: its hold on the gate is given back, then the
// gate closes if no snapshot is left to close it.
void ConsumeAnnounce(coop::net::Session& session, int slot) {
    session.ReleaseAnnounceHold(slot);
    CloseGateIfNoSnapshot(session, slot);
}

// The throwjoin drill's hold (dev::HoldSlot): the arm's intent per slot, set before the slot has an occupant, so it
// is not per occupancy; the catch below is.
bool g_held[coop::net::kMaxPeers] = {};

// A hold that has caught its slot's announce. `gen` names the occupant (Session::peerGenerationForSlot at the catch: a
// host-minted token, never reused, non-zero); `answer` and `drop` are the drill's requests, run by dev::Tick. A
// PerSlotState, so a departure or a replacement clears it with the rest of the slot's person-state.
struct HeldAnnounce {
    bool     on = false;
    bool     answer = false;
    bool     drop = false;
    uint32_t gen = 0;
    uint64_t sinceMs = 0;
};
coop::roster_ledger::PerSlotState<HeldAnnounce> g_catch;

// A catch is answered by its own bound at this age: the arm's own limit, so a join gate (which stays open from the
// announce's receipt) is never held longer than a drill can run.
constexpr uint64_t kJoinHoldMaxMs = 60000;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

}  // namespace

void OnAnnounce(coop::net::Session& session, int slot) {
    if (session.role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;

    if (!session.IsSlotWorldReady(slot)) {
        // dev::HoldSlot: the answer waits for dev::Release; the announce's gate hold stays. The catch is per occupancy
        // (cleared when the slot's occupant goes), and the hold catches ONE announce: a later occupant is never held.
        HeldAnnounce& h = g_catch[slot];
        if (h.on) {   // a second announce replaces the caught one; its own gate hold goes back, the first stays
            session.ReleaseAnnounceHold(slot);
            h.gen = session.peerGenerationForSlot(slot);
            UE_LOGI("world_ready_answer: [dev] slot %d announced again while held -- the caught announce is replaced",
                    slot);
            return;
        }
        if (g_held[slot]) {
            h = HeldAnnounce{true, false, false, session.peerGenerationForSlot(slot), NowMs()};
            g_held[slot] = false;
            return;
        }
        // The first announce of this occupancy completes the join. The join window closes for this joiner
        // here: a host pile moved before this line reconciles by its save-time key; a move after it is a
        // normal live edit. Runs free (no bucket take) -- it is the join, not a repeat.
        UE_LOGI("[PILE-1C] slot %d world-ready -- JOIN-WINDOW CLOSED (in-window host pile moves are now "
                "save-time-key reconciled by the connect replay)", slot);
        static const bool s_pileProbe =
            coop::config::ResolveFlag(::coop::config_registry::rows::pile_delta_probe);
        if (s_pileProbe)
            coop::chat_feed::Push(
                L"[1c-test] JOIN-WINDOW CLOSED -- joiner world-ready; drops from here are post-load (not in-window)",
                coop::chat_feed::Keep::Transient);
        session.MarkSlotWorldReady(slot, true);
        coop::net::fake_link_steps::NoteOrigin(coop::net::fake_link_steps::Origin::WorldReady);
        coop::subsystems::ConnectReplayForSlot(slot);
        ConsumeAnnounce(session, slot);
        // The joined-the-game line fires at the joiner's puppet appearance; this call announces only a
        // puppet that spawned before world-ready (a pre-world menu pose), a no-op otherwise.
        coop::player_handshake::OnClientWorldReady(slot);
        return;
    }

    // A later announce. The connect replay is some 64 lane calls + a walk of the whole object array
    // (npc_world_enum.cpp), so an unbounded re-announce flood would cost that per message. The bucket caps
    // it at two per occupancy (no refill): a legitimate world-change re-seed (at most one) is within the
    // cap; a flood at any pace empties the burst and the slot is ended. MTA's server ends a client at
    // its first repeated in-game notice because it owns the map change
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CRPCFunctions.cpp:94-122); here the client's
    // level travel is its own, so the host cannot see it and caps the repeat instead.
    if (g_rate[slot].Take(kReplayBudget, NowMs())) {
        coop::subsystems::ConnectReplayForSlot(slot);
        ConsumeAnnounce(session, slot);
    } else {
        UE_LOGI("world_ready_answer: slot %d re-announced world-ready past the replay burst -- ending it "
                "(MV-H34); no join announces this often", slot);
        session.Kick(slot, coop::net::EndReason::RepeatedWorldReady, "repeated world-ready");
        ConsumeAnnounce(session, slot);
    }
}

namespace dev {

namespace {
bool ValidSlot(int slot) { return slot >= 1 && slot < static_cast<int>(coop::net::kMaxPeers); }
}  // namespace

void HoldSlot(int slot, bool on) {
    if (ValidSlot(slot)) g_held[slot] = on;
}

bool Caught(int slot) { return ValidSlot(slot) && g_catch[slot].on; }

void Release(int slot) {
    if (!ValidSlot(slot)) return;
    g_held[slot] = false;
    if (g_catch[slot].on) g_catch[slot].answer = true;
}

void Forget(int slot) {
    if (!ValidSlot(slot)) return;
    g_held[slot] = false;
    if (g_catch[slot].on) g_catch[slot].drop = true;
}

void Tick(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    uint64_t now = 0;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        HeldAnnounce& h = g_catch[slot];
        if (!h.on) continue;
        // The occupant that was caught, still here: a connection that ended took its gate hold with it, and a
        // successor holds a gate of its own that this catch never touched.
        const bool same = session.IsSlotConnected(slot) && session.peerGenerationForSlot(slot) == h.gen;
        if (!same) {
            UE_LOGI("world_ready_answer: [dev] slot %d's caught announce belongs to an occupant that left -- dropped", slot);
            h = HeldAnnounce{};
            continue;
        }
        if (h.drop) {
            h = HeldAnnounce{};
            ConsumeAnnounce(session, slot);
            continue;
        }
        if (!h.answer) {
            if (now == 0) now = NowMs();
            if (now - h.sinceMs < kJoinHoldMaxMs) continue;
            UE_LOGW("world_ready_answer: [dev] slot %d's held announce is answered by its bound, %llu ms after the catch",
                    slot, static_cast<unsigned long long>(now - h.sinceMs));
        }
        h = HeldAnnounce{};
        g_held[slot] = false;
        OnAnnounce(session, slot);
    }
}

}  // namespace dev

}  // namespace coop::world_ready_answer

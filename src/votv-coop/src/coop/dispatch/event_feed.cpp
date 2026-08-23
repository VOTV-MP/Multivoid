// coop/event_feed.cpp -- the per-tick reliable-message DRAIN: per-slot join/leave
// edges, then the exhaustive ReliableKind dispatch switch. The kind->handler table
// lives HERE (one greppable place); the case BODIES for the three big families
// (entity lifecycle / keyed device state / ambient world events) live in
// event_dispatch_{entity,state,world}.cpp (2026-06-11 modularity extraction --
// this file had reached 1388 LOC, 112 from the 1500 hard cap). Core session /
// handshake / snapshot / dev-key cases stay inline below.

#include "coop/dispatch/event_feed.h"

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/world/balance_sync.h"
#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"  // IsIniKeyTrue -- v86 Path 1c JOIN-WINDOW CLOSED hands-on cue gate
#include "coop/element/registry.h"           // R-4a EPISODE_DRILL: pick 5 keyed locals to destroy
#include "coop/props/prop_element_tracker.h" // R-4a drill: GetPropElementIdForActor (skip mirrors)
#include "coop/session/world_load_episode.h" // R-4a: NoteReconcileBegin/Complete at the bracket edges
#include "ue_wrap/actors/prop.h"             // R-4a drill: IsKeyedInteractable + GetInteractableKeyString
#include "ue_wrap/engine/engine.h"           // DestroyActor (drill)
#include "ue_wrap/core/reflection.h"         // NumObjects/ObjectAt/IsLive (drill)
#include "coop/interactables/interactable_sync.h"
#include "ui/join_curtain.h"  // instant-world SEAM 1: the short curtain (Show at SnapshotBegin / dismiss at Complete)
#include "coop/session/join_progress.h"
#include "coop/element/mirror_defer.h"  // instant-world SEAM 2+3: arm deferred-hide / reveal-confirmed at the lift
#include "coop/net/session.h"
#include "coop/session/subsystems.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"  // K-6  // v75: OnSnapshotComplete (deferred-adoption ghost-sweep gate)
#include "coop/player/hand_item.h"  // v105: hotbar hand-item display axis (HandItem router)
#include "coop/player/player_damage.h"
#include "coop/creatures/wisp_tear_mirror.h"  // v72 Killer Wisp: WispGrab/WispTear receivers
#include "coop/session/player_handshake.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/props/snapshot_census.h"  // Phase 0: parse the completeness census tail on SnapshotComplete
#include "coop/props/container_contents_sync.h"  // R-4b D9: park aging anchored to the snapshot bracket
#include "coop/save/save_transfer.h"
#include "coop/moderation/seen_players.h"
#include "coop/dev/restore_vitals.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace coop::event_feed {

namespace {

// ARC A / RULE 2: g_lastReadyBySlot is GONE. It was a per-slot READY latch whose
// only reader was the falling-edge departure detector, and that detector was
// replaced by the ledger row transition -- which sees a REPLACEMENT too, where
// no falling edge exists at all (a recycled slot goes X -> Y with no absence in
// between). Leaving the latch as a write-only array would be exactly the kind of
// state that later reads as meaningful.

// "WE are the one leaving" -- a different axis from "was that peer present".
// Set when the local peer flees to the menu, so the Stop()-driven row clears
// tear person-state down without narrating four departures into the menu.
bool g_suppressLeaveLines = false;

// The "<X> left the game" line, driven by the LEDGER ROW TRANSITION. It reads
// the OUTGOING SNAPSHOT, so it no longer has to run before the nickname is
// cleared -- that ordering used to be load-bearing and documented as such. It
// also now fires for a REPLACEMENT, which a disconnect edge could not see: a
// recycled slot goes X -> Y with no absence in between.
void OnSlotReplaced_LeaveLine(int slot, const coop::roster_ledger::Row& outgoing,
                              const coop::roster_ledger::Row& /*incoming*/) {
    if (!outgoing.occupied()) return;
    if (slot == 0) return;  // the host slot is not a "peer who left"
    // The suppression flag is about NARRATION only ("WE are the one leaving", so
    // do not print four departures into the main menu). The bookkeeping below it
    // -- the last-seen stamp and the door release -- is not narration and runs
    // either way.
    if (!g_suppressLeaveLines) {
        coop::chat_feed::Push(
            (outgoing.nick.empty() ? std::wstring(L"Remote player") : outgoing.nick) +
            L" left the game",
            coop::chat_feed::Keep::History);  // a departure is part of the lobby's record
    }
    coop::seen_players::OnSlotDisconnected(slot);  // stamp last-seen (host registry)
    // HostAuth doors: release any door this departing peer held open (a door
    // still held by another peer stays open).
    coop::interactable_sync::OnPeerLeft(slot);
}

void InstallLeaveLineSubscriber() {
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced_LeaveLine);
}

}  // namespace

void SetLocalNickname(const std::wstring& nick) {
    coop::player_handshake::SetLocalNickname(nick);
}

void SuppressPeerLeaveEdges() {
    // See header: the local peer is fleeing to the menu, so the imminent
    // Stop()-driven slot disconnects must NOT be surfaced as "<X> left the game".
    // This is a DIFFERENT axis from the presence gate ("was the peer present" vs
    // "WE are the one leaving"); both are necessary preconditions for a toast, so
    // this stays even though a never-present peer is already silenced by never
    // getting a row at all (2026-07-16 /qf: the two do not overlap).
    g_suppressLeaveLines = true;
}

void OnSessionStart() {
    // File-scope per-slot state persists across Session::Stop()/Start() in
    // the same process. The harness only resets its own `g_wasConnected`
    // bit on each Start; we own the corresponding event_feed state and
    // reset it here so a restart of the session sees clean per-slot
    // edge-detector input.
    g_suppressLeaveLines = false;  // a fresh session narrates departures again
    // Register the ledger's teardown subscribers before anything can occupy a
    // row. Idempotent (the ledger dedupes), so a Stop()/Start() cycle in the
    // same process is safe.
    coop::player_handshake::InstallLedgerSubscribers();
    InstallLeaveLineSubscriber();
    coop::player_handshake::Reset();
    coop::seen_players::OnSessionStart();  // clear stale online marks (same discipline)
    coop::chat_feed::Reset();  // drop any prior session's lingering event lines
    coop::chat_sync::Reset();  // ...and the lobby's chat RECORD with it: a stop-and-
                               // re-host in one process would otherwise seed lobby B's
                               // joiner with lobby A's conversation
    coop::chat_bubbles::ResetSlots();  // 12g: no prior session's overhead bubbles either
}

void Update(net::Session& session, void* localPlayer) {
    // ORDER IS LOAD-BEARING. The reconcile runs BEFORE the reliable drain below,
    // so a peer's row already exists by the time its Join is dispatched (a Join
    // arrives over a configured lane, which is exactly the reconcile's birth
    // condition). Reversed, the first Join's nickname would be written to a slot
    // the ledger did not yet consider occupied, and silently dropped.
    // The host's own row is a PRECONDITION, not something the pump is trusted to
    // have ticked first: everything that needs the host to have an identity
    // (arbitration, the roster assertion, the ID column) calls through here.
    // Idempotent and role-gated; it also keeps row 0's name current if the host
    // renames mid-session.
    coop::roster_ledger::EnsureRowZeroSeeded(session, coop::player_handshake::LocalNickname());
    coop::roster_ledger::ReconcileFromSession(session);   // host: conform to the generations
    coop::player_handshake::PulseRosterRows(session);     // host: re-assert to clients

    // (v131) The per-tick RTT fan-out that used to live here is GONE, with
    // RemotePlayer::SetPing/GetPing/pingMs_. It pushed session.rttMsForSlot(slot)
    // into every puppet -- but a client owns only peerConns_[0], so it wrote -1
    // for every other client and their nameplates showed no ping at all. The host
    // now measures every link once and publishes it on RosterRow; the nameplate
    // and the scoreboard both read the ledger. One measured source, one owner.

    // Per-slot Join announcement + per-slot "left the game" hud. The
    // Join payload is built lazily on first need: in steady state
    // (all peers have received our Join) the joinPayload vector is
    // never constructed, so we don't pay the ToUtf8 + heap-alloc cost
    // every 8 ms at 125 Hz NetPumpTick.
    std::vector<uint8_t> joinPayload;
    bool joinPayloadBuilt = false;
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        // Both the LEFT toast AND the Join send gate on IsSlotReady (lanes
        // configured in the Connected callback), NOT IsSlotConnected (a conn
        // handle, set already in the earlier Connecting callback):
        // - LEFT toast: a peer only ever "left" if it was PRESENT, and presence
        //   requires reaching Connected+lane-config. A doomed connect stuck in
        //   Handshaking (dead/ghost host) never sets IsSlotReady, so its
        //   transport close emits no spurious "<X> left the game" (2026-07-16).
        //   IsSlotReady is cleared on disconnect too, so it is an equally valid
        //   present->absent signal as the old IsSlotConnected -- net_pump's
        //   disconnect edge already uses exactly this.
        // - Send Join: must wait for IsSlotReady, otherwise SendReliableToSlot's
        //   m_idxLane rides GNS lane 0 instead of the assigned Normal lane,
        //   undermining PR-3's head-of-line isolation for the first reliable
        //   message per peer. N-4 (2026-05-29 audit).
        const bool slotReady = session.IsSlotReady(slot);
        // The departure half of this edge is GONE (arc A): a falling
        // IsSlotReady cannot express a REPLACEMENT, and there is no edge at all
        // when a slot is refilled between two ticks. Departure is now a ledger
        // row transition -- see OnSlotReplaced_LeaveLine above.
        if (slotReady) {
            coop::player_handshake::MaybeSendJoinToSlot(
                session, slot, joinPayload, joinPayloadBuilt);
        }
    }

    // Drain delivered reliable messages. The switch handles the inline special
    // cases (handshake / dev-key / snapshot / balance) explicitly; everything
    // else falls to the default, which chains the three Handle*Event family
    // routers (each returns true iff it owns msg.kind -- the family's own switch
    // is the single membership declaration; see event_dispatch.h).
    net::Session::ReliableMessage msg;
    while (session.TryGetReliable(msg)) {
        switch (msg.kind) {
        case net::ReliableKind::Join: {
            coop::player_handshake::HandleJoinMessage(session, msg);
            break;
        }
        case net::ReliableKind::SaveTransferRequest: {
            // v56: a menu-mode joiner asks for the world save. Host-only intake;
            // the stream itself is pumped by net_pump (save_transfer::TickHost).
            if (session.role() == net::Role::Host &&
                msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers) {
                coop::save_transfer::OnRequest(msg.senderPeerSlot);
            }
            break;
        }
        // SaveTransferBegin is NOT handled here. W3 (docs/security/PLAN_02_WIRE_HARDENING.md) moved
        // it to the net thread beside the chunks it announces (session.cpp's divert ->
        // save_transfer's BeginSink_), because draining the announce on the GAME thread while its
        // payload landed on the net thread is what let a hostile host accumulate bytes with no
        // announced size. This case was retired with the move rather than left as a second path
        // (RULE 2) -- the two paths were the bug.
        case net::ReliableKind::ClientWorldReady: {
            // v56: the joiner's world is up + registry-coherent -- open the
            // world-ready send gate and run the connect replay NOW (this
            // replaces the pre-v56 connect-edge replay).
            if (session.role() == net::Role::Host &&
                msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers) {
                // v86 Path 1c hands-on marker: the joiner just hit load-100%, so the JOIN-WINDOW
                // is now CLOSED for it. A host pile MOVED before this line is in-window (its save-time
                // key reconciles the client native); a move AFTER this is a normal post-load live edit.
                // The test drops piles BEFORE this marker appears in the host log.
                UE_LOGI("[PILE-1C] slot %d world-ready -- JOIN-WINDOW CLOSED (in-window host pile moves are "
                        "now save-time-key reconciled by the connect replay)", msg.senderPeerSlot);
                static const bool s_pileProbe =
                    coop::config::ResolveFlag(::coop::config_registry::rows::pile_delta_probe);
                if (s_pileProbe)
                    coop::chat_feed::Push(L"[1c-test] JOIN-WINDOW CLOSED -- joiner world-ready; drops from here are post-load (not in-window)",
                                          coop::chat_feed::Keep::Transient);
                session.MarkSlotWorldReady(msg.senderPeerSlot, true);
                coop::subsystems::ConnectReplayForSlot(msg.senderPeerSlot);
                // Join-line reverse-order cover (2026-07-03): the "<nick> joined the game" line now
                // fires at the joiner's PUPPET APPEARANCE (net_pump's spawn seam, world-ready-gated
                // on the host). This call only announces when the puppet spawned BEFORE world-ready
                // (pre-world menu pose, user 2026-06-17) -- normal flow makes it a no-op.
                coop::player_handshake::OnClientWorldReady(msg.senderPeerSlot);
            }
            break;
        }

        case net::ReliableKind::RestoreVitals: {
            // 2026-05-25 LATE +5h (F3 dev key): peer pressed F3 to refill
            // food/sleep/health/coffeePower. No payload to validate -- the
            // action is fixed. Idempotent so an echo bounce is harmless.
            // RestoreVitals is a dev-key path (host presses F3); receiver
            // must enforce host-only origin or any peer could trivially
            // nullify hunger/survival tension.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: RestoreVitals from non-host senderPeerSlot=%d "
                        "-- dropping (host-only dev-key origin)",
                        msg.senderPeerSlot);
                break;
            }
            ue_wrap::game_thread::Post([] { ::coop::dev::restore_vitals::ApplyLocally(); });
            break;
        }
        case net::ReliableKind::PlayerDamage: {
            // vitals Inc3-WIRE: the host detected an enemy hitting THIS peer's puppet
            // and relayed the damage for us to apply to our OWN player. Host-only
            // origin (only the host runs enemies); a client never legitimately sends
            // it, and it is not in the relay whitelist so it is never forwarded. The
            // owner-side targetElementId check + the apply live in player_damage.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: PlayerDamage from non-host senderPeerSlot=%d "
                        "-- dropping (host-only combat origin)", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::PlayerDamagePayload)) {
                UE_LOGW("event_feed: PlayerDamage payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PlayerDamagePayload));
                break;
            }
            net::PlayerDamagePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::player_damage::OnWireDamage(p);
            break;
        }
        case net::ReliableKind::WispGrab: {
            // v72 Killer Wisp: the host's killerwisp false-grabbed THIS peer's puppet; the
            // host neutralized its own death and tells us to ragdoll-die for real after a
            // fixed delay. Host-only origin (only the host runs the wisp + the detect);
            // wisp_tear_mirror self-verifies the addressed Player Element id. Not relayed.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: WispGrab from non-host senderPeerSlot=%d -- dropping "
                        "(host-only wisp origin)", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::WispGrabPayload)) {
                UE_LOGW("event_feed: WispGrab payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::WispGrabPayload));
                break;
            }
            net::WispGrabPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            const uint8_t sender = static_cast<uint8_t>(msg.senderPeerSlot);
            ue_wrap::game_thread::Post([p, sender] { coop::wisp_tear_mirror::OnWispGrab(p, sender); });
            break;
        }
        case net::ReliableKind::WispTear: {
            // v72 Killer Wisp: play the fatality tear on the LOCAL wisp mirror + (deferred)
            // hold the victim's puppet. Host-only origin; resolves the wisp by element id;
            // not relayed.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: WispTear from non-host senderPeerSlot=%d -- dropping "
                        "(host-only wisp origin)", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::WispTearPayload)) {
                UE_LOGW("event_feed: WispTear payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::WispTearPayload));
                break;
            }
            net::WispTearPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            const uint8_t sender = static_cast<uint8_t>(msg.senderPeerSlot);
            ue_wrap::game_thread::Post([p, sender] { coop::wisp_tear_mirror::OnWispTear(p, sender); });
            break;
        }
        case net::ReliableKind::BalanceSync: {
            // v30 shared balance: the host broadcast its canonical Points -- mirror it.
            // Host-only origin (the host owns the balance); a client never sends it.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: BalanceSync from non-host senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::BalancePayload)) {
                UE_LOGW("event_feed: BalanceSync payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::BalancePayload));
                break;
            }
            net::BalancePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::balance_sync::ApplyFromHost(p.value);  // no-op on the host (authoritative)
            break;
        }
        case net::ReliableKind::BalanceDelta: {
            // v30 shared balance: a client requested a credit (the +1000 dev button) --
            // the host applies it via AddPoints (its poll re-broadcasts the new total).
            // Client->host; OnDeltaRequest no-ops unless we are the host.
            if (msg.payloadLen < sizeof(net::BalancePayload)) {
                UE_LOGW("event_feed: BalanceDelta payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::BalancePayload));
                break;
            }
            net::BalancePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::balance_sync::OnDeltaRequest(p.value);
            break;
        }
        case net::ReliableKind::TeleportClient: {
            // 2026-05-25 LATE +5h (F4 dev key): host snapshotted its pose and
            // sent it; client applies to local mainPlayer. Host echo is
            // a no-op below.
            if (msg.payloadLen < sizeof(net::TeleportClientPayload)) {
                UE_LOGW("event_feed: TeleportClient payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::TeleportClientPayload));
                break;
            }
            // TeleportClient is host-only. Without a senderPeerSlot trust
            // gate, in a 3-peer session client-slot-1 could craft a
            // TeleportClient targeting client-slot-2 (host's PollGroup
            // fan-out would deliver it) -- a positional griefing/exploit
            // vector at LAN scale, hard exploit at internet scale.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: TeleportClient from non-host senderPeerSlot=%d "
                        "-- dropping (host-only)",
                        msg.senderPeerSlot);
                break;
            }
            net::TeleportClientPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary validation (same defensive pattern as the
            // PropRelease velocity check): reject NaN/Inf before the engine
            // call. UE's K2_TeleportTo with a NaN location asserts inside
            // FSweepData::ClampSweepParameters.
            const float vals[6] = {p.locX, p.locY, p.locZ, p.rotPitch, p.rotYaw, p.rotRoll};
            bool finite = true;
            for (float v : vals) { if (!std::isfinite(v)) { finite = false; break; } }
            if (!finite) {
                UE_LOGW("event_feed: TeleportClient payload non-finite -- dropping");
                break;
            }
            // AABB bound (audit-fix 2026-05-25 LATE +5h): finite-check alone
            // allows extreme-but-finite coords (e.g. 1e30) that would still
            // assert inside the engine's teleport math. Mirror the project's
            // own kMaxCoord = 1.0e6f trust boundary from ValidatePose /
            // PropSpawnPayload receiver -- one consistent magnitude rule for
            // any world-position payload. Rotations don't need a magnitude
            // bound because FRotator components are angles (any value is
            // normalized inside K2_TeleportTo).
            if (std::fabs(p.locX) > net::kMaxCoord ||
                std::fabs(p.locY) > net::kMaxCoord ||
                std::fabs(p.locZ) > net::kMaxCoord) {
                UE_LOGW("event_feed: TeleportClient location out of bounds (%.1f,%.1f,%.1f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            // Host echo gate: if WE are the host, this packet originated from
            // us (broadcast bounced back via the reliable channel). Applying
            // would teleport host to its own pose -- harmless but pointless.
            // Skip explicitly so we don't accidentally collide with whatever
            // host was doing the moment it pressed F4.
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: TeleportClient self-echo on host -- no-op");
                break;
            }
            ::coop::teleport_client::ApplyArgs args{
                p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll,
            };
            ue_wrap::game_thread::Post([args] { ::coop::teleport_client::ApplyLocally(args); });
            break;
        }
        case net::ReliableKind::SnapshotBegin: {
            // v34: host opened the connect-snapshot -- flip the client loading screen from
            // "Connecting" to a determinate "Receiving world X/N" bar. Host-only, like
            // TeleportClient: a client peer must not be able to spoof another's loading UI.
            if (msg.payloadLen < sizeof(net::SnapshotBeginPayload)) {
                UE_LOGW("event_feed: SnapshotBegin payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::SnapshotBeginPayload));
                break;
            }
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: SnapshotBegin from non-host senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
                break;
            }
            if (session.role() == net::Role::Host) break;  // self-echo guard (host doesn't load-screen)
            net::SnapshotBeginPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::join_progress::BeginSnapshot(p.propTotal);
            // instant-world ARM (SEAM 1+2): the host snapshot is starting to arrive (client-side; the spawn
            // burst follows). Raise the curtain + open the deferred-hide window so every host mirror spawned
            // below is hidden until the lift (SnapshotComplete) / quiescence reveal. Backup untouched -- this
            // only controls VISIBILITY (worst case = today's dance, best case = instant).
            coop::join_curtain::Show();
            coop::mirror_defer::Arm();
            // P2 (2026-06-10): arm the connect-snapshot claim set. Every
            // PropSpawn dispatched below (inline, same drain, same lane ->
            // strictly after this) claims the client actor it binds; the
            // SnapshotComplete sweep destroys the unclaimed RNG-divergent
            // locals so the client adopts the host's world layout.
            //
            // Runs for EVERY join, INCLUDING a live-capture save-transfer (2026-06-15
            // kerfur-dupe root-cause fix). The live-save blob is captured at one
            // instant, but the connect snapshot is enumerated LATER (props + NPCs at
            // world-ready) and the client's loadObjects materializes the ~19 MB world
            // ASYNC over many seconds -- so the two DIVERGE: a kerfur the host had ON
            // loads as a stale OFF object in the blob while the snapshot delivers the
            // live ON NPC. Reconciling that divergence is EXACTLY this claim set +
            // quiescence-gated sweep's job. (The earlier "skip the sweep on a live-
            // capture join" was the crutch that re-introduced the dupe; the world-wipe
            // it guarded against is now prevented by the >50% safety valve in
            // RunDivergenceSweep_, not by skipping the reconcile.)
            coop::join_membership_sweep::BeginClaimTracking();
            // R-4b D9: while the bracket is open, container-contents parks do
            // not age (their PropSpawns are still in the Bulk stream behind us).
            coop::props::container_contents_sync::NoteJoinSnapshotBracket(true);
            // R-4a end-condition: the bracket apply window opens -- raise/refresh the
            // reconcile window (kind classified by completeSinceArm; see world_load_episode.h).
            coop::world_load_episode::NoteReconcileBegin();
            // [drill] R-4a leg 1 (VOTVCOOP_EPISODE_DRILL=1): destroy 5 keyed locals RIGHT
            // HERE -- inside the bracket window where the field's class-B churn ran. OLD
            // close edge -> 5 "broadcasting DESTROY" lines (RED = the field reproduced);
            // NEW -> 5 "suppressed" + 0 broadcasts (GREEN). Never set outside a drill.
            {
                static const bool sDrill =
                    !coop::config::ReadEnv("VOTVCOOP_EPISODE_DRILL").empty();
                static bool sFired = false;
                if (sDrill && !sFired) {
                    sFired = true;
                    // Target CLIENT-LOCAL keyed props: on a v122 client the element Registry
                    // holds only host-bound MIRRORS (the first drill cut destroyed five pile
                    // mirrors, which the mirror layer healed without touching the seam's
                    // broadcast path). A local = keyed interactable with NO element bound.
                    // One-shot dev walk; drill-only cost.
                    int killed = 0;
                    const int32_t n = ue_wrap::reflection::NumObjects();
                    for (int32_t i = 0; i < n && killed < 5; ++i) {
                        void* obj = ue_wrap::reflection::ObjectAt(i);
                        if (!obj) continue;
                        if (!ue_wrap::prop::IsKeyedInteractable(obj)) continue;
                        if (!ue_wrap::reflection::IsLive(obj)) continue;
                        const std::wstring nm =
                            ue_wrap::reflection::ToString(ue_wrap::reflection::NameOf(obj));
                        if (nm.rfind(L"Default__", 0) == 0) continue;
                        if (coop::prop_element_tracker::GetPropElementIdForActor(obj) !=
                            coop::element::kInvalidId) continue;  // element-bound = a mirror, skip
                        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
                        if (key.empty() || key == L"None") continue;  // keyed locals only
                        UE_LOGW("event_feed: [drill] EPISODE_DRILL destroying keyed LOCAL %p "
                                "key='%ls' inside the bracket window", obj, key.c_str());
                        ue_wrap::engine::DestroyActor(obj);  // UNMARKED -> hits the seam organically
                        ++killed;
                    }
                }
            }
            break;
        }
        case net::ReliableKind::SnapshotComplete: {
            // v34: host finished draining the world snapshot (the LAST Lane::Bulk message
            // after every PropSpawn) -- lift the loading screen. THE hide edge.
            if (msg.payloadLen < sizeof(net::SnapshotEndPayload)) {
                UE_LOGW("event_feed: SnapshotComplete payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::SnapshotEndPayload));
                break;
            }
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: SnapshotComplete from non-host senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
                break;
            }
            if (session.role() == net::Role::Host) break;
            // Phase 0 (docs/COOP_STABLE_ID_SIDECAR.md S4): parse the optional per-class completeness
            // census appended after SnapshotEndPayload. The deferred claim sweep (armed below) reads
            // it to KEEP any class the host did not express completely (the docs/piles/10 guard).
            if (msg.payloadLen > sizeof(net::SnapshotEndPayload)) {
                coop::snapshot_census::SetFromWire(
                    reinterpret_cast<const uint8_t*>(msg.payload) + sizeof(net::SnapshotEndPayload),
                    static_cast<size_t>(msg.payloadLen) - sizeof(net::SnapshotEndPayload));
            }
            coop::join_progress::Complete();
            // R-4b D9: bracket closed -- every PropSpawn it carried has been
            // dispatched above (same lane, strictly before this). Parks
            // re-stamp; the TTL runs from here as a leak-guard.
            coop::props::container_contents_sync::NoteJoinSnapshotBracket(false);
            // instant-world curtain LIFT (SEAM 1+3): the primary world is assembled -- every host
            // PropSpawn/EntitySpawn in this snapshot has been applied above. Fade the curtain out and reveal
            // the CONFIRMED mirrors NOW (~2s BEFORE quiescence -> the world fades in already-assembled, no
            // long blank). The HELD tail (save-time-keyed forms whose local twin is still visible) stays
            // hidden -> the quiescence backstop reveals it after the sweep armed below resolves the dups.
            coop::join_curtain::BeginDismiss();
            coop::mirror_defer::RevealConfirmedAtLift();
            // P2 (2026-06-10): the snapshot drained -- every host prop has
            // claimed its client actor. The unclaimed RNG-divergent locals (the
            // client's own fresh-New-Game litter, OR the stale blob objects the
            // host's live snapshot does not claim -- the kerfur the host turned
            // ON) are swept -- but NOT inline here. A save-loaded prop has its key
            // restored on a LATE loadData tail, and the kerfur NPCs respawn SECONDS
            // after that, so an inline sweep would keyless-skip them into ghosts.
            // ARM a deferred, load-tail-quiescence-gated sweep (TickClientReconcile
            // fires it once the keyless-prop AND allowlisted-NPC populations stop
            // changing = the async load has fully settled). localPlayer is re-
            // resolved at sweep time. Runs for EVERY join incl. live-capture -- see
            // the SnapshotBegin note; the >50% valve guards the world-wipe.
            coop::join_membership_sweep::ArmDivergenceSweep();
            // v75: the connect snapshot is fully delivered, so every save-persisted EntitySpawn
            // (the kerfur) has arrived + armed its deferred adoption. Tell npc_adoption: once those
            // pending adoptions converge it may run its one-shot ghost sweep (the NPC analogue of
            // the prop divergence sweep armed above) -- gated on the SAME load-tail quiescence, so a
            // still-loading local twin is adopted, never swept or fresh-spawned into a duplicate.
            coop::npc_adoption::OnSnapshotComplete();
            coop::kerfur_prop_adoption::OnSnapshotComplete();  // K-6 parity (reserved; no-op today)
            // R-4a end-condition: the bracket apply window closes -- lower the reconcile
            // window + flip the kind classifier (unconditionally, even post-ceiling).
            coop::world_load_episode::NoteReconcileComplete();
            break;
        }

        case net::ReliableKind::AssignPeerSlot: {
            coop::player_handshake::HandleAssignPeerSlot(session, msg);
            break;
        }
        case net::ReliableKind::RosterRow: {
            // The host asserting who occupies a slot (arc A). STATE, not an
            // event: re-sent by the repair pulse, so the handler applies
            // idempotently.
            coop::player_handshake::HandleRosterRow(session, msg);
            break;
        }
        case net::ReliableKind::SkinChange: {
            // v93 skins: store + live re-skin the described slot's puppet; the
            // handler also does the host's rebroadcast to the other clients.
            coop::player_handshake::HandleSkinChange(session, msg);
            break;
        }
        case net::ReliableKind::NameplateChange: {
            // v94: per-peer plate visibility pref -> coop::nameplate store (the
            // handler also does the host's rebroadcast).
            coop::player_handshake::HandleNameplateChange(session, msg);
            break;
        }
        case net::ReliableKind::NickColorChange: {
            // v103 (12f): per-peer nick color pref -> coop::nick_color store (the
            // handler also does the host's rebroadcast).
            coop::player_handshake::HandleNickColorChange(session, msg);
            break;
        }
        case net::ReliableKind::HandItem: {
            // v105: the hotbar hand-item display state -> coop::hand_item store
            // (the handler also does the host's rebroadcast; TickMirrors applies).
            coop::hand_item::HandleHandItem(session, msg);
            break;
        }
        default: {
            // ---- the family routers (event_dispatch_{entity,state,world}.cpp) ----
            // SyncRouter consolidation (2026-06-28): the explicit family case-label
            // lists that used to sit in THIS switch were one of three places a new
            // ReliableKind had to be wired (enum + here + the family's own switch),
            // the documented 3-place hazard ([[feedback-reliablekind-router-checklist]]).
            // Each Handle*Event now returns true iff msg.kind is in its family
            // (processed or validation-dropped), so the family switch IS the single
            // membership declaration; we just chain them. The inline special cases
            // above (Join/Balance/Teleport/Wisp/Snapshot/handshake) are disjoint from
            // every family and take priority via their own case labels.
            if (HandleEntityEvent(session, msg, localPlayer)) break;
            if (HandleStateEvent(session, msg, localPlayer)) break;
            if (HandleSignalEvent(session, msg)) break;
            if (HandleIntentEvent(session, msg, localPlayer)) break;
            if (HandleWorldEvent(session, msg)) break;
            // Audit-fix 2026-05-25 LATE +5h: log-and-drop unknown ReliableKind
            // values instead of silently discarding. A peer running a newer
            // protocol could send a kind we don't yet handle; this surfaces
            // the gap in the log rather than letting it look like nothing
            // happened. Existing pattern across the project.
            UE_LOGW("event_feed: unknown ReliableKind %u -- dropping",
                    static_cast<unsigned>(msg.kind));
            break;
        }
        }
    }
}

}  // namespace coop::event_feed

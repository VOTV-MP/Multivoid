// coop/dispatch/event_feed.cpp -- the per-tick reliable-message drain: the per-slot Join edge, then the
// ReliableKind dispatch switch. The kind-to-handler table lives here; the case bodies for the
// five families (entity, state, signal, intent, world) live in event_dispatch_<family>.cpp.
// Session, handshake, snapshot and dev-key cases stay inline.

#include "coop/dispatch/event_feed.h"

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/world/balance_sync.h"
#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"  // ResolveFlag, ReadEnv
#include "coop/element/registry.h"           // kInvalidId, for the drill
#include "coop/props/prop_element_tracker.h"  // GetPropElementIdForActor, for the drill
#include "coop/session/world_load_episode.h"  // NoteReconcileBegin and Complete at the bracket edges
#include "ue_wrap/actors/prop.h"             // IsKeyedInteractable, GetInteractableKeyString
#include "ue_wrap/engine/engine.h"           // DestroyActor (drill)
#include "ue_wrap/core/reflection.h"         // NumObjects/ObjectAt/IsLive (drill)
#include "coop/interactables/interactable_sync.h"
#include "ui/join_curtain.h"  // the curtain: Show at SnapshotBegin, dismiss at Complete
#include "coop/session/join_progress.h"
#include "coop/element/mirror_defer.h"  // deferred hide, armed at Begin and revealed at the lift
#include "coop/net/session.h"
#include "coop/session/subsystems.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"  // OnSnapshotComplete
#include "coop/player/hand_item.h"  // the hand-item handler
#include "coop/player/player_damage.h"
#include "coop/creatures/wisp_tear_mirror.h"  // WispGrab and WispTear receivers
#include "coop/player/local_body.h"     // the host's own skin belongs in its own roster row
#include "coop/player/nick_color.h"  // ResetSlots() at bringup -- a colour lands before its row
#include "coop/session/player_handshake.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // the claim set and the divergence sweep
#include "coop/props/snapshot_census.h"  // the completeness census tail on SnapshotComplete
#include "coop/props/container_contents_sync.h"  // park ageing anchored to the snapshot bracket
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

// Set when the local peer leaves for the menu, so the Stop-driven row clears tear state down
// without narrating every departure.
bool g_suppressLeaveLines = false;

// The "<X> left the game" line, driven by the ledger row transition. It reads the outgoing
// snapshot, so it needs no ordering against the nickname clear, and it fires for a replacement
// too (a recycled slot goes X to Y with no absence between).
void OnSlotReplaced_LeaveLine(int slot, const coop::roster_ledger::Row& outgoing,
                              const coop::roster_ledger::Row& /*incoming*/) {
    if (!outgoing.occupied()) return;
    if (slot == 0) return;  // the host slot is not a "peer who left"
    // The flag suppresses the narration only; the last-seen stamp and the door release run either
    // way.
    if (!g_suppressLeaveLines) {
        coop::chat_feed::Push(
            (outgoing.nick.empty() ? std::wstring(L"Remote player") : outgoing.nick) +
            L" left the game",
            coop::chat_feed::Keep::History);  // a departure is part of the lobby's record
    }
    coop::seen_players::OnSlotDisconnected(slot);  // stamp last-seen (host registry)
    // Doors this peer held open are released (one still held by another peer stays open).
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
    // The local peer is leaving for the menu, so the Stop-driven slot disconnects must not read as
    // departures. A distinct axis from presence: a never-present peer gets no row at all, and both
    // conditions gate the line.
    g_suppressLeaveLines = true;
}

void OnSessionStart() {
    // File-scope state persists across Session::Stop and Start in one process; a restarted session
    // starts clean here.
    g_suppressLeaveLines = false;  // a fresh session narrates departures again
    // The ledger's teardown subscribers, registered before anything can occupy a row; the ledger
    // dedupes, so a Stop/Start cycle is safe.
    coop::player_handshake::InstallLedgerSubscribers();
    InstallLeaveLineSubscriber();
    coop::player_handshake::Reset();
    coop::seen_players::OnSessionStart();  // clear stale online marks (same discipline)
    coop::chat_feed::Reset();  // drop any prior session's lingering event lines
    coop::chat_sync::Reset();  // and the chat record: a re-host in one process would otherwise seed the new lobby with the old talk
    coop::chat_bubbles::ResetSlots();  // no prior session's bubbles either
    coop::nick_color::ResetSlots();  // nor a prior peer's nick colour
}

void Update(net::Session& session, void* localPlayer) {
    // Order matters: the reconcile runs before the drain, so a peer's row exists by the time its
    // Join is dispatched (a Join arrives over a configured lane, the reconcile's birth condition);
    // reversed, the first Join's nickname would land on a slot the ledger did not consider occupied
    // and be dropped. Row zero is seeded here as a precondition rather than left to the pump;
    // idempotent and role-gated, it also keeps the host's name current.
    coop::roster_ledger::EnsureRowZeroSeeded(session, coop::player_handshake::LocalNickname(),
                                             coop::local_body::LocalSkinName());
    coop::roster_ledger::ReconcileFromSession(session);   // host: conform to the generations
    coop::player_handshake::PulseRosterRows(session);     // host: re-assert to clients

    // The per-slot Join send. The payload is built on first need: in steady state the vector is
    // never constructed, so the pump tick does not pay the UTF-8 conversion and allocation.
    std::vector<uint8_t> joinPayload;
    bool joinPayloadBuilt = false;
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        // The Join send gates on IsSlotReady (lanes configured in the Connected callback), not
        // IsSlotConnected (a handle set in the earlier Connecting callback): sent earlier, the
        // first reliable message would ride GNS lane 0 instead of the assigned lane and lose the
        // head-of-line isolation.
        const bool slotReady = session.IsSlotReady(slot);
        // Departure is a ledger row transition (OnSlotReplaced_LeaveLine above): a falling
        // IsSlotReady cannot express a slot refilled between two ticks.
        if (slotReady) {
            coop::player_handshake::MaybeSendJoinToSlot(
                session, slot, joinPayload, joinPayloadBuilt);
        }
    }

    // The drain. The switch handles the inline cases; everything else falls to the default, which
    // chains the family routers (each returns true iff it owns the kind, so the family's own switch
    // is the single membership declaration).
    net::Session::ReliableMessage msg;
    while (session.TryGetReliable(msg)) {
        switch (msg.kind) {
        case net::ReliableKind::Join: {
            coop::player_handshake::HandleJoinMessage(session, msg);
            break;
        }
        case net::ReliableKind::SaveTransferRequest: {
            // A menu-mode joiner asks for the world save. Host-only intake; net_pump pumps the
            // stream itself.
            if (session.role() == net::Role::Host &&
                msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers) {
                coop::save_transfer::OnRequest(msg.senderPeerSlot);
            }
            break;
        }
        // SaveTransferBegin is not handled here: it is diverted to the net thread beside the chunks
        // it announces (save_transfer's begin sink), since draining the announce on the game thread
        // while the payload landed on the net thread let a hostile host accumulate bytes with no
        // announced size.
        case net::ReliableKind::ClientWorldReady: {
            // The joiner's world is up and registry-coherent: the world-ready send gate opens and
            // the connect replay runs.
            if (session.role() == net::Role::Host &&
                msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers) {
                // The join window closes for this joiner here: a host pile moved before this line
                // reconciles by its save-time key; a move after it is a normal live edit.
                UE_LOGI("[PILE-1C] slot %d world-ready -- JOIN-WINDOW CLOSED (in-window host pile moves are "
                        "now save-time-key reconciled by the connect replay)", msg.senderPeerSlot);
                static const bool s_pileProbe =
                    coop::config::ResolveFlag(::coop::config_registry::rows::pile_delta_probe);
                if (s_pileProbe)
                    coop::chat_feed::Push(L"[1c-test] JOIN-WINDOW CLOSED -- joiner world-ready; drops from here are post-load (not in-window)",
                                          coop::chat_feed::Keep::Transient);
                session.MarkSlotWorldReady(msg.senderPeerSlot, true);
                coop::subsystems::ConnectReplayForSlot(msg.senderPeerSlot);
                // The joined-the-game line fires at the joiner's puppet appearance; this call
                // announces only a puppet that spawned before world-ready (a pre-world menu pose),
                // a no-op otherwise.
                coop::player_handshake::OnClientWorldReady(msg.senderPeerSlot);
            }
            break;
        }

        case net::ReliableKind::RestoreVitals: {
            // The dev refill of food, sleep and health (coffeePower is deliberately excluded). No
            // payload; idempotent, so an echo is harmless. Host-only origin, or any peer could
            // nullify the survival tension.
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
            // The host detected an enemy hitting this peer's puppet and relays the damage for the
            // owner to apply. Host-only origin (only the host runs enemies); not in the relay
            // whitelist. The target id check and the apply live in player_damage.
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
            // The host's killer wisp grabbed this peer's puppet; the host neutralised its own death
            // and tells the owner to ragdoll-die after a fixed delay. Host-only origin;
            // wisp_tear_mirror verifies the addressed element id. Not relayed.
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
            // The fatality tear on the local wisp mirror, then the deferred hold of the victim's
            // puppet. Host-only origin; the wisp is resolved by element id; not relayed.
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
            // The host's canonical balance, mirrored. Host-only origin.
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
        case net::ReliableKind::TeleportClient: {
            // The host's teleport-to-me dev action: its pose, applied to the client's local player.
            // The host's own echo is a no-op below.
            if (msg.payloadLen < sizeof(net::TeleportClientPayload)) {
                UE_LOGW("event_feed: TeleportClient payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::TeleportClientPayload));
                break;
            }
            // Host-only: without the sender gate one client could teleport another through the
            // host's fan-out.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: TeleportClient from non-host senderPeerSlot=%d "
                        "-- dropping (host-only)",
                        msg.senderPeerSlot);
                break;
            }
            net::TeleportClientPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // NaN and Inf rejected before the engine call: K2_TeleportTo with a NaN location
            // asserts inside FSweepData::ClampSweepParameters.
            const float vals[6] = {p.locX, p.locY, p.locZ, p.rotPitch, p.rotYaw, p.rotRoll};
            bool finite = true;
            for (float v : vals) { if (!std::isfinite(v)) { finite = false; break; } }
            if (!finite) {
                UE_LOGW("event_feed: TeleportClient payload non-finite -- dropping");
                break;
            }
            // The magnitude bound: an extreme finite coordinate still asserts inside the teleport
            // math, so the location is held to kMaxCoord like every other world-position payload.
            // Rotations are angles, normalised inside K2_TeleportTo.
            if (std::fabs(p.locX) > net::kMaxCoord ||
                std::fabs(p.locY) > net::kMaxCoord ||
                std::fabs(p.locZ) > net::kMaxCoord) {
                UE_LOGW("event_feed: TeleportClient location out of bounds (%.1f,%.1f,%.1f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            // The host's own broadcast bounces back; applying it would teleport the host to its own
            // pose.
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
            // The host opened the connect snapshot: the client's loading screen becomes a
            // determinate "Receiving world X/N" bar. Host-only: a client must not spoof another's
            // loading UI.
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
            // The curtain rises and the deferred-hide window opens, so every host mirror spawned by
            // the burst below stays hidden until the lift or the quiescence reveal. Visibility
            // only.
            coop::join_curtain::Show();
            coop::mirror_defer::Arm();
            // The claim set is armed: every PropSpawn dispatched below (same drain, same lane,
            // strictly after this) claims the client actor it binds, and the sweep armed at
            // SnapshotComplete destroys the unclaimed divergent locals so the client adopts the
            // host's layout. It runs for every join, a live-capture save transfer included: the
            // blob is captured at one instant, the snapshot is enumerated later at world-ready, and
            // the client materialises the world asynchronously over seconds, so the two diverge (a
            // kerfur the host had on loads as a stale off object while the snapshot delivers the
            // live one). The sweep's over-50% valve, not a skip, guards against a world wipe.
            coop::join_membership_sweep::BeginClaimTracking();
            // While the bracket is open, container-contents parks do not age: their PropSpawns are
            // still in the Bulk stream behind us.
            coop::props::container_contents_sync::NoteJoinSnapshotBracket(true);
            // The bracket's apply window opens: the reconcile window is raised or refreshed (see
            // world_load_episode.h).
            coop::world_load_episode::NoteReconcileBegin();
            // The episode drill (VOTVCOOP_EPISODE_DRILL=1): five keyed locals destroyed inside the
            // bracket window, where the field's churn ran; the seam must suppress their broadcasts.
            // Never set outside a drill.
            {
                static const bool sDrill =
                    !coop::config::ReadEnv("VOTVCOOP_EPISODE_DRILL").empty();
                static bool sFired = false;
                if (sDrill && !sFired) {
                    sFired = true;
                    // Client-local keyed props only: the element registry holds host-bound mirrors,
                    // which the mirror layer heals without touching the seam. A local is a keyed
                    // interactable with no element bound. A one-shot walk, drill-only cost.
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
                        ue_wrap::engine::DestroyActor(obj);  // unmarked, so it hits the seam as a real destroy would
                        ++killed;
                    }
                }
            }
            break;
        }
        case net::ReliableKind::SnapshotComplete: {
            // The host finished the world snapshot (the last Bulk-lane message after every
            // PropSpawn): the loading screen lifts.
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
            // The optional per-class completeness census appended after the payload; the deferred
            // claim sweep keeps any class the host did not express completely.
            if (msg.payloadLen > sizeof(net::SnapshotEndPayload)) {
                coop::snapshot_census::SetFromWire(
                    reinterpret_cast<const uint8_t*>(msg.payload) + sizeof(net::SnapshotEndPayload),
                    static_cast<size_t>(msg.payloadLen) - sizeof(net::SnapshotEndPayload));
            }
            coop::join_progress::Complete();
            // The bracket closed: every PropSpawn it carried was dispatched above. Parks re-stamp,
            // and their TTL runs from here as a leak guard.
            coop::props::container_contents_sync::NoteJoinSnapshotBracket(false);
            // The lift: every host spawn in the snapshot has been applied above, so the curtain
            // fades and the confirmed mirrors are revealed now, about 2 s before quiescence. The
            // held tail (save-time-keyed forms whose local twin is still visible) stays hidden
            // until the sweep armed below resolves the duplicates and the quiescence backstop
            // reveals it.
            coop::join_curtain::BeginDismiss();
            coop::mirror_defer::RevealConfirmedAtLift();
            // Every host prop has claimed its client actor; the unclaimed divergent locals (the
            // client's own fresh-game litter, or stale blob objects the live snapshot does not
            // claim) are swept, but not inline: a save-loaded prop gets its key restored on a late
            // load tail, and the kerfur NPCs respawn seconds after, so an inline sweep would skip
            // them into ghosts. The sweep is deferred until the keyless-prop and allowlisted-NPC
            // populations stop changing; localPlayer is re-resolved at sweep time.
            coop::join_membership_sweep::ArmDivergenceSweep();
            // Every save-persisted EntitySpawn (the kerfur) has arrived and armed its deferred
            // adoption; once those converge npc_adoption may run its one-shot ghost sweep, gated on
            // the same load-tail quiescence, so a still-loading local twin is adopted rather than
            // swept or duplicated.
            coop::npc_adoption::OnSnapshotComplete();
            coop::kerfur_prop_adoption::OnSnapshotComplete();  // informational; no sweep of its own
            // The apply window closes: the reconcile window is lowered and the kind classifier
            // flipped, even past the ceiling.
            coop::world_load_episode::NoteReconcileComplete();
            break;
        }

        case net::ReliableKind::AssignPeerSlot: {
            coop::player_handshake::HandleAssignPeerSlot(session, msg);
            break;
        }
        case net::ReliableKind::RosterRow: {
            // The host asserting who occupies a slot. State, not an event: the repair pulse
            // re-sends it, so the handler applies idempotently.
            coop::player_handshake::HandleRosterRow(session, msg);
            break;
        }
        case net::ReliableKind::SkinChange: {
            // Store and live re-skin the slot's puppet; the handler also does the host's
            // rebroadcast.
            coop::player_handshake::HandleSkinChange(session, msg);
            break;
        }
        case net::ReliableKind::NameplateChange: {
            // The per-peer nameplate visibility preference; the handler also rebroadcasts on the
            // host.
            coop::player_handshake::HandleNameplateChange(session, msg);
            break;
        }
        case net::ReliableKind::NickColorChange: {
            // The per-peer nick colour; the handler also rebroadcasts on the host.
            coop::player_handshake::HandleNickColorChange(session, msg);
            break;
        }
        case net::ReliableKind::HandItem: {
            // The hotbar hand-item display state; the handler rebroadcasts on the host and
            // TickMirrors applies.
            coop::hand_item::HandleHandItem(session, msg);
            break;
        }
        default: {
            // The family routers. Each returns true iff the kind is in its family (processed or
            // dropped by validation), so the family switch is the single membership declaration and
            // a new kind is wired in the enum and one switch. The inline cases above are disjoint
            // from every family.
            if (HandleEntityEvent(session, msg, localPlayer)) break;
            if (HandleStateEvent(session, msg, localPlayer)) break;
            if (HandleSignalEvent(session, msg)) break;
            if (HandleIntentEvent(session, msg, localPlayer)) break;
            if (HandleWorldEvent(session, msg)) break;
            // An unknown kind (a newer protocol) is logged, not silently discarded.
            UE_LOGW("event_feed: unknown ReliableKind %u -- dropping",
                    static_cast<unsigned>(msg.kind));
            break;
        }
        }
    }
}

}  // namespace coop::event_feed

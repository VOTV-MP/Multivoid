// coop/net_pump.cpp -- the per-tick orchestrator: the connection edges, the death policy, the
// reaper, the puppet drive. The sync-module fan-out lists live in coop/subsystems.cpp and the
// outbound pose, held-prop and ragdoll streams in coop/local_streams.cpp.

#include "coop/session/net_pump.h"

#include "coop/comms/chat_feed.h"    // Reset() on the leave-world flee (session UI dies with the session)
#include "coop/comms/chat_bubbles.h" // ResetSlots() on flee -- overhead bubbles are session UI too
#include "coop/comms/chat_sync.h"   // Reset() -- the lobby's chat record dies with the session too
#include "coop/player/nameplate.h"   // ResetSlots() on flee -- HasAny() keeps hud::IsActive() alive in the menu
#include "ui/chat_input.h"           // Close() on flee -- an OPEN chat box must not survive into the menu
#include "ui/voice_panel.h"          // Close() on flee -- don't leave the voice panel open across the transition
#include "coop/dev/leak_probe.h"
#include "coop/dev/heap_probe.h"
#include "coop/dev/perf_probe.h"
#include "coop/element/element_deleter.h"
#include "coop/dispatch/event_feed.h"
#include "coop/session/join_progress.h"
#include "coop/player/local_streams.h"
#include "ui/multiplayer_menu.h"  // MenuTickFn(): the death-flee bypass release condition
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"  // OnClientWorldReady: the deferred-adoption per-world reset
#include "coop/player/puppet_drive.h"      // the puppet array and its drive
#include "coop/props/registry_reaper.h"    // the reaper and the re-seed engine
#include "coop/props/remote_prop_spawn.h"  // OnClientWorldReadyResetSweep (deferred prop sweep per-world reset)
#include "coop/props/join_membership_sweep.h"  // the join claim and the divergence sweep
#include "coop/session/player_handshake.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/player/remote_player.h"
#include "coop/props/remote_prop.h"
#include "coop/props/save_identity_bind.h"  // BindUnboundReCreates: re-bind on re-seed
#include "coop/save/save_transfer.h"
#include "coop/session/subsystems.h"
#include "coop/session/world_load_episode.h"  // the announce waits on the load-tail quiescence latch

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "coop/player/death_revive.h"
#include "coop/session/teleport_client.h"  // the checkpoint join spawn (the client pawn-Set edge)
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/core/types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::net_pump {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// The cached local mainPlayer_C, on top of Registry::Local() so the hot pump path skips its atomic
// load. A CachedObjRef: probed at 125 Hz through teardown windows, where a bare IsLive dereference
// is unsafe.
ue_wrap::CachedObjRef g_netLocal;
// The cached controller for the same pawn (two dispatches saved per tick); reset with g_netLocal.
ue_wrap::CachedObjRef g_netLocalController;

// Connected-state edge detectors, file-scope so a session restart resets them explicitly. The
// aggregate flag gates the global OnDisconnect calls (session-wide state); the per-slot flags let
// one peer's puppet go without wiping every subsystem while another stays.
bool g_wasConnected = false;
std::array<bool, coop::players::kMaxPeers> g_wasConnectedBySlot{};

// The Session being ticked, set at Tick entry and cleared at exit. The ledger's teardown subscriber
// carries only rows, and every site that can fire a transition runs inside a tick; a transition
// fired elsewhere finds no session and does nothing.
coop::net::Session* g_tickSession = nullptr;

// The death-policy one-shot. On a local death the death arc has not armed, the pump tears every
// coop game-side state down synchronously, stops the session and flees to the menu with the detour
// held in transparent bypass (the game's own reload would otherwise run before the deferred
// disconnect cleanup, leaving orphan puppets and mirrors in the dying world). OnSessionStart resets
// it.
bool g_localDeathHandled = false;

// The ceiling on the transparent bypass after a flee. The bypass covers the world teardown (the
// detour otherwise stalls the 50k-actor untitled_1 teardown) and releases the moment
// ui_menu_C::Tick first dispatches, since the MULTIPLAYER button is injected by an observer on it;
// this timer only bounds the case where that tick never resolves, and is generous on purpose.
constexpr int kDeathMenuBypassMs = 30 * 1000;

// The terminal eject to the main menu after the caller tore coop state down: reset the edge
// detectors, Stop, arm the bypass, then travel through the game's own transition verb (the bypass
// first, so the teardown the travel triggers runs with the detour dormant). A one-shot latch: more
// than one path can find a session dead, and the travel is dispatched once; OnSessionStart resets
// it.
bool g_fleeing = false;

// `travel` is false when the game's own quit-to-menu transition is already in flight (a second
// transition would load the menu twice); the rest of the tail is identical.
void FleeToMainMenu(coop::net::Session& session, const char* why, bool travel = true) {
    if (g_fleeing) return;  // already travelling to the menu for this session
    g_fleeing = true;
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    session.Stop();
    // Every session-scoped overlay dies at this funnel (every leave-world path comes through it):
    // the last lines would otherwise ride their 11 s TTL into the menu, and one stale nameplate
    // re-activates the whole HUD there through hud::IsActive(). Not in DisconnectAll, which also
    // runs on a host whose client left, and the host keeps its UI.
    coop::chat_feed::Reset();
    coop::chat_sync::Reset();  // the record + the applied range die with the session
    coop::chat_bubbles::ResetSlots();
    coop::nameplate::ResetSlots();
    ui::chat_input::Close();
    ui::voice_panel::Close();
    // session.Stop() flips every slot to disconnected, and the next event_feed::Update would read
    // that as "<X> left the game" into the just-cleared feed; the edge detectors are neutralised so
    // the local teardown posts no departure. The host-stays case never reaches this funnel, so its
    // toast survives.
    coop::event_feed::SuppressPeerLeaveEdges();
    // The ledger clears here: the reconcile early-returns once the session stops, so the rows would
    // survive into the menu. After SuppressPeerLeaveEdges, so the transitions tear person-state
    // down silently.
    coop::roster_ledger::ClearAll();
    // Hold the detour dormant over the teardown, resuming on the menu's first ui_menu_C::Tick;
    // kDeathMenuBypassMs is the ceiling (and the whole hold when MenuTickFn() is null).
    ue_wrap::game_thread::SetTransparentBypassUntil(coop::multiplayer_menu::MenuTickFn(),
                                                    kDeathMenuBypassMs);
    if (!travel) {
        UE_LOGI("net: %s -- native menu travel already in flight; session stopped + "
                "held dormant (no second transition)", why);
        return;
    }
    if (ue_wrap::engine::ReturnToMainMenu()) {
        UE_LOGI("net: %s -- transition(\"/Game/menu\") dispatched; held dormant", why);
    } else {
        // The travel did not dispatch: the bypass stays armed, our actors are gone, and we are
        // still in the gameplay world; say so loudly.
        UE_LOGE("net: %s -- ReturnToMainMenu FAILED to dispatch; still in the gameplay "
                "world. Relaunch.", why);
    }
}

// The full coop-state teardown for a session ending while the process lives on (a local death, a
// native quit): every puppet and per-slot state, then the session-wide drains. The quit-to-menu
// flee needs it because FleeToMainMenu resets g_wasConnected and so suppresses the aggregate edge;
// a queued weather apply would otherwise run against the old daynightCycle's recycled slot (fatal).
void TearDownCoopStateForSessionEnd(coop::net::Session& session) {
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // DestroySlot is UnregisterPuppet plus destroy-if-live; its interleave with DisconnectSlot
        // is composed here only.
        coop::puppet_drive::DestroySlot(slot);
        coop::subsystems::DisconnectSlot(session, slot);
    }
    coop::subsystems::DisconnectAll();
}

}  // namespace

// The per-person world teardown, driven by the ledger row transition: the departed peer's puppet,
// then the per-slot subsystem fan-out. It fires on a replacement and on a client too, where a
// connection edge never could; every DisconnectSlot body is safe there (the host-authoritative ones
// self-gate).
void OnSlotReplaced_TearDownWorld(int slot, const coop::roster_ledger::Row& outgoing,
                                  const coop::roster_ledger::Row& /*incoming*/) {
    if (!outgoing.occupied()) return;
    if (slot <= 0) return;  // slot 0 is the host; its departure ends the session, not a slot
    auto* s = g_tickSession;
    if (!s) return;         // no session context (nothing to tear down against)
    if (coop::puppet_drive::DestroySlot(slot))
        UE_LOGI("net: peer slot %d (#%u) left -- puppet destroyed", slot,
                static_cast<unsigned>(outgoing.playerNo));
    coop::subsystems::DisconnectSlot(*s, slot);
}

void OnSessionStart() {
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced_TearDownWorld);  // idempotent
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    g_localDeathHandled = false;
    g_fleeing = false;  // re-arm the one-shot flee for this new session
    coop::death_revive::OnSessionStart();  // the travel veto's arm + per-death latches
    coop::registry_reaper::OnSessionStart();  // the been-in-gameplay latch (menu guard)
    coop::local_streams::OnSessionStart();  // held-prop + ragdoll edge detectors
}

bool IsFleeing() {
    return g_fleeing;
}

void FleeAfterNativeMenuTravel(coop::net::Session& session) {
    if (g_fleeing) return;  // internal latch (defense; the reaper's predicate already gates)
    TearDownCoopStateForSessionEnd(session);
    FleeToMainMenu(session, "left gameplay to the menu (native quit)",
                   /*travel=*/false);
}

void FleeToMainMenuOnDeath(coop::net::Session& session, const char* why) {
    // The public entry, so the harness routes a host session death through the same path as the
    // client flees. Idempotent through g_fleeing. Game thread.
    FleeToMainMenu(session, why);
}

// Has this client announced world-ready for the current connection? Gates the world-dependent tick
// blocks during a save-transfer join's menu window (no actors into the menu world). Reset on
// disconnect.
static std::atomic<bool> g_worldReadyAnnounced{false};

// The re-announce request, set when a world-change re-seed completes: a save-transfer join runs two
// level loads, and the re-announce drives the host's ConnectReplayForSlot to re-assert every
// host-authoritative state into the final world (the only way keyless chipPiles re-acquire their
// host eid). Reset on send and on disconnect.
static std::atomic<bool> g_reAnnounceWorldReady{false};

// The UWorld last announced against (game thread). A re-seed re-announces only when the UWorld
// actually swapped: the join's menu-to-game shadow drain is a reap inside the announced world, and
// treating it as a change would replay the full snapshot and re-adopt NPCs into live mirrors
// (duplicate kerfurs). A real level travel re-opens untitled_1 as a new UWorld, so it still
// re-announces. Reset on disconnect.
static void* g_announcedWorld = nullptr;

// The save-transfer kerfur-ghost reconcile lives in npc_adoption (it owns the timing); net_pump
// only notifies it at the announce (OnClientWorldReady) so it resets its per-world state.

// The announce axis' one owner; registry_reaper only requests through here.
void MaybeRequestReAnnounce(coop::net::Session& session, void* reapWorld) {
    if (session.role() == coop::net::Role::Host) return;
    if (reapWorld != g_announcedWorld) {
        g_reAnnounceWorldReady.store(true, std::memory_order_relaxed);
        // The join barrier: the re-announce waits for the new world's load tail as the first
        // announce did; a fresh probe session.
        coop::world_load_episode::ArmQuiesceProbe("world-change re-announce");
        // A reload is a teardown plus a rebuild: raise the reconcile window (kind = load) so the
        // destroy seam and the drop intent stay suppressed through the re-replay. InEpisode is not
        // raised: the lane parks' reload semantics are unmeasured.
        coop::world_load_episode::RaiseReconcileForReload();
    } else {
        UE_LOGI("net_pump: world-change re-seed on the SAME world already announced (%p) -- "
                "NOT re-announcing (suppresses the join menu-shadow-drain double-snapshot + "
                "kerfur re-adopt dupe)", reapWorld);
    }
}

void Tick(coop::net::Session& session) {
    // Game thread only: the puppet array and ElementDeleter::Flush are game-thread side tables; one
    // guard at the top covers everything below.
    UE_ASSERT_GAME_THREAD("net_pump::Tick (puppet drive + ElementDeleter::Flush)");
    // Scope the Session for the ledger's teardown subscriber (g_tickSession); RAII, so an early
    // return leaves no stale pointer.
    struct TickSessionScope {
        explicit TickSessionScope(coop::net::Session& s) { g_tickSession = &s; }
        ~TickSessionScope() { g_tickSession = nullptr; }
    } _tickSessionScope{session};

    // ---- Hitch and source probe (diagnostic, always on, near free) ----
    // [HITCH] times the gap between consecutive game-thread Ticks (the whole frame); [HITCH-SRC],
    // an RAII at the end of the body, this Tick's own duration. A [HITCH] with no [HITCH-SRC] on
    // the same frame is an engine-side stall (GC, render, physics).
    {
        using hclk = std::chrono::steady_clock;
        static hclk::time_point sPrevTickStart{};
        const hclk::time_point nowTp = hclk::now();
        if (sPrevTickStart.time_since_epoch().count() != 0) {
            const long long frameMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(nowTp - sPrevTickStart).count();
            if (frameMs >= 40)
                UE_LOGI("[HITCH] frame = %lld ms (>40ms stutter; if NO [HITCH-SRC] follows this frame, the "
                        "stall was ENGINE-side -- GC/render/physics, the permanent-both-peers GC signature)",
                        frameMs);
        }
        sPrevTickStart = nowTp;
    }
    struct TickDurLog {
        std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
        ~TickDurLog() {
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms >= 10)
                UE_LOGI("[HITCH-SRC] net_pump::Tick itself = %lld ms (OUR code caused this frame's cost)", ms);
        }
    } _tickDurLog;

    // The perf probe (ini perf_probe=1): Init self-latches, Sample self-throttles to ~1 Hz; the
    // Scope brackets the whole body so the report shows Tick's own ms against the per-subsystem
    // buckets.
    namespace PP = coop::dev::perf_probe;
    PP::Init();
    PP::Sample();
    PP::Scope _tickScope{PP::Bucket::NetPumpTick};

    // The leak-attribution probe (ini leak_probe=1, dev): a self-throttled ~4 s GUObjectArray
    // census naming the classes that grow.
    coop::dev::leak_probe::Tick();

    // The raw-heap probe (ini heap_probe=1, dev): names the CRT call site in our module when the
    // UObject census is flat but RAM climbs; installs ucrtbase malloc/free detours on its first
    // armed tick.
    coop::dev::heap_probe::Tick();

    // The host's pending save-transfer chunk sends; a no-op without an active stream.
    if (session.role() == coop::net::Role::Host) coop::save_transfer::TickHost();


    // The world-up gate: a save-transfer joiner runs this Tick at the menu for 30-60 s, where the
    // gameplay sections have nothing to act on but real per-tick cost. The chunk pump, the deleter
    // flush, the reaper, the connection edges, the announce and event_feed stay ungated. Local() is
    // the signal: non-null exactly when a possessed local player exists in a gameplay world, and
    // its negative-miss TTL keeps the menu poll cheap.
    void* const localNow = coop::players::Registry::Get().Local();
    const bool worldUp = (localNow != nullptr);

    // [dev] reseed_orphan_selftest, a one-shot in-process proof of the re-seed-orphan binding; a
    // cheap no-op otherwise.
    if (worldUp) coop::save_identity_bind::RunReseedOrphanSelfTest();

    // The deferred-element destruction flush (the MTA CElementDeleter shape,
    // coop/element/element_deleter.h): every Element parked since the last tick is destroyed here,
    // on the game thread; the steady-state cost is one uncontended mutex plus an empty check.
    // Bare-actor retires with site-specific pre-steps stay direct; only their Element bookkeeping
    // funnels here.
    coop::element::ElementDeleter::Get().Flush();

    // The reaper (coop/props/registry_reaper): dead-Element reconciliation, the world-change
    // re-seed, the gameplay-to-menu guard. True when the menu guard fired (the session torn down),
    // which aborts this Tick.
    if (coop::registry_reaper::Tick(session)) return;

    // The per-slot connection edges. A departed peer's puppet is destroyed, not left frozen; a
    // reconnect spawns a fresh one on the first new pose.
    const bool isConnected = (session.state() == coop::net::ConnState::Connected);
    const bool isHost = (session.role() == coop::net::Role::Host);
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // IsSlotReady (lanes configured), not IsSlotConnected: the connect replay must wait for
        // ConfigureLanes, or the snapshot fan-out ships on lane 0. The disconnect callback clears
        // both flags atomically.
        const bool slotConnected = session.IsSlotReady(slot);
        // Only the connect half lives here. Teardown is driven by the ledger row transition,
        // because a falling edge is wrong twice: on a client it never rises for slots 1-3 (a client
        // only fills peerConns_[0]), and on the host a fast replacement (ready to ready across one
        // tick) has no edge, so the successor would inherit. The row transition compares values and
        // sees both.
        if (!g_wasConnectedBySlot[slot] && slotConnected) {
            // Host side (slots 1..3): the replay fires on the joiner's ClientWorldReady, not here;
            // a menu-mode joiner is connected 30-60 s before it has a world. Client side (slot 0):
            // announce the flashlight, request the save transfer, open our world-ready send gate
            // (subsystems::ClientConnectEdge).
            if (isHost && slot >= 1) {
                UE_LOGI("net: peer slot %d connect edge -- awaiting ClientWorldReady before the replay", slot);
            } else if (!isHost && slot == 0) {
                UE_LOGI("net: host (slot 0) connect edge -- replaying local flashlight");
                coop::subsystems::ClientConnectEdge(session);
            }
        }
        g_wasConnectedBySlot[slot] = slotConnected;
    }

    // A client announces world-ready once per connection, when a gameplay world is up, the prop
    // registry expresses it and the load tail has quiesced: the MTA INITIAL_DATA_STREAM barrier
    // (CGame.cpp). The host streams the whole world state on it, so announcing into a churning load
    // would put the authoritative stream into it. The probe is deadline-capped, so a pathological
    // load still announces. Re-fires on a re-announce request under the same coherence gate.
    const bool reAnnounce = g_reAnnounceWorldReady.load(std::memory_order_relaxed);
    if (!isHost && isConnected &&
        (!g_worldReadyAnnounced.load(std::memory_order_relaxed) || reAnnounce)) {
        if (worldUp &&
            coop::prop_element_tracker::HasSeededOnce() &&
            coop::prop_element_tracker::IsRegistrySeededForCurrentWorld() &&
            !coop::prop_element_tracker::InPurgeEpisode() &&
            coop::world_load_episode::TickQuiesceProbe()) {
            if (session.SendReliableToSlot(0, coop::net::ReliableKind::ClientWorldReady,
                                           nullptr, 0)) {
                g_worldReadyAnnounced.store(true, std::memory_order_relaxed);
                g_reAnnounceWorldReady.store(false, std::memory_order_relaxed);
                // Stamp the world announced against, once per real announce. The same reader as the
                // reaper's world gate: the two are compared in MaybeRequestReAnnounce, and
                // FindObjectByClass answers "a world object exists" (the incoming world, while the
                // player chain still reads null), not "the world the player is in".
                g_announcedWorld = ue_wrap::world_identity::CurrentWorld();
                // A fresh connect replay is about to arrive: reset the deferred-adoption per-world
                // state so the new world re-adopts its save NPCs and re-sweeps orphans.
                coop::npc_adoption::OnClientWorldReady();
                coop::kerfur_prop_adoption::OnClientWorldReady();  // drop the stale prop-kerfur pending set
                // The same reset for the deferred prop divergence sweep: one armed for the prior
                // world must not fire against this one.
                coop::join_membership_sweep::OnClientWorldReadyResetSweep();
                UE_LOGI("net_pump: ClientWorldReady announced (world up + registry coherent + load "
                        "tail quiesced%s)",
                        reAnnounce ? " -- re-announce after world-change" : "");
            }
        }
    }

    if (!isConnected) {
        g_worldReadyAnnounced.store(false, std::memory_order_relaxed);   // re-announce next connection
        g_reAnnounceWorldReady.store(false, std::memory_order_relaxed);
        g_announcedWorld = nullptr;                                      // fresh connection re-stamps
    }

    if (g_wasConnected && !isConnected) {
        // The aggregate disconnect (all peers gone): the OnDisconnect calls for session-wide state
        // (the per-slot block handled the rest). A client that lost the host mid-join drops the
        // loading state so the cover and the console do not hang.
        coop::join_progress::Reset();
        const auto stats = coop::subsystems::DisconnectAll();
        UE_LOGI("net: all peers gone -- cleared %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries; takeObjInFlight=0",
                stats.snapPending, stats.initProcessedDropped);

        // The client eject: losing the host ends the session, so flee to the menu rather than
        // strand the player in a hostless world. The host does not eject here (its last client
        // leaving lands in this block too). One-shot through g_localDeathHandled, shared with the
        // death flee; the teardown above already ran, so FleeToMainMenu does only Stop, the bypass
        // and the travel.
        if (!isHost && !g_localDeathHandled) {
            g_localDeathHandled = true;
            const std::string reason = session.TakeHostCloseReason();
            UE_LOGW("net: HOST CLOSED OUR CONNECTION (reason: %s) -- fleeing to the main menu",
                    reason.empty() ? "connection lost" : reason.c_str());
            FleeToMainMenu(session, "host closed connection");
            return;
        }
    }
    // The client connect-failure edge: a browser join that never reached Connected (a dead address,
    // the host not up). The aggregate edge above needs g_wasConnected to have latched, so without
    // this the loading screen would hang on "Connecting..." until the 90 s failsafe. Precise:
    // client role, a join Active, never connected, the state back at Disconnected. Fail() is
    // idempotent.
    if (!isHost && !g_wasConnected && coop::join_progress::Active() &&
        session.state() == coop::net::ConnState::Disconnected) {
        const std::string why = session.TakeHostCloseReason();
        coop::join_progress::Fail(why.empty() ? "could not connect to the host" : why);
    }
    g_wasConnected = isConnected;

    // Up to ~100 snapshot candidates per tick while an enumeration is in progress (a no-op on
    // empty).
    if (isConnected && worldUp) { PP::Scope _s{PP::Bucket::SnapshotDrain}; coop::prop_snapshot::DrainChunk(); }

    // The per-tick gameplay subsystem chain (connect-broadcast drains, module polls and applies,
    // NPC streams, dev probes), world-up-gated whole: every one acts on gameplay-world state, and a
    // queued connect broadcast describes in-world state, so waiting costs nothing.
    if (worldUp) {
        coop::subsystems::TickGameplay(session, isConnected, isHost, g_fleeing);
    }

    if (g_netLocal.Raw() && !g_netLocal.Alive()) { g_netLocal.Reset(); g_netLocalController.Reset(); }
    if (!g_netLocal.Raw()) {
        g_netLocal.Set(localNow);  // resolved once at the top of this tick
        // The checkpoint join spawn: every client appearance spawns at the checkpoint start point,
        // never at the transferred save's playerTransform (the host's own position). This pawn-Set
        // edge is exactly "a new local body exists": once per world appearance, while the load
        // screen still covers the swap; a save-transfer join's two loads both get it, and the final
        // one is the one that matters.
        if (localNow && isConnected && !isHost) {
            coop::teleport_client::ApplyLocally(
                {ue_wrap::profile::name::kKPPSpawnX, ue_wrap::profile::name::kKPPSpawnY,
                 ue_wrap::profile::name::kKPPSpawnZ, 0.f, 0.f, 0.f});
            UE_LOGI("net_pump: CLIENT spawn -> KPP start point (join/world appearance)");
        }
    }
    // The !g_localDeathHandled gate: once the death is handled every local send stops, or the
    // ragdoll sender would keep reading the dead player's pelvis ~100 times a second on the way to
    // the menu. Raw() is validated by the block just above, this tick.
    if (g_netLocal.Raw() && !g_localDeathHandled) {
        // The pump barrier of the death arc: publishes the OpenLevel veto's inputs from the
        // validated pawn, arms on the `dead` rising edge, runs a revive the detour requested. Here,
        // not in the detour, because there a UFunction dispatch re-enters our own ProcessEvent
        // detour.
        coop::death_revive::Tick(session, g_netLocal.Raw());
        // The death policy: on a local death tear every coop game-side state down this frame, then
        // Stop. Stop alone is not enough: the game's death reload blocks the game thread at once,
        // so the deferred disconnect cleanup never runs. `dead` is true only on a real death (a
        // faint or KO leaves it false). The host gates on running() (it is still hosting in
        // Handshaking, Connected and Disconnected alike); the client on connected(), since the
        // host-close path already ejected it.
        const bool sessionLiveForDeath = isHost ? session.running() : session.connected();
        if (!g_localDeathHandled && sessionLiveForDeath) {
            bool isRagdoll = false, dead = false;
            if (ue_wrap::engine::ReadMainPlayerRagdollState(g_netLocal.Raw(), isRagdoll, dead) && dead) {
                // The death arc owns this edge first: when death_revive has armed this death (seam
                // installed, verbs resolved, session live) the native chain plays and the pump
                // keeps running, since the pump performs the revive. Unarmed, this flee is the
                // fallback that keeps a death survivable for our layer; the arm decision and this
                // one are the same decision at the same instant. g_localDeathHandled stays unset on
                // the armed path: it is shared with the host-close arm above.
                if (!coop::death_revive::ArmedForThisDeath()) {
                    g_localDeathHandled = true;
                    UE_LOGW("net: LOCAL PLAYER DIED -- tearing down coop state synchronously + fleeing "
                            "to the main menu (role=%s; permadeath-rejoinable)",
                            session.role() == coop::net::Role::Host ? "HOST (ends session)" : "CLIENT");
                    // The per-slot teardown plus the session-wide drains, shared with the native
                    // quit-to-menu path.
                    TearDownCoopStateForSessionEnd(session);
                    // Flee to the menu and hold our layer dormant (shared with the host-close
                    // eject); the travel uses the game's own verb, which works for a dead,
                    // ragdolling player.
                    FleeToMainMenu(session, "LOCAL PLAYER DIED");
                    return;
                }
            }
        }
        // Re-resolve the controller only when missing or invalidated (it is stable between possess
        // events); ~250 dispatches a second saved at the 125 Hz pump.
        if (g_netLocalController.Raw() && !g_netLocalController.Alive()) g_netLocalController.Reset();
        if (!g_netLocalController.Raw())
            g_netLocalController.Set(ue_wrap::engine::GetController(g_netLocal.Raw()));
        // The one-shot install of the per-subsystem observers (idempotent).
        { PP::Scope _s{PP::Bucket::InstallObs}; coop::subsystems::Install(session); }
        // The outbound local streams (coop/local_streams). A joining client's pawn is parked and
        // teleported through positions that must not stream as our pose, and ClientWorldReady fires
        // seconds before loadObjects' final teleport, which cannot be hooked
        // (docs/COOP_DISPATCH_VISIBILITY.md); so the client gate is load-tail quiescence
        // (join_membership_sweep), the signal that observes its effect, reset per world. The host
        // keeps the worldUp gate.
        const bool poseAuthoritative =
            isHost ? worldUp
                   : (g_worldReadyAnnounced.load(std::memory_order_relaxed) &&
                      !g_reAnnounceWorldReady.load(std::memory_order_relaxed) &&
                      coop::join_membership_sweep::HasLoadTailQuiesced());
        if (poseAuthoritative)
            coop::local_streams::Tick(session, g_netLocal.Raw(), g_netLocalController.Raw());
    }

    // The per-slot puppet drive (coop/player/puppet_drive); the worldReadyAnnounced load is
    // evaluated in the call, the same observation point as the drive's loop. remote_prop stays here
    // (a prop concern), under the same gate, after the drive.
    if (worldUp) {
        coop::puppet_drive::DriveTick(session,
                                      g_worldReadyAnnounced.load(std::memory_order_relaxed));

        // The receiver-side held-prop driver: the latest PropPose applied (a Key lookup on first
        // arrival, transform writes after); a stream stop over 500 ms is an implicit release.
        { PP::Scope _s{PP::Bucket::RemoteProp}; coop::remote_prop::Tick(session); }
    }  // worldUp (puppet drive + ragdoll + pose-diag + remote prop)

    // Session events (joins, disconnects) to the feed, and our Join; g_netLocal goes along so
    // remote_prop::OnRelease can dispatch Aprop_C.thrown(player).
    { PP::Scope _s{PP::Bucket::EventFeed}; coop::event_feed::Update(session, g_netLocal.Raw()); }
}

bool HasAnnouncedWorldReady() {
    return g_worldReadyAnnounced.load(std::memory_order_relaxed);
}

}  // namespace coop::net_pump

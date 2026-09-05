// coop/net_pump.cpp -- see coop/net_pump.h.
//
// The per-tick orchestrator: the connection edges, the death policy, the reaper, the puppet
// drive. The sync-module fan-out lists live in coop/subsystems.cpp (the wiring registry every new
// feature edits) and the outbound pose, held-prop and ragdoll streams in coop/local_streams.cpp.

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

// The cached local mainPlayer_C for the pump. coop::players::Registry::Get().Local() already
// caches and filters puppets through the controller discriminator; this cache on top skips the
// atomic load in the hot pump path. A CachedObjRef: probed every 125 Hz pump tick, including
// through teardown windows, where a bare IsLive dereference is unsafe.
ue_wrap::CachedObjRef g_netLocal;
// The cached controller for the same pawn: saves two ProcessEvent dispatches per pump tick
// (GetController + GetControlRotation). Bound to g_netLocal's lifetime: Reset when g_netLocal is
// Reset (a level change).
ue_wrap::CachedObjRef g_netLocalController;

// Connected-state edge detection for the disconnect cleanup (destroying the puppet). File-scope,
// not a static local in Tick, so a session restart can reset them explicitly; a local static
// would carry the prior session's value into the new Start. The aggregate flag (g_wasConnected)
// tracks "any peer connected" and gates the global OnDisconnect calls for subsystems with
// session-wide state (weather_sync, prop_lifecycle). The per-slot flags (g_wasConnectedBySlot)
// track per-peer connection edges, so one peer's puppet can be destroyed on its disconnect
// without wiping every subsystem's state while another peer stays.
bool g_wasConnected = false;
std::array<bool, coop::players::kMaxPeers> g_wasConnectedBySlot{};

// The Session the pump is currently ticking, valid for the duration of Tick. The ledger's
// teardown subscriber needs a Session, but the subscriber signature deliberately carries only
// the rows (the ledger knows nothing about transport), and every site that can fire a
// transition (the reconcile, the flee-time ClearAll) runs inside a tick. Set at Tick entry,
// cleared at exit, so a transition fired from anywhere else finds no session and does nothing
// rather than dereferencing a stale pointer.
coop::net::Session* g_tickSession = nullptr;

// The death-policy one-shot. On local death, when the death arc has not armed a revive, the pump
// synchronously tears down all coop game-side state (destroys the puppet actors, drains the
// Element state), stops the session and flees to the main menu with the layer held dormant. The
// teardown is what matters: the game's own death chain reloads the world, and the deferred
// disconnect-edge cleanup on the next tick would never run, so orphan puppet actors and Element
// mirrors would sit in the dying world (the death arc: coop/player/death_revive). The travel
// goes through the game's own verb, mainGamemode_C::transition("/Game/menu")
// (engine::ReturnToMainMenu), which works for a dead, ragdolling player; `disconnect` is a
// no-op without a UE net driver, and a bare `open menu` does not resolve the short name. The
// ProcessEvent detour is held in transparent bypass (game_thread::SetTransparentBypass) over
// the travel, because it otherwise stalls the 50k-actor untitled_1 teardown by dispatching
// ReceiveEndPlay per dying actor through us. Latched once per death; OnSessionStart resets it
// so a rejoin re-arms.
bool g_localDeathHandled = false;

// The safety ceiling on the transparent bypass after a flee to the menu. The bypass covers only
// the world-teardown window: the 50k-actor untitled_1 teardown and the menu travel run with the
// ProcessEvent detour dormant (the observers and the outer SEH otherwise hang the per-actor
// EndPlay swap). Once the menu world is up the detour must resume: the menu's MULTIPLAYER
// button is injected by a POST observer on ui_menu_C::Tick dispatched through the detour, so a
// held bypass would starve it and the death menu would lose the button the rejoin depends on.
// Resuming at the menu is safe: TickGameplay is world-up-gated and the prop reaper is
// gameplay-world-gated. So FleeToMainMenu arms the bypass with ui_menu_C::Tick as the release
// function, and the detour resumes the instant the menu world is up; this timer is only the
// ceiling for the case where that tick never resolves or dispatches, kept generous so a slow
// teardown is never cut short. (If a death-to-rejoin black-screens or hangs, the menu tick
// signal failed; that is the thing to investigate.)
constexpr int kDeathMenuBypassMs = 30 * 1000;

// The terminal local eject to the main menu. The caller has already torn down coop game-side
// state (the death handler inline; the client disconnect edge through the OnDisconnect calls);
// this is the common tail: reset the edge detectors, Stop the session, arm and hold the
// transparent bypass, then travel to the menu through the game's own transition verb. Both the
// local-death flee and the host-kicked / banned / host-gone client flee leave the gameplay world
// this way. Order matters: the bypass is armed before the travel, so the untitled_1 teardown the
// travel triggers runs with the detour dormant. `why` is a short log tag. An idempotent latch: a
// session can be detected dead by more than one path at once (a net disconnect or death edge
// here and the harness's running-to-stopped edge), but transition("/Game/menu") is dispatched
// only once; OnSessionStart resets it. Every session death, host or client, returns the player
// to the main menu so they always know it ended.
bool g_fleeing = false;

// `travel` is false when the game's own quit-to-menu transition is already in flight:
// re-dispatching transition("/Game/menu") on top of it would load the menu twice. The rest of
// the tail (the edge resets, Stop, the bypass over the teardown) is identical either way.
void FleeToMainMenu(coop::net::Session& session, const char* why, bool travel = true) {
    if (g_fleeing) return;  // already travelling to the menu for this session
    g_fleeing = true;
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    session.Stop();
    // Every session-scoped overlay dies at this funnel: the chat feed and its record, the overhead
    // bubbles, the nameplates, an open chat box, the voice panel. Every leave-world path (death,
    // the host-close eject, a native quit to the menu) comes through here, and without the reset
    // the last lines would ride their 11 s TTL into the main-menu overlay; hud::IsActive() keys on
    // chat_feed::HasAny() || nameplate::HasAny() || ..., so one stale nameplate would re-activate
    // the whole HUD in the menu. DisconnectAll is the wrong home: it also runs on the host when a
    // client leaves, and the host keeps its UI. The host-stays-in-world case (its last client
    // leaving) never flees, so its feed keeps the "X left the game" line.
    coop::chat_feed::Reset();
    coop::chat_sync::Reset();  // the record + the applied range die with the session
    coop::chat_bubbles::ResetSlots();
    coop::nameplate::ResetSlots();
    ui::chat_input::Close();
    ui::voice_panel::Close();
    // The feed reset alone is not enough: session.Stop() flips every peer slot from connected to
    // disconnected, and the next event_feed::Update would read that as a per-slot "<X> left the
    // game" departure and push it into the just-cleared feed, so a client's own quit to the menu
    // would show "Host left the game" and ride it into the main menu. The edge detectors are
    // neutralised so the local teardown emits no spurious peer-left toast; the host-stays-on-
    // client-leave case never reaches this funnel, so its legitimate departure toast is preserved.
    coop::event_feed::SuppressPeerLeaveEdges();
    // The ledger clears at session stop, and here rather than in the next reconcile: the reconcile
    // early-returns once the session stops running, so the rows would otherwise survive into the
    // menu and into the next session. Ordered after SuppressPeerLeaveEdges so the transitions tear
    // person-state down silently instead of narrating four departures into the main menu.
    coop::roster_ledger::ClearAll();
    // Hold the detour dormant over the world teardown, but resume the instant the menu's
    // ui_menu_C::Tick first dispatches (the menu world is up), so MULTIPLAYER is injected on the
    // first menu frame. kDeathMenuBypassMs is only the safety ceiling (used as is when MenuTickFn()
    // is null: the menu class never resolved).
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
        // The travel did not dispatch (the gamemode or transition unresolved). The bypass stays
        // armed and our actors are already torn down, but we did not leave the gameplay world; say
        // so loudly.
        UE_LOGE("net: %s -- ReturnToMainMenu FAILED to dispatch; still in the gameplay "
                "world. Relaunch.", why);
    }
}

// The full coop-state teardown for a session ending while the process lives on (a local death,
// or a native quit to the menu): destroy every puppet actor and the per-slot subsystem state,
// then the aggregate session-wide drains. It mirrors what the disconnect edges do, and the
// quit-to-menu flee needs it because FleeToMainMenu resets g_wasConnected, which also suppresses
// the aggregate-disconnect edge: without it, the weather, time and sky caches, the install
// latches and the pending applies would stay armed across the world teardown, and a queued
// weather apply would run against the old daynightCycle's recycled GUObjectArray slot,
// executing the cycle BP body with a foreign `self` (a FindFunctionChecked on an
// ArrowComponent, a fatal error).
void TearDownCoopStateForSessionEnd(coop::net::Session& session) {
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // DestroySlot is the unconditional UnregisterPuppet plus destroy-if-live; the per-slot
        // interleave with DisconnectSlot is composed here, its one owner.
        coop::puppet_drive::DestroySlot(slot);
        coop::subsystems::DisconnectSlot(session, slot);
    }
    coop::subsystems::DisconnectAll();
}

}  // namespace

// The per-person world teardown, driven by the ledger row transition: destroy the departed
// peer's puppet, then run the per-slot subsystem fan-out. It fires on a replacement as well as a
// departure, and it fires on a client too, where a connection edge never could. Every body in
// subsystems::DisconnectSlot is safe there: the host-authoritative ones self-gate on Role::Host
// and become no-ops on a client, and the rest are local-state clears a client owes anyway. None
// of them send from a client.
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
    // The public entry, so the harness can route a host session death (or any session end) to the
    // main menu through the same path the client-death and disconnect flees use (reset the edge
    // detectors, Stop, the transparent bypass, transition("/Game/menu")). Idempotent through the
    // g_fleeing latch; harmless if net_pump already fled the client. Game thread.
    FleeToMainMenu(session, why);
}

// Has this process (as a client) announced world-ready for the current connection? Gates the
// world-dependent client tick blocks (the puppet spawn from poses) during the menu window of a
// save-transfer join: the engine must not spawn actors into the menu world. Reset when the
// connection drops.
static std::atomic<bool> g_worldReadyAnnounced{false};

// The re-announce request: set (client side) when a world-change re-seed completes, so the
// world-ready announce below fires again for the newly settled world. A save-transfer join goes
// through two level loads (a premature first announce binds props to actors the second load
// destroys); the re-announce drives the host's ConnectReplayForSlot to re-assert every
// host-authoritative state into the final world, the only way the client's keyless chipPiles
// (eid-only identity, nothing to re-match on) re-acquire their host eid after the re-seed mints
// fresh local ones. Distinct from g_worldReadyAnnounced, which stays latched true for the
// puppet-spawn gate across world changes. Reset on send and on disconnect.
static std::atomic<bool> g_reAnnounceWorldReady{false};

// The UWorld we last announced ClientWorldReady against (game thread only, as net_pump Tick
// is). A world-change re-seed re-announces only when the current world differs from this, that
// is, the UWorld actually swapped. The join's one-time menu-to-game prop-element-shadow drain is
// a bookkeeping reap within the already-announced game world (no swap); reading it as a world
// change would fire a second ClientWorldReady, the host would re-replay the full snapshot, and
// the client would re-run NPC adoption against its already-bound live mirrors: duplicate
// kerfurs. Gating on a real swap is airtight: re-replaying host state into a "new" world is only
// meaningful when that world's actors were destroyed and recreated, which only a UWorld swap
// does (a real cave or level travel re-opens untitled_1 as a new UWorld, so legitimate travels
// still re-announce and re-bind keyless chipPiles). Reset on disconnect.
static void* g_announcedWorld = nullptr;

// The save-transfer kerfur-ghost reconcile lives in coop/npc_adoption (the deferred class-match
// adoption owns the timing); net_pump only notifies it at the ClientWorldReady announce
// (OnClientWorldReady) so it can reset its per-world state. The ghost sweep fires from
// npc_adoption::Tick, gated on SnapshotComplete plus adoption convergence, never on a fixed
// delay.

// The announce axis' one owner; registry_reaper only requests through here.
void MaybeRequestReAnnounce(coop::net::Session& session, void* reapWorld) {
    if (session.role() == coop::net::Role::Host) return;
    if (reapWorld != g_announcedWorld) {
        g_reAnnounceWorldReady.store(true, std::memory_order_relaxed);
        // The join barrier: the re-announce waits for the new world's load tail exactly as the
        // first announce did; open a fresh probe session for it.
        coop::world_load_episode::ArmQuiesceProbe("world-change re-announce");
        // A world reload is a teardown plus a rebuild: raise the reconcile window (kind = load; the
        // classifier resets) so the destroy seam and the drop intent stay suppressed through the
        // re-replay bracket. InEpisode is not raised here: the lane parks' reload semantics are
        // unmeasured.
        coop::world_load_episode::RaiseReconcileForReload();
    } else {
        UE_LOGI("net_pump: world-change re-seed on the SAME world already announced (%p) -- "
                "NOT re-announcing (suppresses the join menu-shadow-drain double-snapshot + "
                "kerfur re-adopt dupe)", reapWorld);
    }
}

void Tick(coop::net::Session& session) {
    // This whole body is game-thread-only: it drives the puppet array (through puppet_drive, a
    // game-thread-only side table) and runs ElementDeleter::Flush (the controlled game-thread
    // destruction point). One guard at the top enforces the invariant for everything below it.
    UE_ASSERT_GAME_THREAD("net_pump::Tick (puppet drive + ElementDeleter::Flush)");
    // Scope the Session for the ledger's teardown subscriber (see g_tickSession). RAII, so an early
    // return anywhere below cannot leave a stale pointer live.
    struct TickSessionScope {
        explicit TickSessionScope(coop::net::Session& s) { g_tickSession = &s; }
        ~TickSessionScope() { g_tickSession = nullptr; }
    } _tickSessionScope{session};

    // ---- Hitch and source probe (diagnostic, always on, near free) ----
    // [HITCH] times the wall-clock gap between consecutive game-thread Ticks (the whole frame's
    // time, including any engine stall that freezes the frame: GC, render, physics). [HITCH-SRC]
    // (an RAII at the end of this body) reports this Tick's own duration. The discriminator: a
    // [HITCH] with no matching [HITCH-SRC] on the same frame means the stall was engine-side (the
    // GC signature when it is permanent, on both peers, at a fixed period); then the fix is the GC
    // cadence, not a walk fix.
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

    // The perf probe (ini perf_probe=1). Init self-latches; Sample self-throttles to ~1 Hz. The
    // whole-Tick Scope brackets the body so the 1 Hz report shows net_pump::Tick's own ms per frame
    // against the per-subsystem buckets.
    namespace PP = coop::dev::perf_probe;
    PP::Init();
    PP::Sample();
    PP::Scope _tickScope{PP::Bucket::NetPumpTick};

    // The leak-attribution probe (ini leak_probe=1, dev only). Self-gated and self-throttled (a ~4
    // s GUObjectArray census). Names the UObject classes growing over time, or proves a leak is raw
    // heap, not UObjects, when the total count stays flat. Game thread by the assert above.
    coop::dev::leak_probe::Tick();

    // The raw-heap leak-attribution probe (ini heap_probe=1, dev only). When the UObject census
    // above is flat but RAM still climbs, this names the CRT call site in our module responsible
    // (the engine's GMalloc bypasses the CRT). Self-gated and self-throttled; installs ucrtbase
    // malloc/free detours on its first armed tick.
    coop::dev::heap_probe::Tick();

    // Pump the pending save-transfer chunk sends (host). A no-op without an active stream (one bool
    // per slot).
    if (session.role() == coop::net::Role::Host) coop::save_transfer::TickHost();


    // The world-up gate. A menu-mode save-transfer joiner runs this Tick at 60 Hz while still at
    // the main menu (connecting, downloading the host save), a window where the gameplay-only
    // sections below have nothing to act on but real per-tick cost (subsystem polls, ensure
    // retries, GUObjectArray lookups against classes that cannot exist before the gameplay world
    // loads). Everything the menu window needs stays ungated: the host chunk pump above, the
    // element-deleter flush, the reaper (self-gated on the world name; it owns the quit-to-menu
    // flee), the per-slot connect/disconnect edges, the world-ready announce, and event_feed (which
    // delivers SaveTransferBegin). Local() is the signal: non-null exactly when a possessed local
    // player exists in a gameplay world, and its negative-miss TTL (players_registry) makes the
    // poll cheap at the menu.
    void* const localNow = coop::players::Registry::Get().Local();
    const bool worldUp = (localNow != nullptr);

    // [dev] reseed_orphan_selftest: a deterministic in-process proof of the re-seed-orphan binding.
    // Self-gated (one-shot, latching only once a live chipPile native exists), so a cheap no-op
    // otherwise.
    if (worldUp) coop::save_identity_bind::RunReseedOrphanSelfTest();

    // The deferred-element destruction flush (the MTA CElementDeleter shape;
    // coop/element/element_deleter.h). Drains, on the game thread at one controlled point, every
    // Element parked for destruction since the last tick. The steady-state cost is one uncontended
    // mutex acquire plus an empty-queue check. This is the sync module's single deferred-retire
    // funnel: prop_element_tracker (reap/unmark), npc_sync, npc_world_enum, trash_proxy,
    // world_actor_sync and kerfur_reconcile all route Element teardown through
    // ElementDeleter::Enqueue. Bare-actor retires that carry site-specific pre-steps (a proxy
    // un-root, an echo-suppressed convert destroy) stay direct; only their Element bookkeeping
    // funnels here.
    coop::element::ElementDeleter::Get().Flush();

    // Dead-Prop-Element reconciliation, the world-change re-seed and the gameplay-to-menu RAM
    // guard: coop/props/registry_reaper. True when the menu guard fired (the session torn down,
    // fleeing), which aborts this Tick.
    if (coop::registry_reaper::Tick(session)) return;

    // The per-slot connection edges. A departed peer's puppet is destroyed rather than left frozen
    // in place (event_feed also posts "X left the game"); if the peer reconnects, Tick spawns a
    // fresh puppet on the first new pose.
    const bool isConnected = (session.state() == coop::net::ConnState::Connected);
    const bool isHost = (session.role() == coop::net::Role::Host);
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // IsSlotReady (lanes configured), not IsSlotConnected (a connection handle exists): the
        // connect-edge replay must wait for ConfigureLanes to land in the Connected callback, or
        // the snapshot fan-out and the connect-time broadcasts ship on lane 0 instead of their
        // assigned lanes. The disconnect callback clears both flags atomically, so the disconnect
        // edge also fires correctly off IsSlotReady.
        const bool slotConnected = session.IsSlotReady(slot);
        // Only the connect half of the edge lives here. Per-person teardown is driven by the ledger
        // row transition (OnSlotReplaced_TearDownWorld above), because a falling edge here is wrong
        // in two structural ways: on a client it never rises for slots 1-3 (a client only ever
        // fills peerConns_[0]), so the subsystem fan-out would never run there and a client would
        // keep a departed third peer's voice channel, prop mirrors, owner-entity mirrors, trash
        // proxies, flashlight cache and Player Element for the rest of the session; and on the host
        // a fast replacement skips it entirely (ready to ready across one 8 ms tick, no edge), so
        // the successor would inherit. The row transition compares values, so it sees both.
        if (!g_wasConnectedBySlot[slot] && slotConnected) {
            // The per-slot connect edge. Host side (slots 1..3): the replay does not fire here. A
            // menu-mode joiner is connected 30-60 s before it has a world (the save download and
            // load); the replay fires on its ClientWorldReady (event_feed, then
            // subsystems::ConnectReplayForSlot). An already-in-world joiner announces world-ready
            // within a tick of connecting, so its timing is the same. Client to host (slot 0):
            // announce the local flashlight state, request the save transfer and open our
            // world-ready send gate (subsystems::ClientConnectEdge).
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
    // registry expresses it, and the load tail has quiesced (the world_load_episode probe latch).
    // The announce is the MTA INITIAL_DATA_STREAM barrier (CGame.cpp in mtasa-blue): the host opens
    // the send gate and streams the whole world state on it, so announcing before loadObjects'
    // async tail settles would put the entire authoritative stream into a churning world. The probe
    // is deadline-capped (45 s of no progress, 120 s absolute), so a pathological load still
    // announces (degraded, logged loudly by the probe); the barrier can never wedge the join. Fires
    // on the first announce of a connection or on a re-announce request (a world-change re-seed
    // completed, so the host must re-replay into the new world; MaybeRequestReAnnounce armed a
    // fresh probe session for the new world's tail). The coherence gate is identical either way: a
    // menu or stale-world announce would arm the host bracket against an unseeded client.
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
                // Stamp the world just announced against; a later re-seed only re-announces if the
                // current world differs (MaybeRequestReAnnounce). Resolved here, once per real
                // announce, never per frame. The same reader the reaper's world gate uses: the two
                // are compared against each other in MaybeRequestReAnnounce, so they must not be
                // two notions of "the current world", and FindObjectByClass answers "a world object
                // exists" (it returns the incoming world while the player chain still reads null),
                // not "the world the local player is in", which is the question a re-announce asks.
                g_announcedWorld = ue_wrap::world_identity::CurrentWorld();
                // A fresh connect replay (this world's EntitySpawns and SnapshotComplete) is about
                // to arrive: reset the deferred-adoption per-world state so the new world re-adopts
                // its save NPCs and re-sweeps orphans (the ghost sweep fires from
                // npc_adoption::Tick, gated on SnapshotComplete and adoption convergence).
                coop::npc_adoption::OnClientWorldReady();
                coop::kerfur_prop_adoption::OnClientWorldReady();  // drop the stale prop-kerfur pending set
                // The same per-world reset for the deferred prop divergence sweep: a sweep armed
                // for the prior world must not fire against this fresh one (a save transfer is two
                // loads).
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
        // The aggregate disconnect (all peers gone). The global OnDisconnect calls: the per-slot
        // block above handled the subsystems with per-slot state; this catches the ones with
        // session-wide state (weather_sync, npc_sync's host-side counter, prop_lifecycle's dedupe
        // set). If a client lost the host mid-join, drop the loading state so the cover and the
        // console do not hang (a no-op on the host, or with no join in progress).
        coop::join_progress::Reset();
        const auto stats = coop::subsystems::DisconnectAll();
        UE_LOGI("net: all peers gone -- cleared %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries; takeObjInFlight=0",
                stats.snapPending, stats.initProcessedDropped);

        // The client eject: a client losing the host (a kick, a ban, the host quitting or crashing)
        // ends the session; flee to the main menu so the player is not stranded in a hostless world
        // (the same path as a local-death flee). The host does not eject here: a client leaving, or
        // the host kicking its last client, also lands in this aggregate-disconnect block and must
        // not boot the host to the menu. One-shot through the terminal-eject latch
        // (g_localDeathHandled, shared with the death flee; OnSessionStart re-arms it). The
        // subsystem teardown above already ran, so FleeToMainMenu does only Stop, the bypass and
        // the travel.
        if (!isHost && !g_localDeathHandled) {
            g_localDeathHandled = true;
            const std::string reason = session.TakeHostCloseReason();
            UE_LOGW("net: HOST CLOSED OUR CONNECTION (reason: %s) -- fleeing to the main menu",
                    reason.empty() ? "connection lost" : reason.c_str());
            FleeToMainMenu(session, "host closed connection");
            return;
        }
    }
    // The client connect-failure edge: a browser-join client whose connect attempt terminated
    // without ever reaching Connected (a dead direct IP, an unreachable host, the host not up yet).
    // The aggregate-disconnect edge above fires only once g_wasConnected latched true, and its
    // client branch flees on host-close; neither catches a never-connected client, and without this
    // the loading screen would hang on "Connecting..." until the 90 s failsafe while net_pump
    // pumped the full gameplay tick at the menu every frame. Detected precisely: client role, a
    // browser join is Active, never connected this session (the pre-update g_wasConnected), and
    // Start() already drove the state past Handshaking back to Disconnected (session_status's
    // connect-fail path), which for a running client means the attempt is definitively dead. Fail()
    // is a no-op unless Active, and idempotent, so re-firing until the harness drains the abort
    // (Stop and reopen the browser, which ends the menu pump) is harmless.
    if (!isHost && !g_wasConnected && coop::join_progress::Active() &&
        session.state() == coop::net::ConnState::Disconnected) {
        const std::string why = session.TakeHostCloseReason();
        coop::join_progress::Fail(why.empty() ? "could not connect to the host" : why);
    }
    g_wasConnected = isConnected;

    // Process up to ~100 snapshot candidates per tick while a snapshot enumeration is in progress
    // (a no-op on an empty vector).
    if (isConnected && worldUp) { PP::Scope _s{PP::Bucket::SnapshotDrain}; coop::prop_snapshot::DrainChunk(); }

    // The per-tick gameplay subsystem chain (connect-broadcast drains, module polls and applies,
    // NPC streams, trash death-watches, dev probes). World-up-gated whole: every one of these acts
    // on gameplay-world state that cannot exist at the menu, and several poll or ensure against BP
    // classes that only load with the gameplay world. Queued connect broadcasts (the client's own
    // flashlight from the slot-0 connect edge, say) simply wait until the world is up; they
    // describe in-world state, so sending them earlier was never meaningful.
    if (worldUp) {
        coop::subsystems::TickGameplay(session, isConnected, isHost, g_fleeing);
    }

    if (g_netLocal.Raw() && !g_netLocal.Alive()) { g_netLocal.Reset(); g_netLocalController.Reset(); }
    if (!g_netLocal.Raw()) {
        g_netLocal.Set(localNow);  // resolved once at the top of this tick
        // The checkpoint join spawn: every client appearance in a coop world spawns at the
        // checkpoint start point, never at the transferred save's playerTransform (the host's saved
        // position, which would materialise a joiner inside the host's base or on top of the host).
        // The host alone keeps its save position. This pawn-Set edge is exactly "a new local body
        // exists": it fires once per world appearance, the first join and every save-transfer or
        // world-change reload, while the load screen still covers the swap. (A save-transfer join
        // runs two level loads; both pawns get the teleport, and the second, final one is the one
        // that matters.)
        if (localNow && isConnected && !isHost) {
            coop::teleport_client::ApplyLocally(
                {ue_wrap::profile::name::kKPPSpawnX, ue_wrap::profile::name::kKPPSpawnY,
                 ue_wrap::profile::name::kKPPSpawnZ, 0.f, 0.f, 0.f});
            UE_LOGI("net_pump: CLIENT spawn -> KPP start point (join/world appearance)");
        }
    }
    // The `!g_localDeathHandled` gate: the first tick after death this block still runs (death is
    // detected here and the synchronous teardown fires), but once handled all local-send work
    // stops; otherwise the ragdoll sender would keep emitting RagdollPose packets on the stopped
    // session, reading the dead player's pelvis ~100 times a second on the way to the menu. Raw()
    // below is validated by the Alive()/Reset/Set block just above, this tick.
    if (g_netLocal.Raw() && !g_localDeathHandled) {
        // The pump barrier of the death arc. This publishes the OpenLevel veto's inputs from the
        // pawn this tick already validated, arms on the `dead` rising edge, and runs a revive the
        // detour has requested. Here and not in the detour, because the detour runs inside a native
        // call inside the BP VM, where a UFunction dispatch re-enters our own ProcessEvent detour
        // and fires every interceptor and observer; the pump task is where engine calls are
        // ordinary.
        coop::death_revive::Tick(session, g_netLocal.Raw());
        // The death policy. On local death, synchronously tear down all coop game-side state on
        // this frame, then Stop the session. Session::Stop() alone is not enough: the game's death
        // world reload blocks the game thread immediately, so the deferred disconnect-edge cleanup
        // on the next Tick never runs, and our orphan puppet actors and Element mirrors would stay
        // in the dying world. So the puppet actors are destroyed and every subsystem's state
        // drained here, before the reload, mirroring the per-slot and aggregate disconnect edges.
        // One-shot (g_localDeathHandled); a reconnect re-Starts and OnSessionStart re-arms. `dead`
        // is true only on real death (a faint, a KO or the manual C leave it false).
        // The gate: a host is "still hosting" in three states, Handshaking (no client yet),
        // Connected (clients present) and Disconnected (the clients all left, the listen socket
        // still up), and its death matters in all three, so the host gates on running()
        // [Start..Stop], not connected(). The client keeps connected(): if it loses the host it is
        // already ejected by the host-close path above (which latches g_localDeathHandled), so
        // connected() is its precise window.
        const bool sessionLiveForDeath = isHost ? session.running() : session.connected();
        if (!g_localDeathHandled && sessionLiveForDeath) {
            bool isRagdoll = false, dead = false;
            if (ue_wrap::engine::ReadMainPlayerRagdollState(g_netLocal.Raw(), isRagdoll, dead) && dead) {
                // The death arc owns this edge first: the whole native death runs (the sound, `dead
                // := true`, ten seconds, the black screen) and the mod steps in only where the game
                // reaches for the level travel. This flee is the opposite: it fires within one pump
                // tick of `dead`. So when `death_revive` has armed this death (the seam installed,
                // every revive verb resolved, a session live) the flee stands down and the native
                // chain plays. The arm decision is the same decision as this one, made at the same
                // instant: a death whose flee was skipped and whose revive then turned out
                // impossible would tear the world down with our coop state still in it. Unarmed (a
                // stale signature, verbs unresolved), this flee is the fallback that keeps a death
                // survivable for our layer. An armed death falls through rather than returning: the
                // pump must keep running for the ten seconds, because the pump performs the revive.
                // g_localDeathHandled stays unset: it is shared with the host-close arm above, and
                // latching it here would stop this block from ticking; `death_revive` owns its own
                // per-death state.
                if (!coop::death_revive::ArmedForThisDeath()) {
                    g_localDeathHandled = true;
                    UE_LOGW("net: LOCAL PLAYER DIED -- tearing down coop state synchronously + fleeing "
                            "to the main menu (role=%s; permadeath-rejoinable)",
                            session.role() == coop::net::Role::Host ? "HOST (ends session)" : "CLIENT");
                    // The per-slot teardown (puppet actors and slot state) plus the aggregate
                    // session-wide drains, shared with the native quit-to-menu path.
                    TearDownCoopStateForSessionEnd(session);
                    // Flee to the main menu and hold our layer dormant (shared with the host-close
                    // eject). FleeToMainMenu resets the edge detectors and Stops, then arms the
                    // bypass before travelling, so the detour does not hang the 50k-actor
                    // untitled_1 teardown and the per-tick logic cannot resume on stale gameplay
                    // shadows at the menu. The travel uses the game's own verb, which works for a
                    // dead, ragdolling player.
                    FleeToMainMenu(session, "LOCAL PLAYER DIED");
                    return;
                }
            }
        }
        // Re-resolve the controller only when missing or invalidated; the controller pointer stays
        // stable between possess events. Caching it saves ~250 ProcessEvent dispatches a second at
        // the 125 Hz pump.
        if (g_netLocalController.Raw() && !g_netLocalController.Alive()) g_netLocalController.Reset();
        if (!g_netLocalController.Raw())
            g_netLocalController.Set(ue_wrap::engine::GetController(g_netLocal.Raw()));
        // The one-shot install of the per-subsystem observers (idempotent).
        { PP::Scope _s{PP::Bucket::InstallObs}; coop::subsystems::Install(session); }
        // The outbound local streams: pose, held prop, ragdoll (coop/local_streams). The sender
        // gates its own pose: a joining client's pawn exists through the whole 30-60 s load window,
        // and every position it is parked or teleported through would otherwise stream as our pose,
        // garbage at the source. The ClientWorldReady coherence predicate fires seconds too early:
        // the world and the prop registry are coherent off the level-default props while the pawn
        // still sits at the map's parked spot, and loadObjects teleports it at the load tail. That
        // call cannot be hooked (a script-to-script local call touches neither ProcessEvent nor
        // UFunction::Func; docs/COOP_DISPATCH_VISIBILITY.md), so the gate uses the signal that
        // observes its effect: load-tail quiescence (join_membership_sweep), the keyless-prop, NPC
        // and chipPile population stable for 10 scans x 200 ms, which only happens after the spawn
        // flux containing the player teleport has ended. The destructive divergence sweep trusts
        // the same signal; it resets per world (a mid-session world change closes the gate until
        // the new tail settles) and it is client-scoped. The host keeps the worldUp gate.
        const bool poseAuthoritative =
            isHost ? worldUp
                   : (g_worldReadyAnnounced.load(std::memory_order_relaxed) &&
                      !g_reAnnounceWorldReady.load(std::memory_order_relaxed) &&
                      coop::join_membership_sweep::HasLoadTailQuiesced());
        if (poseAuthoritative)
            coop::local_streams::Tick(session, g_netLocal.Raw(), g_netLocalController.Raw());
    }

    // The per-slot puppet drive: coop/player/puppet_drive (pose spawn and apply, the ragdoll drive,
    // the per-puppet interpolation tick, the wisp hold, the pose diagnostic). The
    // worldReadyAnnounced load is evaluated in the call expression, the same observation point as
    // the drive's own loop (both writers ran earlier in this Tick). remote_prop stays here (a prop
    // concern, not a puppet one), under the same worldUp predicate, after the drive.
    if (worldUp) {
        coop::puppet_drive::DriveTick(session,
                                      g_worldReadyAnnounced.load(std::memory_order_relaxed));

        // The receiver-side held-prop driver. Drains the latest PropPose from the session and
        // applies it (a lookup by Key on first arrival, transform writes thereafter). A stream stop
        // (over 500 ms) is treated as an implicit release.
        { PP::Scope _s{PP::Bucket::RemoteProp}; coop::remote_prop::Tick(session); }
    }  // worldUp (puppet drive + ragdoll + pose-diag + remote prop)

    // Surface session events (joins, disconnects) to the feed and send our Join. g_netLocal goes
    // along so remote_prop::OnRelease can call Aprop_C.thrown(player), the natural throw-sound
    // dispatch.
    { PP::Scope _s{PP::Bucket::EventFeed}; coop::event_feed::Update(session, g_netLocal.Raw()); }
}

bool HasAnnouncedWorldReady() {
    return g_worldReadyAnnounced.load(std::memory_order_relaxed);
}

}  // namespace coop::net_pump

// coop/session/net_pump.cpp -- the per-tick orchestrator: the connection edges, the death policy,
// the reaper, the puppet drive. The sync-module fan-out lists live in coop/session/subsystems.cpp
// and the outbound pose, held-prop and ragdoll streams in coop/player/local_streams.cpp.

#include "coop/session/net_pump.h"
#include "net_pump_world_ready_internal.h"  // co-located private header (src tree, not include/)

#include "coop/comms/chat_feed.h"    // Reset() on the leave-world flee (session UI dies with the session)
#include "coop/comms/chat_bubbles.h" // ResetSlots() on flee -- overhead bubbles are session UI too
#include "coop/comms/chat_sync.h"   // Reset() -- the lobby's chat record dies with the session too
#include "coop/player/nameplate.h"   // ResetSlots() on flee -- HasAny() keeps hud::IsActive() alive in the menu
#include "ui/chat_input.h"           // Close() on flee -- an OPEN chat box must not survive into the menu
#include "ui/voice_panel.h"          // Close() on flee -- don't leave the voice panel open across the transition
#include "coop/dev/dev_lanes.h"
#include "coop/dev/leak_probe.h"
#include "coop/dev/heap_probe.h"
#include "coop/dev/perf_probe.h"
#include "coop/element/element_deleter.h"
#include "coop/dispatch/event_feed.h"
#include "coop/session/join_beacon.h"
#include "coop/session/join_progress.h"
#include "coop/player/local_streams.h"
#include "ui/multiplayer_menu.h"  // MenuTickFn(): the death-flee bypass release condition
#include "coop/net/fake_link_steps.h"  // Tick, and the joiner's `wr` origin
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"      // the puppet array and its drive
#include "coop/props/prop_key_index.h"     // Tick before the inbox
#include "coop/props/registry_reaper.h"    // the reaper and the re-seed engine
#include "coop/props/remote_prop_spawn.h"  // OnClientWorldReadyResetSweep (deferred prop sweep per-world reset)
#include "coop/props/join_membership_sweep.h"  // the join claim and the divergence sweep
#include "coop/session/player_handshake.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_snapshot.h"
#include "coop/player/remote_player.h"
#include "coop/props/remote_prop.h"
#include "coop/props/save_identity_bind.h"  // BindUnboundReCreates: re-bind on re-seed
#include "coop/save/save_transfer.h"
#include "coop/session/subsystems.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "coop/player/death_revive.h"
#include "coop/player/cremation.h"
#include "coop/player/revive_protection.h"
#include "coop/player/run_end_travel.h"
#include "coop/items/broom_stroke.h"
#include "coop/props/trash_morph_gate.h"
#include "coop/items/player_inventory_sync.h"  // TakeJoinPlacement: where the join's appearance goes
#include "coop/session/teleport_client.h"  // the checkpoint join spawn (the client pawn-Set edge)
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
// The session ticks run, which TickSerial reports.
uint64_t g_tickSerial = 0;

// The death-policy one-shot. On a local death the death arc has not armed, the pump flees to the menu
// -- FleeToMainMenu stops the session, then tears every coop game-side state down synchronously, then
// travels with the detour held in transparent bypass (the game's own reload would otherwise run before
// the deferred disconnect cleanup, leaving orphan puppets and mirrors in the dying world).
// OnSessionStart resets it.
bool g_localDeathHandled = false;

// The ceiling on the transparent bypass after a flee. The bypass covers the world teardown (the
// detour otherwise stalls the 50k-actor untitled_1 teardown) and releases the moment
// ui_menu_C::Tick first dispatches, since the MULTIPLAYER button is injected by an observer on it;
// this timer only bounds the case where that tick never resolves, and is generous on purpose.
constexpr int kDeathMenuBypassMs = 30 * 1000;

// The terminal eject to the main menu: Stop the session, tear every coop state down (TearDownSession,
// which resets the edge detectors and the announce state among it), arm the bypass, then travel through
// the game's own transition verb (the bypass first, so the engine teardown the travel triggers runs with
// the detour dormant). Both the Stop and the teardown are no-ops on a session already ended. A one-shot
// latch: more than one path can find a session dead, and the travel is dispatched once; OnSessionStart
// resets it.
bool g_fleeing = false;

// The stop TearDownSession has BEGUN tearing down, and the stop it has FINISHED (both Session::StopCount
// values). Begun is the run-once latch -- a second call for the same stop no-ops, so a teardown never runs
// twice. Finished is what EndOwed reads, published only when the pass completes OR faults, so no start
// begins over a teardown still running (EndOwed stays true until then) and a faulted pass is marked done,
// never retried over half-cleared state (as MTA unloads once). Both written on the game thread; finished
// read with acquire by the timeline's EndOwed. A session that never ran never stopped, so both stay here.
std::atomic<uint64_t> g_tornDownBegun{0};
std::atomic<uint64_t> g_tornDownFinished{0};

// `travel` is false when the game's own quit-to-menu transition is already in flight (a second
// transition would load the menu twice); the rest of the tail is identical.
void FleeToMainMenu(coop::net::Session& session, const char* why, bool travel = true) {
    if (g_fleeing) return;  // already travelling to the menu for this session
    g_fleeing = true;
    // The session ends first, then the travel: the network stops, then every coop state goes (MTA,
    // ~CClientGame: StopNetwork at reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:537
    // before SAFE_DELETE(m_pManager) at :559), so no destroy seam sees a connected session and nothing
    // queued runs against the dying world. Both are no-ops for a session already ended.
    session.Stop();
    TearDownSession(session, why);
    // Hold the detour dormant over the teardown, resuming on the menu's first ui_menu_C::Tick;
    // kDeathMenuBypassMs is the ceiling (and the whole hold when MenuTickFn() is null). At a
    // pause-menu decision the pause menu is a ui_menu_C too, so this lifts within a frame;
    // harmless, the session is already stopped.
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
        // still in the gameplay world; say so loudly, and to the player unless a notice already
        // stands -- the reason the session ended (a kick, a ban, the host's own close) comes first.
        UE_LOGE("net: %s -- ReturnToMainMenu FAILED to dispatch; still in the gameplay "
                "world. Relaunch.", why);
        if (!coop::join_progress::NoticePending())
            coop::join_progress::NoteDisconnect(coop::net::EndReason::WorldNotLeft,
                                                "the session has ended; restart the game",
                                                /*afterJoin=*/true);
    }
}

}  // namespace

// The per-person world teardown, driven by the ledger row transition: the departed peer's puppet,
// then the per-slot subsystem fan-out. It fires on a replacement and on a client too, where a
// connection edge never could; every DisconnectSlot body is safe there (the host-authoritative ones
// self-gate).
void OnSlotReplaced_TearDownWorld(int slot, const coop::roster_ledger::Row& outgoing,
                                  const coop::roster_ledger::Row& incoming) {
    if (slot <= 0) return;  // slot 0 is the host; its departure ends the session, not a slot
    auto* s = g_tickSession;
    if (!s) return;         // no session context (nothing to tear down against)
    // The slot's relayed streams follow its occupancy: the departed one's late packets are refused,
    // so no stored pose can respawn its puppet, and a live one starts them over.
    s->OnRosterOrigin(slot, outgoing.occupied(), outgoing.originContext, incoming.occupied(),
                      incoming.originContext);
    if (!outgoing.occupied()) return;
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
    coop::death_revive::OnSessionStart();          // the arm + per-death latches
    coop::player::run_end_travel::OnSessionStart();  // the run-ending seam's counters + one-shots
    coop::trash_morph_gate::OnSessionStart();        // the trash morph gate's per-verb tallies
    coop::broom_stroke::OnSessionStart();            // the broom lane's per-session stroke tally
    coop::registry_reaper::OnSessionStart();  // the been-in-gameplay latch (menu guard)
    coop::local_streams::OnSessionStart();  // held-prop + ragdoll edge detectors
}

bool IsFleeing() {
    return g_fleeing;
}

void FleeAfterNativeMenuTravel(coop::net::Session& session, const char* why) {
    FleeToMainMenu(session, why, /*travel=*/false);
}

void FleeToMainMenuOnDeath(coop::net::Session& session, const char* why) {
    // The public entry for a death the revive gave up on; FleeToMainMenu ends the session before it
    // travels. Idempotent through g_fleeing. Game thread.
    FleeToMainMenu(session, why);
}

void TearDownSession(coop::net::Session& session, const char* why) {
    UE_ASSERT_GAME_THREAD("net_pump::TearDownSession (puppets + every lane's session state)");
    if (session.running()) return;  // a live session is not ended here; its stop comes first
    const uint64_t stops = session.StopCount();
    if (stops == g_tornDownBegun.load(std::memory_order_relaxed)) return;  // never ran, or this stop begun/done
    // One teardown per stop: begun latches it so a later owed-end call for the same stop no-ops -- the
    // run-once shape MTA has (its unload is an idempotent state change, reference/mtasa-blue/Client/core/CModManager.cpp:33-48).
    // The FINISHED count is published at scope exit -- cleanly AND on a fault (the destructor runs on the
    // pump's own unwind, as RunAndWait's guard does) -- so EndOwed stays true until this pass ends (no
    // start begins over it). Marking a FAULTED pass done, never re-run over half-cleared state, is ours;
    // MTA has no fault policy.
    g_tornDownBegun.store(stops, std::memory_order_relaxed);
    struct FinishGuard {
        uint64_t stops;
        ~FinishGuard() { g_tornDownFinished.store(stops, std::memory_order_release); }
    } finish{stops};
    coop::cremation::OnSessionEnd();
    coop::revive_protection::End(coop::revive_protection::EndWhy::Session);
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    ForgetWorldReadyAnnounce();
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // DestroySlot is the puppet's Destroy plus UnregisterPuppet; its interleave with DisconnectSlot
        // is composed here only.
        coop::puppet_drive::DestroySlot(slot);
        coop::subsystems::DisconnectSlot(session, slot);
    }
    coop::subsystems::DisconnectAll();
    // Every session-scoped overlay dies here: the last lines would otherwise ride their 11 s TTL into
    // the menu, and one stale nameplate re-activates the whole HUD there through hud::IsActive(). Not
    // in DisconnectAll, which also runs on a host whose client left, and the host keeps its UI.
    coop::chat_feed::Reset();
    coop::chat_sync::Reset();  // the record + the applied range die with the session
    coop::chat_bubbles::ResetSlots();
    coop::nameplate::ResetSlots();
    ui::chat_input::Close();
    ui::voice_panel::Close();
    // The stop flipped every slot to disconnected, and the next event_feed::Update would read that as
    // "<X> left the game" into the just-cleared feed; the edge detectors are neutralised so the local
    // teardown posts no departure. The host-stays case never ends a session, so its toast survives.
    coop::event_feed::SuppressPeerLeaveEdges();
    // The ledger clears here: the reconcile early-returns once the session stops, so the rows would
    // survive into the menu. After SuppressPeerLeaveEdges, so the transitions tear person-state down
    // silently; their slot fan-out finds the slots above already torn down.
    coop::roster_ledger::ClearAll();
    // The last deferred-element flush: Tick holds the only other one, and no Tick runs for a stopped
    // session.
    coop::element::ElementDeleter::Get().Flush();
    UE_LOGI("net: session ended (%s) -- coop state torn down", why);
}

bool EndOwed(const coop::net::Session& session) {
    return !session.running() &&
           session.StopCount() != g_tornDownFinished.load(std::memory_order_acquire);
}

uint64_t TickSerial() { return g_tickSerial; }

void Tick(coop::net::Session& session) {
    // Game thread only: the puppet array and ElementDeleter::Flush are game-thread side tables; one
    // guard at the top covers everything below.
    UE_ASSERT_GAME_THREAD("net_pump::Tick (puppet drive + ElementDeleter::Flush)");
    // A session's tick: a stopped session has none. Its end is TearDownSession's, run by whoever stopped
    // it, so a composite posted before a stop and run after it finds nothing to do here.
    if (!session.running()) return;
    ++g_tickSerial;
    // Scope the Session for the ledger's teardown subscriber (g_tickSession); RAII, so an early
    // return leaves no stale pointer.
    struct TickSessionScope {
        explicit TickSessionScope(coop::net::Session& s) { g_tickSession = &s; }
        ~TickSessionScope() { g_tickSession = nullptr; }
    } _tickSessionScope{session};
    // The session's hold on the script gate, and the lanes that watch inside a world load, from its first
    // tick (coop/session/subsystems.h). Here, since both loops that drive a session tick come through: the
    // play loop, and a menu-mode join's transfer wait, which blocks the play loop until the world load.
    coop::subsystems::HoldSessionGate();
    coop::prop_key_index::Tick();  // a seed still owed, the births that finished loading: before any inbox read
    coop::subsystems::InstallLoadWatchers(session);

    // ---- Hitch and source probe (diagnostic, always on, near free) ----
    // [HITCH] times the gap between consecutive game-thread Ticks (the whole frame); [HITCH-SRC],
    // an RAII at the end of the body, this Tick's own duration. A [HITCH] with no [HITCH-SRC] is
    // an UNATTRIBUTED frame, not an engine verdict: this Tick is one posted task among many in the
    // same drain, and every other one was outside any bracket, so the drain now carries the outer
    // [HITCH-SRC] (ue_wrap/core/game_thread.cpp) and the perf probe's buckets split the rest.
    {
        using hclk = std::chrono::steady_clock;
        static hclk::time_point sPrevTickStart{};
        const hclk::time_point nowTp = hclk::now();
        if (sPrevTickStart.time_since_epoch().count() != 0) {
            const long long frameMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(nowTp - sPrevTickStart).count();
            if (frameMs >= 40)
                UE_LOGI("[HITCH] frame = %lld ms (>40ms stutter; with NO [HITCH-SRC] on this frame the "
                        "cost is UNATTRIBUTED -- read the [perf] subsystem buckets before calling it "
                        "engine-side GC/render/physics)",
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
    coop::net::fake_link_steps::Tick();  // the fake-link steps that came due


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

    // The client's world-ready announce lives in net_pump_world_ready.cpp; a reason back is the
    // session ending, and the flee is this file's.
    if (const char* why = TickWorldReadyAnnounce(session, localNow, worldUp, isConnected, isHost)) {
        FleeToMainMenu(session, why);
        return;
    }

    if (!isConnected) ForgetWorldReadyAnnounce();  // re-announce next connection

    if (g_wasConnected && !isConnected) {
        // The aggregate disconnect (all peers gone): the OnDisconnect calls for session-wide state
        // (the per-slot block handled the rest). The link is Connected from admission on, long
        // before a joiner's world is up, so a client here may still be inside its join: the phase
        // is read first, since Reset clears it.
        const coop::join_progress::Phase phase = coop::join_progress::CurrentPhase();
        const bool joinInFlight = !isHost && phase != coop::join_progress::Phase::Idle;
        // Still at the menu, no world of the host's yet: the join fails the way a refused dial
        // does. Fail wins the abort, the harness's drain stops the session, drops the cover and
        // reopens the browser, under COULD NOT CONNECT with the first notice of the attempt
        // standing; the flee below would stop the session and travel the menu to the menu on
        // top of that drain.
        const bool noWorldYet = joinInFlight &&
                                (phase == coop::join_progress::Phase::Connecting ||
                                 phase == coop::join_progress::Phase::Downloading);
        if (noWorldYet) {
            const coop::net::HostClose close = session.TakeHostCloseReason();
            const coop::net::EndReason code = close.code == coop::net::EndReason::None
                                                  ? coop::net::EndReason::LinkLost : close.code;
            const coop::net::EndReasonInfo& why = coop::net::Describe(code);
            UE_LOGW("net: HOST CLOSED OUR CONNECTION mid-join [%s] %s (%s) -- the join fails",
                    why.id, why.text,
                    close.text.empty() ? "connection lost" : close.text.c_str());
            coop::join_progress::Fail(code, close.text);
        } else {
            coop::join_progress::Reset();
        }
        if (isHost) {
            // The host's last client left: the session lives on, so only what the peers' presence
            // held goes. A client's session ends instead, below or through the join's abort drain,
            // and TearDownSession takes all of it then.
            const auto stats = coop::subsystems::DisconnectAll();
            UE_LOGI("net: all peers gone -- cleared %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries; takeObjInFlight=0",
                    stats.snapPending, stats.initProcessedDropped);
        }

        // The client eject: losing the host ends the session, so flee to the menu rather than
        // strand the player in a hostless world, one it stands in or one the engine is loading
        // for it. One-shot through g_localDeathHandled, shared with the death flee; FleeToMainMenu
        // ends the session (the stop, then the teardown) before it travels.
        if (!isHost && !noWorldYet && !g_localDeathHandled) {
            g_localDeathHandled = true;
            const coop::net::HostClose close = session.TakeHostCloseReason();
            if (close.code == coop::net::EndReason::None) {
                // A stop this machine initiated captures no close, so there is nothing to show.
                UE_LOGW("net: HOST CLOSED OUR CONNECTION (no close captured) -- fleeing to the "
                        "main menu");
            } else {
                const coop::net::EndReasonInfo& why = coop::net::Describe(close.code);
                UE_LOGW("net: HOST CLOSED OUR CONNECTION [%s] %s (%s) -- fleeing to the main menu",
                        why.id, why.text,
                        close.text.empty() ? "connection lost" : close.text.c_str());
                // The player is about to land at the menu with no session: say why there, with
                // the code, as a failed join while one was still in flight and as a disconnect
                // after it.
                coop::join_progress::NoteDisconnect(close.code, close.text,
                                                    /*afterJoin*/!joinInFlight);
            }
            FleeToMainMenu(session, "host closed connection");
            return;
        }
    }
    // The join screen's stage, from the session's link stage: two atomic loads per tick while
    // the join is in its first phase, and the transfer's own state (under its mutex, so only
    // then, never through the download) supplies the last stage before the download.
    if (!isHost && coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Connecting) {
        namespace jp = coop::join_progress;
        using coop::net::LinkStage;
        switch (session.linkStage()) {
        case LinkStage::Dialing:      jp::NoteStage(jp::Stage::Dialing); break;
        case LinkStage::FindingRoute: jp::NoteStage(jp::Stage::FindingRoute); break;
        case LinkStage::SocketUp:     jp::NoteStage(jp::Stage::ProvingIdentity); break;
        case LinkStage::Admitted:
            // An armed transfer is the wait for the host's world until the phase moves on (the
            // begin, the no-save answer and the finish all land in the timeline loop's next
            // note); a transfer never armed is the in-gameplay join.
            jp::NoteStage(coop::save_transfer::GetClientState() ==
                                  coop::save_transfer::ClientState::Idle
                              ? jp::Stage::Joining : jp::Stage::WaitingForWorld);
            break;
        case LinkStage::Idle: break;
        }
    }
    // The client connect-failure edge: a browser join that never reached Connected (a dead address,
    // the host not up). The aggregate edge above needs g_wasConnected to have latched, so without
    // this the loading screen would hang on "Connecting..." with no phase token moving. Precise:
    // client role, a join Active, never connected, the state back at Disconnected. Fail() is
    // idempotent.
    if (!isHost && !g_wasConnected && coop::join_progress::Active() &&
        session.state() == coop::net::ConnState::Disconnected) {
        const coop::net::HostClose close = session.TakeHostCloseReason();
        coop::join_progress::Fail(close.code == coop::net::EndReason::None
                                      ? coop::net::EndReason::LinkLost : close.code,
                                  close.text);
    }
    g_wasConnected = isConnected;

    // Up to ~100 snapshot candidates per tick while an enumeration is in progress (a no-op on
    // empty).
    if (isConnected && worldUp) { PP::Scope _s{PP::Bucket::SnapshotDrain}; coop::prop_snapshot::DrainChunk(); }

    // The host's once-a-second word to each joiner. Its owners: save_transfer (the chunk pump and
    // the capture), prop_snapshot::DrainChunk (the bracket, just now), and join_beacon itself for
    // the Draining phase once the bracket is closed. Ungated by worldUp: a host serving a joiner is
    // in a world, and the gate is for the joiner's own menu ticks. A no-op on a client; a slot
    // nobody worked falls silent, and a delivered bracket stops its own Draining.
    if (isHost) coop::join_beacon::Tick();

    // Outside the world-up gate on purpose: what these dev readers see lies inside the world load,
    // and a reader that only starts once the world is up cannot see that window at all.
    coop::dev::dev_lanes::TickLoadWindow(session);

    // The per-tick gameplay subsystem chain (connect-broadcast drains, module polls and applies,
    // NPC streams, dev probes), world-up-gated whole: every one acts on gameplay-world state, and a
    // queued connect broadcast describes in-world state, so waiting costs nothing.
    if (worldUp) {
        coop::subsystems::TickGameplay(session, isConnected, isHost, g_fleeing);
    }

    if (g_netLocal.Raw() && !g_netLocal.Alive()) { g_netLocal.Reset(); g_netLocalController.Reset(); }
    if (!g_netLocal.Raw()) {
        g_netLocal.Set(localNow);  // resolved once at the top of this tick
        // The join spawn: a client appearance is placed by us, not left at the transferred save's
        // playerTransform (the host's own position). This pawn-Set edge is exactly "a new local
        // body exists": once per world appearance, while the load screen still covers the swap. A
        // returning player appears once at the spot their profile says they were standing on (a
        // drill's join_at_host leaves that one appearance at the host's); every other body of the
        // session, a respawn included, at the start point.
        if (localNow && isConnected && !isHost) {
            namespace PIS = coop::player_inventory_sync;
            float x = 0, y = 0, z = 0, yaw = 0;
            switch (PIS::TakeJoinPlacement(x, y, z, yaw)) {
            case PIS::JoinPlacement::ProfilePose:
                coop::teleport_client::ApplyLocally({x, y, z, 0.f, yaw, 0.f});
                UE_LOGI("net_pump: CLIENT spawn -> the profile's pose (%.0f, %.0f, %.0f) yaw=%.0f",
                        x, y, z, yaw);
                break;
            case PIS::JoinPlacement::AtHost:
                UE_LOGI("net_pump: CLIENT spawn -> left where the transferred save put it, the host's "
                        "position (join_at_host)");
                break;
            case PIS::JoinPlacement::StartPoint:
                coop::teleport_client::ApplyLocally(
                    {ue_wrap::profile::name::kKPPSpawnX, ue_wrap::profile::name::kKPPSpawnY,
                     ue_wrap::profile::name::kKPPSpawnZ, 0.f, 0.f, 0.f});
                UE_LOGI("net_pump: CLIENT spawn -> KPP start point (join/world appearance)");
                break;
            }
        }
    }
    // The !g_localDeathHandled gate: once the death is handled every local send stops, or the
    // ragdoll sender would keep reading the dead player's pelvis ~100 times a second on the way to
    // the menu. Raw() is validated by the block just above, this tick.
    if (g_netLocal.Raw() && !g_localDeathHandled) {
        // The pump barrier of the death arc: arms on the `dead` rising edge and runs a revive the
        // run-ending seam asked for. Here, not in the seam's own callback, because that runs inside
        // the VM's body loop, where a UFunction dispatch re-enters the interpreter.
        coop::death_revive::Tick(session, g_netLocal.Raw());
        coop::cremation::Tick(session);
        coop::revive_protection::Tick();
        // A revive that gave up has torn the session down and fled, as the local-death branch below
        // does; nothing after it may run for this session, the lanes' Install least of all.
        if (g_fleeing) return;
        // The death policy: on a local death FleeToMainMenu stops the session and tears every coop
        // game-side state down this frame. Stopping alone is not enough: the game's death reload
        // blocks the game thread at once, so the deferred disconnect cleanup never runs. `dead` is
        // true only on a real death (a faint or KO leaves it false). The host gates on running()
        // (it is still hosting in Handshaking, Connected and Disconnected alike, and Tick runs only for a
        // running session); the client on connected(), since the host-close path already ejected it.
        const bool sessionLiveForDeath = isHost || session.connected();
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
                    // End the session (stop, then every coop state), flee to the menu and hold our
                    // layer dormant (shared with the host-close eject); the travel uses the game's own
                    // verb, which works for a dead, ragdolling player.
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
        // teleported through positions that must not stream as our pose. ClientWorldReady follows the
        // world's save replay (the gamemode's loadObjects, watched by name once it returns,
        // docs/coop-dispatch-visibility.md), so the client gate is that announce plus load-tail
        // quiescence (join_membership_sweep), the signal that observes the load's effect, reset per
        // world. The host keeps the worldUp gate.
        const bool poseAuthoritative =
            isHost ? worldUp
                   : (IsWorldSettled() &&
                      coop::join_membership_sweep::HasLoadTailQuiesced());
        if (poseAuthoritative)
            coop::local_streams::Tick(session, g_netLocal.Raw(), g_netLocalController.Raw());
    }

    // The per-slot puppet drive (coop/player/puppet_drive); the worldReadyAnnounced load is
    // evaluated in the call, the same observation point as the drive's loop. remote_prop stays here
    // (a prop concern), under the same gate, after the drive.
    if (worldUp) {
        coop::puppet_drive::DriveTick(session,
                                      HasAnnouncedWorldReady());

        // The receiver-side held-prop driver: the latest PropPose applied (a Key lookup on first
        // arrival, transform writes after); a stream stop over 500 ms is an implicit release.
        { PP::Scope _s{PP::Bucket::RemoteProp}; coop::remote_prop::Tick(session); }
    }  // worldUp (puppet drive + ragdoll + pose-diag + remote prop)

    // Session events (joins, disconnects) to the feed, and our Join; g_netLocal goes along so
    // remote_prop::OnRelease can dispatch Aprop_C.thrown(player).
    { PP::Scope _s{PP::Bucket::EventFeed}; coop::event_feed::Update(session, g_netLocal.Raw()); }
}

}  // namespace coop::net_pump

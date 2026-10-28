// coop/session/net_pump_world_ready.cpp -- the client's world-ready announce: the gates it waits on,
// the ClientWorldReady send, the re-announce after a world change, and the queries over that state.
// The tick that calls it lives in coop/session/net_pump.cpp.

#include "coop/session/net_pump.h"
#include "net_pump_world_ready_internal.h"  // co-located private header (src tree, not include/)

#include "coop/dev/announce_hold.h"  // the pre-announce drill arms hold this client's announce
#include "coop/session/join_progress.h"
#include "coop/net/fake_link_steps.h"  // NoteOrigin: the joiner's `wr` origin
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"  // OnClientWorldReady: the deferred-adoption per-world reset
#include "coop/element/bind_wait.h"  // Clear: the messages held for the old world's binds
#include "coop/world/event_fire_sync.h"  // OnClientWorldReady: the fires queued in the join window
#include "coop/world/daily_task_sync.h"  // OnClientWorldReady: a task kept from the world left behind
#include "coop/props/prop_key_index.h"     // CanAnswer at the world-ready gate
#include "coop/props/join_membership_sweep.h"  // OnClientWorldReadyResetSweep
#include "coop/session/rig_ready.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/session/world_load_episode.h"  // the announce waits on the load-tail quiescence latch

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>

namespace coop::net_pump {

namespace WLE = coop::world_load_episode;
namespace WI = ue_wrap::world_identity;

// Has this client announced world-ready for the current connection? Gates the world-dependent tick
// blocks during a save-transfer join's menu window (no actors into the menu world). Reset on
// disconnect.
static std::atomic<bool> g_worldReadyAnnounced{false};

// The re-announce request, set when a world-change re-seed completes on a world other than the one
// this connection settled on (a mid-session world swap): the re-announce drives the host's
// ConnectReplayForSlot to re-assert every host-authoritative state into the new world (the only way
// keyless chipPiles re-acquire their host eid). Reset on send and on disconnect.
static std::atomic<bool> g_reAnnounceWorldReady{false};

// The gate a re-announce after the join last waited on, named by the pump: the join is Idle then, so
// join_progress names nothing. Game thread; reset with the announce state.
static coop::join_progress::WorldGate g_reAnnounceGate = coop::join_progress::WorldGate::None;

// The world announced in, held as a weak identity (its slot and serial), for IsInAnnouncedWorld: a later
// world is a new object even where the allocator hands it the old one's address. Game thread; stamped from
// the local pawn at each announce, reset on disconnect.
static ue_wrap::CachedObjRef g_announcedWorldRef;

// The latch that acts on a request's failure once per connection, and the one-shot for a world change
// that finds no baseline world. Game thread; both cleared with the announce state.
static bool g_failureActed = false;
static bool g_noBaselineSaid = false;

// The save-transfer kerfur-ghost reconcile lives in npc_adoption (it owns the timing); net_pump
// only notifies it at the announce (OnClientWorldReady) so it resets its per-world state.

// The world-ready announce state, forgotten when the connection goes and when the session ends, so the
// next connection announces again and re-stamps its world.
void ForgetWorldReadyAnnounce() {
    g_worldReadyAnnounced.store(false, std::memory_order_relaxed);
    g_reAnnounceWorldReady.store(false, std::memory_order_relaxed);
    g_reAnnounceGate = coop::join_progress::WorldGate::None;
    g_announcedWorldRef.Reset();
    g_failureActed = false;
    g_noBaselineSaid = false;
    coop::world_load_episode::ClearBaselineWorld();
}

// The announce axis' one owner; registry_reaper only requests through here. No-op on the host.
// MTA never re-announces: the server owns a map change, and a repeated in-game notice is a
// disconnect (reference/mtasa-blue/Server/mods/deathmatch/logic/CRPCFunctions.cpp:94-122). A level
// travel here is local to the client, so the client says it and the host caps the repeat.
void MaybeRequestReAnnounce(coop::net::Session& session, void* reapWorld) {
    if (session.role() == coop::net::Role::Host) return;
    // A world is the same only when the same address holds the same live object. Without a baseline
    // world there is nothing to compare: the join's own announce covers it.
    if (!WLE::HasBaselineWorld()) {
        if (!g_noBaselineSaid)
            UE_LOGI("net_pump: a world change with no baseline world -- %s",
                    WLE::AwaitingSaveReplay() ? "the join waits on its replay"
                                              : "the first announce will cover it");
        g_noBaselineSaid = true;
        return;
    }
    if (WLE::IsBaselineWorld(reapWorld)) {
        UE_LOGI("net_pump: world-change re-seed on the SAME world this connection settled on (%p) -- "
                "NOT re-announcing (suppresses the join menu-shadow-drain double-snapshot + "
                "kerfur re-adopt dupe)", reapWorld);
        return;
    }
    // The re-announce waits for the new world's save replay and its load tail, as the first announce
    // did: a request the probe's tick turns into a session.
    if (WLE::RequestWorldChangeProbe()) {
        g_reAnnounceWorldReady.store(true, std::memory_order_relaxed);
        // A reload is a teardown plus a rebuild: raise the reconcile window (kind = load) so the
        // destroy seam and the drop intent stay suppressed through the re-replay. InEpisode is not
        // raised: the lane parks' reload semantics are unmeasured.
        coop::world_load_episode::RaiseReconcileForReload();
        UE_LOGI("net_pump: world change -- re-announce owed (%p)", reapWorld);
    } else {
        UE_LOGI("net_pump: world change while the join waits on its replay -- the join covers it (%p)",
                reapWorld);
    }
}

// MTA's client has no local world to restore (its map arrives in CMapManager::SendMapInformation,
// reference/mtasa-blue/Server/mods/deathmatch/logic/CMapManager.cpp:195), so its in-game notice
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:3526) waits only for the
// engine; ours also waits for the host's save to finish replaying here, as Source's server
// restores a loaded save's entities in the client-activation path (EndRestoreEntities inside
// CServerGameClients::ClientActive, reference/source-sdk-2013/src/game/server/gameinterface.cpp:2710).
const char* TickWorldReadyAnnounce(coop::net::Session& session, void* localNow, bool worldUp,
                                   bool isConnected, bool isHost) {
    // A client announces world-ready once per connection, when a gameplay world is up, the prop
    // registry expresses it, the load tail has quiesced and the prop key index holds the world, so the
    // host's stream is read by key against it. MTA gates the same step on the engine being loaded
    // (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:1364-1367) and then asks for
    // the stream (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:463); this
    // message is both. Its terms have no key index to wait for, its ids being minted by their owner
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CElementIDs.cpp:23). The host streams the
    // whole world state on it, so announcing into a churning load would put the authoritative stream
    // into it. The load tail is not read before the world's own save replay has returned, which a
    // failed watch or a world change's bound ends by name; the probe after it is deadline-capped, so a
    // pathological load still announces. Re-fires on a re-announce request under the same coherence
    // gate.
    const bool reAnnounce = g_reAnnounceWorldReady.load(std::memory_order_relaxed);
    if (!isHost && isConnected &&
        (!g_worldReadyAnnounced.load(std::memory_order_relaxed) || reAnnounce)) {
        // A connection that joined from inside a world (no join load) has no request: its baseline is
        // the world it stands in at the connect.
        if (!g_worldReadyAnnounced.load(std::memory_order_relaxed) && !WLE::AwaitingSaveReplay() &&
            !WLE::HasBaselineWorld() && WI::CurrentWorldKind() == WI::WorldKind::Gameplay) {
            WLE::SetBaselineWorld(WI::CurrentWorld());
            UE_LOGI("net_pump: baseline world set at the connect (no join)");
        }
        // Name the gate BEFORE testing the rest: most of these have no deadline of their own, and
        // with the whole-join failsafe deleted the joiner's phase token is what stands behind them.
        // TickQuiesceProbe must still run every tick, so it is called first and its result reused --
        // naming a gate must never cost the probe a tick. The key index is asked last, once the rest
        // are clear.
        const bool seeded   = coop::prop_element_tracker::HasSeededOnce();
        const bool thisWorld = seeded && coop::prop_element_tracker::IsRegistrySeededForCurrentWorld();
        const bool purging  = coop::prop_element_tracker::InPurgeEpisode();
        const bool quiesced = coop::world_load_episode::TickQuiesceProbe();
        const bool awaitingReplay = WLE::AwaitingSaveReplay();
        const WLE::RequestFailureInfo& f = WLE::RequestFailure();
        if (f.failure != WLE::Failure::None && !g_failureActed) {
            g_failureActed = true;
            const bool watchDead = f.failure == WLE::Failure::WatchDead;
            const auto code = watchDead ? coop::net::EndReason::ScriptHooksUnavailable
                                        : coop::net::EndReason::WorldNeverSettled;
            if (coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
                coop::join_progress::Fail(code, f.text);
            else {
                coop::join_progress::NoteDisconnect(code, f.text, /*afterJoin*/ true);
                return watchDead ? "the save-replay watch is not live"
                                 : "the new world's save replay never returned";
            }
        }
        // An install found unable to index after the session started (its watches resolve on the game
        // thread) can never clear the key term: the join ends by name rather than waiting on it.
        if (coop::prop_key_index::Unavailable())
            coop::join_progress::Fail(coop::net::EndReason::ScriptHooksUnavailable,
                                      "the prop key index cannot hold this world");
        const bool keysReady = worldUp && seeded && thisWorld && !purging && quiesced &&
                               coop::prop_key_index::CanAnswer();
        using WG = coop::join_progress::WorldGate;
        const WG gate = !worldUp    ? WG::NoLocalPlayer
                      : !seeded   ? WG::RegistryUnseeded
                      : !thisWorld ? WG::RegistryOtherWorld
                      : purging   ? WG::RegistryPurging
                      : awaitingReplay ? WG::SaveReplayPending
                      : !quiesced ? WG::WorldSettling
                      : !keysReady ? WG::KeyIndexPending
                                  : WG::None;
        coop::join_progress::NoteWorldReadyGate(gate);
        // A re-announce made after the join (a world change) waits at the same gates with the join Idle,
        // where the call above names nothing.
        if (reAnnounce && coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
            gate != g_reAnnounceGate) {
            g_reAnnounceGate = gate;
            if (gate != WG::None)
                UE_LOGI("net_pump: the re-announce waits on this machine -- %s",
                        coop::join_progress::WorldGateText(gate));
        }
        if (keysReady && !coop::dev::announce_hold::Held()) {
            if (session.SendReliableToSlot(0, coop::net::ReliableKind::ClientWorldReady,
                                           nullptr, 0)) {
                g_worldReadyAnnounced.store(true, std::memory_order_relaxed);
                g_reAnnounceWorldReady.store(false, std::memory_order_relaxed);
                g_reAnnounceGate = WG::None;
                // Stamp the world announced against, once per real announce. The same reader as the
                // reaper's world gate: the two are compared in MaybeRequestReAnnounce, and
                // FindObjectByClass answers "a world object exists" (the incoming world, while the
                // player chain still reads null), not "the world the player is in".
                WLE::SetBaselineWorld(WI::CurrentWorld());  // a null read keeps the baseline held
                g_announcedWorldRef.Set(ue_wrap::world_identity::WorldOf(localNow));  // fresh this tick
                // A fresh connect replay is about to arrive: reset the deferred-adoption per-world
                // state so the new world re-adopts its save NPCs and re-sweeps orphans.
                coop::npc_adoption::OnClientWorldReady();
                coop::kerfur_prop_adoption::OnClientWorldReady();  // drop the stale prop-kerfur pending set
                coop::element::bind_wait::Clear("the world changed");  // the messages held for the old world's binds
                coop::event_fire_sync::OnClientWorldReady();  // the fires queued in the join window replay now
                coop::daily_task_sync::OnClientWorldReady();  // a task kept from the world left behind goes
                // The same reset for the deferred prop divergence sweep: one armed for the prior
                // world must not fire against this one.
                coop::join_membership_sweep::OnClientWorldReadyResetSweep();
                // The join's wait changes owner here: this client has a world, and everything left
                // is the host's bracket. Named so, the cover stops saying "loading the world" over
                // a wait that is the host's, and the phase gets a watchdog of its own.
                coop::join_progress::NoteWorldReady();
                coop::net::fake_link_steps::NoteOrigin(coop::net::fake_link_steps::Origin::WorldReady);
                UE_LOGI("net_pump: ClientWorldReady announced (world up + registry coherent + load "
                        "tail quiesced + key index caught up%s)",
                        reAnnounce ? " -- re-announce after world-change" : "");
                coop::rig_ready::Say("world-ready");
            }
        }
    }
    return nullptr;
}

bool HasAnnouncedWorldReady() {
    return g_worldReadyAnnounced.load(std::memory_order_relaxed);
}

bool IsWorldSettled() {
    return g_worldReadyAnnounced.load(std::memory_order_relaxed) &&
           !g_reAnnounceWorldReady.load(std::memory_order_relaxed);
}

bool IsInAnnouncedWorld(void* obj) {
    if (!obj || !g_worldReadyAnnounced.load(std::memory_order_relaxed) ||
        g_reAnnounceWorldReady.load(std::memory_order_relaxed))
        return false;
    // A world the reader cannot name (a recook renamed a field it resolves, so the object's world or
    // the stamp reads null) is no licence for the client to act on its own: the connection's announce
    // stands for it, as it did before this test was per world.
    void* const world = ue_wrap::world_identity::WorldOf(obj);
    if (!world || !g_announcedWorldRef.Raw()) return true;
    return g_announcedWorldRef.Is(world);
}

}  // namespace coop::net_pump

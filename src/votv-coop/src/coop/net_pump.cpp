// coop/net_pump.cpp -- see coop/net_pump.h.
//
// Extracted from harness.cpp 2026-05-28 to keep harness.cpp at its
// boot/scenario glue role and bring net_pump.cpp under the modular
// soft cap (audit deferred-plan PR-4.13).
//
// 2026-06-12 (modular soft cap, 1290 LOC): the sync-module fan-out lists
// moved to coop/subsystems.cpp (the wiring registry every new feature edits)
// and the outbound pose/held-prop/ragdoll streams moved to
// coop/local_streams.cpp. This file is the per-tick ORCHESTRATOR: connection
// edges, death policy, the reaper, the puppet drive.

#include "coop/net_pump.h"

#include "coop/dev/leak_probe.h"
#include "coop/dev/heap_probe.h"
#include "coop/dev/perf_probe.h"
#include "coop/element/element_deleter.h"
#include "coop/event_feed.h"
#include "coop/join_progress.h"
#include "coop/local_streams.h"
#include "coop/multiplayer_menu.h"  // MenuTickFn(): the death-flee bypass release condition
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/npc_adoption.h"
#include "coop/kerfur_prop_adoption.h"  // K-6  // OnClientWorldReady (v75 deferred-adoption per-world reset)
#include "coop/remote_prop_spawn.h"  // OnClientWorldReadyResetSweep (deferred prop sweep per-world reset)
#include "coop/player_handshake.h"
#include "coop/players_registry.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_snapshot.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/save_transfer.h"
#include "coop/subsystems.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hot_path_guard.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

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

// Per-slot remote-player puppets. Indexed by the coop::players::Registry
// slot convention: slot 0 = host (only used on CLIENT processes to hold the
// host's puppet); slots 1..kMaxPeers-1 = clients (used on HOST processes to
// hold each connected client's puppet). On HOST, slot 0 is unused
// (representing host self); on CLIENT, slots 1..kMaxPeers-1 are unused.
// Each entry's actor / spawn-retry / Tick state is independent.
std::array<coop::RemotePlayer, coop::players::kMaxPeers> g_puppets{};

// Cached local mainPlayer_C for the net pump. coop::players::Registry::Get().Local()
// already caches + filters puppets via the controller discriminator
// (per RULE 1 + [[feedback-always-use-user-test-poses]]); we just hold
// a local cache on top to skip the atomic load in the hot pump path.
void* g_netLocal = nullptr;
// Cached controller for the same pawn -- avoids 2 ProcessEvent dispatches per
// pump tick (GetController + GetControlRotation). Bound to g_netLocal's lifetime:
// nulled when g_netLocal is nulled (level change).
void* g_netLocalController = nullptr;

// Connected-state edge detection for the disconnect cleanup (destroy
// the puppet). File-scope (NOT a static-local in Tick) so a session
// restart can reset them explicitly -- otherwise the local-static would
// hold the prior session's value across the new Start.
//
// AGGREGATE flag (g_wasConnected) tracks "any peer connected" -- gates
// global OnDisconnect calls for subsystems with session-wide state
// (weather_sync, prop_lifecycle).
//
// PER-SLOT flags (g_wasConnectedBySlot) track per-peer connection
// edges -- used to destroy the corresponding puppet on per-peer
// disconnect WITHOUT wiping all subsystem state (matters when peer-1
// drops while peer-2 stays connected).
bool g_wasConnected = false;
std::array<bool, coop::players::kMaxPeers> g_wasConnectedBySlot{};

// Death policy one-shot (2026-06-01, client-death OOM fix). On LOCAL death we
// SYNCHRONOUSLY tear down ALL coop game-side state (destroy puppet actors + drain
// Element state) + Stop the session, then FLEE to the main menu with our layer held
// dormant. The full investigation (4 hands-on + an autonomous probe arc) established:
//   - The balloon is VOTV's OWN possessed-ragdoll leak in the GAMEPLAY world (~165 MB/s
//     to OOM). VOTV's native death never leaves the world -- it just leaves you
//     ragdolling. So the cure is to LEAVE the gameplay world.
//   - `disconnect` is a no-op (no UE netdriver); raw `open menu` does not travel
//     (short name unresolved); VOTV travels via its OWN verb mainGamemode_C::transition
//     ("/Game/menu", full path) -- engine::ReturnToMainMenu. This works for a DEAD/
//     ragdolling player (a direct call, no pause needed).
//   - Our ProcessEvent detour HANGS the 50k-actor untitled_1 teardown (it dispatches
//     ReceiveEndPlay per dying actor through us) AND, after the menu loads, our per-tick
//     coop logic resuming on stale gameplay prop shadows balloons RAM ~1 min later. The
//     fix for BOTH is to HOLD the detour in transparent bypass (game_thread::
//     SetTransparentBypass) over the travel + the time at the menu -> our layer is fully
//     dormant. Validated: RSS flat + trending DOWN for 160 s (the old world frees).
// Latched once per death; reset by OnSessionStart so a rejoin (relaunch) re-arms.
bool g_localDeathHandled = false;

// How long to hold the transparent bypass after a local-death flee to the menu. This
// covers ONLY the world-teardown window -- the bypass exists so VOTV's 50k-actor
// untitled_1 teardown + menu travel runs with our ProcessEvent detour dormant (our
// observers + the outer SEH otherwise hang the per-actor EndPlay swap -- game_thread.cpp
// :34-37, which documents the bypass as a TEARDOWN-window tool that "auto-expires so the
// fresh menu world runs with our layer fully normal again"). Once the menu world is up
// the detour MUST resume, because:
//   - the menu's MULTIPLAYER button is injected by a POST observer on ui_menu_C::Tick
//     (multiplayer_menu.cpp) dispatched THROUGH the detour; a held bypass starves it, so
//     the death-menu loses MULTIPLAYER and the player can't rejoin -- which the
//     permadeath-REJOINABLE death policy explicitly depends on (vitals-death RE 4.5).
//   - resuming at the menu is SAFE: TickGameplay is world-up-gated (Local()==null at the
//     menu) and the prop reaper is gameplay-world-gated (inert at the menu) -- the v56/v57
//     churn fix (2026-06-10) is the REAL menu-safety, not this bypass. The old 30-min hold
//     PREDATED those gates and only kept our layer dormant to dodge the reaper churn; with
//     the gate in place that hold is RULE-2 baggage (this file's own reaper comment already
//     notes "the gameplay-world gate on the prop reaper keeps it from churning" if the
//     bypass expires at the menu). So we resume right after teardown instead.
// The bypass now RESUMES on a CONDITION, not this timer: FleeToMainMenu arms it with
// the menu's ui_menu_C::Tick as the release function, so the detour resumes the instant
// the menu world is up (teardown past) and re-injects MULTIPLAYER on that very frame --
// no fixed-timer window where the death-menu shows without MULTIPLAYER (the prior 15 s
// gap; a user who quit inside it never saw the button). This is only a SAFETY CEILING for
// the case where ui_menu_C::Tick never resolves/dispatches; kept generous so a slow
// teardown is never cut short. (If a death->rejoin black-screens/hangs, the menu tick
// signal failed -- investigate that, do NOT restore the 30-min hold.)
constexpr int kDeathMenuBypassMs = 30 * 1000;

// Terminal local eject to the main menu. The caller has ALREADY torn down coop
// game-side state (the death handler inline; the client disconnect edge via the
// OnDisconnect calls) -- this does the common tail: reset the edge detectors +
// Stop the session, then arm + HOLD the transparent bypass, then travel to the
// menu via VOTV's own transition verb. Used by BOTH the local-death flee AND the
// host-kicked/banned/host-gone client flee -- both leave the gameplay world the
// same way. Order matters: bypass is armed BEFORE the travel so the 50k-actor
// untitled_1 teardown the travel triggers runs with our ProcessEvent detour
// dormant (the death-policy rationale above). `why` is a short log tag.
// Idempotent latch: a session can be detected dead by more than one path at once
// (a net disconnect/death edge here AND the harness running->stopped edge), but we
// must dispatch transition("/Game/menu") only ONCE. Reset by OnSessionStart so the
// next session re-arms. (2026-06-08: generalized so EVERY session death -- host OR
// client -- returns the user to the main menu so they always know it ended.)
bool g_fleeing = false;

// True once the local peer has been in the gameplay world during THIS session. Lets the
// per-4s world check tell "left gameplay to the menu" (flee) apart from the transient
// not-yet-in-gameplay window at the start of a join (no flee). Reset by OnSessionStart.
bool g_everInGameplayThisSession = false;

void FleeToMainMenu(coop::net::Session& session, const char* why) {
    if (g_fleeing) return;  // already travelling to the menu for this session
    g_fleeing = true;
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    session.Stop();
    // Hold the detour dormant over the world teardown, but RESUME the instant the
    // menu's ui_menu_C::Tick first dispatches (menu world up) so MULTIPLAYER is
    // injected on the first menu frame. kDeathMenuBypassMs is only the safety
    // ceiling (used as-is if MenuTickFn() is null -- menu class never resolved).
    ue_wrap::game_thread::SetTransparentBypassUntil(coop::multiplayer_menu::MenuTickFn(),
                                                    kDeathMenuBypassMs);
    if (ue_wrap::engine::ReturnToMainMenu()) {
        UE_LOGI("net: %s -- transition(\"/Game/menu\") dispatched; held dormant", why);
    } else {
        // Travel did not dispatch (gamemode/transition unresolved). The bypass
        // stays armed (still prevents reaper churn) and our actors are already
        // torn down, but we did NOT leave the gameplay world. Surface it loudly.
        UE_LOGE("net: %s -- ReturnToMainMenu FAILED to dispatch; still in the gameplay "
                "world. Relaunch.", why);
    }
}

}  // namespace

void OnSessionStart() {
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    g_localDeathHandled = false;
    g_fleeing = false;  // re-arm the one-shot flee for this new session
    g_everInGameplayThisSession = false;
    coop::local_streams::OnSessionStart();  // held-prop + ragdoll edge detectors
}

void FleeToMainMenuOnDeath(coop::net::Session& session, const char* why) {
    // Public entry so the harness can route a HOST session death (or any session end)
    // to the main menu via the SAME validated path the client-death / disconnect flees
    // use (reset edge detectors + Stop + transparent-bypass + transition("/Game/menu")).
    // Idempotent via the g_fleeing latch -- harmless if net_pump already fled the client.
    // Game thread.
    FleeToMainMenu(session, why);
}

coop::RemotePlayer& Puppet(int slot) {
    // g_puppets is a GT-only-by-convention side-table (no mutex). Enforce it.
    UE_ASSERT_GAME_THREAD("g_puppets (net_pump::Puppet)");
    return g_puppets[slot];
}

// v56: has THIS process (as a client) announced world-ready for the current
// connection? Gates the world-dependent client tick blocks (puppet spawn from
// poses) during the menu-window of a save-transfer join -- the engine must not
// spawn actors into the MENU world. Reset when the connection drops.
static std::atomic<bool> g_worldReadyAnnounced{false};

// Re-announce request: set true (client-side) when a world-change re-seed
// completes, so the world-ready announce below FIRES AGAIN for the newly
// settled world. A save-transfer join goes through TWO level loads (the
// premature first announce binds props to actors the second load destroys);
// the re-announce drives the host's ConnectReplayForSlot to re-assert every
// host-authoritative state into the final world -- the only way the client's
// keyless chipPiles (eid-only identity, no key to re-match on) re-acquire
// their host eid after the re-seed mints fresh local ones. Distinct from
// g_worldReadyAnnounced (which stays latched-true for the puppet-spawn gate
// across world-changes). Reset on send + on disconnect.
static std::atomic<bool> g_reAnnounceWorldReady{false};

// The UWorld we last announced ClientWorldReady against (GT-only -- net_pump Tick is GT). A
// "world-change re-seed" only RE-announces when the CURRENT world differs from this, i.e. the
// UWorld actually SWAPPED. The join's one-time menu->game prop-element-shadow drain is a bookkeeping
// reap WITHIN the already-announced game world (no swap); misreading it as a world change fired a
// spurious SECOND ClientWorldReady -> the host re-replayed the full snapshot -> the client reset +
// re-ran NPC adoption against its already-bound live mirrors -> DUPLICATE kerfurs (2026-06-16, the
// "kerfurs still duping at join" + email-restorm + BeginDeferred-null churn). Gating on a real swap
// is airtight: re-replaying host state into a "new" world is only meaningful when that world's
// actors were actually destroyed+recreated -- which only a UWorld swap does (a real cave/level
// travel re-opens untitled_1 = a NEW UWorld, so legitimate travels still re-announce + re-bind
// keyless chipPiles). Reset on disconnect.
static void* g_announcedWorld = nullptr;

// v75: the save-transfer kerfur-ghost reconcile moved into coop/npc_adoption (the deferred
// class-match adoption owns the timing now). net_pump only NOTIFIES npc_adoption at the
// ClientWorldReady announce (OnClientWorldReady) so it can reset per-world state; the ghost sweep
// itself fires from npc_adoption::Tick, gated on SnapshotComplete + adoption convergence (NOT a
// fixed delay -- v74's 2 s delay was the crutch that, combined with the impossible key-match, let
// the double persist).

void Tick(coop::net::Session& session, float displayOffsetX) {
    // This whole body is game-thread-only: it drives g_puppets (a GT-only-by-
    // convention side-table) and runs ElementDeleter::Flush (the controlled
    // game-thread destruction point). One guard at the top enforces the
    // invariant for everything below it.
    UE_ASSERT_GAME_THREAD("net_pump::Tick (g_puppets + ElementDeleter::Flush)");

    // Perf probe (MEASURE-first 15-FPS audit; ini perf_probe=1). Init self-latches;
    // Sample self-throttles to ~1 Hz. The whole-Tick Scope brackets the body so the
    // 1 Hz report shows net_pump::Tick's own ms/frame against the per-subsystem buckets.
    namespace PP = coop::dev::perf_probe;
    PP::Init();
    PP::Sample();
    PP::Scope _tickScope{PP::Bucket::NetPumpTick};

    // Leak-attribution probe (ini leak_probe=1, dev-only). Self-gated + self-
    // throttled (~4 s GUObjectArray census). Names the UObject class(es) growing
    // over time -> the RAM-balloon's source (or proves the leak is raw heap, not
    // UObjects, when the total count stays flat). Game thread by the assert above.
    coop::dev::leak_probe::Tick();

    // Raw-heap leak-attribution probe (ini heap_probe=1, dev-only). When the
    // UObject census above is flat but RAM still climbs, this names the OUR-module
    // CRT call site responsible (engine GMalloc bypasses the CRT). Self-gated +
    // self-throttled; installs ucrtbase malloc/free detours on its first armed tick.
    coop::dev::heap_probe::Tick();

    // v56: pump pending save-transfer chunk sends (host). No-op without an
    // active stream (one bool per slot).
    if (session.role() == coop::net::Role::Host) coop::save_transfer::TickHost();

    // World-up gate (v56 menu-window balloon fix, 2026-06-10). A menu-mode
    // save-transfer joiner runs this Tick at 60 Hz while still at the MAIN
    // MENU (connecting + downloading the host save) -- a window where the
    // gameplay-only sections below have NOTHING to act on but real per-tick
    // cost (subsystem polls, ensure-retries, GUObjectArray lookups against
    // classes that cannot exist before the gameplay world loads). Everything
    // the menu window NEEDS stays ungated: the host chunk pump above, the
    // element-deleter flush, the reaper (self-gated on the world name; owns
    // the quit-to-menu flee), the per-slot connect/disconnect edges, the
    // world-ready announce, and event_feed (which delivers SaveTransferBegin).
    // Local() is the signal: non-null exactly when a possessed local player
    // exists in a gameplay world -- and its negative-miss TTL (players_
    // registry) makes this poll cheap at the menu.
    void* const localNow = coop::players::Registry::Get().Local();
    const bool worldUp = (localNow != nullptr);

    // Pose-apply diagnostic (lag hunt 2026-06-06): the wire is proven clean (net-diag), so
    // measure the APPLY side. Per remote puppet, accumulate the FRESH-pose count (isNew/sec)
    // and the latest stream target below, then log once/sec the target vs the puppet's rendered
    // position + the trailing distance. Healthy = ~sendHz fresh/s + a small trail; a big/growing
    // trail means the interp/engine apply lags the on-time stream; low fresh/s = upstream
    // staleness. Game-thread-only Tick -> static locals need no atomics.
    static std::array<int, coop::players::kMaxPeers> sPoseFresh{};
    static std::array<coop::net::PoseSnapshot, coop::players::kMaxPeers> sPoseTarget{};
    static std::chrono::steady_clock::time_point sNextPoseDiag{};

    // Deferred-element destruction flush (MTA CElementDeleter shape; see
    // coop/element/element_deleter.h). Drains, on the game thread at one
    // controlled point, any Elements parked for destruction by owner-map
    // drains since the last tick. Steady-state cost is a single uncontended
    // mutex acquire + empty-queue check (no producer routes destruction here
    // yet -- that lands with the Npc/Prop ownership migration).
    coop::element::ElementDeleter::Get().Flush();

    // Dead-Prop-Element reconciliation (PR-FOUNDATION 2026-05-30). A mass GC
    // purge (cave/level transition, save-load) flags ~2000 props PendingKill AT
    // ONCE without firing per-actor K2_DestroyActor, so the sole eviction path
    // (the PRE observer) is bypassed and the dead Prop Element shadows leak --
    // unbounded across transitions, exhausting the 16384 tracker caps after ~7
    // and silently breaking prop tracking. Throttle to ~once per 4 s + cap 256
    // evicts/call so steady-state cost is one bounded Registry walk every few
    // seconds (no-op when nothing is dead) and a post-purge backlog drains over
    // a handful of scans. Runs regardless of connection state (props seed at
    // boot; a transition can happen before any peer connects) and on BOTH roles
    // (each peer maintains its OWN local props). Game-thread (this Tick) -- the
    // reaper Enqueues to the deleter, flushed at the top of the NEXT tick.
    {
        PP::Scope _s{PP::Bucket::Reaper};
        using ReapClock = std::chrono::steady_clock;
        static ReapClock::time_point sNextReap{};
        // The reaper evicts up to this many dead Prop Elements per 4s scan (bounds
        // the per-Flush ~Prop count). SEPARATE from the re-seed episode threshold
        // below.
        constexpr size_t kReapEvictCap = 256;
        // World-change re-seed trigger (snapshot-completeness fix 2026-05-30).
        // The one-shot boot seed runs on the pre-travel world; after VOTV's
        // boot-time `open untitled_1` (and any future cave/level travel) the new
        // level's PLACED props are live-but-untracked (placed props don't fire a
        // catchable Init POST -- the very reason the seed exists). The host then
        // tracks ~70 of ~3300 props -> the late-joiner snapshot ships ~2%.
        //
        // DETECTOR: the reaper ONLY ever finds props flagged PendingKill WITHOUT a
        // K2_DestroyActor (normal destruction goes through that observer -> reaped
        // is 0 in steady gameplay). So a single scan reaping >= kReseedPurge props
        // is a mass purge = a level/world transition. The threshold is well below
        // the eviction cap so it also catches SMALL transitions (e.g. exiting a
        // cave back to the main world purges only the cave's props -- a 256-cap-hit
        // gate would miss it and leave the main world untracked; audit 2026-05-30),
        // while staying above any incidental GC.
        constexpr size_t kReseedPurge = 64;
        // We defer the re-seed to the END of the purge EPISODE -- the scan where the
        // reaper drain CATCHES UP (reaps < kReseedPurge => backlog fully drained).
        // Running it against a fully-drained registry avoids any recycled-address
        // idempotency edge (a new prop reusing a not-yet-drained dead prop's actor
        // address would be skipped by MarkPropElement). The episode flag (not a
        // cooldown) gates re-arming: one purge drains over many 4s scans, all ONE
        // episode; a genuinely NEW transition (later cave entry) starts a fresh
        // episode and re-seeds again. Same GT cost class as the boot seed (both
        // ~thousands of MarkPropElement on this Tick); the reaper leaves the
        // re-seeded props (fresh valid internalIdx -> IsLiveByIndex true). Smoke
        // 2026-05-30: snapshot 70 -> 2314 (the 2314-vs-3328-found gap is empty/None-
        // key props, correctly excluded as non-syncable).
        //
        // Gate hardening 2026-06-10: the flag moved to prop_element_tracker
        // (SetInPurgeEpisode/InPurgeEpisode) so the snapshot coherence gate
        // can read it -- the world-stamp alone proved insufficient (the
        // boot/save-load flow leaves the stamped UWorld alive while the
        // registry is majority-dead; smoke-falsified). net_pump still owns
        // every detection edge below (RULE 2: one flag, one owner of writes).
        const auto reapNow = ReapClock::now();
        if (reapNow >= sNextReap) {
            sNextReap = reapNow + std::chrono::seconds(4);
            // Gameplay-world gate (2026-06-01, post-flee menu-leak fix). The reaper +
            // world-change re-seed are GAMEPLAY-only. After a gameplay->menu travel (the
            // local-death flee to the main menu) the tracker still holds thousands of
            // now-dead prop-element shadows; running the reaper/re-seed at the MENU reaps
            // them and then re-seeds on the menu's OWN actors in a vicious allocating loop
            // that balloons RAM to OOM ~1 min after the flee (user-observed: menu was fine
            // for a minute, then ballooned -- exactly when the temporary detour bypass
            // expired and this resumed). At a non-gameplay world we skip BOTH: the shadows
            // sit inert and the reaper resumes + cleans them on the next gameplay entry.
            // Gameplay world is untitled_1; the menu is /Game/menu.menu.
            void* reapWorld = R::FindObjectByClass(P::name::WorldClass);
            // Allocation-free name checks (audit 2026-06-10): the old ToString
            // wstring here was the one survivor of the balloon-fix pattern in
            // this file -- 4s-throttled so never a balloon, but same pattern,
            // same diff. Substring semantics preserved ("ntitled" matches the
            // gameplay world prefix-case-agnostically).
            const bool inGameplayWorld =
                reapWorld && R::NameContains(R::NameOf(reapWorld), L"ntitled");
            // RAM-balloon guard (2026-06-08, user: HOST pressed MAIN MENU mid-session ->
            // ballooning). VOTV's OWN quit-to-menu travels to /Game/menu WITHOUT going
            // through our FleeToMainMenu, so the session stays running + our whole layer
            // keeps churning at the menu = the balloon. Once we've been in gameplay this
            // session, a transition to the MENU world (matched specifically, so a cave /
            // streamed sublevel never trips it) means the local peer LEFT -> flee now (Stop
            // the session + arm the dormancy bypass). The harness running->stopped edge
            // (host) would also catch the resulting stop; g_fleeing dedupes. Piggybacks on
            // this existing 4 s world scan (no new per-tick cost) -- 4 s << the ~1 min balloon.
            if (inGameplayWorld) {
                g_everInGameplayThisSession = true;
            } else if (g_everInGameplayThisSession && !g_fleeing && session.running() &&
                       reapWorld && R::NameContains(R::NameOf(reapWorld), L"menu")) {
                UE_LOGW("net: gameplay->MENU while a session is live (VOTV quit-to-menu?) -- "
                        "fleeing to end the session + stop the layer churn (RAM-balloon guard)");
                coop::prop_element_tracker::SetInPurgeEpisode(false);  // left gameplay (matches the !inGameplayWorld reset below)
                FleeToMainMenu(session, "left gameplay to the menu");
                return;
            }
            if (!inGameplayWorld) coop::prop_element_tracker::SetInPurgeEpisode(false);  // inert at menu; re-arm on gameplay re-entry
            const size_t reaped = inGameplayWorld
                ? coop::prop_element_tracker::ReapDeadLocalPropElements(kReapEvictCap)
                : 0;
            // Catch up ALREADY-connected peers to the now-complete prop set:
            // their connect-edge snapshot (if it ran at all -- the fork-A
            // coherence gate defers it mid-transition) enumerated the PRE-
            // re-seed registry. Re-trigger the per-slot snapshot;
            // prop_snapshot queues it if a drain is in flight and
            // re-enumerates the (now complete) registry at dequeue, and the
            // client's RegisterPropMirror dedupes props it already holds.
            // No-op when no peer is connected yet. Host-only. (DEFERRED
            // joiners are independent of this added>0 path: the re-seed's
            // generation bump wakes them via prop_snapshot's DrainChunk
            // flush unconditionally.)
            auto retriggerReadySlots = [&session]() {
                if (session.role() != coop::net::Role::Host) return;
                for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
                    if (session.IsSlotReady(slot)) {
                        coop::prop_snapshot::TriggerForSlot(slot);
                        UE_LOGI("net_pump: re-snapshot ready slot %d after re-seed", slot);
                    }
                }
            };
            // CLIENT re-announce gate (2026-06-16). Re-announce world-ready ONLY when the UWorld
            // actually SWAPPED since our last announce -- NOT for the join's menu-shadow drain within
            // the same world (which spuriously double-snapshotted + duped kerfurs). See g_announcedWorld.
            auto maybeReAnnounce = [&session, reapWorld]() {
                if (session.role() == coop::net::Role::Host) return;
                if (reapWorld != g_announcedWorld) {
                    g_reAnnounceWorldReady.store(true, std::memory_order_relaxed);
                } else {
                    UE_LOGI("net_pump: world-change re-seed on the SAME world already announced (%p) -- "
                            "NOT re-announcing (suppresses the join menu-shadow-drain double-snapshot + "
                            "kerfur re-adopt dupe)", reapWorld);
                }
            };
            if (reaped >= kReseedPurge) {
                if (!coop::prop_element_tracker::InPurgeEpisode()) {
                    coop::prop_element_tracker::SetInPurgeEpisode(true);
                    UE_LOGI("net_pump: mass-purge detected (reaped %zu >= %zu) -- world-change re-seed deferred to drain-complete",
                            reaped, kReseedPurge);
                }
            } else if (coop::prop_element_tracker::InPurgeEpisode()) {
                // Drain caught up: the old level's dead Prop Elements are fully
                // evicted and the new level has loaded. Re-seed now (clean -- no
                // recycled-address collisions) + catch up connected peers.
                coop::prop_element_tracker::SetInPurgeEpisode(false);
                const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
                UE_LOGI("net_pump: world-change re-seed added %zu live keyed prop(s) (snapshot-completeness)", added);
                if (added > 0) retriggerReadySlots();
                // CLIENT: re-announce world-ready so the host re-replays its authoritative state into
                // the new world (re-binds our keyless chipPiles to host eids) -- but ONLY if the world
                // actually swapped (maybeReAnnounce). The join's same-world shadow drain must not.
                maybeReAnnounce();
            } else if (inGameplayWorld &&
                       coop::prop_element_tracker::HasSeededOnce() &&
                       !coop::prop_element_tracker::IsRegistrySeededForCurrentWorld()) {
                // Fork A small-travel companion: a travel purging fewer than
                // kReseedPurge keyed elements never starts a purge episode, so
                // the episode-end re-seed -- the only post-travel stamp
                // refresher -- would never run and the snapshot coherence gate
                // would stay closed forever. The reap above (cap 256 > 64)
                // already evicted the sub-64 dead THIS scan, so this re-seed
                // runs against a drained registry (no recycled-address
                // window, the reason the EPISODE path defers to drain-
                // complete). HasSeededOnce keeps the boot window owned by
                // SeedKnownKeyedProps (observers-before-seed doctrine).
                // Ordered as `else if` behind the episode branches: the first
                // scan of a MASS travel takes the episode path and this never
                // preempts the throttled drain.
                const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
                UE_LOGI("net_pump: world changed without a mass purge -- re-seeded (%zu new keyed)", added);
                if (added > 0) retriggerReadySlots();
                maybeReAnnounce();  // only if the UWorld actually swapped (see g_announcedWorld)
            } else if (inGameplayWorld &&
                       coop::prop_element_tracker::HasSeededOnce() &&
                       coop::prop_element_tracker::IsRegistrySeededForCurrentWorld()) {
                // STEADY gameplay world (no level transition): the ONLY ongoing
                // detector for props that come into existence mid-session via
                // EX_CallMath -- the Q spawn-menu, the toolgun, the ambient
                // spawners, and grab-morph chipPiles. Those bypass UObject::
                // ProcessEvent (our sole hook seam), so the Init-POST observer
                // never sees them: they sit UNTRACKED (overlay "key=.. / no eid")
                // and are never mirrored to peers (user 2026-06-16: spawn-menu
                // props + piles invisible on the client). This 0.25 Hz
                // GUObjectArray re-seed (same cadence + cost class as the reaper
                // above; the FindObjectByClass world resolve is shared) mints
                // each new keyed prop's eid AND each keyless chipPile's eid (the
                // SeedWalk_ IsChipPile branch). added==0 (steady, nothing newly
                // spawned) costs only the walk -- zero peer traffic.
                //
                // R1 (2026-06-17, MTA CEntityAddPacket): broadcast ONE bracket-free
                // additive PropSpawn per NEWLY-adopted prop (prop_snapshot::
                // ExpressIncrementalSpawn), NOT a full re-bracketed snapshot. The old
                // retriggerReadySlots() re-fired a SnapshotBegin/Complete bracket every
                // ~4s during a join, and each bracket RE-ARMED the client's destructive
                // divergence sweep (~10x in 90s = the join-churn that thrashed piles +
                // doomed kerfur mirrors). An incremental add never opens a bracket -> the
                // sweep is not re-armed. MTA's strictly-incremental streaming: one
                // CEntityAddPacket per new entity, never a world re-send
                // (Server/.../CStaticFunctionDefinitions.cpp:8349). ExpressIncrementalSpawn
                // is host-only internally; the re-seed still mints local eids on a client
                // (its own tracking) but broadcasts nothing. (retriggerReadySlots stays --
                // the episode-end + small-travel branches above legitimately re-bracket on
                // a real world transition, where the client SHOULD re-reconcile.)
                std::vector<void*> newProps;
                const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps(&newProps);
                if (added > 0) {
                    UE_LOGI("net_pump: steady-world re-seed adopted %zu NEW runtime-spawned keyed prop(s) "
                            "(spawn-menu/toolgun/ambient/pile) -- broadcasting one PropSpawn each "
                            "(incremental delta, no re-bracket; MTA CEntityAddPacket shape)", added);
                    for (void* a : newProps) coop::prop_snapshot::ExpressIncrementalSpawn(a);
                }
            }
        }
    }

    // Detect peer disconnect (Connected -> Handshaking/Disconnected). DESTROY
    // the puppet -- a frozen-in-place puppet of a peer who already quit is
    // confusing and clutters the world. event_feed will also have posted
    // "X left the game". If the peer ever reconnects, Tick auto-spawns a
    // fresh puppet on the first new pose.
    const bool isConnected = (session.state() == coop::net::ConnState::Connected);
    const bool isHost = (session.role() == coop::net::Role::Host);
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // IsSlotReady (lanes configured) not IsSlotConnected (just has a conn
        // handle): connect-edge replay must wait for ConfigureLanes to land in
        // the Connected callback. Otherwise snapshot fan-out + connect-time
        // broadcasts ship on lane 0 (default) instead of their assigned lane
        // mapping. The disconnect callback clears both flags atomically so
        // the disconnect-edge also fires correctly off IsSlotReady.
        const bool slotConnected = session.IsSlotReady(slot);
        if (g_wasConnectedBySlot[slot] && !slotConnected) {
            // N-3 (2026-05-29 audit): UnregisterPuppet drops the Player Element
            // from players::Registry. If the puppet was never spawned (peer
            // disconnected after Join but before any PoseSnapshot), g_puppets[
            // slot].valid() is false but playerBySlot_[slot] may still hold a
            // mirror Element installed by EstablishMirrorForSlot. Calling
            // UnregisterPuppet unconditionally is safe -- DropPlayerElement_
            // is a no-op when playerBySlot_ is null.
            coop::players::Registry::Get().UnregisterPuppet(static_cast<uint8_t>(slot));
            if (g_puppets[slot].valid()) {
                g_puppets[slot].Destroy();
                UE_LOGI("net: peer slot %d disconnected -- puppet destroyed", slot);
            }
            coop::subsystems::DisconnectSlot(session, slot);
        }
        if (!g_wasConnectedBySlot[slot] && slotConnected) {
            // Per-slot connect edge.
            // - HOST side (slot 1..3): v56 -- the replay no longer fires here. A
            //   menu-mode joiner is connected ~30-60 s BEFORE it has a world
            //   (save download + load); the replay now fires on its
            //   ClientWorldReady (event_feed -> subsystems::ConnectReplayForSlot).
            //   An already-in-world joiner (env flow / current browser flow)
            //   announces world-ready within a tick of connecting, so the replay
            //   timing is unchanged for them.
            // - CLIENT -> HOST (slot 0): announce LOCAL flashlight state +
            //   save-transfer request + open our world-ready send gate
            //   (subsystems::ClientConnectEdge).
            if (isHost && slot >= 1) {
                UE_LOGI("net: peer slot %d connect edge -- awaiting ClientWorldReady before the replay", slot);
            } else if (!isHost && slot == 0) {
                UE_LOGI("net: host (slot 0) connect edge -- replaying local flashlight");
                coop::subsystems::ClientConnectEdge(session);
            }
        }
        g_wasConnectedBySlot[slot] = slotConnected;
    }

    // v56: a CLIENT announces world-ready ONCE per connection, when a gameplay
    // world is up AND the prop registry expresses IT (the same coherence signals
    // the host's bracket gate trusts -- design-workflow B3: a menu/stale-world
    // announce would arm the host bracket against an unseeded client and re-open
    // the fuzzy-fallback balloon). The menu world can never pass: the seed stamp
    // only ever points at a live gameplay ('ntitled') world.
    // Fire on the FIRST announce of a connection OR on a re-announce request
    // (a world-change re-seed completed -> the host must re-replay into the new
    // world). The coherence gate is identical either way: a menu/stale-world
    // announce would arm the host bracket against an unseeded client.
    const bool reAnnounce = g_reAnnounceWorldReady.load(std::memory_order_relaxed);
    if (!isHost && isConnected &&
        (!g_worldReadyAnnounced.load(std::memory_order_relaxed) || reAnnounce)) {
        if (worldUp &&
            coop::prop_element_tracker::HasSeededOnce() &&
            coop::prop_element_tracker::IsRegistrySeededForCurrentWorld() &&
            !coop::prop_element_tracker::InPurgeEpisode()) {
            if (session.SendReliableToSlot(0, coop::net::ReliableKind::ClientWorldReady,
                                           nullptr, 0)) {
                g_worldReadyAnnounced.store(true, std::memory_order_relaxed);
                g_reAnnounceWorldReady.store(false, std::memory_order_relaxed);
                // Stamp the world we just announced against; a later re-seed only re-announces if the
                // current world differs (maybeReAnnounce). Resolved here (rare -- once per real
                // announce), never per-frame. R/P are the file-scope reflection/profile aliases.
                g_announcedWorld = R::FindObjectByClass(P::name::WorldClass);
                // v75: a fresh connect replay (this world's EntitySpawns + SnapshotComplete) is
                // about to arrive -- reset the deferred-adoption per-world state so the new world
                // re-adopts its save-NPCs + re-sweeps orphans (the ghost sweep itself fires from
                // npc_adoption::Tick, gated on SnapshotComplete + adoption convergence).
                coop::npc_adoption::OnClientWorldReady();
                coop::kerfur_prop_adoption::OnClientWorldReady();  // K-6: drop stale prop-kerfur pending
                // Same per-world reset for the deferred PROP divergence sweep -- a sweep armed for
                // the prior world must not fire against this fresh one (save-transfer = two loads).
                coop::remote_prop_spawn::OnClientWorldReadyResetSweep();
                UE_LOGI("net_pump: ClientWorldReady announced (world up + registry coherent%s)",
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
        // Aggregate disconnect (all peers gone). Fire the global OnDisconnect
        // calls -- the per-slot edge block above already handled subsystems
        // with per-slot state, this catches subsystems with session-wide state
        // (weather_sync, npc_sync host-side counter, prop_lifecycle dedupe set).
        // v34: if a client lost the host mid-join, drop the loading state so the cover/console
        // don't hang (no-op on the host / when no join is in progress).
        coop::join_progress::Reset();
        const auto stats = coop::subsystems::DisconnectAll();
        UE_LOGI("net: all peers gone -- cleared %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries; takeObjInFlight=0",
                stats.snapPending, stats.initProcessedDropped);

        // CLIENT eject: a client losing the host (KICK / BAN / host quit / host
        // crash) ends the session -- flee to the main menu so the player isn't
        // stranded in a now-hostless world (the SAME path as a local-death flee).
        // HOST role does NOT eject here: a client leaving -- or the host kicking
        // its last client -- also lands in this aggregate-disconnect block, and
        // must NOT boot the host to the menu. One-shot via the terminal-eject
        // latch (g_localDeathHandled, reused: death OR host-close; rejoin re-arms
        // it in OnSessionStart). The subsystem teardown above already ran, so
        // FleeToMainMenu just does Stop + bypass + travel.
        if (!isHost && !g_localDeathHandled) {
            g_localDeathHandled = true;
            const std::string reason = session.TakeHostCloseReason();
            UE_LOGW("net: HOST CLOSED OUR CONNECTION (reason: %s) -- fleeing to the main menu",
                    reason.empty() ? "connection lost" : reason.c_str());
            FleeToMainMenu(session, "host closed connection");
            return;
        }
    }
    // CLIENT connect-FAILURE edge (regression C/D, 2026-06-06). A browser-join client whose
    // connect attempt terminated WITHOUT ever reaching Connected -- a dead direct IP, an
    // unreachable host, the host not up yet. The aggregate-disconnect edge above only fires
    // once g_wasConnected latched true (a real connection that later dropped), and its client
    // branch flees on host-close; NEITHER catches a never-connected client. Without this the
    // loading screen hangs on "Connecting..." until the 90 s failsafe AND net_pump keeps
    // pumping the full gameplay tick at the menu every frame (the lag + RAM balloon the user
    // hit on a dead-master native connect). Detect it precisely: client role, a browser join
    // is Active, we never connected this session (pre-update g_wasConnected), and Start()
    // already drove state past Handshaking back to Disconnected (session_status's connect-fail
    // path) -- which for a running client means the attempt is definitively dead. Fail() is a
    // no-op unless Active + idempotent, so re-firing until the harness drains the abort (Stop +
    // reopen browser, which ENDS the menu pump) is harmless.
    if (!isHost && !g_wasConnected && coop::join_progress::Active() &&
        session.state() == coop::net::ConnState::Disconnected) {
        const std::string why = session.TakeHostCloseReason();
        coop::join_progress::Fail(why.empty() ? "could not connect to the host" : why);
    }
    g_wasConnected = isConnected;

    // Process up to ~100 snapshot candidates per tick if a snapshot enumeration
    // is in progress (no-op on empty vector).
    if (isConnected && worldUp) { PP::Scope _s{PP::Bucket::SnapshotDrain}; coop::prop_snapshot::DrainChunk(); }

    // Per-tick gameplay subsystem chain (connect-broadcast drains, module
    // polls/applies, NPC streams, trash death-watches, dev probes).
    //
    // The whole chain is world-up-gated (menu-window balloon fix): every one of
    // these acts on gameplay-world state that cannot exist at the menu, and
    // several poll/ensure against BP classes that only load with the gameplay
    // world. Queued connect broadcasts (e.g. the client's own flashlight from
    // the slot-0 connect edge) simply WAIT there until the world is up -- they
    // describe in-world state, so sending them earlier was never meaningful.
    if (worldUp) {
        coop::subsystems::TickGameplay(session, isConnected, isHost, g_fleeing);
    }

    // (The local-death flee to the main menu now happens SYNCHRONOUSLY on the death
    // frame in the teardown block below -- arm the held bypass + engine::ReturnToMainMenu
    // -- so there is no per-tick backstop here. The old `open menu` / `disconnect`
    // re-issue backstop was removed: both were no-ops, and our detour is held in bypass
    // for the travel so nothing of ours runs to "back it up" anyway.)

    if (g_netLocal && !R::IsLive(g_netLocal)) { g_netLocal = nullptr; g_netLocalController = nullptr; }
    if (!g_netLocal) g_netLocal = localNow;  // resolved once at the top of this tick
    // The `!g_localDeathHandled` gate: the FIRST tick after death this block still runs
    // (it is where death is detected + the synchronous teardown fires), but once handled
    // we STOP all local-send work. Hands-on showed the ragdoll sender kept emitting 1140+
    // RagdollPose packets on the stopped session post-death -- reading the dead player's
    // pelvis ~100x/s while we are en route to the menu. Now the only per-tick work in the
    // dead window is the forced-menu backstop above.
    if (g_netLocal && !g_localDeathHandled) {
        // DEATH POLICY (2026-06-01 client-death OOM fix, hardened after hands-on). On
        // local death, SYNCHRONOUSLY tear down ALL coop game-side state on THIS frame,
        // then Stop the session. Hands-on proved Session::Stop() alone is insufficient:
        // the client log froze exactly at this line, i.e. VOTV's death world-reload
        // blocked the game thread immediately, so the deferred disconnect-edge cleanup
        // (next Tick) NEVER RAN -- our orphan puppet actors + Element mirrors stayed in
        // the dying world and it still ballooned to OOM. So we destroy our puppet ACTORS
        // + drain every subsystem's state HERE, before the reload, mirroring the per-slot
        // + aggregate disconnect edges. One-shot (g_localDeathHandled). Role-correct via
        // Session::Stop (client leaves itself; host ends the session for all -- "HOST
        // DEATH ENDS SESSION"). Permadeath-rejoinable: reconnect re-Starts + OnSessionStart
        // re-arms. `dead` is true ONLY on real death (faint/KO/manual-C leave it false).
        // Death-flee gate. A HOST is "still hosting" in THREE states -- Handshaking
        // (no client yet), Connected (clients present), Disconnected (clients all
        // left, listen socket still up; session_status.cpp:325) -- and ALL THREE
        // balloon on death the same way (VOTV's possessed-ragdoll leak is LOCAL,
        // independent of client count). So the host gates on running() [Start..Stop],
        // not connected(): a solo host (or one whose clients had left) gated on
        // connected() died UNDETECTED and ballooned -- the bug this fixes. The CLIENT
        // keeps connected(): if it loses the host it is already ejected by the
        // host-close path above (which latches g_localDeathHandled), so connected() is
        // its precise window. One relaxed atomic load either way -- same hot-path cost
        // as the connected() it replaces.
        const bool sessionLiveForDeath = isHost ? session.running() : session.connected();
        if (!g_localDeathHandled && sessionLiveForDeath) {
            bool isRagdoll = false, dead = false;
            if (ue_wrap::engine::ReadMainPlayerRagdollState(g_netLocal, isRagdoll, dead) && dead) {
                g_localDeathHandled = true;
                UE_LOGW("net: LOCAL PLAYER DIED -- tearing down coop state synchronously + fleeing "
                        "to the main menu (role=%s; permadeath-rejoinable)",
                        session.role() == coop::net::Role::Host ? "HOST (ends session)" : "CLIENT");
                // Per-slot teardown: drop + destroy each puppet ACTOR (Destroy() also tears
                // down its ragdoll body) + per-slot subsystem state.
                for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
                    coop::players::Registry::Get().UnregisterPuppet(static_cast<uint8_t>(slot));
                    if (g_puppets[slot].valid()) g_puppets[slot].Destroy();
                    coop::subsystems::DisconnectSlot(session, slot);
                }
                // Aggregate teardown: session-wide Element/dedup state (mirrors the
                // g_wasConnected disconnect edge).
                coop::subsystems::DisconnectAll();
                // Flee to the MAIN MENU and HOLD our layer dormant (shared with the
                // host-close eject). The balloon is VOTV's own possessed-ragdoll leak in
                // the GAMEPLAY world (only cure: leave it); FleeToMainMenu resets the edge
                // detectors + Stops, then arms the bypass BEFORE travelling so our detour
                // doesn't HANG the 50k-actor untitled_1 teardown and our per-tick logic
                // can't resume on stale gameplay shadows at the menu (RSS flat + trending
                // down, probe: 160 s). Travel uses VOTV's own verb (works for a DEAD/
                // ragdolling player, no pause needed).
                FleeToMainMenu(session, "LOCAL PLAYER DIED");
                return;
            }
        }
        // Re-resolve the controller only when missing or invalidated; the
        // controller pointer stays stable between possess events. Caching here
        // saves ~250 ProcessEvent dispatches/sec at 125 Hz pump.
        if (g_netLocalController && !R::IsLive(g_netLocalController)) g_netLocalController = nullptr;
        if (!g_netLocalController) g_netLocalController = ue_wrap::engine::GetController(g_netLocal);
        // One-shot install of the per-subsystem observers (idempotent).
        { PP::Scope _s{PP::Bucket::InstallObs}; coop::subsystems::Install(session); }
        // Outbound local streams: pose + held-prop + ragdoll (coop/local_streams).
        coop::local_streams::Tick(session, g_netLocal, g_netLocalController);
    }

    // Per-slot pose drive. Iterate ALL peer slots [0, kMaxPeers) and skip
    // our OWN slot -- so each peer drives a puppet for every OTHER peer.
    // (PR-FOUNDATION Tier 2 host-relay: a client now sees not just the host
    // at slot 0 but also peer-clients at slots 2/3 once the host relays
    // their poses (T2-2) + identities (T2-1, PlayerJoined). Before relay
    // exists, TryGetRemotePose returns false for peer-client slots, so this
    // is a harmless no-op at those slots.) Each slot has its own RemotePlayer
    // puppet + spawn-retry backoff.
    //
    // World-up-gated together with the ragdoll drive / puppet Tick / remote-
    // prop apply below: all of it spawns or drives engine actors, which is
    // meaningless (and at the menu, actively wrong) without a gameplay world.
    // Poses keep streaming meanwhile -- TryGetRemotePose holds only the
    // LATEST snapshot per slot, so nothing accumulates while gated.
    if (worldUp) {
    {
        PP::Scope _s{PP::Bucket::Puppets};
        using namespace std::chrono;
        // Per-slot spawn retry timer (static-local: harmless across session
        // restarts because the timer is monotonic and a stale "wait until X"
        // either lets us spawn immediately if X is in the past, or makes us
        // wait the bounded remaining time -- no risk of carrying state that
        // affects correctness).
        static std::array<steady_clock::time_point, coop::players::kMaxPeers> sNextSpawnAttempt{};
        const auto now = steady_clock::now();
        // Host self-assigns slot 0 in the registry. Idempotent setter so it's
        // fine to run every tick. Client LocalPeerId is set asynchronously by
        // the wire-layer AssignPeerSlot handler (event_feed.cpp).
        if (isHost) {
            coop::players::Registry::Get().SetLocalPeerId(coop::players::kPeerIdHost);
        }
        const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
        for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
            // Never drive a puppet for our own slot. On the host localSlot==0;
            // on a client localSlot is its assigned slot (kPeerIdUnknown=0xFF
            // until AssignPeerSlot lands, which matches no slot -> all slots
            // eligible, but TryGetRemotePose gates spawning anyway).
            if (static_cast<uint8_t>(slot) == localSlot) continue;
            coop::net::PoseSnapshot remote;
            bool isNew = false;
            if (!session.TryGetRemotePose(slot, remote, &isNew)) continue;
            sPoseTarget[slot] = remote;  // pose-diag: latest stored pose (the interp target source)
            if (!g_puppets[slot].valid()) {
                // v56: a save-transfer joiner receives poses while still at the
                // MENU (downloading + loading the host save) -- never spawn a
                // puppet into a non-gameplay world; the pose keeps streaming
                // and the spawn happens on the first pose after world-ready.
                if (session.role() == coop::net::Role::Client &&
                    !g_worldReadyAnnounced.load(std::memory_order_relaxed)) continue;
                // Spawn-retry backoff: BeginDeferredActorSpawnFromClass refuses
                // if the world is mid-transition (OMEGA->story under multi-
                // instance CPU contention can take many seconds; refused
                // spawns reach hundreds/sec at 60 Hz pump rate = pure log
                // noise + wasted reflection calls). Only retry once per second
                // after a failure (RULE 1: don't crutch the engine, just wait).
                if (now < sNextSpawnAttempt[slot]) continue;
                UE_LOGI("net: first remote pose on slot %d -> auto-spawning puppet", slot);
                if (!g_puppets[slot].Spawn()) {
                    UE_LOGW("net: slot %d puppet spawn failed; will retry in 1 s", slot);
                    sNextSpawnAttempt[slot] = now + seconds(1);
                    continue;
                }
                // Register with the central Registry. peerId == slot directly.
                coop::players::Registry::Get().RegisterPuppet(
                    static_cast<uint8_t>(slot), &g_puppets[slot]);
                // Apply the cached nickname now that the puppet exists. The
                // identity (Join / PlayerJoined) can arrive BEFORE the first
                // pose spawns the puppet -- in that race the handler cached
                // the nick but found no puppet to label. Reading the cache
                // here closes that gap for the host puppet AND cross-peer
                // puppets (T2-1). Falls back to the placeholder if no
                // identity has landed yet; the handler re-applies on arrival.
                g_puppets[slot].SetNickname(
                    coop::player_handshake::NicknameForSlot(slot));
                // Two-phase join announcement: the puppet just spawned. The CLIENT announces here --
                // "Joined <host>'s game" (slot 0) or cross-peer "<nick> joined the game" -- and this
                // path is already gated on the client's own g_worldReadyAnnounced (the `continue` at the
                // top of this block), so it fires at the client's world-ready. The HOST does NOT announce
                // here anymore: a menu-mode joiner streams a pose from its menu/loading pawn BEFORE its
                // world loads, so spawning the host's puppet on that first pose fired "<nick> joined"
                // while the joiner was still on the loading screen (user 2026-06-17). The host now
                // announces a joiner from event_feed on its ClientWorldReady (OnClientWorldReady).
                if (session.role() == coop::net::Role::Client)
                    coop::player_handshake::AnnouncePeerSpawned(session.role(), slot);
            }
            // Only RE-BASE the interpolation on a NEW packet; re-pushing the
            // latest every frame would zero `errorPos_` mid-window and freeze
            // motion. The per-frame advance happens in Tick() below.
            if (isNew) {
                coop::net::PoseSnapshot withOffset = remote;
                withOffset.x += displayOffsetX;  // loopback mirror shift (0 for real coop)
                g_puppets[slot].SetTargetPose(withOffset);
                ++sPoseFresh[slot];  // pose-diag: a fresh target this second
            }
        }
    }
    // v22: per-slot ragdoll PELVIS-physics drive. Decoupled from the pose stream
    // above (a momentary pose-packet gap must not stall the ragdoll velocity feed).
    // Only FRESH packets apply -- the puppet's mirror body integrates between them.
    // SetRagdollPose applies the velocity to the body + stamps the streamed pelvis
    // rotation for the next Tick's ApplyToEngine. Runs BEFORE the Tick loop so the
    // velocity is set before the same-frame ApplyToEngine reads the body.
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (!g_puppets[slot].valid()) continue;
        coop::net::RagdollPoseSnapshot rdoll;
        bool rdollNew = false;
        if (session.TryGetRemoteRagdollPose(slot, rdoll, &rdollNew) && rdollNew) {
            g_puppets[slot].SetRagdollPose(rdoll);
        }
    }

    // Tick every live puppet (independent of which slot received pose data
    // this frame -- the per-puppet interpolation needs Tick every frame even
    // when no fresh pose arrived).
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (g_puppets[slot].valid()) g_puppets[slot].Tick();
    }

    // Pose-apply diagnostic emit (once/sec). target = the latest pose received for this slot;
    // puppet = its rendered location AFTER this frame's interp Tick; trail = how far the
    // rendered puppet is behind the target it should be converging to.
    {
        const auto pdNow = std::chrono::steady_clock::now();
        if (pdNow >= sNextPoseDiag) {
            sNextPoseDiag = pdNow + std::chrono::seconds(1);
            for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
                if (!g_puppets[slot].valid()) { sPoseFresh[slot] = 0; continue; }
                const ue_wrap::FVector cur = g_puppets[slot].GetLocation();
                const float dx = sPoseTarget[slot].x - cur.X, dy = sPoseTarget[slot].y - cur.Y;
                const float trail = std::sqrt(dx * dx + dy * dy);
                UE_LOGI("pose-diag[slot %d]: fresh=%d/s targetSpeed=%.0f target=(%.0f,%.0f) "
                        "puppet=(%.0f,%.0f) trail=%.0fcm", slot, sPoseFresh[slot],
                        sPoseTarget[slot].speed, sPoseTarget[slot].x, sPoseTarget[slot].y,
                        cur.X, cur.Y, trail);
                if (trail > 500.f)
                    UE_LOGW("pose-diag[slot %d]: puppet TRAILS its target by %.0f cm (fresh=%d/s) "
                            "-- the apply/interp is behind the on-time pose stream", slot, trail,
                            sPoseFresh[slot]);
                sPoseFresh[slot] = 0;
            }
        }
    }

    // Receiver-side held-prop driver. Drains the latest PropPose from the
    // session and applies it (lookup-by-Key on first arrival, transform writes
    // thereafter). Stream-stop timeout (>500 ms) treated as implicit release.
    { PP::Scope _s{PP::Bucket::RemoteProp}; coop::remote_prop::Tick(session); }
    }  // worldUp (puppet drive + ragdoll + pose-diag + remote prop)

    // Surface session events (joins/disconnects) to the feed + send our Join.
    // Pass g_netLocal so remote_prop::OnRelease can call Aprop_C.thrown(player)
    // for the natural throw-sound dispatch (Path B in
    // research/findings/votv-throw-sound-path-2026-05-24.md).
    { PP::Scope _s{PP::Bucket::EventFeed}; coop::event_feed::Update(session, g_netLocal); }
}

}  // namespace coop::net_pump

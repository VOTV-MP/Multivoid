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

#include "coop/session/net_pump.h"

#include "coop/comms/chat_feed.h"    // Reset() on the leave-world flee (session UI dies with the session)
#include "coop/comms/chat_bubbles.h" // ResetSlots() on flee -- overhead bubbles are session UI too
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
#include "coop/creatures/kerfur_prop_adoption.h"  // K-6  // OnClientWorldReady (v75 deferred-adoption per-world reset)
#include "coop/creatures/wisp_grab_hold.h"  // Killer Wisp v2: grab-window body placement (ticks AFTER the puppet pose loop)
#include "coop/props/remote_prop_spawn.h"  // OnClientWorldReadyResetSweep (deferred prop sweep per-world reset)
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/session/player_handshake.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/player/remote_player.h"
#include "coop/props/remote_prop.h"
#include "coop/props/save_identity_bind.h"  // (b) re-bind-on-re-seed: BindUnboundReCreates (09:54 ghost fix)
#include "coop/save/save_transfer.h"
#include "coop/session/subsystems.h"
#include "coop/session/world_load_episode.h"  // JOIN BARRIER 2026-07-12: the announce waits on the load-tail quiescence latch

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/walk_timer.h"  // L5: [WALK-TIME] profiling of the reaper World walk + the re-seed census
#include "ue_wrap/core/sdk_profile.h"
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

// `travel`: false when VOTV's OWN quit-to-menu transition is ALREADY in flight
// (the RAM-balloon guard path) -- re-dispatching transition("/Game/menu") on top
// of it made the menu load TWICE (user 2026-07-04: "выходит в меню два раза").
// The rest of the tail (edge resets + Stop + bypass over the teardown) is
// identical either way.
void FleeToMainMenu(coop::net::Session& session, const char* why, bool travel = true) {
    if (g_fleeing) return;  // already travelling to the menu for this session
    g_fleeing = true;
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    session.Stop();
    // The chat/event feed is SESSION UI: every leave-world path funnels through this
    // flee (death, host-close eject, native quit-to-menu), and without a reset the
    // last lines ride their 11 s TTL into the MAIN MENU overlay (user 2026-07-11,
    // eyer death -> menu). The host-stays-in-world case (its last client leaving)
    // never flees, so its feed keeps the "X left the game" line as before.
    //
    // 2026-07-15: the reset was INCOMPLETE -- only chat_feed cleared, so an open chat
    // box, overhead bubbles, and nameplates leaked into the menu (user: "chat and
    // probably something else leaking into main menu from SESSION"). Every SESSION-
    // SCOPED overlay must die at this funnel (DisconnectAll is the WRONG home -- it
    // also runs on the HOST when a client leaves, and the host must KEEP its UI).
    // hud::IsActive() keys on chat_feed::HasAny() || nameplate::HasAny() || ... so a
    // stale nameplate alone re-activates the whole HUD in the menu.
    coop::chat_feed::Reset();
    coop::chat_bubbles::ResetSlots();
    coop::nameplate::ResetSlots();
    ui::chat_input::Close();
    ui::voice_panel::Close();
    // The feed Reset above is NOT sufficient on its own: session.Stop() (called
    // above) flips every peer slot connected->disconnected, and the NEXT
    // event_feed::Update reads that as a per-slot "<X> left the game" departure and
    // Pushes it into the just-cleared feed -- so a client's OWN quit-to-menu surfaced
    // "Host left the game" AFTER this reset and rode its 11 s TTL into the MAIN MENU
    // (user 2026-07-15; the message is also semantically wrong -- WE left, not the
    // host). Neutralize the edge detectors so the local teardown emits no spurious
    // peer-left toast. Host-stays-on-client-leave never reaches this funnel, so its
    // legitimate departure toast is preserved.
    coop::event_feed::SuppressPeerLeaveEdges();
    // Hold the detour dormant over the world teardown, but RESUME the instant the
    // menu's ui_menu_C::Tick first dispatches (menu world up) so MULTIPLAYER is
    // injected on the first menu frame. kDeathMenuBypassMs is only the safety
    // ceiling (used as-is if MenuTickFn() is null -- menu class never resolved).
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
        // Travel did not dispatch (gamemode/transition unresolved). The bypass
        // stays armed (still prevents reaper churn) and our actors are already
        // torn down, but we did NOT leave the gameplay world. Surface it loudly.
        UE_LOGE("net: %s -- ReturnToMainMenu FAILED to dispatch; still in the gameplay "
                "world. Relaunch.", why);
    }
}

// Full coop-state teardown for a session ending while the process lives on (local
// death OR a native quit-to-menu): destroy every puppet actor + per-slot subsystem
// state, then the aggregate session-wide drains. Mirrors what the disconnect edges
// do -- extracted 2026-07-04 because the RAM-balloon quit-to-menu flee SKIPPED it
// entirely (FleeToMainMenu resets g_wasConnected, which also suppresses the
// aggregate-disconnect edge!), leaving weather/time/sky caches + install latches +
// pending applies armed across the world teardown. A queued weather apply then ran
// against the old daynightCycle's RECYCLED GUObjectArray slot -> the cycle BP body
// executed with a foreign `self` -> FindFunctionChecked("setRainProperties") on an
// ArrowComponent -> LowLevelFatalError (user's client crash, ESC->menu 14:45).
void TearDownCoopStateForSessionEnd(coop::net::Session& session) {
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        coop::players::Registry::Get().UnregisterPuppet(static_cast<uint8_t>(slot));
        if (g_puppets[slot].valid()) g_puppets[slot].Destroy();
        coop::subsystems::DisconnectSlot(session, slot);
    }
    coop::subsystems::DisconnectAll();
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

    // ---- L5 HITCH + source probe (2026-06-23, diagnostic, always-on, ~free). The user reports a
    // PERMANENT ~3-4s frame stutter on both peers. The instrumented periodic walks (reseed 20s, *_sync,
    // the join-tail reaper 4s-but-transient) do NOT match a permanent 3-4s -> the suspect is uninstrumented
    // UE GC. [HITCH] times the wall-clock gap between consecutive game-thread Ticks (= the whole frame's
    // time, including any engine stall that freezes the frame -- GC/render/physics). [HITCH-SRC] (an RAII at
    // the end of this body) reports OUR Tick's own duration. The discriminator: a [HITCH] with NO matching
    // [HITCH-SRC] on the same frame = the stall was ENGINE-side (the GC signature, since it is permanent +
    // both-peers + fixed-period). If [HITCH] fires every ~3-4s with no [HITCH-SRC], GC is the root and the
    // fix is the GC cadence (gc.TimeBetweenPurgingPendingKillObjects / incremental GC), not a walk fix.
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

    // [dev] reseed_orphan_selftest: deterministic in-process proof of the 09:54 re-seed-orphan fix above.
    // Self-gated (one-shot, latches only once a live chipPile native exists) -> cheap no-op otherwise.
    if (worldUp) coop::save_identity_bind::RunReseedOrphanSelfTest();

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
    // controlled point, any Elements parked for destruction since the last
    // tick. Steady-state cost is a single uncontended mutex acquire +
    // empty-queue check. This IS the sync module's single deferred-retire
    // funnel: prop_element_tracker (reap/unmark), npc_sync, npc_world_enum,
    // trash_proxy, world_actor_sync, and kerfur_reconcile all route Element
    // teardown through ElementDeleter::Enqueue (13 producers as of the
    // 2026-06-28 sync consolidation). Bare-actor retires that carry site-
    // specific pre-steps (proxy un-root, echo-suppressed convert destroy)
    // stay direct -- only their Element bookkeeping funnels here.
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
        // (v106: the v105b forced-reconcile request path is RETIRED -- pickup
        // destroys broadcast at the K2_DestroyActor Func seam, drop/place actors
        // express at the hand edge / FinishSpawningActor Func seam, all
        // event-driven at the moment they happen. This reap + the periodic
        // census below remain the SAFETY NET for mass GC purges and any
        // non-BeginDeferred spawn path -- background cadence, no user-visible
        // latency rides on them anymore.)
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
            // L5 (FPS): the World instance changes ~once per session (level travel), so DON'T full-walk
            // ~237k GUObjectArray for it every 4s -- cache it + IsLiveByIndex-revalidate, re-resolving (one
            // walk) only on a miss. A travel kills the old world -> serial bump -> IsLiveByIndex false ->
            // re-resolve finds the new /Game/menu world, so the menu-travel RAM-balloon guard below STILL
            // fires; the death-watch scans the Prop Registry (not reapWorld), so it is unaffected. (engine.cpp:60
            // EnsureWorldContext caches identically but returns the immortal GameInstance -- the reaper needs
            // the WORLD instance for its gameplay-vs-menu name check, so it keeps its own cache.)
            static void*   g_reapWorld    = nullptr;
            static int32_t g_reapWorldIdx = -1;
            if (!g_reapWorld || !R::IsLiveByIndex(g_reapWorld, g_reapWorldIdx)) {
                ue_wrap::ScopedWalkTimer _wt("reaper:FindWorld");
                g_reapWorld    = R::FindObjectByClass(P::name::WorldClass);
                g_reapWorldIdx = g_reapWorld ? R::InternalIndexOf(g_reapWorld) : -1;
            }
            void* reapWorld = g_reapWorld;
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
                        "ending the session + stopping the layer churn (RAM-balloon guard)");
                coop::prop_element_tracker::SetInPurgeEpisode(false);  // left gameplay (matches the !inGameplayWorld reset below)
                // FULL teardown first (2026-07-04 client ESC->menu crash): this path used
                // to skip the disconnect fanout entirely (FleeToMainMenu's edge reset also
                // suppresses the aggregate-disconnect edge), leaving stale weather/time/sky
                // caches + pending applies armed over the teardown -- see
                // TearDownCoopStateForSessionEnd. travel=false: the user's own menu
                // transition is ALREADY in flight; a second dispatch loaded the menu twice.
                TearDownCoopStateForSessionEnd(session);
                FleeToMainMenu(session, "left gameplay to the menu (native quit)",
                               /*travel=*/false);
                return;
            }
            if (!inGameplayWorld) coop::prop_element_tracker::SetInPurgeEpisode(false);  // inert at menu; re-arm on gameplay re-entry
            std::vector<coop::element::ElementId> reapedEids;
            const size_t reaped = inGameplayWorld
                ? coop::prop_element_tracker::ReapDeadLocalPropElements(kReapEvictCap, &reapedEids)
                : 0;
            // Reaper escalation (2026-06-27, purge-timing race fix lever (a)). A mass-purge
            // backlog otherwise drains at kReapEvictCap (256) per 4 s throttle -- ~9 scans x 4 s
            // = ~37 s for a ~2300 backlog. That slow drain is the 09:54 ghost root: the
            // episode-end re-seed (below) fires only after the drain catches up, so a 37 s drain
            // lands the re-seed ~37 s into the join -- AFTER the save-identity bind/reconcile has
            // already armed on the pre-purge world, orphaning the save-authoritative natives
            // (tracked-but-unbound = ghosts). The 16:42 run worked only because a fast 4096-cap
            // self-heal drain happened to land the re-seed BEFORE the bind armed. So when THIS
            // scan hit the eviction cap (backlog remains) or a purge episode is still mid-drain,
            // cancel the 4 s throttle: the next tick reaps again immediately and the backlog
            // drains at frame cadence (~256/frame, ~0.15 s total) under the join cover instead of
            // 256/4 s. Per-call cost stays bounded at 256 (no single-frame hitch). Self-
            // terminating: the first scan that reaps < cap with the episode ended restores the
            // throttle. This races the re-seed back AHEAD of the bind (restores 16:42 timing);
            // lever (b) re-binds deterministically if a late GC purge still lands after the arm.
            if (inGameplayWorld &&
                (reaped >= kReapEvictCap || coop::prop_element_tracker::InPurgeEpisode())) {
                sNextReap = reapNow;  // drain again next tick -- no 4 s wait while a backlog remains
            }
            // PART 1 (2026-06-18) HOST-AUTHORITATIVE prop/pile death-watch. A host prop/pile
            // destroyed via a BP-internal EX_CallMath path (garbage-truck collect, ambient cull,
            // removeWOrespawn/LifeSpan despawn, grab-morph) NEVER fires K2_DestroyActor, so the
            // ProcessEvent detour can't see it -- the ONLY thing that ever removed the peer's
            // stale mirror was the retired 4s full re-snapshot's sweep. The reaper above detects
            // exactly these (a dead LOCAL element K2 never drained). In STEADY state (NOT a mass-
            // purge transition -- that is engine teardown the client does on its own travel) the
            // HOST now broadcasts an explicit PropDestroy(eid) per vanish -- MTA Packet_EntityRemove,
            // by IDENTITY (the sound replacement for the retired unsound proximity death-watch).
            // The client resolves the host-range eid -> drops its mirror. Idempotent vs the instant
            // OnPileGrabPre grab-destroy (a 2nd by-eid PropDestroy is a logged no-op on the receiver).
            // ASSUMPTION (audit L3, 2026-06-18): VOTV is ONE persistent untitled_1 world with no
            // in-gameplay sublevel/cave streaming, so the only thing that purges props is a full world
            // swap (reaps ALL ~3300 = a mass purge, excluded by reaped<kReseedPurge). IF a partial
            // LoadStreamLevel cave-travel is ever added (a <64 purge inside a live UWorld with peers in
            // DIFFERENT sublevels), this would PropDestroy the unloaded sublevel's props on a peer still
            // there -- re-gate on a membership/co-location check then.
            if (session.role() == coop::net::Role::Host && reaped > 0 &&
                reaped < kReseedPurge && !coop::prop_element_tracker::InPurgeEpisode()) {
                int sent = 0;
                for (coop::element::ElementId eid : reapedEids) {
                    if (eid == coop::element::kInvalidId || eid == 0) continue;
                    coop::net::PropDestroyPayload dp{};
                    dp.key.len = 0;  // eid-only: the client resolves the mirror by host-range eid
                    dp.elementId = static_cast<uint32_t>(eid);
                    session.SendPropDestroy(dp);
                    ++sent;
                }
                if (sent > 0)
                    UE_LOGI("net_pump: host death-watch -- broadcast %d explicit PropDestroy(eid) for "
                            "steady-state prop/pile vanish(es) the un-hookable BP path never replicated "
                            "(MTA per-entity remove, by identity)", sent);
            }
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
                    // JOIN BARRIER: the re-announce waits for the NEW world's load tail exactly
                    // like the first announce did -- open a fresh probe session for it.
                    coop::world_load_episode::ArmQuiesceProbe("world-change re-announce");
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
                // (a) RE-SEED-ORPHAN FIX (2026-06-28, the 09:54 ghost root). The episode-defer above already
                // waits for the reap backlog to fall below kReseedPurge, BUT the reaper Take()s each dead Prop
                // Element and DEFERS ~Element to ElementDeleter::Flush (worker-thread safety, element_deleter.h).
                // The top-of-Tick Flush ran BEFORE this Tick's reaper, so the natives the reaper just Took THIS
                // Tick are still pending -- their registry actor->eid reverse is STALE until ~Element. A re-seed
                // running now would adjudicate that half-gone state (MarkPropElement's IsBoundMirrorNative /
                // EidForActor guards read a Taken-but-not-yet-destructed Element), orphaning a churned save-native
                // bind into a tracked-but-unbound ghost. Flush the pending deferred-deletes FIRST so the re-seed
                // sees SETTLED state -- force the precondition, don't race it. [[feedback-snapshot-before-state-ready]]
                coop::element::ElementDeleter::Get().Flush();
                const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
                UE_LOGI("net_pump: world-change re-seed added %zu live keyed prop(s) (snapshot-completeness)", added);
                // (b) RE-BIND ON RE-SEED (2026-06-28). A GC-churned save-native re-creates UNBOUND at its save
                // position (the per-family cursor was already consumed -> it overflow-drops). Re-bind it BY
                // POSITION right HERE -- the purge-surviving stable key is the host-shipped save-time position
                // (1cm exact; the moved-in-window case is handled by b3 PropSnapPos / proxy-wins). Don't wait
                // ~30s for the late quiescence divergence sweep to do it -- that gap IS the 09:54 orphan window.
                // Cheap early-out (returns 0) when nothing churned; only the rare post-purge re-seed pays the
                // GUObjectArray walk -- NOT the 0.25 Hz steady re-seed below (W-2 perf rule).
                if (coop::save_identity_bind::IsEnabled()) {
                    const int rebound = coop::save_identity_bind::BindUnboundReCreates();
                    if (rebound > 0)
                        UE_LOGI("net_pump: post-purge re-seed re-bound %d unbound save-native(s) (chip by position, "
                                "kerfur by key; 09:54 orphan window closed at the re-seed edge, not the late sweep)", rebound);
                }
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
                // PERF (FPS #3, user 2026-06-22 -- a periodic ~4s FPS stutter on BOTH peers): ReSeedKnownKeyedProps
                // is a FULL ~237k-entry GUObjectArray census (+ a ToString(NameOf) string-alloc per keyed-
                // interactable + a second full walk for the world). Running it every ~4s unconditionally cost the
                // whole walk even when nothing spawned (added==0). GUARD it on the GUObjectArray HIGH-WATER MARK:
                // NumObjects() grows when a new UObject is appended (the common case for a spawn-menu/toolgun/
                // ambient/morph prop), so do the census EXACTLY then and SKIP the no-op walk while NumObjects is
                // unchanged -- eliminating the at-rest stutter (the idle client never walks). A spawn that reuses
                // a freed slot leaves NumObjects flat; a periodic SAFETY census every ~5th invocation (~20s)
                // bounds that coverage gap, and any later array growth catches it immediately. The join/world-
                // change branches above are UNGATED (they always re-seed in full), so snapshot coherence is intact.
                static int32_t sLastSteadyNum = -1;
                static int     sSinceFullWalk = 0;
                const int32_t  curNum   = R::NumObjects();
                const bool     grew     = (curNum != sLastSteadyNum);
                const bool     periodic = (++sSinceFullWalk >= 5);   // ~20s safety walk (this branch runs ~0.25 Hz)
                // (v106: recycled-slot drop/place actors -- NumObjects flat,
                // `grew` blind -- are expressed event-driven at the hand edge /
                // FinishSpawningActor Func seam; the ~20s periodic walk here is
                // the safety net only.)
                if (grew || periodic) {
                    if (periodic && !grew)
                        UE_LOGI("net_pump: steady-world re-seed -- periodic SAFETY census (NumObjects flat at %d; "
                                "catches any free-slot-reused spawn the high-water guard skipped)", curNum);
                    sLastSteadyNum  = curNum;
                    sSinceFullWalk  = 0;
                    std::vector<void*> newProps;
                    ue_wrap::ScopedWalkTimer _wt("reseed:KnownKeyedProps");  // L5 profile (this branch is high-water-gated)
                    const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps(&newProps);
                    if (added > 0) {
                        UE_LOGI("net_pump: steady-world re-seed adopted %zu NEW runtime-spawned keyed prop(s) "
                                "(spawn-menu/toolgun/ambient/pile) -- broadcasting one PropSpawn each "
                                "(incremental delta, no re-bracket; MTA CEntityAddPacket shape)", added);
                        // The late-registration deliver-missing owner: a generic prop broadcasts an
                        // incremental PropSpawn; a kerfur OFF-prop (skipped by the generic express, no
                        // KerfurConvert fired) takes the convert-safe deferred path. See
                        // coop::prop_snapshot::DeliverLateRegisteredProps + the OWNER BOUNDARY there.
                        coop::prop_snapshot::DeliverLateRegisteredProps(newProps);
                    }
                }
                // else: NumObjects unchanged -> nothing newly allocated -> SKIP the full census (the FPS fix).
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

    // v56 + JOIN BARRIER 2026-07-12: a CLIENT announces world-ready ONCE per connection, when a
    // gameplay world is up, the prop registry expresses IT, AND the load tail has QUIESCED
    // (world_load_episode probe latch). The announce is the MTA INITIAL_DATA_STREAM barrier
    // (reference/mtasa-blue Server/.../CGame.cpp:1393): the host opens the send gate + streams the
    // whole world state on it, so announcing before loadObjects' async tail settles put the entire
    // authoritative stream into a churning world -- the two-authority join seam behind RCA roots
    // 1/2/4/5 (research/findings/join-identity/votv-join-barrier-DESIGN-2026-07-12.md). The probe
    // is deadline-capped (45 s no-progress / 120 s absolute) so a pathological load still
    // announces (DEGRADED, logged LOUD by the probe) -- the barrier can never wedge the join.
    // Fire on the FIRST announce of a connection OR on a re-announce request (a world-change
    // re-seed completed -> the host must re-replay into the new world; maybeReAnnounce armed a
    // fresh probe session for the NEW world's tail). The coherence gate is identical either way:
    // a menu/stale-world announce would arm the host bracket against an unseeded client.
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
                // Per-slot teardown (puppet actors + slot state) + the aggregate
                // session-wide drains -- shared with the native quit-to-menu path.
                TearDownCoopStateForSessionEnd(session);
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
        //
        // v94 JOIN-JUMP root fix (user 2026-07-02: "хост видит как клиенты ПРЫГАЮТ
        // по позициям где проходил ХОСТ"): a joining client's pawn EXISTS through
        // the whole 30-60 s load window and every position it was parked/teleported
        // through was streamed as OUR pose -- garbage AT THE SOURCE, so the SENDER
        // gates (one owner).
        //
        // Take-6 final (two failed attempts first, both 2026-07-02): the take-4
        // predicate (ClientWorldReady coherence) fires SECONDS before the game
        // PLACES the pawn -- the world + prop registry are coherent off the
        // level-default props while the pawn still sits at the map's parked spot
        // (-37695,69978); gm loadObjects -> transformToPlayer teleports it at the
        // load TAIL (19:10 log: announce :54, placement :58, the 4 s of streamed
        // parking = the "jump"). A Func-table POST latch on loadObjects was built
        // and FAILED: gm loadObjects is called from the ubergraph via
        // EX_LocalFinalFunction, and script->script local calls dispatch through
        // ProcessLocalScriptFunction -- they touch NEITHER ProcessEvent NOR
        // UFunction::Func (20:07 + 20:14 verdicts: hook installed on the live
        // class, loadObjects demonstrably ran, POST never fired; the client never
        // streamed at all -- see docs/COOP_DISPATCH_VISIBILITY.md).
        //
        // The gate therefore uses the signal that OBSERVES loadObjects' effect
        // instead of hooking its call: load-tail QUIESCENCE (join_membership_sweep
        // g_sweepFired) -- the keyless-prop + NPC + chipPile population is stable
        // for 10 scans x 200 ms only after loadObjects' spawn flux (which contains
        // the player teleport) has ENDED. It is the same signal the DESTRUCTIVE
        // divergence sweep already trusts, it resets per world/bracket (a
        // mid-session world change closes the gate until the new world's tail
        // settles), and it is client-scoped -- exactly where the join-jump lives.
        // The HOST keeps the worldUp gate: its own load window matters only while
        // a client is connected through a host world-change (pre-existing take-4
        // residual, никем не наблюдался; the client side is the reported bug).
        const bool poseAuthoritative =
            isHost ? worldUp
                   : (g_worldReadyAnnounced.load(std::memory_order_relaxed) &&
                      !g_reAnnounceWorldReady.load(std::memory_order_relaxed) &&
                      coop::join_membership_sweep::HasLoadTailQuiesced());
        if (poseAuthoritative)
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
                // v93 skins (docs/COOP_CLIENT_MODEL.md): the puppet wears the skin
                // this peer announced (Join field / SkinChange), any slot incl. the
                // host. Empty (not yet announced) -> kel baseline; the skin re-applies
                // live when it lands (player_handshake::StoreSkinForSlot).
                if (!g_puppets[slot].Spawn(coop::player_handshake::SkinForSlot(slot))) {
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
                // Join announcement at the APPEARANCE seam: the puppet just spawned, which is
                // the moment the user actually sees the peer (2026-07-03: the old host announce
                // on ClientWorldReady+5s ran ~6 s before the puppet in the measured live flow).
                // CLIENT: announce here -- "Joined <host>'s game" (slot 0, +5s: own loading
                // screen) or cross-peer "<nick> joined the game" (immediate); the whole block is
                // gated on the client's own g_worldReadyAnnounced (the `continue` above). HOST:
                // announce here only once the slot is world-ready -- a pre-world menu/loading
                // pose can spawn the puppet early (user 2026-06-17), and in that order
                // OnClientWorldReady announces instead (both funnel through the same latch).
                if (session.role() == coop::net::Role::Client ||
                    session.IsSlotWorldReady(slot))
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

    // Killer-wisp grab-window body placement (v2 choreography). MUST run AFTER the
    // puppet Tick loop above: a held victim puppet is snapped to the wisp's
    // 'playerGrab' socket, overwriting the streamed pose for the hold window (the
    // module's own liveness guards release it). Cheap no-op when nothing is held.
    coop::wisp_grab_hold::Tick();

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
    // research/findings/physics-grab/votv-throw-sound-path-2026-05-24.md).
    { PP::Scope _s{PP::Bucket::EventFeed}; coop::event_feed::Update(session, g_netLocal); }
}

}  // namespace coop::net_pump

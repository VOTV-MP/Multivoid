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
#include "coop/creatures/kerfur_prop_adoption.h"  // K-6  // OnClientWorldReady (v75 deferred-adoption per-world reset)
#include "coop/player/puppet_drive.h"      // 2026-07-18 decomposition: puppet array + drive
#include "coop/props/registry_reaper.h"    // 2026-07-18 decomposition: reaper/re-seed engine
#include "coop/props/remote_prop_spawn.h"  // OnClientWorldReadyResetSweep (deferred prop sweep per-world reset)
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/session/player_handshake.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/player/remote_player.h"
#include "coop/props/remote_prop.h"
#include "coop/props/save_identity_bind.h"  // (b) re-bind-on-re-seed: BindUnboundReCreates (09:54 ghost fix)
#include "coop/save/save_transfer.h"
#include "coop/session/subsystems.h"
#include "coop/session/world_load_episode.h"  // JOIN BARRIER 2026-07-12: the announce waits on the load-tail quiescence latch

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/cached_obj_ref.h"
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

// (The per-slot puppet array moved to coop/player/puppet_drive, 2026-07-18
// decomposition -- net_pump reaches it via puppet_drive::DestroySlot/DriveTick.)

// Cached local mainPlayer_C for the net pump. coop::players::Registry::Get().Local()
// already caches + filters puppets via the controller discriminator
// (per RULE 1 + [[feedback-always-use-user-test-poses]]); we just hold
// a local cache on top to skip the atomic load in the hot pump path.
// CachedObjRef (islive-zeroav design section 3): probed every 125 Hz pump tick
// INCLUDING through teardown windows -- a bare-IsLive deref here was census row 1.
ue_wrap::CachedObjRef g_netLocal;
// Cached controller for the same pawn -- avoids 2 ProcessEvent dispatches per
// pump tick (GetController + GetControlRotation). Bound to g_netLocal's lifetime:
// Reset when g_netLocal is Reset (level change).
ue_wrap::CachedObjRef g_netLocalController;

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

// The Session the pump is currently ticking, valid for the duration of Tick.
// The ledger's teardown subscriber needs a Session but the subscriber signature
// deliberately carries only the rows (the ledger knows nothing about transport),
// and every site that can fire a transition -- the reconcile, the flee-time
// ClearAll -- runs inside a tick. Set at Tick entry, cleared at exit, so a
// transition fired from anywhere else finds no session and does nothing rather
// than dereferencing a stale pointer.
coop::net::Session* g_tickSession = nullptr;

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

// (The been-in-gameplay latch moved to registry_reaper with the menu-guard's
// detection half, 2026-07-18 decomposition.)

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
    coop::chat_sync::Reset();  // the record + the applied range die with the session
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
    // The ledger clears at session STOP, and it has to be done HERE rather than
    // left to the next reconcile: the reconcile early-returns once the session
    // stops running, so the rows would otherwise survive into the menu and into
    // the next session. Ordered after SuppressPeerLeaveEdges so the four
    // transitions tear person-state down silently instead of narrating four
    // departures into the main menu.
    coop::roster_ledger::ClearAll();
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
        // DestroySlot = unconditional UnregisterPuppet + destroy-if-live (the old
        // inline pair, verbatim inside puppet_drive); the per-slot interleave with
        // DisconnectSlot is preserved here, its one composition owner.
        coop::puppet_drive::DestroySlot(slot);
        coop::subsystems::DisconnectSlot(session, slot);
    }
    coop::subsystems::DisconnectAll();
}

}  // namespace

// The per-person WORLD teardown, driven by the ledger row transition. Both halves
// of the old falling-edge pair moved here verbatim: destroy the departed peer's
// puppet, then run the per-slot subsystem fan-out.
//
// It fires on a REPLACEMENT as well as a departure, and -- the part that was
// structurally missing -- it fires ON A CLIENT, where the old edge never could.
// Every body in subsystems::DisconnectSlot was read before this change (T5): the
// host-authoritative ones self-gate on Role::Host and become no-ops on a client,
// and the rest are local-state clears that a client SHOULD have been doing all
// along. None of them send from a client.
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
    // Public entry so the harness can route a HOST session death (or any session end)
    // to the main menu via the SAME validated path the client-death / disconnect flees
    // use (reset edge detectors + Stop + transparent-bypass + transition("/Game/menu")).
    // Idempotent via the g_fleeing latch -- harmless if net_pump already fled the client.
    // Game thread.
    FleeToMainMenu(session, why);
}

// (Puppet(slot) moved to coop/player/puppet_drive, 2026-07-18 decomposition.)

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

// The announce axis' ONE owner (2026-07-18 decomposition): the body is the old
// reaper-side maybeReAnnounce lambda's, verbatim -- registry_reaper now only
// requests through here.
void MaybeRequestReAnnounce(coop::net::Session& session, void* reapWorld) {
    if (session.role() == coop::net::Role::Host) return;
    if (reapWorld != g_announcedWorld) {
        g_reAnnounceWorldReady.store(true, std::memory_order_relaxed);
        // JOIN BARRIER: the re-announce waits for the NEW world's load tail exactly
        // like the first announce did -- open a fresh probe session for it.
        coop::world_load_episode::ArmQuiesceProbe("world-change re-announce");
        // R-4a: a world reload is a teardown+rebuild -- raise the reconcile window
        // (kind=load; the classifier resets) so the destroy seam + drop intent stay
        // suppressed through the re-replay bracket. InEpisode is NOT raised here
        // (the lane parks' reload semantics are unmeasurable today -- design doc §3).
        coop::world_load_episode::RaiseReconcileForReload();
    } else {
        UE_LOGI("net_pump: world-change re-seed on the SAME world already announced (%p) -- "
                "NOT re-announcing (suppresses the join menu-shadow-drain double-snapshot + "
                "kerfur re-adopt dupe)", reapWorld);
    }
}

void Tick(coop::net::Session& session) {
    // This whole body is game-thread-only: it drives the puppet array (via
    // puppet_drive, a GT-only-by-convention side-table) and runs
    // ElementDeleter::Flush (the controlled game-thread destruction point).
    // One guard at the top enforces the invariant for everything below it.
    UE_ASSERT_GAME_THREAD("net_pump::Tick (puppet drive + ElementDeleter::Flush)");
    // Scope the Session for the ledger's teardown subscriber (see g_tickSession).
    // RAII so an early return anywhere below cannot leave a stale pointer live.
    struct TickSessionScope {
        explicit TickSessionScope(coop::net::Session& s) { g_tickSession = &s; }
        ~TickSessionScope() { g_tickSession = nullptr; }
    } _tickSessionScope{session};

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

    // Dead-Prop-Element reconciliation + world-change re-seed + the gameplay->menu
    // RAM-balloon guard -> coop/props/registry_reaper (2026-07-18 decomposition;
    // the whole ~4s scan block verbatim). Returns true when the menu guard fired
    // (session torn down + fleeing) -- abort this Tick exactly like the old
    // inline `return` did.
    if (coop::registry_reaper::Tick(session)) return;

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
        // ARC A: the DISCONNECT half of this edge is GONE. Per-person teardown is
        // driven by the ledger ROW TRANSITION now (OnSlotReplaced_TearDownWorld
        // above). The falling edge was wrong in two structural ways:
        //   - On a CLIENT it never rose for slots 1-3 (a client only ever fills
        //     peerConns_[0]), so the whole ~20-subsystem fan-out NEVER RAN there:
        //     a client kept a departed third peer's voice channel, prop mirrors,
        //     owner-entity mirrors, trash proxies, flashlight cache and Player
        //     Element for the rest of the session.
        //   - On the HOST a fast REPLACEMENT skips it entirely -- ready->ready
        //     across one 8 ms tick, no edge at all, and the successor inherits.
        // The row transition compares VALUES, so it sees both.
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

    if (g_netLocal.Raw() && !g_netLocal.Alive()) { g_netLocal.Reset(); g_netLocalController.Reset(); }
    if (!g_netLocal.Raw()) g_netLocal.Set(localNow);  // resolved once at the top of this tick
    // The `!g_localDeathHandled` gate: the FIRST tick after death this block still runs
    // (it is where death is detected + the synchronous teardown fires), but once handled
    // we STOP all local-send work. Hands-on showed the ragdoll sender kept emitting 1140+
    // RagdollPose packets on the stopped session post-death -- reading the dead player's
    // pelvis ~100x/s while we are en route to the menu. Now the only per-tick work in the
    // dead window is the forced-menu backstop above.
    // Raw() below: validated by the Alive()/Reset/Set block just above, THIS tick.
    if (g_netLocal.Raw() && !g_localDeathHandled) {
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
            if (ue_wrap::engine::ReadMainPlayerRagdollState(g_netLocal.Raw(), isRagdoll, dead) && dead) {
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
        if (g_netLocalController.Raw() && !g_netLocalController.Alive()) g_netLocalController.Reset();
        if (!g_netLocalController.Raw())
            g_netLocalController.Set(ue_wrap::engine::GetController(g_netLocal.Raw()));
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
            coop::local_streams::Tick(session, g_netLocal.Raw(), g_netLocalController.Raw());
    }

    // Per-slot puppet drive -> coop/player/puppet_drive (2026-07-18
    // decomposition; the whole block verbatim: pose spawn/apply, ragdoll
    // drive, per-puppet interp Tick, wisp hold, pose-diag emit). The
    // worldReadyAnnounced load is evaluated IN the call expression -- the same
    // observation point as the old in-loop load (both writers ran earlier in
    // this same Tick). remote_prop stays HERE (a prop concern, not a puppet
    // one), under the SAME worldUp predicate, after the drive -- the original
    // order tick-loop -> wisp -> pose-diag -> remote_prop is preserved.
    if (worldUp) {
        coop::puppet_drive::DriveTick(session,
                                      g_worldReadyAnnounced.load(std::memory_order_relaxed));

        // Receiver-side held-prop driver. Drains the latest PropPose from the
        // session and applies it (lookup-by-Key on first arrival, transform writes
        // thereafter). Stream-stop timeout (>500 ms) treated as implicit release.
        { PP::Scope _s{PP::Bucket::RemoteProp}; coop::remote_prop::Tick(session); }
    }  // worldUp (puppet drive + ragdoll + pose-diag + remote prop)

    // Surface session events (joins/disconnects) to the feed + send our Join.
    // Pass g_netLocal so remote_prop::OnRelease can call Aprop_C.thrown(player)
    // for the natural throw-sound dispatch (Path B in
    // research/findings/physics-grab/votv-throw-sound-path-2026-05-24.md).
    { PP::Scope _s{PP::Bucket::EventFeed}; coop::event_feed::Update(session, g_netLocal.Raw()); }
}

bool HasAnnouncedWorldReady() {
    return g_worldReadyAnnounced.load(std::memory_order_relaxed);
}

}  // namespace coop::net_pump

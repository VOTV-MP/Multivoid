// coop/net_pump.cpp -- see coop/net_pump.h.
//
// Extracted from harness.cpp 2026-05-28 to keep harness.cpp at its
// boot/scenario glue role and bring net_pump.cpp under the modular
// soft cap (audit deferred-plan PR-4.13).

#include "coop/net_pump.h"

#include "coop/element/element_deleter.h"
#include "coop/event_feed.h"
#include "coop/garbage_sync.h"
#include "coop/trash_collect_sync.h"
#include "coop/save_block.h"
#include "coop/save_button_disable.h"
#include "coop/grab_observer.h"
#include "coop/ini_config.h"
#include "coop/item_activate.h"
#include "coop/player_damage.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/npc_sync.h"
#include "coop/player_handshake.h"
#include "coop/players_registry.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_lifecycle.h"
#include "coop/prop_snapshot.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/weather_sync.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hot_path_guard.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"
#include "ue_wrap/vitals.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

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

// Held-prop edge detector: file-scope for the same reason as g_wasConnected.
// A static-local would carry a stale prop pointer + key across a session stop/
// restart, causing the next pump to fire SendPropRelease for the OLD session's
// key on the NEW session -- a real bug found by the audit. Cleared by
// OnSessionStart on each session.Start.
void* g_lastHeldProp = nullptr;
coop::net::WireKey g_lastHeldKey{};
uint64_t g_propEmitCount = 0;

// v22 ragdoll-physics edge detector (same file-scope rationale as g_lastHeldProp:
// a static-local would carry a stale "was ragdolling" across a session restart and
// emit a spurious recover-edge false on the next session). Cleared by OnSessionStart.
bool g_wasRagdolling = false;
uint64_t g_ragdollEmitCount = 0;

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

// How long to hold the transparent bypass after a local-death flee to the menu (our
// layer stays dormant at the menu). 30 min -- far longer than anyone sits at a dead
// menu before relaunching to rejoin (permadeath). If it ever expires while still at the
// menu, the gameplay-world gate on the prop reaper keeps it from churning.
constexpr int kDeathMenuBypassMs = 30 * 60 * 1000;

// Terminal local eject to the main menu. The caller has ALREADY torn down coop
// game-side state (the death handler inline; the client disconnect edge via the
// OnDisconnect calls) -- this does the common tail: reset the edge detectors +
// Stop the session, then arm + HOLD the transparent bypass, then travel to the
// menu via VOTV's own transition verb. Used by BOTH the local-death flee AND the
// host-kicked/banned/host-gone client flee -- both leave the gameplay world the
// same way. Order matters: bypass is armed BEFORE the travel so the 50k-actor
// untitled_1 teardown the travel triggers runs with our ProcessEvent detour
// dormant (the death-policy rationale above). `why` is a short log tag.
void FleeToMainMenu(coop::net::Session& session, const char* why) {
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    session.Stop();
    ue_wrap::game_thread::SetTransparentBypass(kDeathMenuBypassMs);
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

// Game thread, ~send-rate: read the local player's pose. Pulled in from
// harness.cpp 2026-05-28; full rationale comments preserved.
bool ReadLocalPose(void* local, void* controller, coop::net::PoseSnapshot& out) {
    if (!local) return false;
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    const ue_wrap::FRotator actorRot = ue_wrap::engine::GetActorRotation(local);
    const ue_wrap::FVector vel = ue_wrap::engine::GetActorVelocity(local);
    // BODY yaw: read from the ACTOR (the real body facing direction). Earlier
    // attempt sent controller yaw -- but VOTV's Character body LAGS the camera
    // (head-leads-body is natural to the source), so sending controller yaw made
    // the puppet body show the CAMERA direction instead of the BODY direction
    // = sideways-when-camera-leads (user-confirmed regression).
    // HEAD pitch: read from the CONTROLLER (actor pitch is always 0 on an
    // upright character; the controller carries the real view pitch). Cached
    // by Tick to skip re-resolving GetController every tick.
    out.x = loc.X;
    out.y = loc.Y;
    // Z = source actor.Z (capsule centre on an ACharacter). Receiver
    // reconstructs visible body offset from its own mainPlayer_C's
    // RelLoc.Z at puppet spawn -- same BP class on every peer so the
    // authored constant matches. MTA-fidelity shape (CEntitySA streams
    // matrix.vPos = capsule centre).
    out.z = loc.Z;
    // Normalize yaw and pitch into the canonical FRotator axis range (-180, 180]
    // BEFORE they go on the wire. UE4's AController::GetControlRotation returns
    // RAW ControlRotation unnormalized: looking 10 deg DOWN reads back as
    // Pitch=350. Without this normalize, pitch=350 fails coop::net::ValidatePose's
    // (-90, 90) bound and the ENTIRE packet is dropped on the receiver -- root
    // cause of the "puppet freezes when host looks below horizontal" bug.
    out.yaw = ue_wrap::NormalizeAxis(actorRot.Yaw);
    const ue_wrap::FRotator ctlRot = controller
        ? ue_wrap::engine::GetControlRotation(controller)
        : actorRot;
    out.pitch = ue_wrap::NormalizeAxis(ctlRot.Pitch);
    // headYawDelta: the source's controller-yaw LEAD over its body yaw, in
    // (-180, 180]. The puppet's AnimBP headLookAt yaw reads this so the puppet's
    // head turns to match where the source's CAMERA is looking (free-look /
    // camera-lead-body) -- decoupled from body facing.
    out.headYawDelta = ue_wrap::NormalizeAxis(ctlRot.Yaw - actorRot.Yaw);
    out.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
    // Pack the source's airborne state. The receiver's BUA-POST observer
    // reads this bit to clear useLegIK on the puppet during the airborne
    // window so the foot-IK trace doesn't plant the puppet's feet to
    // ground while the source is jumping/falling. A-2 (2026-05-29 audit):
    // CMC pointer + MovementMode bytes accessed through ue_wrap::puppet::
    // ReadCharacterIsFalling -- symmetric with the write-side wrapper
    // DriveCharacterMovement (Principle 7: gameplay/coop layer reads
    // engine memory only through ue_wrap). The wrapper also adds IsLive
    // guards on actor + CMC that the prior inline code lacked -- raw
    // deref of a PendingKill mainPlayer_C (e.g. mid level transition)
    // was technically UB; false-positive rate in steady-state gameplay
    // is zero because the possessed mainPlayer_C never carries
    // PendingKill/Unreachable flags.
    out.stateBits = 0;
    if (ue_wrap::puppet::ReadCharacterIsFalling(local)) {
        out.stateBits |= coop::net::kStateBitInAir;
    }
    // v20 (Inc2b): piggyback the LOCAL player's ragdoll/faint state. isRagdoll
    // is the AnimBP gate flipped by EVERY ragdoll cause (manual C-key, exhaustion
    // faint, KO); we sync it as a per-peer DISPLAY flag so each peer's puppet
    // flops on the others' screens. Death is EXCLUDED -- a death-ragdoll routes
    // through VOTV's native SP menu flow + ends the session ([[project-coop-no-
    // host-migration]]), so it must not show as a recoverable faint on a puppet
    // that's about to be torn down. One wrapper read (a couple of derefs after
    // the offset cache fills) on this hot path -- same cost class as the
    // ReadCharacterIsFalling above it.
    {
        bool isRagdoll = false, dead = false;
        if (ue_wrap::engine::ReadMainPlayerRagdollState(local, isRagdoll, dead) &&
            isRagdoll && !dead) {
            out.stateBits |= coop::net::kStateBitRagdoll;
        }
    }
    // v19: piggyback the LOCAL player's vitals (health fraction + food + sleep)
    // in the 3 bytes that were `_pad` -- ZERO wire-size change. ue_wrap::vitals
    // reads THIS machine's UsaveSlot_C (one per machine), so the values are
    // per-peer-authoritative (host packs host's, each client its own). Default
    // to full (255) until the save resolves so a booting peer doesn't flash an
    // empty bar; overwrite per field only on a successful read. The receiver
    // treats these as DISPLAY-ONLY (puppet nameplate) -- never a saveSlot write.
    // After the one-time resolution cache fills, each Read is O(1) (a couple of
    // cheap derefs); 3-4 per pose-send tick is negligible on this hot path.
    out.healthFrac = out.foodFrac = out.sleepFrac = 255;
    {
        namespace V = ue_wrap::vitals;
        float health = 0.f, maxHealth = coop::net::kVitalScalarMax, food = 0.f, sleep = 0.f;
        bool gotHealth = false;
        if (V::Read(V::Field::Health, &health)) {
            gotHealth = true;
            V::Read(V::Field::MaxHealth, &maxHealth);  // best-effort; 100 fallback on miss
            out.healthFrac = coop::net::QuantizeUnitFraction(
                maxHealth > 0.f ? health / maxHealth : 1.f);
        }
        if (V::Read(V::Field::Food, &food))
            out.foodFrac = coop::net::QuantizeUnitFraction(food * (1.f / coop::net::kVitalScalarMax));
        if (V::Read(V::Field::Sleep, &sleep))
            out.sleepFrac = coop::net::QuantizeUnitFraction(sleep * (1.f / coop::net::kVitalScalarMax));
        // One-shot proof the vitals chain RESOLVED (vs silently defaulting to a
        // full bar). Latched on the first successful health read so it can't spam
        // this hot path. Game-thread only -> a plain static latch is safe here.
        static bool sVitalsLogged = false;
        if (gotHealth && !sVitalsLogged) {
            sVitalsLogged = true;
            UE_LOGI("vitals: first local read OK -- health=%.1f/%.1f food=%.1f sleep=%.1f "
                    "-> wire bytes h=%u f=%u s=%u", health, maxHealth, food, sleep,
                    out.healthFrac, out.foodFrac, out.sleepFrac);
        }
    }
    return true;
}

}  // namespace

void InstallObservers(coop::net::Session& session) {
    coop::grab_observer::Install();
    coop::prop_lifecycle::InstallInventory(&session);
    coop::prop_lifecycle::Install(&session);
    coop::npc_sync::Install(&session);
    coop::item_activate::Install(&session);  // Phase 5F flashlight
    coop::player_damage::Install(&session);  // vitals Inc3-WIRE damage relay (send + owner-apply)
    coop::weather_sync::Install(&session);   // Phase 5W weather
    coop::garbage_sync::SetSession(&session);
    coop::garbage_sync::Install();           // Phase 5G garbage
    coop::trash_collect_sync::SetSession(&session);
    coop::trash_collect_sync::Install();     // trash-stack press-E collect mirror
    // PR-FOUNDATION-2 (B): client world-save block (host-only persistence).
    // No-op on the host; on the client installs the SaveGameToSlot detour once.
    coop::save_block::Install(&session);
    // PR-FOUNDATION-2 (B part 2): grey out the client pause-menu "Save Game"
    // button (honest UX over the hard block). No-op on the host.
    coop::save_button_disable::Install(&session);
    // NOTE: coop::shutdown::Install / UpdateWindowTitle are called from
    // the timeline tick lambda DIRECTLY in harness.cpp -- they MUST NOT
    // be gated on g_netLocal like this function is (HWND subclass +
    // window title must work BEFORE the local player has been possessed,
    // e.g. on the OMEGA splash where the user might X-close before
    // gameplay).
}

void OnSessionStart() {
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    g_lastHeldProp = nullptr;
    g_lastHeldKey = {};
    g_propEmitCount = 0;
    g_wasRagdolling = false;
    g_ragdollEmitCount = 0;
    g_localDeathHandled = false;
}

coop::RemotePlayer& Puppet(int slot) {
    // g_puppets is a GT-only-by-convention side-table (no mutex). Enforce it.
    UE_ASSERT_GAME_THREAD("g_puppets (net_pump::Puppet)");
    return g_puppets[slot];
}

void Tick(coop::net::Session& session, float displayOffsetX) {
    // This whole body is game-thread-only: it drives g_puppets (a GT-only-by-
    // convention side-table) and runs ElementDeleter::Flush (the controlled
    // game-thread destruction point). One guard at the top enforces the
    // invariant for everything below it.
    UE_ASSERT_GAME_THREAD("net_pump::Tick (g_puppets + ElementDeleter::Flush)");

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
        static bool sInPurgeEpisode = false;
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
            const bool inGameplayWorld =
                reapWorld && R::ToString(R::NameOf(reapWorld)).find(L"ntitled") != std::wstring::npos;
            if (!inGameplayWorld) sInPurgeEpisode = false;  // inert at menu; re-arm on gameplay re-entry
            const size_t reaped = inGameplayWorld
                ? coop::prop_element_tracker::ReapDeadLocalPropElements(kReapEvictCap)
                : 0;
            if (reaped >= kReseedPurge) {
                if (!sInPurgeEpisode) {
                    sInPurgeEpisode = true;
                    UE_LOGI("net_pump: mass-purge detected (reaped %zu >= %zu) -- world-change re-seed deferred to drain-complete",
                            reaped, kReseedPurge);
                }
            } else if (sInPurgeEpisode) {
                // Drain caught up: the old level's dead Prop Elements are fully
                // evicted and the new level has loaded. Re-seed now (clean -- no
                // recycled-address collisions) + catch up connected peers.
                sInPurgeEpisode = false;
                const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
                UE_LOGI("net_pump: world-change re-seed added %zu live keyed prop(s) (snapshot-completeness)", added);
                // Catch up ALREADY-connected peers to the now-complete prop set:
                // their connect-edge snapshot enumerated the PRE-re-seed (partial)
                // registry -- an early joiner during host boot, OR a peer connected
                // when the host travelled (cave/level-change). Re-trigger the
                // per-slot snapshot; prop_snapshot queues it if a drain is in flight
                // and re-enumerates the (now complete) registry at dequeue, and the
                // client's RegisterPropMirror dedupes props it already holds. No-op
                // when no peer is connected yet (the normal boot case -- the eventual
                // connect-edge snapshot then reads the complete registry). Host-only.
                if (added > 0 && session.role() == coop::net::Role::Host) {
                    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
                        if (session.IsSlotReady(slot)) {
                            coop::prop_snapshot::TriggerForSlot(slot);
                            UE_LOGI("net_pump: re-snapshot ready slot %d after world-change re-seed", slot);
                        }
                    }
                }
            }
        }
    }

    // Lazily bring up the on-screen event feed once the GameInstance exists (it
    // is the persistent widget outer). One-time FindObjectByClass until it
    // succeeds, then it stops -- never a per-tick walk after init.
    if (!ue_wrap::hud_feed::IsInitialized()) {
        if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) ue_wrap::hud_feed::Init(gi);
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
            // Abort any pending/in-progress snapshot drain to this slot so we
            // don't iterate ~1700 candidates calling SendReliableToSlot into a
            // dead connection.
            coop::prop_snapshot::CancelForSlot(slot);
            // Per-slot subsystem cleanup. Only subsystems with actual per-slot
            // state get a call here. prop_lifecycle / npc_sync / weather_sync
            // hold GLOBAL state that the aggregate OnDisconnect below handles
            // correctly on full disconnect.
            coop::remote_prop::OnDisconnectForSlot(slot);
            coop::item_activate::OnDisconnectForSlot(slot);
        }
        if (!g_wasConnectedBySlot[slot] && slotConnected) {
            // Per-slot connect-edge replay.
            // - HOST -> NEW CLIENT (slot 1..3): full snapshot + flashlight +
            //   weather replay so the new client converges to host state.
            // - CLIENT -> HOST (slot 0): announce LOCAL flashlight state so
            //   the host can show it on our puppet. Weather is host-
            //   authoritative; snapshot is host->client only.
            if (isHost && slot >= 1) {
                UE_LOGI("net: peer slot %d connect edge -- replaying snapshot + flashlight + weather + peer states", slot);
                coop::prop_snapshot::TriggerForSlot(slot);
                coop::item_activate::QueueConnectBroadcastForSlot(slot);
                coop::weather_sync::QueueConnectBroadcastForSlot(slot);
                // T2-4: also catch the new client up to EXISTING peers'
                // current item state (the lines above replay only the HOST's
                // own state + host-authoritative weather/props).
                coop::item_activate::ReplayPeerStatesToSlot(slot);
            } else if (!isHost && slot == 0) {
                UE_LOGI("net: host (slot 0) connect edge -- replaying local flashlight");
                coop::item_activate::QueueConnectBroadcastForSlot(slot);
            }
        }
        g_wasConnectedBySlot[slot] = slotConnected;
    }
    if (g_wasConnected && !isConnected) {
        // Aggregate disconnect (all peers gone). Fire the global OnDisconnect
        // calls -- the per-slot edge block above already handled subsystems
        // with per-slot state, this catches subsystems with session-wide state
        // (weather_sync, npc_sync host-side counter, prop_lifecycle dedupe set).
        // Stashed state belongs to the now-dead session; replaying it on the
        // next session (possibly a different machine after IP change) would
        // carry wrong-peer state. A fresh snapshot enqueues on the next
        // connected edge.
        coop::remote_prop::ForceRelease();
        const auto propStats = coop::prop_lifecycle::OnDisconnect();
        const size_t snapPending = coop::prop_snapshot::OnDisconnect();
        coop::npc_sync::OnDisconnect();
        coop::item_activate::OnDisconnect();
        coop::weather_sync::OnDisconnect();
        UE_LOGI("net: all peers gone -- cleared %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries; takeObjInFlight=0",
                snapPending, propStats.initProcessedDropped);

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
    g_wasConnected = isConnected;

    // Process up to ~100 snapshot candidates per tick if a snapshot enumeration
    // is in progress (no-op on empty vector).
    if (isConnected) coop::prop_snapshot::DrainChunk();

    // Per-tick drains for subsystems that internally retry until the reliable
    // channel accepts a queued connect-time broadcast (item_activate +
    // weather_sync) and apply any per-peer payloads that arrived BEFORE the
    // corresponding puppet was spawned. Cheap early-return when no pending state.
    coop::item_activate::TickConnect();
    coop::weather_sync::TickConnect();

    // (The local-death flee to the main menu now happens SYNCHRONOUSLY on the death
    // frame in the teardown block below -- arm the held bypass + engine::ReturnToMainMenu
    // -- so there is no per-tick backstop here. The old `open menu` / `disconnect`
    // re-issue backstop was removed: both were no-ops, and our detour is held in bypass
    // for the travel so nothing of ours runs to "back it up" anyway.)

    if (g_netLocal && !R::IsLive(g_netLocal)) { g_netLocal = nullptr; g_netLocalController = nullptr; }
    if (!g_netLocal) g_netLocal = coop::players::Registry::Get().Local();
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
                    coop::prop_snapshot::CancelForSlot(slot);
                    coop::remote_prop::OnDisconnectForSlot(slot);
                    coop::item_activate::OnDisconnectForSlot(slot);
                }
                // Aggregate teardown: session-wide Element/dedup state (mirrors the
                // g_wasConnected disconnect edge).
                coop::remote_prop::ForceRelease();
                coop::prop_lifecycle::OnDisconnect();
                coop::prop_snapshot::OnDisconnect();
                coop::npc_sync::OnDisconnect();
                coop::item_activate::OnDisconnect();
                coop::weather_sync::OnDisconnect();
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
        InstallObservers(session);
        coop::net::PoseSnapshot mine;
        if (ReadLocalPose(g_netLocal, g_netLocalController, mine)) session.SetLocalPose(mine);

        // Held-prop replication. Read mainPlayer.grabbing_actor; if non-null,
        // build a PropPoseSnapshot from the prop's current world transform and
        // publish to the net thread. On the edge held -> not-held, send a
        // RELIABLE PropRelease so the peer re-enables SimulatePhysics (and we
        // never rely solely on the 500 ms stream-stop timeout).
        //
        // 2026-05-29 (post-A-4 follow-up): both fields read via
        // ue_wrap::engine::ReadMainPlayerGrabState (Principle 7 -- no inline raw
        // struct-offset derefs at coop layer). The wrapper resolves
        // mainPlayer.holding_actor via reflected_offset (MainPlayer_holding_actor
        // IS defined), so holdingActor is populated whenever the field exists.
        // Whether picking up a chipPile/clump actually SETS holding_actor (vs the
        // clump self-driving off its own holdPlayer) is a live-BP question
        // (Candidate A vs B); the [probe] below answers it from a real hands-on.
        ue_wrap::engine::MainPlayerGrabState gs{};
        void* heldActor = nullptr;
        if (ue_wrap::engine::ReadMainPlayerGrabState(g_netLocal, gs)) {
            heldActor = gs.grabbingActor;
            // 2026-05-27: chipPile/clump pickup sets mainPlayer.holding_actor
            // INSTEAD of grabbing_actor (their morph path doesn't use the
            // PhysicsHandle). Fall back to holding_actor so the PropPose
            // stream covers them too -- gated by IsKeyedInteractable since
            // holding_actor can also point at non-prop carry targets.
            if (!heldActor && gs.holdingActor && R::IsLive(gs.holdingActor) &&
                ue_wrap::prop::IsKeyedInteractable(gs.holdingActor)) {
                heldActor = gs.holdingActor;
            }
        }
        // [probe] garbage-ball pickup diagnostic (2026-05-31, ini
        // garbage_pickup_probe=1): resolves whether a chipPile/clump pickup sets
        // grabbing_actor or holding_actor (the open Candidate-A-vs-B question)
        // from a real hands-on carry, plus the held actor's class + key +
        // keyed-interactable verdict, proving the held-pose path can see the
        // clump. Only fires while something is held, throttled ~4 Hz; gated off
        // by default so steady-state cost is one atomic-bool load.
        static const bool sProbeGarbage =
            ::coop::ini_config::IsIniKeyTrue("garbage_pickup_probe");
        if (sProbeGarbage && (gs.grabbingActor || gs.holdingActor)) {
            static uint32_t sN = 0;
            if ((sN++ % 30) == 0) {
                void* ga = gs.grabbingActor;
                void* ha = gs.holdingActor;
                const bool gaLive = ga && R::IsLive(ga);
                const bool haLive = ha && R::IsLive(ha);
                const int haKeyed =
                    haLive ? (int)ue_wrap::prop::IsKeyedInteractable(ha) : -1;
                const std::wstring hKey = (haKeyed == 1)
                    ? ue_wrap::prop::GetInteractableKeyString(ha)
                    : std::wstring(L"-");
                UE_LOGI("[probe garbage_pickup]: grabbing_actor=%p(%ls) "
                        "holding_actor=%p(%ls) holdingKeyed=%d holdingKey='%ls'",
                        ga, gaLive ? R::ClassNameOf(ga).c_str() : L"-",
                        ha, haLive ? R::ClassNameOf(ha).c_str() : L"-",
                        haKeyed, hKey.c_str());
            }
        }
        if (heldActor && R::IsLive(heldActor)) {
            const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(heldActor);
            coop::net::PropPoseSnapshot pp{};
            pp.key.len = 0;
            for (size_t i = 0; i < keyW.size() && i < 31; ++i) {
                // The save UUIDs are ASCII; this lossless narrowing is fine.
                pp.key.data[pp.key.len++] = static_cast<char>(keyW[i]);
            }
            const auto loc = ue_wrap::engine::GetActorLocation(heldActor);
            const auto rot = ue_wrap::engine::GetActorRotation(heldActor);
            pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
            // Normalize at the wire boundary: physics-prop rotation accumulates
            // through FQuat<->Euler conversions and can end up at Yaw=359.8 or
            // Pitch=-270; receiver's canonical (-180,180] guard would reject.
            pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
            pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
            session.SetLocalPropPose(true, pp);
            // Throttled emit log: first 3 + every 60th, matches receiver
            // throttle so the two logs can be diff'd line-for-line.
            const uint64_t n = ++g_propEmitCount;
            if (n <= 3 || (n % 60) == 0) {
                UE_LOGI("net: PropPose emit #%llu -> world(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f) key.len=%d",
                        static_cast<unsigned long long>(n),
                        pp.x, pp.y, pp.z, pp.pitch, pp.yaw, pp.roll,
                        static_cast<int>(pp.key.len));
            }
            g_lastHeldProp = heldActor;
            g_lastHeldKey = pp.key;
        } else if (g_lastHeldProp) {
            // Edge: was holding, now not. Stop sending PropPose + tell peer.
            // Read the body's CURRENT linear+angular velocity via
            // prop::GetPhysicsVelocity. By the time this branch runs, the
            // engine has executed the BP graph clearing grabbing_actor, the
            // PHC.ReleaseComponent call, and any post-release AddImpulse the
            // BP issues. PhysX has not stepped yet, so the body still carries
            // the inherited kinematic-tracking velocity ("вжух" mouse-flick
            // launch energy) PLUS any impulse-derived velocity, summed into
            // ONE velocity. Forward both linear + angular so the receiver
            // can SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
            // for an identical launch.
            session.SetLocalPropPose(false, {});
            ue_wrap::prop::VelocityState vel{};
            if (R::IsLive(g_lastHeldProp)) {
                vel = ue_wrap::prop::GetPhysicsVelocity(g_lastHeldProp);
            }
            const float linMagSq = vel.linearCmS.X * vel.linearCmS.X +
                                   vel.linearCmS.Y * vel.linearCmS.Y +
                                   vel.linearCmS.Z * vel.linearCmS.Z;
            UE_LOGI("net: held -> released (vel.ok=%d linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f))",
                    vel.ok ? 1 : 0,
                    vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                    std::sqrt(linMagSq),
                    vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            session.SendPropRelease(g_lastHeldKey,
                                    vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                                    vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            g_lastHeldProp = nullptr;
            g_lastHeldKey = {};
        }

        // v22 ragdoll PHYSICS stream. While the local player's native ragdoll
        // exists (C-key/faint/KO), read its pelvis world transform + linear+angular
        // velocity and publish so each peer's mirror body slaves its pelvis to the
        // real ragdoll (instead of free-simulating its own flop). Mirrors the held-
        // prop stream above: publish each frame while active, one false-edge on
        // recover. ReadLocalRagdollPelvisPhysics returns false when not ragdolling,
        // so the active->idle transition is the recover edge.
        {
            ue_wrap::FVector rdLoc{}, rdLin{}, rdAng{};
            ue_wrap::FRotator rdRot{};
            if (ue_wrap::engine::ReadLocalRagdollPelvisPhysics(g_netLocal, rdLoc, rdRot, rdLin, rdAng)) {
                coop::net::RagdollPoseSnapshot rp{};
                rp.x = rdLoc.X; rp.y = rdLoc.Y; rp.z = rdLoc.Z;
                // Normalize the rotation at the wire boundary (the receiver drives
                // SetActorRotation off it + the canonical FRotator guard expects
                // (-180,180]); velocities are magnitudes, sent raw.
                rp.pitch = ue_wrap::NormalizeAxis(rdRot.Pitch);
                rp.yaw   = ue_wrap::NormalizeAxis(rdRot.Yaw);
                rp.roll  = ue_wrap::NormalizeAxis(rdRot.Roll);
                rp.linVelX = rdLin.X; rp.linVelY = rdLin.Y; rp.linVelZ = rdLin.Z;
                rp.angVelX = rdAng.X; rp.angVelY = rdAng.Y; rp.angVelZ = rdAng.Z;
                session.SetLocalRagdollPose(true, rp);
                g_wasRagdolling = true;
                const uint64_t n = ++g_ragdollEmitCount;
                if (n <= 3 || (n % 60) == 0) {
                    const float linMag = std::sqrt(rdLin.X * rdLin.X + rdLin.Y * rdLin.Y + rdLin.Z * rdLin.Z);
                    UE_LOGI("net: RagdollPose emit #%llu -> pelvis(%.0f, %.0f, %.0f) rot(%.0f, %.0f, %.0f) |linVel|=%.0f cm/s",
                            static_cast<unsigned long long>(n),
                            rp.x, rp.y, rp.z, rp.pitch, rp.yaw, rp.roll, linMag);
                }
            } else if (g_wasRagdolling) {
                session.SetLocalRagdollPose(false, {});
                g_wasRagdolling = false;
                UE_LOGI("net: RagdollPose stream STOP (local player recovered)");
            }
        }
    }

    // Per-slot pose drive. Iterate ALL peer slots [0, kMaxPeers) and skip
    // our OWN slot -- so each peer drives a puppet for every OTHER peer.
    // (PR-FOUNDATION Tier 2 host-relay: a client now sees not just the host
    // at slot 0 but also peer-clients at slots 2/3 once the host relays
    // their poses (T2-2) + identities (T2-1, PlayerJoined). Before relay
    // exists, TryGetRemotePose returns false for peer-client slots, so this
    // is a harmless no-op at those slots.) Each slot has its own RemotePlayer
    // puppet + spawn-retry backoff.
    {
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
            if (!g_puppets[slot].valid()) {
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
            }
            // Only RE-BASE the interpolation on a NEW packet; re-pushing the
            // latest every frame would zero `errorPos_` mid-window and freeze
            // motion. The per-frame advance happens in Tick() below.
            if (isNew) {
                coop::net::PoseSnapshot withOffset = remote;
                withOffset.x += displayOffsetX;  // loopback mirror shift (0 for real coop)
                g_puppets[slot].SetTargetPose(withOffset);
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

    // Receiver-side held-prop driver. Drains the latest PropPose from the
    // session and applies it (lookup-by-Key on first arrival, transform writes
    // thereafter). Stream-stop timeout (>500 ms) treated as implicit release.
    coop::remote_prop::Tick(session);

    // Surface session events (joins/disconnects) to the feed + send our Join.
    // Pass g_netLocal so remote_prop::OnRelease can call Aprop_C.thrown(player)
    // for the natural throw-sound dispatch (Path B in
    // research/findings/votv-throw-sound-path-2026-05-24.md).
    coop::event_feed::Update(session, g_netLocal);

    // Expire old chat-feed lines (10 s TTL) so a "X joined the game" line
    // doesn't linger forever.
    ue_wrap::hud_feed::Tick();
}

}  // namespace coop::net_pump

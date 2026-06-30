// coop/remote_prop_spawn.cpp -- wire-driven PropSpawn receiver
// (extracted from coop/remote_prop.cpp M-1 2026-05-29).
//
// See coop/remote_prop_spawn.h for the public interface + design notes.
//
// Behavior preserved byte-for-byte from the prior in-line implementation
// in remote_prop.cpp: same dedup gates (exact-key, fuzzy-position),
// same drive-skip semantics (PropPose owns position when held), same
// rekey-on-fuzzy-match pattern, same collision-restore-on-natural-spawn-
// class workaround, same fresh-spawn pipeline (BeginDeferred + setKey
// BEFORE FinishSpawningActor), same mirror registration on every path.

#include "coop/props/remote_prop_spawn.h"

#include "coop/props/trash_pile_sync.h"  // sweep->death-watch unwatch (v57 audit CRIT-2)

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/session/ini_config.h"  // IsIniKeyTrue -- hands-on probe flags live in votv-coop.ini [dev], not bats/env
#include "coop/creatures/kerfur_entity.h"  // GetKerfurMirrorEidForActor -- exempt host-driven kerfur mirrors in the grab-guard predicate (the SWEEP excludes mirrors structurally via pr.mirror since R3)
#include "coop/creatures/kerfur_prop_adoption.h"  // K-6: defer a kerfur-prop fuzzy-miss to the polled adoption
#include "coop/creatures/kerfur_reconcile.h"  // scope A: post-quiescence retry of the kerfur off->active dup retire
#include "coop/session/mirror_defer.h"  // instant-world: hide a freshly-spawned host mirror until lift/quiescence reveal
#include "coop/net/protocol.h"
#include "coop/creatures/npc_sync.h"  // IsAllowlistedClass -- the NPC half of the load-tail quiescence probe
#include "coop/props/pile_spawn_bind.h"  // anti-smear 2026-06-30: pile spawn-time twin-destroy / adopt / census
#include "coop/dev/spawn_order_probe.h"  // Phase 1 step 1A: keyless load-spawn coverage probe (read-only)
#include "coop/props/save_identity_bind.h"     // Phase 1 step 2b: eid-range bind summary at quiescence
#include "coop/element/quiescence_drain.h"    // anti-smear 2026-06-30: the join-window order owner (sequence + steady triggers + deferred queues)
#include "coop/props/snapshot_census.h"  // Phase 0: per-class completeness floor for the claim sweep
#include "coop/dev/force_overdestroy_test.h"  // dev-only: floor-disable toggle for the controlled proof
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_proxy.h"   // phase 1: the host-authoritative AStaticMeshActor trash mirror (dup fix)
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD -- the no-mutex contract tripwire
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>   // getenv -- the read-only [PILE-DELTA] probe gate (L1 orphan histogram)
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::remote_prop_spawn {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// PropSpawn UFunction resolution (cached one-shot). Spawn path uses the
// deferred-spawn pair on UGameplayStatics CDO; setKey is on Aprop_C base;
// Conv_StringToName is on UKismetStringLibrary CDO (used to convert wire
// Key string -> live FName for setKey).
void* g_gsCdo            = nullptr;
void* g_beginSpawnFn     = nullptr;
void* g_finishSpawnFn    = nullptr;
void* g_propSetKeyFn     = nullptr;
bool  g_spawnResolved    = false;

bool ResolveSpawnFns() {
    if (g_spawnResolved && R::IsLive(g_gsCdo)) return true;
    g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (!g_gsCdo) return false;
    void* cls = R::ClassOf(g_gsCdo);
    if (!cls) return false;
    g_beginSpawnFn  = R::FindFunction(cls, P::name::BeginDeferredSpawnFn);
    g_finishSpawnFn = R::FindFunction(cls, P::name::FinishSpawningActorFn);
    if (!g_beginSpawnFn || !g_finishSpawnFn) {
        UE_LOGW("remote_prop::OnSpawn: GameplayStatics spawn fns missing (begin=%p finish=%p)",
                g_beginSpawnFn, g_finishSpawnFn);
        return false;
    }
    // setKey on Aprop_C base. The PropClass UClass may not be loaded at the
    // first call (the engine loads BP classes on-demand); on first miss we
    // retry next call.
    if (void* propCls = R::FindClass(P::name::PropClass)) {
        g_propSetKeyFn = R::FindFunction(propCls, P::name::PropSetKeyFn);
    }
    g_spawnResolved = true;
    return true;
}

// ClassNameToWString is now a shared public helper (definition after the anon
// namespace; declared in remote_prop_spawn.h). Internal anon-namespace callers
// resolve it via enclosing-namespace lookup.

// True for prop classes whose locally-spawned instance lands with collision
// disabled (NoCollision) via a natural-spawn pipeline that calls
// spawnedNaturally(), and whose wire-converged copy therefore needs an
// explicit SetCollisionEnabled(QueryAndPhysics) restore. Per the mushroom
// fall-through RE 2026-05-25:
//   AmushroomSpawner_C::Spawn -> spawnedNaturally() -> NoCollision.
// Currently only the cap mushroom is confirmed; extend if other natural-
// spawn paths exhibit the same pattern. The check is cheap (single string
// compare) so calling it in OnSpawn's hot path is fine.
//
// ==== RULE 1 RETIREMENT PLAN (audit fix 2026-05-25, issue #4) ====
// This restore is an INTERIM fix at the SYMPTOM (collision write at
// OnSpawn-time). The architecturally correct ROOT-CAUSE fix is to suppress
// AmushroomSpawner_C::Spawn on the client entirely, so the local actor
// never goes through spawnedNaturally() in the first place (Option B from
// the RE doc). Gating criteria for retirement:
//   (1) Phase 5N1 Stream B-Spawners ships -- single BeginDeferredActor
//       SpawnFromClass observer with a class allowlist that includes
//       AmushroomSpawner_C (covers all natural spawners simultaneously).
//   (2) Hands-on test confirms mushrooms sync correctly with this restore
//       call commented out (the test the user is running now is the
//       inverse -- with restore IN, verifying no fall-through).
//   (3) Then remove this helper + the IsCollisionRestoreClass check +
//       PropFoodMushroomClass constant. Per RULE 2 the kerfur-of-the-day
//       fix goes when the proper fix lands; no parallel paths.
// Tracked in [[project-coop-mushroom-state-re]] memory entry under
// "Stream B-Spawners" follow-up scope.
inline bool IsCollisionRestoreClass(const std::wstring& cls) {
    return cls == ue_wrap::profile::name::PropFoodMushroomClass;
}

// Helper for OnSpawn's three convergence/spawn paths: if the class needs
// collision restore, call ue_wrap::prop::ForceRestoreDefaultCollision and
// log the outcome with the path label so a fall-through regression is
// diagnosable from the log. No-op for classes that don't need restore.
void RestoreCollisionIfNeeded(const wchar_t* pathLabel,
                              const std::wstring& classW,
                              void* actor) {
    if (!IsCollisionRestoreClass(classW)) return;
    const bool ok = ue_wrap::prop::ForceRestoreDefaultCollision(actor);
    if (ok) {
        UE_LOGI("remote_prop::OnSpawn[%ls]: collision restored (QueryAndPhysics) on '%ls' actor=%p",
                pathLabel, classW.c_str(), actor);
    } else {
        UE_LOGW("remote_prop::OnSpawn[%ls]: collision restore FAILED on '%ls' actor=%p -- prop may fall through ground",
                pathLabel, classW.c_str(), actor);
    }
}

// SP-parity kinematic reconcile (perf root-cause fix, 2026-06-09 -- the real fix
// the reverted kAtRest experiment was reaching for). When the host prop is NOT
// simulating (settled/static/frozen -> the SP-parity snapshot stamp carries no
// kSimulatePhysics; SP's Aprop_C::init() = SetSimulatePhysics(NOT(static||frozen||
// sleep))), force the client's pre-existing Aprop_C copy KINEMATIC *before* any
// teleport-converge. A kinematic body does not wake and is NOT ejected when the
// teleport lands it in the client's RNG-divergent layout -- this kills the
// ~320-prop_C penetration storm that locked the client at ~5-20 FPS / crashed PhysX
// (the host, loading the identical props asleep from disk, holds 110+ FPS on the
// same DLL). SAFE where kAtRest's PutRigidBodyToSleep crashed: SetSimulatePhysics
// (false) removes the body from the solver island entirely (no de-penetration), it
// does not poke a body still resolving contacts in-solver.
//   Scoped to Aprop_C (GetStaticMesh != null): non-Aprop_C keyed interactables
// (chipPile/trashBitsPile/clump) are kinematic BY DESIGN -- GetStaticMesh returns
// null on them (the 2a-safety gate, prop.cpp) precisely because driving their
// physics frees-then-derefs; the diagnosis confirmed their teleports are
// physics-harmless (no wake), so they need NO kinematic touch and we must NOT poke
// their root (UAF). No-op when the host prop IS simulating (held/falling): the
// mirror keeps simulating / is PropPose-driven.
void ReconcileToHostPhysics(void* actor, uint8_t physFlags) {
    const bool hostSimulating =
        (physFlags & coop::net::propspawn_flags::kSimulatePhysics) != 0;
    if (hostSimulating) return;
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);  // Aprop_C only; null for non-Aprop_C
    if (!mesh) return;
    coop::remote_prop::DriveSimulate(mesh, /*simulate=*/false);
}

// SP-parity simulate state from the wire identity flags: SP's Aprop_C::init()
// computes SetSimulatePhysics(NOT(static || frozen || sleep)) -- a normal
// settled prop is simulate-ENABLED but ASLEEP (that is how the save loads it,
// and what VOTV's PhysicsHandle grab requires). 2026-06-10 hands-on root
// cause: leaving bound/converged/fresh-mirror props force-kinematic made them
// UNGRABBABLE on the client until a host grab+release re-enabled physics
// ("client can't interact with the wall office objects").
bool SpParitySimulate(uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    return (physFlags & (pf::kStatic | pf::kFrozen | pf::kSleep)) == 0;
}

// Restore SP-parity physics AFTER a teleport-converge. Safe from the P1 wake
// storm NOW: with the v56 save-transfer join the converge target is the HOST's
// rest pose in an IDENTICAL world -- the body wakes at a valid pose and
// re-settles (vs the pre-save-transfer case of waking inside divergent
// geometry, the ~320-prop penetration storm ReconcileToHostPhysics exists
// for). Aprop_C only (same scoping rationale as ReconcileToHostPhysics).
void RestoreSpParityPhysicsAfterConverge(void* actor, uint8_t physFlags) {
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);
    if (!mesh) return;
    coop::remote_prop::DriveSimulate(mesh, SpParitySimulate(physFlags));
}

// ---- P2 claim tracking state (2026-06-10) -------------------------------
// Game-thread only: armed/recorded/swept exclusively from the event_feed
// drain (SnapshotBegin / PropSpawn / SnapshotComplete dispatch inline on the
// GT) + the net_pump disconnect edge. No mutex needed. See the design block
// in remote_prop_spawn.h.
std::unordered_set<void*> g_claimedActors;
bool g_claimTrackingActive = false;

// ---- Deferred divergence sweep (quiescence-gated; the kerfur-savetransfer
// ghost fix 2026-06-15). The sweep no longer runs inline at SnapshotComplete --
// see ArmDivergenceSweep/TickClientReconcile + the design note in the header.
// All game-thread-only (the client tick + the event_feed drain), no mutex.
bool g_sweepPending = false;                          // armed at SnapshotComplete; cleared when the sweep runs
bool g_sweepFired   = false;                          // sticky: set when the sweep runs, until re-armed (HasLoadTailQuiesced)
// (g_sweepReconciled, the 2026-06-17 reconcile-once latch, was RETIRED by R1+R3 on
// 2026-06-18: R1 stopped the steady re-seed re-bracketing -- the dominant re-arm --
// and R3's membership + the >50% valve make any remaining re-fire bounded + safe. See
// the note in ArmDivergenceSweep. RULE 2: no latch, no parallel band-aid.)
std::chrono::steady_clock::time_point g_sweepArmedAt{};
std::chrono::steady_clock::time_point g_sweepLastProgressAt{};  // reset on any progress (active purge OR moving
                                                                // population) -> base of the NO-PROGRESS deadline
std::chrono::steady_clock::time_point g_sweepLastScan{};  // {} (epoch 0) => not yet scanned this arm
int  g_sweepLastUnsettledCount = -1;
int  g_sweepStableScans        = 0;
constexpr int kSweepScanIntervalMs = 200;    // 5 Hz quiescence probe while pending (matches npc_adoption)
constexpr int kSweepQuiesceScans   = 10;     // load-tail population (keyless props, chipPiles, AND allowlisted
                                             // NPCs) stable across 10 scans (~2 s) = the async loadObjects pass
                                             // has drained. Longer than the old 600 ms: the kerfur NPCs load with
                                             // multi-hundred-ms gaps after the props, so a short window false-
                                             // signals mid-load -> the adoption fresh-spawns a duplicate of a twin
                                             // still to come (the 2026-06-15 kerfur-dupe).
// Two-tier deadline (2026-06-25, the docs/piles/10 purge-aware gate). The sweep must adjudicate the FULLY
// reloaded world, but a join-time world-reload mass-purge drains async over ~50 s (14:11 hands-on: arm ->
// re-seed-done ~50 s) -- a fixed SINCE-ARM deadline would fire INSIDE the purge trough before the re-seed,
// never exercising the floor (the exact self-sabotage this gate avoids). So:
//   - kSweepDeadlineMs: a NO-PROGRESS timer (base = g_sweepLastProgressAt, reset whenever a purge is draining
//     OR the load-tail population is still moving). It fires only after this long with NOTHING happening --
//     a genuine stall, never a legitimate long drain. Same 45 s value, new semantics.
//   - kSweepHardCapMs: an ABSOLUTE ceiling from arm. Bounds a pathological stuck-purge-flag (no-progress
//     keeps resetting) so the reconcile can never defer forever. Curtain is already down at Complete, so this
//     only delays the INVISIBLE reconcile, not player control -> a generous 120 s is safe.
constexpr int kSweepDeadlineMs     = 45000;   // NO-PROGRESS deadline (since last progress, not since arm)
constexpr int kSweepHardCapMs      = 120000;  // absolute ceiling since arm (stuck-purge backstop)

// Record that the host snapshot bound `actor` (exact-key, fuzzy, or fresh
// spawn) -- the actor is accounted-for by the host's world and must survive
// the sweep. No-op when tracking is disarmed (live PropSpawns outside a join
// cost one bool read).
void RecordClaim(void* actor) {
    if (!g_claimTrackingActive || !actor) return;
    g_claimedActors.insert(actor);
}

// The keyless-pile position-bind index + its twin-destroy / 30cm-adopt / orphan-
// census live in coop/props/pile_spawn_bind.{h,cpp} (the spawn-time mechanism); the
// deferred reconcile queues + the drain-edge sequence live in coop/element/quiescence_drain
// (the join-window ORDER owner). This file keeps only the claim set (above) + the OnSpawn /
// sweep call-sites below, which drive pile_spawn_bind:: with g_claimedActors and fire the
// quiescence_drain sequence. (Anti-smear refactor 2026-06-30; the old single coop/pile_reconcile
// held all three concepts + a generic kerfur destroy queue in a "pile"-named file.)

}  // namespace

// Build a wide string from WireClassName. Lossless for ASCII (VOTV class names).
// Shared (declared in remote_prop_spawn.h) so remote_prop::OnConvert can reuse it.
std::wstring ClassNameToWString(const coop::net::WireClassName& cn) {
    std::wstring s;
    s.reserve(cn.len);
    for (uint8_t i = 0; i < cn.len && i < 63; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(cn.data[i])));
    }
    return s;
}

void RecordClaimIfTracking(void* actor) {
    // Fork B 2c (2026-06-10): public claim primitive for the SELF-announce
    // sites (Init POST / takeObj POST / held-item / convert broadcasts). A
    // client announcing a prop while a bracket is open creates a live
    // in-universe actor the host's already-enumerated bracket cannot express
    // -- without a claim the sweep would destroy the announcer's own prop at
    // SnapshotComplete while the host keeps the mirror. Claims = "entities
    // expressed on the wire this bracket, in EITHER direction".
    RecordClaim(actor);
}

void OnSpawn(const coop::net::PropSpawnPayload& payload, int senderSlot,
             void* localPlayer, bool fromConvert, bool deferKerfur,
             void** outSpawned, bool skipBind) {
    if (outSpawned) *outSpawned = nullptr;  // cleared up-front; set only on a successful spawn
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    const std::wstring classW = ClassNameToWString(payload.className);
    const std::wstring keyW   = coop::remote_prop::KeyToWString(payload.key);
    // v54: the Aprop_C list_props identity row (empty for non-Aprop_C senders
    // / classes without a row). Used by the fuzzy identity gate + the Path C
    // pre-Finish write.
    const std::wstring propNameW = coop::remote_prop::KeyToWString(payload.propName);
    UE_LOGI("remote_prop::OnSpawn: cls='%ls' key='%ls' name='%ls' loc=(%.1f, %.1f, %.1f) rot=(%.1f, %.1f, %.1f) physFlags=0x%02x",
            classW.c_str(), keyW.c_str(), propNameW.c_str(),
            payload.locX, payload.locY, payload.locZ,
            payload.rotPitch, payload.rotYaw, payload.rotRoll,
            static_cast<int>(payload.physFlags));
    // A non-keyable trash CLUMP rides this same pipeline identified by our EID, not a
    // Key (setKey doesn't stick on it). For the clump, key is None but elementId != 0;
    // it's deduped + registered + resolved by eid. Aprop_C keeps the keyed path.
    // [[project-bug-trash-chippile-uaf-crash]]
    const bool eidOnly = (keyW.empty() || keyW == L"None");
    if (classW.empty() || (eidOnly && payload.elementId == 0)) {
        // "None" = FName(NAME_None) stringified. A keyed prop with no key AND no eid is
        // a legacy/in-flight straggler -- reject defensively so it doesn't trigger the
        // dedup spam loop on the receiver.
        UE_LOGW("remote_prop::OnSpawn: unkeyed + no eid (cls='%ls' key='%ls') -- dropping",
                classW.c_str(), keyW.c_str());
        return;
    }
    // PROXY PATH (phase 1): trash (chipPile/clump + variants) is mirrored by a host-authoritative
    // AStaticMeshActor WE own (coop/trash_proxy) -- NO blueprint (no self-morph / GC / stale-index ->
    // no dup) -- instead of the real BP. Branch BEFORE the BP dedup / converge / physics machinery:
    // SpawnProxy is idempotent (re-skins an existing proxy for the same eid, so a re-spawn convergence
    // never doubles). Re-skin (pile<->clump) is OnConvert::ReskinProxy; teardown is OnDestroy::RetireProxy.
    // Trash is eid-only (the EID is the identity, Key=None). skipBind respected: OnConvert's
    // convert-before-spawn fallthrough passes skipBind=true and binds E itself.
    if (coop::trash_proxy::IsTrashProxyClass(classW)) {
        // (X) native-authoritative guard (a): if this eid already resolves to a LIVE bound-mirror NATIVE
        // (the client loaded the same host save + save_identity_bind bound its native actorChipPile_C as the
        // host-range mirror at this eid), the NATIVE *is* the mirror -- do NOT spawn a redundant bare-
        // AStaticMeshActor proxy over it. The proxy has no int_player_C/collision (through-wall grab, walk-
        // through); suppressing it for the bound eid restores the native interaction surface for free. The
        // native is already RegisterPropMirror'd by the bind, so the pose drive resolves it via
        // ResolveLiveActorByEid -- nothing to (re-)register here. Only save-loaded piles hit this; runtime/
        // host-only piles + carried clumps have no bound native -> IsBoundMirrorNative is false -> they spawn
        // the proxy as before (the proxy is irreducible for them). Blast radius nil (per-eid gate).
        if (auto* be = coop::element::Registry::Get().Get(payload.elementId)) {
            void* bn = be->GetActor();
            // IsLiveByIndex (NOT IsLive): the cached mirror actor is engine-GC-owned; read only the
            // GUObjectArray slot, never deref a possibly-freed pointer ([[feedback-islive-unsafe-on-freed-cached-pointer]]).
            if (bn && R::IsLiveByIndex(bn, be->GetInternalIdx()) &&
                coop::prop_element_tracker::IsBoundMirrorNative(bn)) {
                if (outSpawned) *outSpawned = bn;
                return;
            }
        }
        const bool isClump = coop::trash_proxy::IsClumpClass(classW);
        ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
        ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
        void* proxy = coop::trash_proxy::SpawnProxy(payload.elementId, payload.chipType, isClump, senderSlot, loc, rot,
                                                    ue_wrap::FVector{payload.scaleX, payload.scaleY, payload.scaleZ});
        if (proxy) {
            if (!skipBind) {
                coop::remote_prop::RegisterPropMirror(payload.elementId, proxy, L"", classW, senderSlot);
                // instant-world: hide the freshly-registered proxy until reveal. Proxies are already
                // collision-less (collisionOff=false). hasMatchPos => a moved-save pile whose local native@old
                // is still visible -> HOLD to quiescence; else (derived/window pile, no native twin) reveal at
                // the curtain-lift. (skipBind = a convert re-skin of an existing proxy -> OnConvert owns it;
                // g_revealed also guards against re-hiding an already-revealed one.)
                coop::mirror_defer::OnMirrorSpawned(payload.elementId, proxy, /*collisionOff=*/false,
                                                    /*holdUntilQuiescence=*/payload.hasMatchPos != 0);
            }
            if (outSpawned) *outSpawned = proxy;
            // LEVEL-PILE DUP DESTROY (take-31, probe-greenlit: all 16 PILE-PROBE = 1cm=1). A level-placed
            // chipPile gets a host eid + THIS proxy, but the client's NATIVE level-loaded chipPile COEXISTS
            // with the proxy = the visible dup (the sweep is blind to natives -- they enter the Registry
            // lazily). Destroy the co-located native AFTER the proxy spawns (no visible gap) so the proxy is
            // the sole mirror. EXACT ~1cm match (the probe confirmed bit-exact load); GRACEFUL on 0 (a DERIVED/
            // gameplay pile has no native twin -- a thrown pile arrives via PropConvert re-skin, and a derived
            // pile the joiner spawns has no local native -> skip, never destroy); EXACT-or-SKIP on >1 (a dense
            // cluster with >1 native within 1cm is ambiguous -> keep both, never destroy the wrong one). NOT
            // adopt -- adopt reintroduces the BP self-morph/GC/stale-index the proxy model exists to avoid;
            // destroy + let the proxy own the identity. The join-time index (EnsurePileBindIndex) is built ONCE
            // before the proxy-spawn burst (one GUObjectArray walk, NOT one per pile) and shrinks as natives are
            // consumed -- O(index) scan per spawn, no per-spawn full-array walk. Replaces the read-only
            // PILE-PROBE (RULE 2: the diagnostic is retired now that it greenlit the destroy).
            //
            // GATED on g_claimTrackingActive (audit CRITICAL-4): a native level-pile twin to destroy ONLY
            // exists DURING the join reconcile bracket (where level piles are expressed against the index built
            // from the just-loaded world). OUTSIDE the bracket the destroy is both pointless (a mid-gameplay
            // re-pile / derived pile has NO native twin -- they were de-duped at join or never existed) AND
            // dangerous: EnsurePileBindIndex would REBUILD via a full GUObjectArray walk inside the BeginDeferred
            // thunk's game-thread stack (a warm-path regression on the first post-bracket pile-land), and a
            // derived pile landing within 1cm of an UNRELATED level-native could destroy the wrong actor. The
            // bracket gate (the same one the adopt-bind path uses) closes both windows.
            if (!isClump && g_claimTrackingActive) {
                // Twin-destroy (coop/props/pile_spawn_bind). v86 Path 1c: match the client's save-loaded NATIVE
                // against the pile's SAVE-TIME position (payload.matchX/Y/Z, stamped by the host's join
                // snapshot from g_blobPileXforms) -- the frozen value BOTH peers loaded from the same save.
                // A pile the host MOVED in the join-load window spawns its proxy @new (`loc`) but its native
                // loaded @old; matching on the save-time key reconciles them (pre-1c the match was `loc`,
                // which goes >1cm-blind on a moved pile = the two-channel DUP). No key stamped (a mid-game
                // spawn / unseeded-at-save pile leaves hasMatchPos=0) -> fall back to `loc`. The gate (a
                // native level-pile twin exists ONLY during the join bracket) stays at this call-site.
                const ue_wrap::FVector twinMatchPos =
                    payload.hasMatchPos ? ue_wrap::FVector{payload.matchX, payload.matchY, payload.matchZ}
                                        : loc;
                coop::pile_spawn_bind::TryDestroyTwin(payload, twinMatchPos, payload.hasMatchPos != 0,
                                                     g_claimedActors);
            }
        } else {
            UE_LOGW("remote_prop::OnSpawn: trash proxy spawn FAILED for eid=%u class='%ls'",
                    payload.elementId, classW.c_str());
        }
        return;
    }
    // Phase 5S0 de-dupe: if a local Aprop_C derivative with the same Key
    // already exists (e.g. both peers loaded the same save and both already
    // have this prop, OR a prior PropSpawn already created it), don't
    // duplicate. Instead force-converge: update the local actor's transform
    // to match the host's authoritative payload. This is the path that
    // corrects mushroom-desync (each peer's spawner placed the same-Key
    // mushroom at a different position; snapshot bootstrap teleports
    // client's to match host's). 2026-05-24.
    bool dedupeFellBack = false;
    void* existing = nullptr;
    if (fromConvert) {
        // v81 MORPH V2: a convert re-skins eid E in place (oldEid==newEid==E). The still-live OLD
        // rendering of E (the pile-A being morphed, or the clump being re-piled) is ALREADY at
        // Registry::Get(E) -- letting the eid-dedup below resolve it as `existing` would converge the
        // morph onto the actor we are replacing instead of fresh-spawning the new one. Skip the dedup
        // entirely for a convert: OnConvert captures the old rendering itself, then destroys it after
        // the new one is spawned + rebound. (Pre-v81, fromConvert minted a FRESH newEid so this never
        // collided; the bind model reuses E, so the skip is now load-bearing.)
    } else if (eidOnly) {
        // Non-keyable clump: dedup by EID (the Registry mirror binding), not key.
        if (auto* e = coop::element::Registry::Get().Get(payload.elementId)) {
            void* a = e->GetActor();
            if (a && R::IsLive(a)) existing = a;
        }
    } else {
        existing = coop::prop_element_tracker::ResolveLiveActorByKey(keyW, &dedupeFellBack);
        if (dedupeFellBack) {
            // The key index was STALE for this prop (a world-change purged the indexed
            // actors and the slow steady-state re-seed hasn't caught up) -- this
            // de-dupe paid an O(N) GUObjectArray scan. During the connect snapshot
            // ~2300 PropSpawns arrive in a burst; without intervention EACH would scan
            // -> O(N_props x N_objects) -> the client RSS balloons multi-GB. Self-heal:
            // drain dead + re-seed the index NOW (throttled to once / 200 ms) so the
            // rest of this same-tick burst resolves O(1). [[project-bug-prop-resnapshot-leak]]
            coop::prop_element_tracker::ReconcileIndexThrottled();
        }
    }
    if (existing) {
        // P2 claim: the host snapshot accounts for this pre-existing client
        // actor (exact-key / eid match) -- protect it from the unclaimed-
        // divergent sweep at SnapshotComplete. Covers the drive-skip early
        // return below too.
        RecordClaim(existing);
        // Fork B 2d (2026-06-10): the LOCAL player's grab is authoritative
        // for props THEY hold (symmetric with the remote drive-skip below).
        // At a re-bracket the host re-expresses a prop this client is
        // holding; reconciling it kinematic + teleport-converging would
        // break the live PhysicsHandle hold. Claimed + mirror-bound, no
        // physics/transform touch.
        if (ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, existing)) {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' is held by the LOCAL player -- skipping reconcile/converge (local grab owns it)",
                    keyW.c_str());
            coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
            return;
        }
        // Skip the convergence write if THIS prop
        // is currently under active kinematic drive by the PropPose stream
        // (host is holding it). Otherwise the SetActorLocation here would
        // stomp the active drive for one frame, producing a visible
        // teleport-pop until the next PropPose Tick corrects it. The
        // PropPose stream is authoritative for held props; let it own
        // position while held.
        if (coop::remote_prop::IsActorUnderAnyDrive(existing)) {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' is under active kinematic drive (some slot) -- skipping convergence (PropPose owns position)",
                    keyW.c_str());
            // A2 (2026-05-29): still register the mirror so subsequent
            // PropDestroy from sender resolves via eid (we just declined
            // to teleport-pop, but the wire identity binding is still
            // useful).
            coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
            return;
        }
        // GRACEFUL SNAPSHOT LOAD (perf root-cause, 2026-06-09). The exact-key
        // match means `existing` IS the SAME logical base-world prop the host
        // is describing -- the client spawned it from its own deterministic New
        // Game world, at the SAME position (base scenery is identical across
        // peers; that is why de-dupe works at all). Blindly teleporting it to
        // the host transform via K2_SetActorLocation/Rotation (bTeleport=true)
        // WAKES its rigid body -- which is why we forced it kinematic just above.
        // On a heavy save (s_1234: 392 simulating
        // trashBitsPile_C @ physFlags 0x05 + their sub-bits) the connect
        // snapshot woke hundreds of already-resting bodies at once; they never
        // re-sleep -> a PERMANENT ~45 ms/frame engine physics cost (client locks
        // at ~20 FPS while the host -- which loads the same props asleep from
        // disk -- holds 116 FPS). perf_probe proved our game-thread code is only
        // ~5 ms/frame, so the cost is the woken physics scene, not our code.
        //
        // The epsilon-gate converges ONLY when the prop actually MOVED (host
        // relocated/saved it elsewhere; client's New Game copy is at the
        // original spot). When it is already where the host says -- the case for
        // essentially all undisturbed base scenery -- skip the write entirely:
        // no teleport, no wake, no spike. Epsilon 2 cm / 1 deg: deterministic
        // base spawns are sub-mm identical; a player-relocated prop is far
        // outside this, so genuine divergence still converges (and that handful
        // re-settles transiently, which is fine).
        const ue_wrap::FVector  curLoc = ue_wrap::engine::GetActorLocation(existing);
        const ue_wrap::FRotator curRot = ue_wrap::engine::GetActorRotation(existing);
        const float dx = curLoc.X - payload.locX;
        const float dy = curLoc.Y - payload.locY;
        const float dz = curLoc.Z - payload.locZ;
        constexpr float kAlignedDistCm = 2.0f;   // squared-compared below
        constexpr float kAlignedAngDeg = 1.0f;
        const bool locAligned = (dx * dx + dy * dy + dz * dz)
                              <= (kAlignedDistCm * kAlignedDistCm);
        auto angAligned = [](float a, float b) {
            float d = std::fabs(std::fmod(std::fabs(a - b), 360.0f));
            if (d > 180.0f) d = 360.0f - d;
            return d <= kAlignedAngDeg;
        };
        const bool rotAligned = angAligned(curRot.Pitch, payload.rotPitch)
                             && angAligned(curRot.Yaw,   payload.rotYaw)
                             && angAligned(curRot.Roll,  payload.rotRoll);
        if (locAligned && rotAligned) {
            // ALIGNED (the save-transfer common case): NO transform write and NO
            // physics touch -- the client's save-loaded copy already carries the
            // exact SP physics state (simulate-enabled + asleep for normal props),
            // which keeps it grabbable. 2026-06-10 hands-on: the previous
            // unconditional kinematic reconcile here made every bound prop
            // ungrabbable on the client until a host grab+release re-enabled it.
            UE_LOGI("remote_prop::OnSpawn: key '%ls' resolves to live actor %p -- already aligned (d=%.2fcm), skipping teleport (no physics wake)",
                    keyW.c_str(), existing, std::sqrt(dx * dx + dy * dy + dz * dz));
        } else {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' resolves to live actor %p -- diverged (d=%.1fcm), converging transform to host (loc=(%.1f,%.1f,%.1f))",
                    keyW.c_str(), existing, std::sqrt(dx * dx + dy * dy + dz * dz),
                    payload.locX, payload.locY, payload.locZ);
            // SP-PARITY KINEMATIC CONVERGE (perf root-cause 2026-06-09, narrowed
            // 2026-06-10): kinematic BEFORE the teleport so the move neither
            // wakes nor ejects the body, then RESTORE the SP-parity simulate
            // state at the host's rest pose (identical save-transferred world
            // -- a valid pose; the body re-settles instead of storming).
            ReconcileToHostPhysics(existing, payload.physFlags);
            ue_wrap::engine::SetActorLocation(existing,
                ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
            ue_wrap::engine::SetActorRotation(existing,
                ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
            RestoreSpParityPhysicsAfterConverge(existing, payload.physFlags);
        }
        // 2026-05-25 mushroom fall-through fix: client's local copy may have
        // gone through spawnedNaturally() in its own AmushroomSpawner_C::Spawn
        // path, leaving NoCollision on the StaticMesh. Wire convergence
        // reuses the actor in-place (skipping a fresh Init); we must
        // explicitly restore default collision so subsequent host PropPose
        // releases don't drop the body into the void.
        RestoreCollisionIfNeeded(L"exact-key", classW, existing);
        // (kAtRest PutRigidBodyToSleep REVERTED 2026-06-09: teleport-converging a
        // simulating body into the client's RNG-divergent layout penetrates -> PhysX
        // ejects it (objects flying) and PutRigidBodyToSleep mid-de-penetration
        // null-derefs PhysX (fatal crash). The teleport-reconcile-then-sleep approach
        // fights the physics engine; the real fix is host-authoritative KINEMATIC
        // mirrors. See [[project-fps-physics-wake-kAtRest-2026-06-09]].)
        // A2 mirror binding: associate sender's wire eid with the resolved
        // local actor so future PropDestroy / eid-routed lookups land
        // correctly.
        coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
        return;
    }
    // KEYLESS-PILE POSITION-BIND (v56 follow-up, 2026-06-10; see the index
    // block above). Gates:
    //   - bracket open: only SNAPSHOT expressions bind -- a mid-gameplay
    //     keyless spawn has no local twin by construction;
    //   - !fromConvert: OnConvert synthesizes this same payload for a
    //     convert-born pile, which also has NO local counterpart -- binding
    //     it to a nearby unrelated pile in a dense cluster would mis-mirror;
    //   - candidates are chipPile-lineage ONLY (index pre-filter), and the
    //     match requires the EXACT wire class + equal chipType + nearest
    //     within 30 cm -- so a held-clump expression (different class) can
    //     never bind, and save-aligned true matches (sub-mm) always win.
    // No match -> fall through to the fresh-spawn mirror below (a pile the
    // host created AFTER its save has no local twin -- it must spawn; a
    // local pile the host no longer has stays unclaimed -> swept).
    if (eidOnly && g_claimTrackingActive && !fromConvert && payload.elementId != 0) {
        // 30cm adopt find+consume lives in coop/props/pile_spawn_bind. Returns the matched
        // native pile (already removed from the index) + the matched squared distance + the shared
        // per-bracket bind-log seq; the host-eid mirror registration + physics reconcile below STAY here
        // (remote_prop internals -- RegisterPropMirror / ReconcileToHostPhysics / RestoreCollisionIfNeeded).
        float bestD2 = 0.f;
        int   nBind  = 0;
        void* pile = coop::pile_spawn_bind::FindAndConsumeAdoptCandidate(
            payload, classW, g_claimedActors, &bestD2, &nBind);
        if (pile) {
            RecordClaim(pile);
            // RETIRE the client-local identity (audit CRITICAL-1, 2026-06-10).
            // A save-loaded pile was seed-walked into the tracker with a
            // CLIENT-MINTED eid; from this bind on, its sole cross-peer
            // identity is the HOST eid (the mirror registration below). If the
            // local Element stayed, a local grab morph-destroy would make the
            // K2_DestroyActor PRE observer broadcast a STRAY PropDestroy with
            // the superseded client eid alongside the death-watch's correct
            // host-eid destroy. Unmark drains the reverse map + frees the
            // Element shadow; GetPropElementIdForActor then reads kInvalidId
            // and the PRE observer stays silent (keyless + no eid) -- the
            // fresh-mirror invariant: the WATCH is the sole destroy speaker.
            coop::prop_element_tracker::UnmarkKnownKeyedProp(pile);
            // Same reconcile shape as the exact-key path: kinematic to host
            // physics, converge only on real divergence (save-aligned piles
            // are sub-mm identical -- the common case writes nothing, wakes
            // nothing).
            ReconcileToHostPhysics(pile, payload.physFlags);
            const ue_wrap::FVector curLoc = ue_wrap::engine::GetActorLocation(pile);
            const float ddx = curLoc.X - payload.locX;
            const float ddy = curLoc.Y - payload.locY;
            const float ddz = curLoc.Z - payload.locZ;
            constexpr float kAlignedCm = 2.0f;
            const bool aligned = (ddx * ddx + ddy * ddy + ddz * ddz) <= (kAlignedCm * kAlignedCm);
            if (!aligned) {
                ue_wrap::engine::SetActorLocation(pile,
                    ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
                ue_wrap::engine::SetActorRotation(pile,
                    ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
            }
            RestoreCollisionIfNeeded(L"pile-bind", classW, pile);
            coop::remote_prop::RegisterPropMirror(payload.elementId, pile, keyW, classW, senderSlot);
            coop::prop_element_tracker::IndexActorKey(pile, keyW);  // None-guarded no-op (parity with the fresh path)
            // (No pile death-watch enroll here since 2026-06-17. RegisterPropMirror above bound this
            // local pile to payload.elementId, so a later local grab resolves the eid via
            // ResolveMirrorEidByActor in the InpActEvt_use PRE observer -> PropDestroy tells the host.)
            if (nBind <= 3 || (nBind % 200) == 0) {
                UE_LOGI("remote_prop_spawn: pile-bind #%d -- eid=%u '%ls' chipType=%u -> OWN local pile %p (d=%.1fcm%s)",
                        nBind, payload.elementId, classW.c_str(),
                        static_cast<unsigned>(payload.chipType), pile,
                        std::sqrt(bestD2), aligned ? ", aligned" : ", converged");
            }
            return;
        }
    }
    // Phase 5S0 Gap I-1 (2026-05-24): exact-Key match failed. Try fuzzy
    // de-dupe for divergent-key same-position spawns. Per-peer-divergent
    // natural spawners (AmushroomMaster_C, AmushroomSpawner_C,
    // AundergroundGarbageSpawner_C) place same logical entities with
    // DIFFERENT Keys at slightly-different positions on each peer; without
    // this, the host's broadcast would spawn a NEW prop adjacent to the
    // client's pre-existing one. Conservative 30 cm radius + same-class
    // gate. Mismatches in dense-prop areas surface in the log.
    constexpr float kFuzzyRadiusCm = 30.f;
    // Non-keyable clump: skip the key/position fuzzy dedup (it rekeys the match via
    // setKey, meaningless for the clump) -- go straight to a fresh eid-bound spawn.
    void* fuzzy = eidOnly ? nullptr : ue_wrap::prop::FindNearbySameClass(
            classW,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ},
            kFuzzyRadiusCm,
            propNameW == L"None" ? std::wstring() : propNameW);
    // ANTI-COLLISION GATE (kerfur-only, 2026-06-24 fuzzy-gate fix#1; see kerfur_prop_adoption.cpp + the
    // 14:05 RCA). For a kerfur PROP the Aprop_Key is cross-peer-stable (save-persisted), so a 30cm fuzzy
    // match whose key DIFFERS from the wire key is a DIFFERENT kerfur -- never rekey/steal it (the same
    // collision class as the deferred 500cm path). Drop the match -> fall through to fresh-spawn. Strictly
    // kerfur-gated: NON-kerfur props (mushroom/garbage spawners with per-peer-divergent keys) KEEP the
    // intended Gap-I-1 divergent-key dedup unchanged (RULE 1 -- no regression to that path).
    if (fuzzy && classW.find(L"prop_kerfurOmega") != std::wstring::npos) {
        const std::wstring fuzzyKey = ue_wrap::prop::GetKeyString(fuzzy);
        if (!fuzzyKey.empty() && fuzzyKey != L"None" && fuzzyKey != keyW) {
            UE_LOGI("remote_prop::OnSpawn: kerfur fuzzy match (wire key '%ls') resolves to a neighbor with a "
                    "DIFFERENT key '%ls' -- anti-collision: not stealing it, fresh-spawning instead",
                    keyW.c_str(), fuzzyKey.c_str());
            fuzzy = nullptr;
        }
    }
    if (fuzzy) {
        // P2 claim: same as the exact-key path -- the fuzzy match binds this
        // pre-existing client actor to the host's prop; protect it from the
        // sweep. Covers the drive-skip early return below too.
        RecordClaim(fuzzy);
        // Fork B 2d: local-held guard, same as the exact-key path. (Reached
        // when the held prop was rekeyed by an earlier bracket and the new
        // bracket fuzzy-matches it.) Rekey is skipped too -- the next
        // bracket re-fuzzy-matches; correctness rides the eid mirror bind.
        if (ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, fuzzy)) {
            UE_LOGI("remote_prop::OnSpawn: fuzzy match '%ls' is held by the LOCAL player -- skipping reconcile/converge/rekey (local grab owns it)",
                    classW.c_str());
            coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
            return;
        }
        // Mirror of the exact-Key guard: if the fuzzy match resolves to the
        // actor currently under active kinematic drive, skip convergence.
        // Otherwise the SetActorLocation here
        // would stomp the PropPose stream for one frame -- same
        // teleport-pop bug as the exact-Key path.
        if (coop::remote_prop::IsActorUnderAnyDrive(fuzzy)) {
            UE_LOGI("remote_prop::OnSpawn: Gap-I-1 fuzzy match '%ls' -> active drive actor (some slot) -- skipping (PropPose owns position)",
                    classW.c_str());
            // A2 audit fix (2026-05-29): mirror binding for the drive-skip
            // fuzzy path -- symmetric with the exact-key drive-skip path
            // above. Without this, Registry::Get(eid) on this peer would
            // never resolve for a fuzzy+drive-skipped prop.
            coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
            return;
        }
        UE_LOGI("remote_prop::OnSpawn: Gap-I-1 FUZZY MATCH '%ls' (wire key '%ls') -> existing actor %p within %.1f cm -- de-duping, converging transform + rekeying",
                classW.c_str(), keyW.c_str(), fuzzy, kFuzzyRadiusCm);
        // SP-parity kinematic converge + restore (same shape as the exact-key
        // diverged branch; the 2026-06-10 grabbability fix applies here too).
        ReconcileToHostPhysics(fuzzy, payload.physFlags);
        ue_wrap::engine::SetActorLocation(fuzzy,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(fuzzy,
            ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        RestoreSpParityPhysicsAfterConverge(fuzzy, payload.physFlags);
        // REKEY the fuzzy-matched actor to the
        // host's wire Key. Without this, the actor keeps its client-local
        // NewGuid Key (K_c) while host PropPose packets carry K_h --
        // FindByKeyString(K_h) on client misses, host's grab/move stream
        // would be silently dropped (UE_LOGW "no local match"). The
        // result would be a prop that successfully de-duped (no visible
        // duplicate) but is permanently invisible to the host's
        // authoritative position stream -- a worse regression than the
        // duplicate it prevented. Aprop_C.setKey updates
        // mainGamemode.keyObj_key/obj cross-peer; the old K_c orphan
        // entry is harmless (no host packet references it).
        if (ResolveSpawnFns() && g_propSetKeyFn) {
            const R::FName keyFName = ue_wrap::fname_utils::StringToFName(keyW);
            if (keyFName.ComparisonIndex != 0) {
                ParamFrame sk(g_propSetKeyFn);
                if (sk.SetRaw(L"Key", &keyFName, sizeof(keyFName))) {
                    if (Call(fuzzy, sk)) {
                        UE_LOGI("remote_prop::OnSpawn: Gap-I-1 rekey ok -- actor %p now has wire key '%ls' (FName idx=%u); subsequent PropPose resolves correctly",
                                fuzzy, keyW.c_str(), keyFName.ComparisonIndex);
                    } else {
                        UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey setKey ProcessEvent call failed -- prop unreachable via host PropPose");
                    }
                } else {
                    UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey setKey 'Key' param not found");
                }
            } else {
                UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey StringToFName('%ls') -> NAME_None; skipped",
                        keyW.c_str());
            }
        } else {
            UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey skipped -- setKey UFunction unresolved");
        }
        // Index the fuzzy-matched actor under the WIRE key, unconditionally --
        // resolution binds via our key->actor index, independent of whether the
        // engine-level setKey write above succeeded. The fuzzy match (same class
        // within 30 cm) already established this actor IS the host's prop, so the
        // host's subsequent PropPose/PropDestroy under the wire key must resolve
        // here O(1). Doing this only on setKey success (the prior placement) left
        // a rare setKey failure falling back to a per-frame GUObjectArray scan for
        // every packet on that held prop. keyW is validated non-empty/non-None at
        // OnSpawn entry; fuzzy is live.
        coop::prop_element_tracker::IndexActorKey(fuzzy, keyW);
        // 2026-05-25 mushroom fall-through fix (Gap-I-1 path): same reason as
        // exact-key. The fuzzy-matched local actor came from client's own
        // AmushroomSpawner_C::Spawn -> spawnedNaturally() -> NoCollision.
        // Without this restore, host's release-edge PropPose-driven mushroom
        // sits on top of the floor briefly then PhysX drops it through
        // because the body has NoCollision.
        RestoreCollisionIfNeeded(L"fuzzy", classW, fuzzy);
        // (kAtRest sleep REVERTED 2026-06-09 -- see exact-key branch above.)
        // A2 (2026-05-29) mirror binding: the fuzzy-matched actor was just
        // rekey'd to the wire Key above, so future PropPose / PropDestroy
        // from sender resolves via key OR eid lookup.
        coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
        return;
    }
    // K-6 (prop-form kerfur join fix): we reached here = no exact-key match and no inline fuzzy match.
    // For a kerfur PROP, the client's own save-loaded twin may still be ASYNC-LOADING (the snapshot
    // PropSpawn arrives before the world's late tail materializes it), so fresh-spawning NOW would
    // create a DUPLICATE beside the still-coming twin (the prop-form join-dup root). DEFER to the
    // polled class+pose adoption (mirrors npc_adoption) -- it binds the twin when it appears, or
    // fresh-spawns once at quiescence. deferKerfur is false on that fallback + on the convert
    // materialize, so neither re-defers here. Keyed by class name (cheap; matches prop_kerfurOmega + skins).
    if (deferKerfur && classW.find(L"prop_kerfurOmega") != std::wstring::npos) {
        coop::kerfur_prop_adoption::Arm(payload);
        return;
    }
    if (!ResolveSpawnFns()) {
        UE_LOGW("remote_prop::OnSpawn: spawn UFunctions unresolved -- dropping");
        return;
    }
    void* actorClass = R::FindClass(classW.c_str());
    if (!actorClass) {
        UE_LOGW("remote_prop::OnSpawn: class '%ls' not found in GUObjectArray -- dropping (likely cooked-content class not loaded)",
                classW.c_str());
        return;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("remote_prop::OnSpawn: no world context -- dropping");
        return;
    }
    // Build FTransform from wire rotation/scale/location. ue_wrap::FTransform
    // (types.h) is the canonical 48-byte layout matching engine FTransform;
    // RULE 2: no parallel local FTransform48 type.
    ue_wrap::FTransform xform{};  // ctor defaults: identity rot, zero loc, unit scale
    E::RotatorToQuat(payload.rotPitch, payload.rotYaw, payload.rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = payload.locX;
    xform.TY = payload.locY;
    xform.TZ = payload.locZ;
    xform.SX = payload.scaleX;
    xform.SY = payload.scaleY;
    xform.SZ = payload.scaleZ;
    // Phase 1: BeginDeferredActorSpawnFromClass -> uninitialized AActor*.
    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(g_beginSpawnFn);
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(g_gsCdo, begin)) {
            UE_LOGE("remote_prop::OnSpawn: BeginDeferredActorSpawnFromClass call failed");
            return;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("remote_prop::OnSpawn: BeginDeferred returned null");
        return;
    }
    // P2 claim -- BEFORE any later failure return (audit CRITICAL 2026-06-10):
    // a fresh wire spawn of an RNG-divergent class is itself a live actor of a
    // sweep-target class. Claiming here (not after FinishSpawningActor) means
    // a FinishSpawningActor failure leaks one claimed half-spawned actor --
    // the exact pre-P2 behavior of that failure path -- instead of leaving an
    // unclaimed half-CONSTRUCTED actor for the sweep to K2_DestroyActor
    // mid-construction (no BeginPlay, unregistered PhysX body -> crash risk).
    RecordClaim(spawned);
    // Phase 2 (CRITICAL): set the Key on the spawned actor BEFORE
    // FinishSpawningActor. Aprop_C.Init() runs inside FinishSpawningActor's
    // UserConstructionScript and, when ResetKey=true or no Key is set, calls
    // KismetGuidLibrary::NewGuid -> FName -> self->Key. Writing Key FIRST
    // (via the BP-callable setKey UFunction) skips that overwrite branch and
    // the prop ends up with our wire Key + registered in
    // mainGamemode.keyObj_key/obj cross-peer.
    // Audit Fix 4 (2026-05-27): resolve setKey ON THE ACTUAL SPAWNED CLASS,
    // not the cached Aprop_C.setKey UFunction*. ChipPile/clump/trashBits
    // have their OWN setKey UFunctions on different UClasses with possibly
    // different parameter layouts (per UE4 ProcessEvent, dispatching a
    // foreign-class UFunction* on an actor of a different class can
    // silently corrupt memory or crash). Per-class FindFunction is one
    // GUObjectArray walk per CLASS (not per actor); the result is cached
    // here ad-hoc since we only have ~10 keyed-interactable leaf classes.
    void* setKeyFn = R::FindFunction(actorClass, P::name::PropSetKeyFn);
    if (!setKeyFn) {
        UE_LOGW("remote_prop::OnSpawn: setKey UFunction not found on class '%ls' -- spawn will use auto-generated Key",
                classW.c_str());
    } else {
        // Convert wire Key string -> live FName via Conv_StringToName, then
        // call <class>.setKey(FName) so the receiver's prop carries the
        // SAME Key string as the sender's. Subsequent PropPose updates with
        // this key resolve via prop_wrap::FindByKeyString (which compares by
        // ToString, NOT by ComparisonIndex -- the cross-peer-stable path).
        const R::FName keyFName = ue_wrap::fname_utils::StringToFName(keyW);
        if (keyFName.ComparisonIndex == 0) {
            UE_LOGW("remote_prop::OnSpawn: StringToFName('%ls') -> NAME_None; setKey skipped",
                    keyW.c_str());
        } else {
            ParamFrame sk(setKeyFn);
            if (!sk.SetRaw(L"Key", &keyFName, sizeof(keyFName))) {
                UE_LOGW("remote_prop::OnSpawn: setKey 'Key' param not found on '%ls'",
                        classW.c_str());
            } else if (!Call(spawned, sk)) {
                UE_LOGW("remote_prop::OnSpawn: setKey ProcessEvent call failed on '%ls'",
                        classW.c_str());
            } else {
                UE_LOGI("remote_prop::OnSpawn: setKey('%ls') ok on '%ls' (FName idx=%u)",
                        keyW.c_str(), classW.c_str(), keyFName.ComparisonIndex);
            }
        }
    }
    // v54 SP-parity identity (white-cube root cause, RE 2026-06-10): write the
    // list_props row `Name` + the Static/removeWOrespawn/frozen/sleep bools on
    // the DEFERRED actor BEFORE FinishSpawningActor -- Finish runs the UCS ->
    // Aprop_C::init() pass which resolves list_props[Name] into the true mesh/
    // mass/collision/SetSimulatePhysics(!(Static||frozen||sleep)). Without the
    // Name, a generic prop_C mirror constructs as the CDO 'cube' row (the
    // host's broken-cubicle wall panels mirrored as white cubes). Same field
    // set + ordering SP's own loadObjects->loadData->init() achieves, minus
    // the cube flash (SP writes AFTER Finish and re-runs init()).
    if (ue_wrap::prop::IsDescendantOfProp(spawned)) {
        R::FName nameRow{0, 0};
        if (!propNameW.empty() && propNameW != L"None") {
            nameRow = ue_wrap::fname_utils::StringToFName(propNameW);
            if (nameRow.ComparisonIndex == 0) {
                UE_LOGW("remote_prop::OnSpawn: StringToFName(propName '%ls') -> NAME_None; "
                        "mirror keeps its class-default Name (may render as the CDO mesh)",
                        propNameW.c_str());
            }
        }
        namespace pf = coop::net::propspawn_flags;
        ue_wrap::prop::WriteSpParityIdentity(
            spawned, nameRow,
            (payload.physFlags & pf::kStatic) != 0,
            (payload.physFlags & pf::kRemoveWOrespawn) != 0,
            (payload.physFlags & pf::kFrozen) != 0,
            (payload.physFlags & pf::kSleep) != 0);
    }
    // v5 Inc2 echo suppression: mark this actor as wire-induced BEFORE
    // FinishSpawningActor (which runs Aprop_C::Init via UserConstructionScript,
    // tripping our Init POST observer in harness.cpp). The observer's call
    // to ConsumeIncomingSpawn will then return true and skip the broadcast
    // back to the sender -> no echo loop.
    coop::prop_echo_suppress::MarkIncomingSpawn(spawned);
    // Phase 3: FinishSpawningActor -> runs UserConstructionScript + BeginPlay.
    {
        ParamFrame finish(g_finishSpawnFn);
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(g_gsCdo, finish)) {
            UE_LOGE("remote_prop::OnSpawn: FinishSpawningActor call failed");
            return;
        }
    }
    UE_LOGI("remote_prop::OnSpawn: spawned %p of '%ls' at (%.1f, %.1f, %.1f)",
            spawned, classW.c_str(), payload.locX, payload.locY, payload.locZ);
    // Stamp the trash VARIANT + keep the mirror clump from self-converting.
    //
    // The former position-based "consume the co-located source pile" here was REMOVED
    // 2026-06-08 (RULE 2): chipPiles are NOT co-located cross-peer -- AundergroundGarbage
    // Spawner places them with the global UNSEEDED RNG, and the client boots a blank save
    // -- so FindNearestChipPile here matched the WRONG pile (or none), which was the
    // dominant clump-DUPE source (it destroyed an unrelated pile while the landed pile
    // spawned anyway -> net multiplication). v52: the ball->pile convert is now ONE atomic
    // PropConvert (destroy ball by oldEid + spawn pile by newEid), and a re-grabbed pile
    // drops its cross-peer mirror by IDENTITY via the trash_collect_sync mirror-pile
    // death-watch -> PropDestroy(eid). The wire chipType (read off the held clump at grab
    // time) is authoritative for the variant.
    // research/findings/votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md.
    const uint8_t variant = payload.chipType;
    if (classW.find(L"garbageClump") != std::wstring::npos) {
        // Silence THIS mirror clump's own ground-hit -> turn-to-pile handler.
        // FinishSpawningActor (above) auto-binds the StaticMesh OnComponentHit delegate
        // (prop_garbageClump ubergraph 2702 -> BeginDeferredActorSpawnFromClass(pile)). On
        // release we re-enable collision+physics+throw velocity so the mirror flies + lands
        // HERE; without this guard that handler fires -> a SECOND pile atop the owner's
        // authoritative one. Disabling hit-notify lets the mirror land visually but never
        // self-convert; the owner's death-watch (trash_collect_sync) stays the sole pile
        // source. (canConvert=false@0x024C does NOT work -- the hit handler re-sets it true.)
        ue_wrap::engine::SetActorRootNotifyRigidBodyCollision(spawned, false);
    }
    // Stamp the variant after FinishSpawningActor (actor fully constructed). No-op for
    // non-trash classes (SetChipType reflection-gates on a chipType property, so it never
    // touches an Aprop_C's StaticMesh ptr at the same 0x0238 offset). Repaints via setTex().
    if (variant != 0) {
        ue_wrap::prop::SetChipType(spawned, variant);
        UE_LOGI("remote_prop::OnSpawn: applied chipType=%u variant to '%ls'",
                static_cast<unsigned>(variant), classW.c_str());
    }
    // (v52 RULE 1+2: the former kFreshLanded -> turnToPile(landingVel) call here was a
    // CATASTROPHIC BUG, removed. The comment claimed turnToPile "operates on `this`, spawns
    // nothing" -- the disassembly proves the OPPOSITE: actorChipPile_C::turnToPile is the
    // pile->clump GRAB morph -- it BeginDeferred-spawns a clump (Max:=2.0), throws it with the
    // velocity, and K2_DestroyActor's SELF. So calling it on a freshly-spawned landed pile
    // DESTROYED that pile (-> ResolveLiveActorByEid failed -> the mirror was unwatched + an
    // incoming PropDestroy found "no local actor") AND spawned a stray, untracked, self-
    // converting clump -> the persistent clump DUPE. A landed pile needs no morph: it just
    // spawns and sits. The impact dust+sound is a deferred polish (needs the correct verb, NOT
    // the grab morph). kFreshLanded + TurnChipPileToPile retired with it.)
    // Phase 4: physics state. The mesh is Aprop_C.StaticMesh.
    void* mesh = ue_wrap::prop::GetStaticMesh(spawned);
    if (mesh) {
        // SP-parity (2026-06-10 grabbability fix): a settled normal prop is
        // simulate-ENABLED + asleep in SP -- forcing the mirror kinematic made
        // it ungrabbable on this peer. kSimulatePhysics (host body live-awake)
        // implies the formula's true case; static/frozen/sleep stay disabled.
        const bool sim = SpParitySimulate(payload.physFlags);
        coop::remote_prop::DriveSimulate(mesh, sim);
        const bool hasLinVel =
            payload.initLinVelX != 0.f || payload.initLinVelY != 0.f || payload.initLinVelZ != 0.f;
        const bool hasAngVel =
            payload.initAngVelX != 0.f || payload.initAngVelY != 0.f || payload.initAngVelZ != 0.f;
        if (hasLinVel) coop::remote_prop::DriveSetLinearVelocity(mesh, payload.initLinVelX, payload.initLinVelY, payload.initLinVelZ);
        if (hasAngVel) coop::remote_prop::DriveSetAngularVelocity(mesh, payload.initAngVelX, payload.initAngVelY, payload.initAngVelZ);
        UE_LOGI("remote_prop::OnSpawn: physics applied (sim=%d hasLinVel=%d hasAngVel=%d)",
                sim ? 1 : 0, hasLinVel ? 1 : 0, hasAngVel ? 1 : 0);
    }
    // 2026-05-25 mushroom fall-through fix (fresh-spawn path): defensive --
    // Aprop_food_mushroom_C::Init runs as part of FinishSpawningActor and
    // CONDITIONALLY writes collision (per Init body: reads, compares to a
    // default, conditionally writes; exact condition not yet RE'd). On a
    // pure wire-spawn (no save reference, no spawnedNaturally trigger) it
    // likely lands correct, but a write here is cheap and idempotent. Keep
    // the restore call symmetric across all 3 OnSpawn convergence paths so
    // a future Init-body change can't silently regress only one path.
    RestoreCollisionIfNeeded(L"fresh-spawn", classW, spawned);
    // (kAtRest sleep REVERTED 2026-06-09 -- see exact-key branch.)
    // v81 MORPH V2: a convert re-skins eid E in place and the rebind path is local-vs-mirror
    // specific (RebindLocalElementActor vs RegisterPropMirror rebindInPlace), so OnConvert binds
    // explicitly -- hand it the spawned actor and skip the default mirror bind here.
    if (outSpawned) *outSpawned = spawned;
    if (skipBind) return;
    // A2 (2026-05-29) mirror binding: bind sender's wire eid to the freshly
    // spawned local actor. Subsequent Registry::Get(eid) on this peer
    // resolves to this actor; PropDestroy with the same eid drains the
    // mirror + destroys the actor.
    coop::remote_prop::RegisterPropMirror(payload.elementId, spawned, keyW, classW, senderSlot);
    // Index the mirror's key so a PropPose drive can resolve it O(1). Essential
    // for NON-Aprop_C mirrors (garbageClump/chipPile): the cold FindByKeyString
    // fallback in ResolveLiveActorByKey walks IsDescendantOfProp (Aprop_C only),
    // so a clump mirror that isn't indexed here would never resolve -> the
    // kinematic drive could never start and the clump would appear but not
    // follow the collector's hand. Harmless + faster for Aprop_C mirrors too.
    coop::prop_element_tracker::IndexActorKey(spawned, keyW);
    // instant-world: hide the freshly-spawned + registered prop mirror until reveal (AFTER the bind so it is
    // always enumerable). Real prop -> collisionOff=true (hide alone leaves collision on; a hidden mirror
    // must not be grab-trace-hittable / physics-active). hasMatchPos => a save-time-keyed form whose local
    // twin is still visible -> HOLD to quiescence; else (host-only/derived form, no twin) reveal at the lift.
    coop::mirror_defer::OnMirrorSpawned(payload.elementId, spawned, /*collisionOff=*/true,
                                        /*holdUntilQuiescence=*/payload.hasMatchPos != 0);
    // (The chipPile MIRROR used to be enrolled in the mirror-pile death-watch here. RETIRED
    // 2026-06-17, RULE 1+2: the death-watch's near-camera "grabbed" inference was unsound -- a peer
    // bumping a pile until it died near the camera read as a grab and wiped the pile on both peers
    // ("piles destroyed when touched a lot"). A peer's later grab of THIS mirror is now caught by the
    // InpActEvt_use PRE observer (trash_collect_sync) reading lookAtActor=pile -> ResolveMirrorEidByActor
    // (RegisterPropMirror above bound it to payload.elementId) -> PropDestroy(eid). Covers the
    // pre-existing / snapshot pile case too, since the resolve works for any registered mirror. Fires
    // only on a real E-press, never a bump/stream-out/physics-death.)
}

void BeginClaimTracking() {
    g_claimedActors.clear();
    g_claimTrackingActive = true;
    // A fresh bracket supersedes any sweep still pending from a prior bracket
    // (the two-level-load case): cancel it so it can't fire against this new
    // claim set; SnapshotComplete re-arms it. Clear the sticky fired latch too
    // (the new world's load tail has not quiesced yet).
    g_sweepPending = false;
    g_sweepFired = false;
    g_sweepStableScans = 0;
    g_sweepLastUnsettledCount = -1;
    // A fresh bracket re-enumerates pile-bind candidates (a re-bracket runs
    // against the post-sweep world; stale entries would be dead pointers).
    coop::pile_spawn_bind::Reset();  // index only -- the deferred reconcile queues (quiescence_drain) survive the bracket
    // Phase 0: drop any completeness census from a prior bracket; SnapshotComplete delivers this
    // bracket's. Until then HostCountForClass returns -1 (the floor is a no-op -> >50% valve only).
    coop::snapshot_census::Reset();
    // Phase 1 step 1A probe: arm the read-only keyless load-spawn coverage recorder for this join.
    coop::dev::spawn_order_probe::ArmForJoin();
    UE_LOGI("remote_prop_spawn: claim tracking ARMED (snapshot bracket open) -- "
            "unclaimed in-universe locals will be destroyed at SnapshotComplete");
}

// The one real sweep. Driven (deferred) by TickClientReconcile once the load
// tail has quiesced -- NOT inline at SnapshotComplete (see ArmDivergenceSweep).
// Internal linkage: the only callers are ArmDivergenceSweep's tick driver.
static void RunDivergenceSweep_(void* localPlayer) {
    if (!g_claimTrackingActive) {
        // A SnapshotComplete without its Begin (wire anomaly / disconnect race)
        // must NOT sweep with an empty claim set -- that would destroy every
        // in-universe actor including legitimately claimed ones.
        UE_LOGW("remote_prop_spawn: claim sweep requested but tracking is not armed -- skipping");
        return;
    }
    // Fork B 2e (2026-06-10): SWEEP(client) == EXPRESS(host) + CLIENT-
    // FORBIDDEN. The sweep may destroy exactly what the host snapshot can
    // express a binding for -- keyed IsClassKeyedInteractable actors + the
    // keyless chipPile lineage (eid-expressed since HALF 1) -- plus the
    // wire-suppressed intermediate the client already destroys-on-sight
    // while connected (mushroom7: keyed, in-universe, never expressed ->
    // swept; parity with the connected-state destroy + wire-ingress drop).
    // Everything keyless that is NOT chipPile-lineage (a held clump
    // mid-flight, a pre-Init Aprop_C, event clumps whose setKey never
    // sticks) is OUT of universe and never swept. The pre-2e 4-class
    // RNG-divergent scope could neither destroy the orphan props a host
    // re-bracket no longer expresses (intact cubicles, moved-wall doubles)
    // nor protect a flying keyless clump mirror -- both fixed by the
    // universe test. (IsRngDivergentClass + ResolveRngDivergentBases
    // retired with it, RULE 2.)
    //
    // (The "watched-pile interaction" caveat once noted here is GONE 2026-06-17 with the pile
    // death-watch retirement -- there are no watched piles for the sweep to spare anymore; a pile
    // re-grab is handled by the InpActEvt_use observer, independent of this sweep + its claim set.)
    //
    // R3 (2026-06-18, MTA CElementGroup membership). The doom candidates are the
    // client's OWN LOCAL Prop Elements -- its save-loaded world -- enumerated from the
    // tracked Prop registry, NOT a GUObjectArray scan of every keyed-interactable in
    // existence. This is MTA's deletion-by-tracked-membership (Server/.../
    // CElementGroup.cpp:28 iterates the explicit member list, never the world): we only
    // ever adjudicate what THIS client loaded. Two old guards collapse for free:
    //   - host-driven wire MIRRORS (pr.mirror) are excluded at the SOURCE. A kerfur prop
    //     mirror -- or any host-expressed prop -- is never a save-load divergence, so the
    //     old per-actor kerfur-mirror exemption is now automatic (RULE 2: it goes).
    //   - keyless NON-pile transients (a held clump mid-flight, a pre-Init Aprop) are
    //     never MarkPropElement'd -> never LOCAL Prop Elements -> never enumerated here,
    //     so the old scan's keyless-skip is structurally unreachable (the defensive skip
    //     below stays only as a ~0 tripwire).
    // The >50%% valve STAYS (the agent's "membership collapses it" was wrong -- fewer
    // host claims means MORE unclaimed, not fewer): membership bounds the set to "what
    // you loaded", but an INCOMPLETE host bracket still leaves most of it unclaimed ->
    // dooming most of the loaded world. That is an incomplete snapshot, not a divergence
    // -- the valve aborts it (below). Per-player state actors ARE local Prop Elements, so
    // their exemption stays too. ONE registry snapshot (no per-actor mutex; the eid/idx/
    // mirror flag are captured under the Registry lock), ZERO GUObjectArray walks.
    std::vector<coop::element::Registry::ActorIdPair> propPairs;
    coop::element::Registry::Get().SnapshotActorsByType(
        coop::element::ElementType::Prop, propPairs);
    int inClass = 0;        // live, non-mirror, LOCAL Prop Elements (the membership universe)
    int claimedCount = 0;
    std::vector<void*> doomed;
    doomed.reserve(propPairs.size());
    std::vector<std::wstring> doomedClass;  // Phase 0: class per doomed actor, parallel to `doomed`
    doomedClass.reserve(propPairs.size());
    std::unordered_map<std::wstring, int> doomedByClass;
    std::unordered_map<std::wstring, int> claimedByClass;  // Phase 0: claimed count per class (floor numerator)
    std::unordered_map<std::wstring, int> keylessSkippedByClass;
    for (const auto& pr : propPairs) {
        if (pr.mirror) continue;                                    // host-driven mirror -> not a save divergence (automatic kerfur exemption)
        if (!pr.actor) continue;
        if (!R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;  // dead (no deref) -- the reaper owns it
        ++inClass;
        void* a = pr.actor;
        // ClassNameOf moved AHEAD of the claimed check (Phase 0): the completeness floor needs the
        // claimed count PER CLASS, so the claimed branch must tag its class too.
        const std::wstring acls = R::ClassNameOf(a);
        if (g_claimedActors.count(a)) { ++claimedCount; ++claimedByClass[acls]; continue; }  // host-expressed / self-claimed -> converged, keep
        // PER-PLAYER state actors (inventory container etc.) are this player's own
        // per-save state -- never host-expressed AND never swept. The 2026-06-10 smoke
        // swept the client's inventory container and fataled at the next GC purge. STAYS:
        // a per-player prop IS a local Prop Element, so membership alone would doom it.
        if (coop::prop_lifecycle::IsPerPlayerPropClass(acls)) continue;
        if (ue_wrap::prop::IsChipPile(a)) {            // expressible keyed OR keyless (eid lane)
            doomed.push_back(a);
            doomedClass.push_back(acls);
            ++doomedByClass[acls];
            continue;
        }
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(a);
        if (key.empty() || key == L"None") {
            ++keylessSkippedByClass[acls];  // defensive tripwire: a tracked keyless non-pile should not exist post-quiescence
            continue;
        }
        // v57 audit CRIT-2: a SWEPT trashBitsPile must be unwatched from the
        // counter channel BEFORE it dies -- the sweep runs AFTER join_progress
        // ::Complete() (Idle) in the same drain, so the next Tick's death-watch
        // would see a near-camera vanish with inTransition=false and broadcast
        // a keyed PropDestroy for a pile the HOST legitimately still has.
        if (ue_wrap::prop::IsTrashBitsPile(a)) {
            coop::trash_pile_sync::NotifyWireDestroy(key);
        }
        doomed.push_back(a);
        doomedClass.push_back(acls);
        ++doomedByClass[acls];
    }
    if (!keylessSkippedByClass.empty()) {
        size_t skippedTotal = 0;
        for (const auto& [k, v] : keylessSkippedByClass) skippedTotal += v;
        std::wstring topCls;
        int topCnt = 0;
        for (const auto& [k, v] : keylessSkippedByClass) {
            if (v > topCnt) { topCnt = v; topCls = k; }
        }
        UE_LOGW("remote_prop_spawn: sweep SKIPPED %zu keyless unclaimed actor(s) across %zu class(es) "
                "(top: %d x '%ls') -- expected ~0 post-quiescence; a non-zero keyed-later count would "
                "mean the quiescence gate fired too early (regression tripwire)",
                skippedTotal, keylessSkippedByClass.size(), topCnt, topCls.c_str());
    }
    // PHASE 0 PER-CLASS COMPLETENESS FLOOR (2026-06-25, docs/COOP_STABLE_ID_SIDECAR.md S4 -- the
    // docs/piles/10 over-destroy guard). The >50% valve below is GLOBAL, so a whole-class wipe slips
    // under it whenever that class is a minority of the world (11:16: 100% of 870 piles = 31% of all
    // props -> no abort -> ALL piles vanished). This floor is PER-CLASS and uses a POSITIVE signal:
    // the host's INDEPENDENT GUObjectArray census (snapshot_census, NOT the registry the expression
    // path used). For each doomed actor, if the host reported a live count for its class AND we
    // claimed FEWER than that, the snapshot for the class is INCOMPLETE -> KEEP (the missing
    // expressions are in flight or failed; dooming wipes genuine objects). EXACT, not a percentage:
    // it keeps "host expressed 0 of 870" yet still dooms a legitimate clear (host genuinely has 0 ->
    // no census entry / count 0 -> claimed >= count -> doomed as a real deletion). A class with no
    // census entry is unaffected (the >50% valve still guards it). Applied BEFORE the valve so the
    // valve sees the genuine-divergence remainder.
    if (coop::snapshot_census::HasCensus() && !doomed.empty() &&
        !coop::dev::force_overdestroy_test::FloorDisabledForTest()) {
        std::vector<void*> keptDoomed;
        std::vector<std::wstring> keptDoomedClass;
        keptDoomed.reserve(doomed.size());
        keptDoomedClass.reserve(doomed.size());
        std::unordered_map<std::wstring, int> floorKeptByClass;
        for (size_t i = 0; i < doomed.size(); ++i) {
            const std::wstring& c = doomedClass[i];
            const int hostHas = coop::snapshot_census::HostCountForClass(c);
            const auto cit = claimedByClass.find(c);
            const int claimedOfC = (cit == claimedByClass.end()) ? 0 : cit->second;
            if (hostHas > 0 && claimedOfC < hostHas) {
                ++floorKeptByClass[c];   // incomplete snapshot for class c -> KEEP, do not doom
                continue;
            }
            keptDoomed.push_back(doomed[i]);
            keptDoomedClass.push_back(c);
        }
        if (keptDoomed.size() != doomed.size()) {
            doomedByClass.clear();   // rebuild the histogram from the surviving doomed set
            for (const auto& c : keptDoomedClass) ++doomedByClass[c];
            size_t keptTotal = 0;
            std::wstring topCls;
            int topCnt = 0;
            for (const auto& [c, v] : floorKeptByClass) {
                keptTotal += static_cast<size_t>(v);
                if (v > topCnt) { topCnt = v; topCls = c; }
                const int hostHas = coop::snapshot_census::HostCountForClass(c);
                const auto cit = claimedByClass.find(c);
                const int claimedOfC = (cit == claimedByClass.end()) ? 0 : cit->second;
                UE_LOGW("remote_prop_spawn: completeness FLOOR kept %d unclaimed '%ls' -- host census %d, "
                        "claimed only %d this bracket (INCOMPLETE snapshot, NOT a divergence; docs/piles/10 guard)",
                        v, c.c_str(), hostHas, claimedOfC);
            }
            UE_LOGW("remote_prop_spawn: completeness floor KEPT %zu of %zu doomed actor(s) across %zu class(es) "
                    "(top: %d x '%ls') -- the host under-expressed these classes; the unclaimed locals SURVIVE",
                    keptTotal, doomed.size(), floorKeptByClass.size(), topCnt, topCls.c_str());
            doomed.swap(keptDoomed);
            doomedClass.swap(keptDoomedClass);
        }
    }

    // SAFETY VALVE (2026-06-15, post-live-save regression). A legitimate divergence
    // is a SMALL delta: the client loaded the host's save, so the host cannot have
    // changed more than a fraction of the world between that save and this join. A
    // sweep that wants to destroy MORE THAN HALF the in-universe props is therefore
    // NOT divergence -- it is an incomplete snapshot (the host re-seeded mid-connect
    // and sent a tiny first bracket, racing the chunked drain), and destroying on it
    // wipes the just-loaded world (the 2026-06-15 hands-on: 2979 of 3229 destroyed).
    // ABORT instead -- the claimed props stay converged; the unclaimed survive (a
    // later fuller bracket re-expresses them, and the host's PropDestroy stream still
    // removes genuine deletions). A few stale ghosts beat an empty world. The
    // live-capture path skips the sweep entirely (event_feed); this guards every
    // OTHER path (stale fallback / fresh boot) against the same class of bug.
    if (inClass > 0 && static_cast<int>(doomed.size()) * 2 > inClass) {
        UE_LOGW("remote_prop_spawn: claim sweep ABORTED -- would destroy %zu of %d in-universe "
                "actor(s) (>50%%); the host snapshot is INCOMPLETE (partial/racing bracket), not a "
                "divergence. Keeping the loaded world (%d claimed stay converged).",
                doomed.size(), inClass, claimedCount);
        // (sync-refactor 2026-06-27) The membership DOOM-destroy is aborted (incomplete snapshot), but the
        // IDENTITY reconcile must STILL run -- it is per-eid, armed-bounded (re-bind/retire by stable
        // position/eid), NEVER a mass-destroy, so it is safe even on an incomplete world. Skipping it here
        // was why variant-1 / twin-retire / b3 never ran on a save-transfer join with a mass-purge (the
        // 15:25 verify: valve aborted -> reconcile skipped -> purge-churned natives left unbound). Run it
        // (The deferred twins/corrections/destroys SURVIVE this bracket now -- they drain at the steady-state
        // tick; only the spawn-time index is reset here. Anti-smear split 2026-06-30.)
        coop::element::quiescence_drain::RunReconcile(/*joinSweep=*/false);
        g_claimedActors.clear();
        g_claimTrackingActive = false;
        coop::pile_spawn_bind::Reset();
        return;
    }

    // Phase 3: destroy with OnDestroy-parity teardown. Echo-suppressed
    // (MarkIncomingDestroy) so our K2_DestroyActor PRE observer does not
    // broadcast these local-only teardowns. deferred=false: we run from the
    // event_feed drain on the game thread, not inside a BP graph.
    for (void* a : doomed) {
        coop::remote_prop::ClearAnyDriveFor(a);
        ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, a);
        coop::prop_lifecycle::DestroyLocalProp(a, /*deferred=*/false);
    }
    UE_LOGI("remote_prop_spawn: claim sweep -- %d in-universe actors live, "
            "%d claimed (expressed on the wire this bracket), %d unclaimed locals destroyed "
            "(client adopts host world)",
            inClass, claimedCount, static_cast<int>(doomed.size()));
    // Doomed-class histogram (audit ask): the sweep must NAME what it kills
    // -- a wrongly-universed class shows up here, not as a delayed crash.
    {
        std::vector<std::pair<std::wstring, int>> hist(doomedByClass.begin(), doomedByClass.end());
        std::sort(hist.begin(), hist.end(),
                  [](const auto& l, const auto& r) { return l.second > r.second; });
        int shown = 0;
        for (const auto& [hcls, cnt] : hist) {
            if (++shown > 10) {
                UE_LOGI("remote_prop_spawn:   doomed ... +%zu more classes",
                        hist.size() - 10);
                break;
            }
            UE_LOGI("remote_prop_spawn:   doomed %d x '%ls'", cnt, hcls.c_str());
        }
    }
    // R1 + R3 retired the reconcile-once latch that lived here (g_sweepReconciled):
    // R1 stopped the steady-world re-seed from re-bracketing (the dominant ~10x/join
    // re-arm = the churn the latch band-aided), and R3's membership enumeration + the
    // >50%% valve make any REMAINING re-fire (only a genuine world transition re-brackets
    // now) bounded and safe -- a re-sweep can no longer thrash the world or doom mirrors
    // (mirrors are excluded at the source). So there is nothing left to latch against.

    // ---- L1 ORPHAN CENSUS (2026-06-23, READ-ONLY -- insertion #2). We are now post-quiescence
    // (g_sweepFired set above), post-burst, and past the >50%% valve. The leftover g_pileBindIndex
    // entries are native level-chipPiles that NO arriving proxy claimed within 1cm. The PropSpawn
    // bracket is PROVABLY drained before this sweep fires (in-lane FIFO Begin->[every PropSpawn]->
    // Complete on Lane::Bulk; SnapshotComplete only ARMS the sweep -- agent-verified 2026-06-23),
    // so a leftover is a real host-DRIFT orphan: the host MOVED the pile (its proxy is elsewhere)
    // or COLLECTED it (no proxy) since the save the client loaded. These are EXACTLY the orphans
    // the sweep's Registry doom above MISSES -- a level-native enters the Prop Registry lazily, so
    // SnapshotActorsByType(Prop) never enumerates it. This is the L1 hole: a human aims at one of
    // these surviving natives, grabs it through the REAL interaction system (it is a real
    // actorChipPile_C, not our proxy), so no GrabIntent is sent and the host never sees the grab.
    //
    // READ-ONLY for now (absence-removal is Phase 2): band each orphan by the distance to the
    // nearest live PILE-form proxy + log the histogram, so the removal thresholds come from REAL
    // host-drift data (measure before cut). DIVERGES from MTA, which removes purely by ID/
    // membership (Client/.../CPacketHandler.cpp Packet_EntityRemove deletes only the exact IDs the
    // server names, never by position): our chipPiles are KEYLESS + deterministically level-placed,
    // so the host pile and the client native share NO cross-peer id -> position is the only key.
    // MTA's client starts EMPTY (it never holds a local save), so it never has this orphan class;
    // the position+valve approach is the justified adaptation (RULE 2026-05-28 divergence note --
    // the >50%% valve is the partial-snapshot guard MTA gets for free from its empty-start + JOINED
    // gate). Cold path (once per join), bounded (a few dozen leftovers x a proxy-set walk each).
    // ALWAYS log when the index was built this bracket -- even 0 orphans. A CLEAN join drains the index
    // to EMPTY (every twin matched within 1cm), and gating on non-empty made a 0-orphan join SILENT --
    // indistinguishable from a census that never ran (the exact ambiguity the 2026-06-23 clean same-machine
    // smoke hit: 869 built, all matched, no [PILE-CENSUS] line -> looked broken). The summary line is the
    // proof the census ran + the count; N=0 on a clean join is the expected, INFORMATIVE result.
    // FRESH walk at the sweep (GC-ROBUST) -- do NOT re-use g_pileBindIndex's build-time internal indices.
    // The 2026-06-23 calibration proved why: a mass-purge runs right at the sweep (prop_element_tracker
    // reaps 256 dead Prop Elements/call, draining the join-tail backlog -- log-confirmed at the SAME second
    // as the sweep, repeating every ~4s), churning the GUObjectArray so every stored internalIdx goes STALE
    // -> IsLiveByIndex(actor, idx) false-negatives on every survivor -> "0 live of 17" while the drift had
    // seeded 8 orphans. So re-enumerate live native chipPiles with FRESH indices: the burst's 1cm twin-
    // destroy already removed every MATCHED native, so the live survivors ARE the orphan set directly.
    // One GUObjectArray walk, once per join (cold path), pointer-compare class filter before any read.
    // v86 Path 1c: retry the save-time twin-destroys that MISSED at the world-ready burst (the moved
    // pile's native@old had not async-loaded yet). We are now post-quiescence (this whole sweep is gated
    // by TickClientReconcile on kSweepQuiesceScans stable scans) + past the >50%% valve above, so the late
    // natives are present (the census below SEES them). Keyed by save-time position (not blind proximity),
    // with its own >50% abort-valve. Runs BEFORE the census so the census reflects the removals.
    // The join-window reconcile (twin-retire -> re-bind -> kerfur-retire -> deferred-destroy -> b3 pos-correction
    // -> census) is the ONE order owner coop::element::quiescence_drain::RunReconcile, driven HERE at the join-window
    // quiescence sweep AND by the steady-state coop::element::quiescence_drain::OnTick (the D1 structural fix: the
    // mechanisms were join-window-one-shot, so a save-pile grabbed/moved AFTER this sweep armed a twin nothing
    // consumed = the 15:01:49 ghost). joinSweep=true logs the one-shot orphan census. See coop/element/quiescence_drain.h.
    coop::element::quiescence_drain::RunReconcile(/*joinSweep=*/true);
    // NOTE: the kerfur off->active retire sweep (scope A) IS driven here now -- it is step 3 of RunReconcile
    // (anti-smear 2026-06-30; it used to be a separate driver in kerfur_convert::PollKerfurConversions = a 3rd
    // parallel order owner, now removed). The bracket-not-armed case (SnapshotBegin-lost flake leaves
    // g_sweepPending false) is covered by quiescence_drain::OnTick, which runs every client tick before the
    // g_sweepPending gate and ORs kerfur_reconcile::HasPendingRetire into its HasPendingWork. [[feedback-one-owner-order-axis]]

    g_claimedActors.clear();
    g_claimTrackingActive = false;
    coop::pile_spawn_bind::Reset();  // bracket over: drop candidate pointers (would dangle past the GC below). The
                                     // deferred reconcile queues (quiescence_drain) survive -> the steady tick drains them.
    // Pair the mass destruction with the engine's own purge (end-of-frame
    // CollectGarbage, the post-level-transition pattern). Without it the
    // sweep's pending-kill actors + the ~3k-spawn bracket's transients sit
    // until UE's 61 s periodic purge -- smoke-measured as a 10.6 GB client
    // RSS plateau that grazed the 12 GB process commit cap. Runs under the
    // join cover; the purge hitch is invisible.
    if (!ue_wrap::engine::ForceGarbageCollection()) {
        UE_LOGW("remote_prop_spawn: post-sweep CollectGarbage unresolved -- relying on the engine's periodic purge");
    }
}

// Load-tail quiescence probe for the deferred sweep + the NPC adoption (both gate on
// HasLoadTailQuiesced). Counts two populations whose sum settles exactly when the
// async loadObjects pass finishes materializing the world:
//   (a) ALLOWLISTED NPCs (the kerfur load tail). Counted live, non-CDO, tracked OR
//       untracked -- adoption/binding does NOT change the count (the actor stays live +
//       allowlisted), so only loadObjects spawning a new twin perturbs it. This is the
//       half the old prop-only probe MISSED: the kerfur NPCs respawn SECONDS after the
//       props' keys mint (2026-06-15 hands-on), so a prop-only quiescence fired before
//       the twins existed and the adoption fresh-spawned a duplicate next to the still-
//       loading twin. Genuinely-absent twins (a host kerfur turned on after capture)
//       have no local actor, so they do not perturb the count -> never block quiescence.
//   (b) Keyless, UNCLAIMED, in-universe, non-per-player, non-chipPile props that read
//       Key=None -- the prop load tail (a straggler that has not minted its key yet,
//       exactly the set the sweep's keyless-skip would spare).
// While either tail is still loading the sum CHANGES; when it stops changing for
// kSweepQuiesceScans the load has drained. ONE GUObjectArray walk (pure pointer-compare
// class filters before any key/name read), throttled to 5 Hz, only while a sweep pends.
static int CountLoadTailUnsettled_() {
    const int32_t n = R::NumObjects();
    int unsettled = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        // (a) allowlisted-NPC load tail (lineage test first; the NameOf/IsLive only run
        // for the handful that pass it). A false return (allowlist not yet resolved)
        // just degrades to prop-only quiescence -- never worse than the old behavior.
        if (coop::npc_sync::IsAllowlistedClass(cls)) {
            if (!R::IsLive(obj)) continue;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
            ++unsettled;
            continue;
        }
        // (b) keyless-prop load tail (the original probe population).
        if (!ue_wrap::prop::IsClassKeyedInteractable(cls)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO (alloc-free)
        // (b') chipPile field (docs/piles/10 purge-aware gate, 2026-06-25). COUNT live chipPiles into the
        // population (was a `continue` skip): a join-time world-reload purge DROPS the field (871 -> ...) and
        // the async reload CLIMBS it back (... -> 870), so counting it makes the gate refuse to quiesce
        // through EITHER half of the reload -- including the <=4 s window where the purge has physically
        // started but InPurgeEpisode is not yet flagged (the reaper is on a 4 s throttle). This is the
        // PHYSICAL-population half of the fix; it does not depend on the flag's detection latency. Counted
        // EVEN IF claimed (a claimed-then-purged pile still perturbs the field), so it sits ABOVE the
        // g_claimedActors skip below. Shared with NPC adoption (HasLoadTailQuiesced): making adoption also
        // wait for pile stability is conservative + correct (piles + kerfur NPCs load in the same async pass).
        if (ue_wrap::prop::IsChipPile(obj)) { ++unsettled; continue; }
        if (g_claimedActors.count(obj)) continue;                       // claimed: never a sweep candidate
        if (coop::prop_lifecycle::IsPerPlayerPropClass(R::ClassNameOf(obj))) continue;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") ++unsettled;
    }
    return unsettled;
}

void ArmDivergenceSweep() {
    if (!g_claimTrackingActive) {
        // SnapshotComplete without its Begin (wire anomaly / disconnect race) --
        // do not arm a sweep with no claim set behind it.
        UE_LOGW("remote_prop_spawn: divergence sweep arm requested but tracking not armed -- skipping");
        return;
    }
    // (The 2026-06-17 reconcile-once latch that gated here -- g_sweepReconciled -- is
    // RETIRED by R1+R3. It band-aided the join-churn: the host's steady-world re-seed
    // re-bracketed ~10x/join, each re-arming this sweep, which re-doomed unclaimed locals
    // -- thrashing piles + repeatedly dooming kerfur mirrors. R1 fixed that at the SOURCE
    // (steady re-seed now broadcasts bracket-free incremental PropSpawns -- no re-bracket,
    // no re-arm). The only re-entry left is a genuine world transition (cave/level travel),
    // where re-reconciling against the NEW world is CORRECT, and R3's membership enumeration
    // + the >50%% valve keep that re-fire bounded + non-churning (mirrors excluded at the
    // source; can't wipe the world). So we always proceed to arm. RULE 2: the latch is gone.)
    // Defer the one real sweep. Claim tracking stays armed (NOT disarmed here) so
    // any host PropSpawn / client self-announce during the quiesce window still
    // claims its actor via RecordClaim and is spared.
    g_sweepPending = true;
    g_sweepFired = false;
    g_sweepArmedAt = std::chrono::steady_clock::now();
    g_sweepLastProgressAt = g_sweepArmedAt;  // no-progress deadline base; no generation capture -- a clean
                                             // no-purge join (boot seed holds, population already stable) must
                                             // be allowed to quiesce immediately.
    g_sweepLastScan = {};            // force a quiescence scan on the next tick
    g_sweepLastUnsettledCount = -1;
    g_sweepStableScans = 0;
    UE_LOGI("remote_prop_spawn: divergence sweep ARMED -- deferring to load-tail quiescence "
            "(keyless-prop + allowlisted-NPC population stable x%d scans @%dms, or %dms hard deadline)",
            kSweepQuiesceScans, kSweepScanIntervalMs, kSweepDeadlineMs);
}

void TickClientReconcile() {
    // Steady-state identity reconcile (D1 structural fix, sync-refactor 2026-06-27): runs EVERY tick, even
    // when the join one-shot is disarmed -- it self-gates cheaply (a quiescence bool + a pending-work bool +
    // a 250 ms debounce) and only walks the array when a save-pile grabbed/moved after the join sweep armed a
    // twin. Below this line is the join-window one-shot trigger (disarmed = zero cost). See coop/sync.
    coop::element::quiescence_drain::OnTick();
    if (!g_sweepPending) return;  // zero cost when disarmed (the steady state)
    UE_ASSERT_GAME_THREAD("remote_prop_spawn::TickClientReconcile");  // no-mutex: all sweep state is GT-only
    const auto now = std::chrono::steady_clock::now();
    const auto msSince = [now](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
    };
    // Throttle to ~5 Hz (skip the throttle on the very first scan after arming,
    // when g_sweepLastScan is the default-constructed epoch).
    if (g_sweepLastScan.time_since_epoch().count() != 0 &&
        msSince(g_sweepLastScan) < kSweepScanIntervalMs) return;
    g_sweepLastScan = now;

    // Two-tier deadline (docs/piles/10 purge-aware gate): the NO-PROGRESS timer (since the last progress edge)
    // OR the ABSOLUTE ceiling (since arm). The absolute ceiling fires even through a stuck purge so the
    // reconcile can never defer forever; the no-progress timer never pre-empts a legitimately-draining purge
    // (which keeps resetting g_sweepLastProgressAt below).
    const bool deadlineHit = msSince(g_sweepArmedAt) >= kSweepHardCapMs ||
                             msSince(g_sweepLastProgressAt) >= kSweepDeadlineMs;
    if (!deadlineHit) {
        // REGISTRY MID-PURGE (or never seeded) -> the loaded world is INCOMPLETE; never adjudicate against it.
        // A draining purge IS progress (reset the no-progress base) so a ~50 s legitimate reload does not trip
        // the deadline; the population-stability run restarts once the world re-seeds (!InPurgeEpisode is the
        // clean "registry re-seeded" edge -- the episode-end re-seed is synchronous, net_pump.cpp:538-539).
        if (!coop::prop_element_tracker::HasSeededOnce() ||
            coop::prop_element_tracker::InPurgeEpisode()) {
            g_sweepLastProgressAt = now;
            g_sweepLastUnsettledCount = -1;
            g_sweepStableScans = 0;
            return;  // keep waiting (the absolute ceiling above still bounds a permanently-stuck purge)
        }
        const int unsettled = CountLoadTailUnsettled_();
        if (unsettled != g_sweepLastUnsettledCount) {
            g_sweepLastUnsettledCount = unsettled;
            g_sweepLastProgressAt = now;  // population still moving = progress -> hold off the no-progress deadline
            g_sweepStableScans = 0;
            return;  // population still changing -> load tail (props, piles, or NPCs) not drained, keep waiting
        }
        if (++g_sweepStableScans < kSweepQuiesceScans) return;  // stable, but not for long enough yet
    }

    // Gate satisfied (quiesced or deadline) -- run the one real sweep. Re-resolve
    // the live local player here (a pointer stashed at arm time could go stale; the
    // sweep needs it to release the grav-hand if a doomed actor is being held --
    // exactly the kerfur-ghost-grab case). Cold path, one FindObjectByClass.
    void* localPlayer = R::FindObjectByClass(P::name::MainPlayerClass);
    const char* fireReason =
        !deadlineHit                                       ? "load tail quiesced"
        : (msSince(g_sweepArmedAt) >= kSweepHardCapMs)     ? "ABSOLUTE ceiling (stuck purge backstop)"
                                                           : "no-progress deadline";
    UE_LOGI("remote_prop_spawn: divergence sweep FIRING (%s; %lldms after arm, %lldms since last progress)",
            fireReason, static_cast<long long>(msSince(g_sweepArmedAt)),
            static_cast<long long>(msSince(g_sweepLastProgressAt)));
    g_sweepPending = false;
    g_sweepFired = true;  // load tail drained -> npc_adoption may now fresh-spawn no-twin save NPCs
    coop::save_identity_bind::ForceSaveChurnForTest();  // [dev] force_save_churn: synthetic unbind so variant-1 runs N>0 (verify probe)
    RunDivergenceSweep_(localPlayer);
    // Phase 1 step 1A probe: load tail has quiesced -> emit the keyless-spawn coverage verdict (read-only).
    coop::dev::spawn_order_probe::EmitVerdictAtQuiescence();
    // Phase 1 step 2b bind: load tail has quiesced -> emit the eid-range bind summary (bound count, case i/ii).
    coop::save_identity_bind::EmitBindSummary();
    // instant-world quiescence BACKSTOP: the sweep just destroyed the join-window ghosts/dups, so reveal
    // every still-hidden survivor (the held tail + anything spawned after the curtain-lift) and close the
    // deferred-hide window. Ghosts destroyed above are liveness-skipped inside mirror_defer. Worst case this
    // is exactly today's end-state -- the backup is untouched; this only un-hides what it resolved.
    coop::mirror_defer::RevealAllSurvivorsAtQuiescence();
}

bool IsInDivergenceUniverseUnclaimed(void* actor) {
    // The divergence-universe membership test WITHOUT the g_sweepPending precondition: an UNCLAIMED,
    // in-universe, keyed Aprop the host has not expressed -- a save-loaded local awaiting adjudication
    // against the host snapshot. Used by IsPendingSweepCandidate (a sweep is armed) AND by the
    // pre-quiescence join-window grab guard in trash_collect_sync (the sweep is not yet armed) so both
    // share ONE membership definition (RULE 2 -- no second copy of this lineage logic).
    if (!actor) return false;
    if (g_claimedActors.count(actor)) return false;  // host-expressed / self-claimed legit drop -> not a ghost
    void* cls = R::ClassOf(actor);
    if (!ue_wrap::prop::IsClassKeyedInteractable(cls)) return false;  // out of the divergence universe
    if (!R::IsLive(actor)) return false;
    if (ue_wrap::prop::IsChipPile(actor)) return false;  // pile has its own collect/share + grab-hook destroy path
    if (coop::prop_lifecycle::IsPerPlayerPropClass(R::ClassNameOf(actor))) return false;  // per-player: never swept
    // A kerfur prop MIRROR is host-driven state (its host-range eid is bound when the convert/adoption
    // materializes it), NEVER a save-loaded local awaiting adjudication -- so it is not a divergence
    // candidate for any caller of THIS predicate (IsPendingSweepCandidate + the trash_collect pre-
    // quiescence grab guards). (The divergence SWEEP itself no longer needs this: since R3 it enumerates
    // only LOCAL Prop Elements via SnapshotActorsByType and excludes host-driven mirrors at the source
    // with pr.mirror -- so this exemption now serves ONLY the grab-guard predicate, which is handed an
    // arbitrary actor and must still recognize a kerfur mirror.) Exempt it like the chipPile / per-player
    // ones. (2026-06-17 kerfur join fix; the reconcile-once gate it companioned was retired by R1+R3.)
    if (coop::kerfur_entity::GetKerfurMirrorEidForActor(actor) != coop::element::kInvalidId) return false;
    return true;  // unclaimed in-universe keyed Aprop = a divergence candidate awaiting adjudication
}

bool IsPendingSweepCandidate(void* actor) {
    // A divergence sweep is armed AND this actor is one of its candidates.
    return g_sweepPending && IsInDivergenceUniverseUnclaimed(actor);
}

bool HasLoadTailQuiesced() { return g_sweepFired; }

void OnClientWorldReadyResetSweep() {
    if (g_sweepPending)
        UE_LOGI("remote_prop_spawn: client world-ready -- cancelling a pending divergence sweep "
                "from the prior world (the new snapshot bracket will re-arm it)");
    g_sweepPending = false;
    g_sweepFired = false;
    g_sweepStableScans = 0;
    g_sweepLastUnsettledCount = -1;
}

void ResetClaimTracking() {
    if (g_claimTrackingActive) {
        UE_LOGI("remote_prop_spawn: claim tracking reset mid-snapshot (disconnect) -- "
                "%zu claims dropped, no sweep", g_claimedActors.size());
    }
    g_claimedActors.clear();
    g_claimTrackingActive = false;
    // A mid-snapshot drop must also cancel any deferred sweep armed this session
    // (the tick driver would otherwise fire it against a torn-down world).
    g_sweepPending = false;
    g_sweepFired = false;
    g_sweepStableScans = 0;
    g_sweepLastUnsettledCount = -1;
    coop::pile_spawn_bind::Reset();  // session teardown: drop the spawn-time index (dangling-pointer hygiene)
    coop::element::quiescence_drain::Reset();  // session teardown: drop the deferred reconcile queues (the ONLY site that clears them)
    coop::kerfur_reconcile::Reset();  // scope A: drop any unconsumed save-time kerfur retire across sessions
    coop::mirror_defer::Reset();  // instant-world: reveal any still-hidden mirror + disarm the deferred-hide window
}

}  // namespace coop::remote_prop_spawn

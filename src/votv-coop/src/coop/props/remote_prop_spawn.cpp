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
#include "coop/config/config.h"  // IsIniKeyTrue -- hands-on probe flags live in votv-coop.ini [dev], not bats/env
#include "coop/creatures/kerfur_entity.h"  // GetKerfurMirrorEidForActor -- exempt host-driven kerfur mirrors in the grab-guard predicate (the SWEEP excludes mirrors structurally via pr.mirror since R3)
#include "coop/creatures/kerfur_prop_adoption.h"  // K-6: defer a kerfur-prop fuzzy-miss to the polled adoption
#include "coop/creatures/kerfur_reconcile.h"  // scope A: post-quiescence retry of the kerfur off->active dup retire
#include "coop/element/mirror_defer.h"  // instant-world: hide a freshly-spawned host mirror until lift/quiescence reveal
#include "coop/net/protocol.h"
#include "coop/creatures/npc_sync.h"  // IsAllowlistedClass -- the NPC half of the load-tail quiescence probe
#include "coop/props/pile_spawn_bind.h"  // anti-smear 2026-06-30: pile spawn-time twin-destroy / adopt / census
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: the claim set + divergence sweep (extracted out); OnSpawn records into it
#include "coop/dev/spawn_order_probe.h"  // Phase 1 step 1A: keyless load-spawn coverage probe (read-only)
#include "coop/dev/join_window_pos_trace.h"  // F1: keyed-prop join-window position root discrimination (read-only, point A)
#include "coop/props/save_identity_bind.h"     // Phase 1 step 2b: eid-range bind summary at quiescence
#include "coop/element/quiescence_drain.h"    // anti-smear 2026-06-30: the join-window order owner (sequence + steady triggers + deferred queues)
#include "coop/props/snapshot_census.h"  // Phase 0: per-class completeness floor for the claim sweep
#include "coop/dev/force_overdestroy_test.h"  // dev-only: floor-disable toggle for the controlled proof
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_proxy.h"   // phase 1: the host-authoritative AStaticMeshActor trash mirror (dup fix)
#include "coop/props/native_pile_mirror.h"  // nativization 2026-06-30: a rooted real chipPile native is the PILE mirror
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
        const ue_wrap::FVector scale{payload.scaleX, payload.scaleY, payload.scaleZ};
        // PILE form in STEADY STATE (not the join-reconcile bracket) -> a ROOTED REAL actorChipPile_C
        // native (native int_player_C hover GUI + collision + per-instance rotation), replacing the bare
        // AStaticMeshActor proxy for piles. Scoped to !IsClaimTrackingActive: a steady-state runtime/
        // host-only pile has NO save-loaded level twin, so it never races the in-bracket TryDestroyTwin
        // reconcile below (which stays on the proven proxy path) -- native-spawn + twin-destroy are
        // mutually exclusive, so a native+twin dup is impossible by construction. In-bracket level piles
        // are already native via the IsBoundMirrorNative bind (save_identity_bind); the clump form (LifeSpan
        // + autonomous re-pile) stays a proxy. Inertness PROVEN: the 2026-06-30 collision-ON inert probe
        // (60s live + inert, native hover GUI present). [[lesson-runtime-staticmeshactor-must-be-movable]]
        const bool nativePile = !isClump && !coop::join_membership_sweep::IsClaimTrackingActive();
        void* proxy = nullptr;
        if (nativePile) {
            // Materialize binds + marks save-native internally (unless skipBind, where OnConvert owns the bind).
            // payload.rot is the host's captured visible-mesh rotation for a chipPile (prop_snapshot uses
            // GetVisibleMeshWorldRotation) -> the native consumes it (host->client) so its roll matches the host.
            ue_wrap::FRotator meshRot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
            proxy = coop::native_pile_mirror::Materialize(payload.elementId, classW, payload.chipType,
                                                          loc, meshRot, scale, senderSlot, skipBind, /*rebindInPlace=*/false);
        } else {
            ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
            proxy = coop::trash_proxy::SpawnProxy(payload.elementId, payload.chipType, isClump, senderSlot, loc, rot, scale);
            if (proxy && !skipBind)
                coop::remote_prop::RegisterPropMirror(payload.elementId, proxy, L"", classW, senderSlot);
        }
        if (proxy) {
            if (!skipBind) {
                // instant-world: hide the freshly-registered mirror until reveal. hasMatchPos => a moved-save
                // pile whose local native@old is still visible -> HOLD to quiescence; else (derived/window
                // pile, no native twin) reveal at the curtain-lift. (skipBind = a convert re-skin -> OnConvert
                // owns it; g_revealed guards against re-hiding an already-revealed one.)
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
            // GATED on coop::join_membership_sweep::IsClaimTrackingActive() (audit CRITICAL-4): a native level-pile twin to destroy ONLY
            // exists DURING the join reconcile bracket (where level piles are expressed against the index built
            // from the just-loaded world). OUTSIDE the bracket the destroy is both pointless (a mid-gameplay
            // re-pile / derived pile has NO native twin -- they were de-duped at join or never existed) AND
            // dangerous: EnsurePileBindIndex would REBUILD via a full GUObjectArray walk inside the BeginDeferred
            // thunk's game-thread stack (a warm-path regression on the first post-bracket pile-land), and a
            // derived pile landing within 1cm of an UNRELATED level-native could destroy the wrong actor. The
            // bracket gate (the same one the adopt-bind path uses) closes both windows.
            if (!isClump && coop::join_membership_sweep::IsClaimTrackingActive()) {
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
                                                     coop::join_membership_sweep::ClaimedActors());
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
        // Non-keyable clump: dedup by EID (the Registry mirror binding), not key. IsLiveByIndex, never raw
        // IsLive: a purge frees the row-held actor while the row lingers; raw IsLive on the freed pointer can
        // misread TRUE and "dedup" onto freed memory (docs/piles/12 IsLive sweep 2026-07-03).
        if (auto* e = coop::element::Registry::Get().Get(payload.elementId)) {
            void* a = e->GetActor();
            if (a && R::IsLiveByIndex(a, e->GetInternalIdx())) existing = a;
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
        coop::join_membership_sweep::RecordClaimIfTracking(existing);
        // F1 probe (read-only, point A): a genuine KEYED prop the host is expressing onto our pre-existing
        // local copy -- record the host-carried pos + whether the host held it (drive-skip) + an order stamp,
        // so the quiescence verdict can tell loadObjects-clobber from host-held-at-snapshot. Keyed only
        // (eidOnly clumps / converts / keyless are not the F1 saved-rock case). Gated: no work when disabled.
        if (!eidOnly && !fromConvert && !keyW.empty() && keyW != L"None" &&
            coop::dev::join_window_pos_trace::IsEnabled()) {
            const bool hostHeld = ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, existing)
                               || coop::remote_prop::IsActorUnderAnyDrive(existing);
            coop::dev::join_window_pos_trace::NoteSnapshotExpression(
                keyW, static_cast<uint32_t>(payload.elementId), existing,
                ue_wrap::FVector{payload.locX, payload.locY, payload.locZ}, hostHeld);
        }
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
    if (eidOnly && coop::join_membership_sweep::IsClaimTrackingActive() && !fromConvert && payload.elementId != 0) {
        // 30cm adopt find+consume lives in coop/props/pile_spawn_bind. Returns the matched
        // native pile (already removed from the index) + the matched squared distance + the shared
        // per-bracket bind-log seq; the host-eid mirror registration + physics reconcile below STAY here
        // (remote_prop internals -- RegisterPropMirror / ReconcileToHostPhysics / RestoreCollisionIfNeeded).
        float bestD2 = 0.f;
        int   nBind  = 0;
        void* pile = coop::pile_spawn_bind::FindAndConsumeAdoptCandidate(
            payload, classW, coop::join_membership_sweep::ClaimedActors(), &bestD2, &nBind);
        if (pile) {
            coop::join_membership_sweep::RecordClaimIfTracking(pile);
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
        coop::join_membership_sweep::RecordClaimIfTracking(fuzzy);
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
    // ScopedMirrorSpawn: this UFunction call dispatches through ProcessEvent,
    // so the peer-symmetric ambient broadcaster's BeginDeferred POST fires
    // INSIDE it (before the actor exists to MarkIncomingSpawn) -- the scope is
    // its re-entrancy guard (owner-mirror 2026-07-10, see prop_echo_suppress.h).
    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn mirrorScope;
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
    coop::join_membership_sweep::RecordClaimIfTracking(spawned);
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
    // Ambient owner-effect mirrors (pinecone/stick/crystal, 2026-07-10): the
    // NATIVE prop self-expires via the spawner's SetLifeSpan(600); the mirror's
    // despawn normally arrives via the owner's death-watch PropDestroy, but an
    // owner that DISCONNECTS leaves the mirror orphaned forever. SP-parity
    // backstop: give the mirror its own lifespan (900 s > the native 600 s, so
    // the wire destroy always wins in the normal case and an orphan self-reaps).
    if (payload.key.len == 0) {
        for (size_t i = 0; i < P::name::kAmbientPropSpawnMirrorClassesSize; ++i) {
            if (classW != P::name::kAmbientPropSpawnMirrorClasses[i]) continue;
            // SetLifeSpan lives on AActor -- FindFunction is exact-owner (no
            // SuperStruct climb; audit 2026-07-10 CRITICAL: the leaf-class
            // lookup returned null every call AND paid a futile full-array
            // walk per mirror). Resolve ONCE on the Actor class, latch.
            static void* s_lifeFn = nullptr;
            static bool  s_lifeTried = false;
            if (!s_lifeTried) {   // GT-only path (event drain) -- plain statics
                s_lifeTried = true;
                if (void* actorCls = R::FindClass(P::name::ActorClassName))
                    s_lifeFn = R::FindFunction(actorCls, L"SetLifeSpan");
                if (!s_lifeFn)
                    UE_LOGW("remote_prop::OnSpawn: Actor.SetLifeSpan not resolved -- "
                            "ambient-mirror orphan backstop disabled");
            }
            if (s_lifeFn) {
                ParamFrame life(s_lifeFn);
                if (life.Set<float>(L"InLifespan", 900.f)) Call(spawned, life);
            }
            break;
        }
    }
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


}  // namespace coop::remote_prop_spawn

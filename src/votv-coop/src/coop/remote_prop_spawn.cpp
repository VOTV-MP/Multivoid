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

#include "coop/remote_prop_spawn.h"

#include "coop/trash_pile_sync.h"  // sweep->death-watch unwatch (v57 audit CRIT-2)

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/prop_echo_suppress.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_lifecycle.h"
#include "coop/remote_prop.h"
#include "coop/trash_collect_sync.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// Build a wide string from WireClassName. Lossless for ASCII (VOTV class names).
std::wstring ClassNameToWString(const coop::net::WireClassName& cn) {
    std::wstring s;
    s.reserve(cn.len);
    for (uint8_t i = 0; i < cn.len && i < 63; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(cn.data[i])));
    }
    return s;
}

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

// Record that the host snapshot bound `actor` (exact-key, fuzzy, or fresh
// spawn) -- the actor is accounted-for by the host's world and must survive
// the sweep. No-op when tracking is disarmed (live PropSpawns outside a join
// cost one bool read).
void RecordClaim(void* actor) {
    if (!g_claimTrackingActive || !actor) return;
    g_claimedActors.insert(actor);
}

// ---- Keyless-pile position-bind index (v56 follow-up, 2026-06-10) --------
// With the save-transfer join the client's world is LOADED FROM THE HOST'S
// OWN SAVE: its chipPiles are the same piles, settled at the same positions.
// Binding the host's keyless eid expression to the client's OWN local pile
// (instead of sweep-destroying all ~870 and fresh-spawning mirrors) is now
// SOUND. Pre-save-transfer worlds had per-peer RNG pile layouts -- the very
// reason the eidOnly lane historically skipped local matching.
//
// Bracket-scoped, built lazily ONCE per bracket on the first keyless-pile
// expression (one GUObjectArray walk -- NOT one walk per pile; the ~870-
// expression burst would otherwise rescan 870x, the exact storm the
// dedupeFellBack self-heal above guards against). Game-thread only (the
// event_feed drain), like the claim set.
struct PileBindCandidate {
    void*   actor;
    int32_t idx;  // InternalIndex captured at build time -- bind-time liveness is
                  // IsLiveByIndex (no deref of a possibly-GC-freed pointer; the
                  // index lives for the whole multi-second bracket)
    float   x, y, z;
    uint8_t chipType;
};
std::vector<PileBindCandidate> g_pileBindIndex;
bool g_pileBindIndexBuilt = false;
int  g_pileBindCount = 0;  // per-bracket bind counter (throttles the log)

void ResetPileBindIndex() {
    g_pileBindIndex.clear();
    g_pileBindIndex.shrink_to_fit();
    g_pileBindIndexBuilt = false;
    g_pileBindCount = 0;
}

void EnsurePileBindIndex() {
    if (g_pileBindIndexBuilt) return;
    g_pileBindIndexBuilt = true;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsChipPile(obj)) continue;  // lineage test, pure pointer walks
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
        if (g_claimedActors.count(obj)) continue;  // already bound earlier this bracket
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        // chipType read once at build time: save-loaded piles carry it from the
        // save (both peers loaded the SAME save, so host==client). If a pile
        // class ever set it lazily post-load, the equality gate would miss --
        // a harmless fallback to fresh-spawn+sweep, never a wrong bind.
        g_pileBindIndex.push_back(
            {obj, R::InternalIndexOf(obj), loc.X, loc.Y, loc.Z,
             ue_wrap::prop::GetChipType(obj)});
    }
    UE_LOGI("remote_prop_spawn: pile-bind index built -- %zu local chipPile candidate(s)",
            g_pileBindIndex.size());
}

}  // namespace

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
             void* localPlayer, bool fromConvert) {
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
    if (eidOnly) {
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
        EnsurePileBindIndex();
        constexpr float kPileBindRadiusCm = 30.f;
        int best = -1;
        float bestD2 = kPileBindRadiusCm * kPileBindRadiusCm;
        for (int i = 0; i < static_cast<int>(g_pileBindIndex.size()); ++i) {
            const auto& c = g_pileBindIndex[i];
            const float dx = c.x - payload.locX;
            const float dy = c.y - payload.locY;
            const float dz = c.z - payload.locZ;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > bestD2) continue;
            if (c.chipType != payload.chipType) continue;
            if (g_claimedActors.count(c.actor)) continue;  // keyed lane claimed it meanwhile
            if (!R::IsLiveByIndex(c.actor, c.idx)) continue;  // bracket-long raw ptr: no deref before this
            if (!R::NameEquals(R::NameOf(R::ClassOf(c.actor)), classW.c_str())) continue;
            best = i;
            bestD2 = d2;
        }
        if (best >= 0) {
            void* pile = g_pileBindIndex[best].actor;
            g_pileBindIndex[best] = g_pileBindIndex.back();
            g_pileBindIndex.pop_back();
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
            // Death-watch by IDENTITY, exactly like a fresh pile mirror: a
            // local grab morph-destroys it -> PropDestroy(eid) tells the host.
            coop::trash_collect_sync::WatchPile(pile, payload.elementId);
            const int nBind = ++g_pileBindCount;
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
    // v52: enrol a chipPile MIRROR in the mirror-pile death-watch. A pile, when grabbed locally,
    // morphs to a held ball + self-destructs (BP-internal/unobservable), so whoever grabs the
    // shared pile must detect its local death and broadcast PropDestroy(eid) -> the other peer
    // drops its copy by identity. Watching ALL chipPile mirrors here (not only the PropConvert-
    // born ones) is what covers a PRE-EXISTING / snapshot pile: without it, the peer grabbing an
    // initial pile never told the owner to delete its copy = the "initial pile not destroyed"
    // dupe. NOTE: NOT gated on eidOnly -- a pre-existing world chipPile is broadcast KEYED (a synth
    // Key from the Init-POST path), so it is NOT key=None; we still watch it by its eid (which
    // resolves cross-peer via RegisterPropMirror, and the relaxed PropDestroy eid-range lets the
    // grabber's destroy through). chipPile only (clumps/balls are transient, owner-death-watched
    // separately). Idempotent per eid; a wire-driven destroy drops the watch via NotifyPileConsumed.
    if (payload.elementId != 0 && ue_wrap::prop::IsChipPile(spawned)) {
        coop::trash_collect_sync::WatchPile(spawned, payload.elementId);
    }
}

void BeginClaimTracking() {
    g_claimedActors.clear();
    g_claimTrackingActive = true;
    // A fresh bracket re-enumerates pile-bind candidates (a re-bracket runs
    // against the post-sweep world; stale entries would be dead pointers).
    ResetPileBindIndex();
    UE_LOGI("remote_prop_spawn: claim tracking ARMED (snapshot bracket open) -- "
            "unclaimed in-universe locals will be destroyed at SnapshotComplete");
}

void DestroyUnclaimedDivergentProps(void* localPlayer) {
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
    // Watched-pile interaction (audit Finding 3, re-verified under 2e):
    // trash_collect_sync::OnDisconnect clears g_watchedPiles at both
    // net_pump teardown sites, and within a bracket every WatchPile
    // enrollment happens inside OnSpawn AFTER its RecordClaim -- watched
    // piles are always claimed, never swept.
    //
    // Phase 1: collect. ONE GUObjectArray walk (the SeedWalk_ shape), ZERO
    // engine dispatches inside it (class test = pure SuperStruct pointer
    // walks; the per-actor key read happens in phase 2 over the unclaimed
    // MINORITY only -- chipPile/clump GetKey is a ProcessEvent dispatch).
    // Destroy AFTER the walk so K2_DestroyActor's GUObjectArray mutations
    // never invalidate the iteration. One walk per BRACKET -- cold path.
    const int32_t n = R::NumObjects();
    int inClass = 0;
    int claimedCount = 0;
    std::vector<void*> pending;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsClassKeyedInteractable(R::ClassOf(obj))) continue;
        if (!R::IsLive(obj)) continue;  // flag read before any wstring alloc
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // CDO
        ++inClass;
        if (g_claimedActors.count(obj)) {
            ++claimedCount;
            continue;
        }
        pending.push_back(obj);
    }
    // Phase 2: universe test on the unclaimed minority.
    std::vector<void*> doomed;
    doomed.reserve(pending.size());
    std::unordered_map<std::wstring, int> doomedByClass;
    // Ghost-twin probe (2026-06-10 forensics): the keyless-skip below is the
    // ADOPTION HOLE -- "keyless" is a TRANSIENT pre-Init state, and the world
    // load's late tail (+872 keyed actors appeared AFTER the sweep in the
    // hands-on run) turns skipped actors into keyed, interactive, never-
    // adjudicated ghosts (the wall-panel dupe). Until the structural fix
    // (deferred adjudication or a load-quiescence gate) lands, NAME what the
    // sweep skipped so every join quantifies the exposure.
    std::unordered_map<std::wstring, int> keylessSkippedByClass;
    for (void* a : pending) {
        if (!R::IsLive(a)) continue;
        const std::wstring acls = R::ClassNameOf(a);
        // PER-PLAYER state actors are out of universe in BOTH directions:
        // never snapshot-expressed by the host AND never swept here -- the
        // local instance is this player's own state (per-save key, can
        // never claim-bind). The 2026-06-10 smoke swept the client's
        // inventory container and the client fataled at the next GC purge.
        if (coop::prop_lifecycle::IsPerPlayerPropClass(acls)) continue;
        if (ue_wrap::prop::IsChipPile(a)) {
            // Expressible keyed OR keyless (HALF 1 eid expression).
            doomed.push_back(a);
            ++doomedByClass[acls];
            continue;
        }
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(a);
        if (key.empty() || key == L"None") {
            ++keylessSkippedByClass[acls];  // the adoption-hole exposure (see probe note)
            continue;  // keyless non-pile: NOT expressible -> NEVER swept
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
                "(top: %d x '%ls') -- pre-Init late-tail stragglers; these become untracked ghosts "
                "if their keys mint later (adoption hole, fix pending)",
                skippedTotal, keylessSkippedByClass.size(), topCnt, topCls.c_str());
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
    g_claimedActors.clear();
    g_claimTrackingActive = false;
    ResetPileBindIndex();  // bracket over: drop candidate pointers (would dangle past the GC below)
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

void ResetClaimTracking() {
    if (g_claimTrackingActive) {
        UE_LOGI("remote_prop_spawn: claim tracking reset mid-snapshot (disconnect) -- "
                "%zu claims dropped, no sweep", g_claimedActors.size());
    }
    g_claimedActors.clear();
    g_claimTrackingActive = false;
    ResetPileBindIndex();  // dangling-pointer hygiene across sessions (mirrors the claim set)
}

}  // namespace coop::remote_prop_spawn

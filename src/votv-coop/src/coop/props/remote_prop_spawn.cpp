// coop/props/remote_prop_spawn.cpp -- the PropSpawn receiver: a wire prop resolves to a local actor
// by exact key, by eid (trash), by a position bind (a keyless pile in the join bracket) or by a
// fuzzy same-class match within 30 cm, converges it to the host's transform when it diverged, and
// binds it as the mirror; with no match a fresh mirror is materialised (prop_fresh_spawn.cpp).

#include "coop/props/remote_prop_spawn.h"

#include "coop/props/trash_pile_sync.h"  // the sweep's death-watch unwatch

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_entity.h"  // GetKerfurMirrorEidForActor, the grab guard's kerfur exemption
#include "coop/creatures/kerfur_prop_adoption.h"  // a kerfur prop's fuzzy miss defers to the polled adoption
#include "coop/creatures/kerfur_reconcile.h"  // the post-quiescence retry of the kerfur off-to-active retire
#include "coop/element/mirror_defer.h"  // hide a fresh host mirror until the reveal
#include "coop/net/protocol.h"
#include "coop/creatures/npc_sync.h"  // IsAllowlistedClass, the NPC half of the quiescence probe
#include "coop/player/hand_item.h"    // CollectHandAxisActors: the hand axis is not adoptable
#include "coop/player/players_registry.h"  // kMaxPeers (the hand-axis buffer)
#include "coop/props/pile_spawn_bind.h"  // the pile's spawn-time twin destroy and adopt
#include "coop/props/join_membership_sweep.h"  // the claim set and the divergence sweep
#include "coop/dev/spawn_order_probe.h"  // the keyless load-spawn coverage probe
#include "coop/dev/join_window_pos_trace.h"  // the keyed-prop join-window position trace
#include "coop/dev/spawn_match_probe.h"  // the fuzzy-match candidate set and the adoption watch
#include "coop/props/save_identity_bind.h"     // the eid-range bind summary at quiescence
#include "coop/props/snapshot_census.h"  // the per-class completeness floor for the claim sweep
#include "coop/dev/force_overdestroy_test.h"  // dev-only: floor-disable toggle for the controlled proof
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"      // ExpressIncrementalSpawn, the handback re-express
#include "coop/props/prop_fresh_spawn.h"   // the deferred-spawn materialiser
#include "coop/props/prop_lifecycle.h"
#include "coop/props/prop_wire_parity.h"   // the shared physics and collision helpers
#include "coop/props/remote_prop.h"
#include "coop/props/trash_proxy.h"   // the host-authoritative trash mirror
#include "coop/props/native_pile_mirror.h"  // a rooted real chipPile native as the pile mirror
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD -- the no-mutex contract tripwire
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>   // getenv, the read-only pile-delta probe gate
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

// The shared physics and collision helpers (prop_wire_parity) under local names.
using coop::prop_wire_parity::ReconcileToHostPhysics;
using coop::prop_wire_parity::RestoreCollisionIfNeeded;
using coop::prop_wire_parity::RestoreSpParityPhysicsAfterConverge;

}  // namespace

// A wide string from a WireClassName, lossless for ASCII; shared with remote_prop::OnConvert.
std::wstring ClassNameToWString(const coop::net::WireClassName& cn) {
    std::wstring s;
    s.reserve(cn.len);
    for (uint8_t i = 0; i < cn.len && i < 63; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(cn.data[i])));
    }
    return s;
}

// The host-authority gate: true when the resolved actor is the host's own (a live non-mirror
// local, or an untracked world actor) and the peer's PropSpawn was answered with a handback (the
// actor enrolled if untracked, under its own key, and re-expressed under the host identity); the
// caller then returns without claiming, rekeying, converging or binding. False on a client, for
// the host's own word, or for a client-band mirror prior (the normal adopt path).
static bool HostAuthorityHandback_(void* actor, const std::wstring& keyW,
                                   const std::wstring& classW, int senderSlot,
                                   const char* how) {
    if (senderSlot == 0 || !actor) return false;               // the host's own word never conflicts with itself
    if (!coop::prop_element_tracker::SessionIsHost()) return false;
    const coop::element::ElementId prior = coop::element::Registry::Get().EidForActor(actor);
    if (prior != coop::element::kInvalidId) {
        coop::element::Element* pe = coop::element::Registry::Get().Get(prior);
        if (pe && pe->IsMirror()) return false;                // client-born prop we mirror -> idempotent adopt path
    }
    // A live non-mirror local or an untracked world actor: host-authoritative.
    if (prior == coop::element::kInvalidId) {
        const std::wstring ownKey = ue_wrap::prop::GetInteractableKeyString(actor);
        coop::prop_element_tracker::MarkPropElement(
            actor, (ownKey == L"None") ? std::wstring() : ownKey, R::ClassNameOf(actor),
            coop::prop_element_tracker::EnrollSource::kExpressSeam);
    }
    const coop::element::ElementId hostEid =
        coop::prop_element_tracker::GetPropElementIdForActor(actor);
    const ue_wrap::FVector hl = E::GetActorLocation(actor);
    UE_LOGW("remote_prop::OnSpawn: HOST-AUTHORITY HANDBACK (%s) -- slot=%d PropSpawn wire-eid names our own "
            "actor %p (host eid=%u key='%ls' cls='%ls' loc=(%.1f,%.1f,%.1f)); refusing peer-band rekey/bind, "
            "re-expressing under the host identity",
            how, senderSlot, actor,
            (hostEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(hostEid),
            keyW.c_str(), classW.c_str(), hl.X, hl.Y, hl.Z);
    if (coop::kerfur_entity::IsKerfurActor(actor)) {
        UE_LOGW("remote_prop::OnSpawn: handback target is a KERFUR -- enroll only; KerfurConvert owns "
                "kerfur delivery (no generic re-express)");
        return true;
    }
    coop::prop_snapshot::ExpressIncrementalSpawn(actor);
    return true;
}

void OnSpawn(const coop::net::PropSpawnPayload& payload, int senderSlot,
             void* localPlayer, bool fromConvert, bool deferKerfur,
             void** outSpawned, bool skipBind) {
    if (outSpawned) *outSpawned = nullptr;  // cleared up-front; set only on a successful spawn
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    const std::wstring classW = ClassNameToWString(payload.className);
    const std::wstring keyW   = coop::remote_prop::KeyToWString(payload.key);
    // The Aprop_C identity row (empty for a non-Aprop_C sender), for the fuzzy identity gate and
    // the pre-Finish write.
    const std::wstring propNameW = coop::remote_prop::KeyToWString(payload.propName);
    UE_LOGI("remote_prop::OnSpawn: cls='%ls' key='%ls' name='%ls' loc=(%.1f, %.1f, %.1f) rot=(%.1f, %.1f, %.1f) physFlags=0x%02x",
            classW.c_str(), keyW.c_str(), propNameW.c_str(),
            payload.locX, payload.locY, payload.locZ,
            payload.rotPitch, payload.rotYaw, payload.rotRoll,
            static_cast<int>(payload.physFlags));
    // A trash clump rides this pipeline identified by eid, not key (setKey does not stick on it):
    // key None, elementId set. Aprop_C keeps the keyed path.
    const bool eidOnly = (keyW.empty() || keyW == L"None");
    if (classW.empty() || (eidOnly && payload.elementId == 0)) {
        // "None" is FName(NAME_None) stringified; a prop with no key and no eid is a straggler,
        // dropped so it cannot loop the dedupe.
        UE_LOGW("remote_prop::OnSpawn: unkeyed + no eid (cls='%ls' key='%ls') -- dropping",
                classW.c_str(), keyW.c_str());
        return;
    }
    // The proxy path: trash (chipPile, clump and variants) is mirrored by an AStaticMeshActor we
    // own (coop/trash_proxy), never the real BP (no self-morph, GC or stale index, so no dupe),
    // branching before the BP dedupe, converge and physics machinery. SpawnProxy is idempotent (a
    // re-spawn re-skins the proxy for the same eid); a re-skin is OnConvert's, a teardown
    // OnDestroy's. With skipBind, OnConvert binds the element itself.
    if (coop::trash_proxy::IsTrashProxyClass(classW)) {
        // If this eid already resolves to a live bound-mirror native (the client loaded the same
        // host save and save_identity_bind bound its native chipPile as the mirror), the native is
        // the mirror: no proxy over it (a proxy has no interaction surface or collision), and
        // nothing to register. Only save-loaded piles reach this; runtime piles and carried clumps
        // spawn the proxy.
        if (auto* be = coop::element::Registry::Get().Get(payload.elementId)) {
            void* bn = be->GetActor();
            // IsLiveByIndex, not IsLive: the cached actor is engine-owned, so only its
            // GUObjectArray slot is read, never a possibly freed pointer.
            if (bn && R::IsLiveByIndex(bn, be->GetInternalIdx()) &&
                coop::prop_element_tracker::IsBoundMirrorNative(bn)) {
                if (outSpawned) *outSpawned = bn;
                return;
            }
        }
        const bool isClump = coop::trash_proxy::IsClumpClass(classW);
        ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
        const ue_wrap::FVector scale{payload.scaleX, payload.scaleY, payload.scaleZ};
        // A pile in steady state (outside the join bracket) becomes a rooted real chipPile native
        // (the native hover GUI, collision and per-instance rotation) instead of the bare proxy.
        // Scoped to the steady state: a runtime pile has no save-loaded twin, so it never races the
        // in-bracket twin destroy below, and a native-plus-twin dupe is impossible by construction;
        // in-bracket level piles are already native via the bind, and the clump form stays a proxy.
        const bool nativePile = !isClump && !coop::join_membership_sweep::IsClaimTrackingActive();
        void* proxy = nullptr;
        if (nativePile) {
            // Materialize binds and marks save-native itself (unless skipBind, OnConvert's). The
            // payload rotation is the host's captured visible-mesh rotation, so the native's roll
            // matches the host.
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
                // Hide the fresh mirror until the reveal: a moved-save pile (hasMatchPos) whose
                // local native at the old spot is still visible holds to quiescence; a derived or
                // window pile reveals at the curtain lift. A convert re-skin (skipBind) is
                // OnConvert's.
                coop::mirror_defer::OnMirrorSpawned(payload.elementId, proxy, /*collisionOff=*/false,
                                                    /*holdUntilQuiescence=*/payload.hasMatchPos != 0);
            }
            if (outSpawned) *outSpawned = proxy;
            // The level-pile twin destroy: a level-placed chipPile gets a host eid and this proxy,
            // and the client's native level-loaded chipPile coexists with it, the visible dupe (the
            // sweep is blind to natives, which enter the Registry lazily). The co-located native is
            // destroyed after the proxy spawns: an exact ~1 cm match, graceful on none (a derived
            // pile has no twin), skip on more than one (never destroy the wrong one). Destroy, not
            // adopt: adopting brings back the BP self-morph the proxy model exists to avoid. The
            // index is built once before the burst and shrinks as natives are consumed. Gated on
            // the join bracket: only there does a native twin exist, and outside it the index would
            // be rebuilt by a full array walk inside the spawn thunk, and a derived pile could
            // destroy an unrelated level native within 1 cm.
            if (!isClump && coop::join_membership_sweep::IsClaimTrackingActive()) {
                // The twin destroy (coop/props/pile_spawn_bind) matches the client's native against
                // the pile's save-time position (stamped by the host's snapshot), the value both
                // peers loaded: a pile the host moved in the join window spawns its proxy at the
                // new spot while the native loaded at the old, and matching on the live position
                // went blind to it. With no stamp (a mid-game spawn) the live position is the
                // fallback.
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
    // The dedupe: a local Aprop_C with the same Key already exists (both peers loaded the same
    // save, or a prior PropSpawn created it), so it is converged to the host's transform rather
    // than duplicated. The host-authority gate comes first: a peer PropSpawn (a container extract,
    // a held-item express) can resolve onto an actor the host already owns, and the old flow
    // rekeyed it to the peer's key or stacked a peer-band mirror over it, unbinding the host's own
    // identity (its destroy observer silenced). The actor is enrolled host-side under its own key
    // and re-expressed under the host identity; the sender adopts the host eid. A client-band
    // mirror prior falls through to the idempotent adopt; a kerfur is enrolled only (KerfurConvert
    // owns it).
    bool dedupeFellBack = false;
    void* existing = nullptr;
    if (fromConvert) {
        // A convert re-skins an eid in place, and the old rendering is still at Registry::Get(E);
        // letting the dedupe resolve it would converge the morph onto the actor being replaced.
        // OnConvert captures and destroys the old rendering itself, so the dedupe is skipped whole.
    } else if (eidOnly) {
        // A clump dedupes by eid (the mirror binding), not key. IsLiveByIndex, never IsLive: a
        // purge frees the row's actor while the row lingers, and IsLive on the freed pointer can
        // read true.
        if (auto* e = coop::element::Registry::Get().Get(payload.elementId)) {
            void* a = e->GetActor();
            if (a && R::IsLiveByIndex(a, e->GetInternalIdx())) existing = a;
        }
    } else {
        existing = coop::prop_element_tracker::ResolveLiveActorByKey(keyW, &dedupeFellBack);
        if (dedupeFellBack) {
            // The key index was stale (a world change purged the indexed actors), so this dedupe
            // paid an O(N) array scan; during the snapshot ~2,300 PropSpawns arrive in a burst, and
            // each scanning balloons the client by gigabytes. Self-heal now (throttled to once per
            // 200 ms) so the rest of the burst resolves O(1).
            coop::prop_element_tracker::ReconcileIndexThrottled();
        }
    }
    if (existing) {
        // Host authority first, before the claim, the physics branches, the converge and the bind.
        if (HostAuthorityHandback_(existing, keyW, classW, senderSlot, "exact-key")) return;
        // The claim: the host snapshot accounts for this pre-existing client actor, so the
        // unclaimed sweep at SnapshotComplete spares it; it covers the early returns below.
        coop::join_membership_sweep::RecordClaimIfTracking(existing);
        // The read-only join-window trace: a keyed prop the host expresses onto our pre-existing
        // copy, with the host position, whether the host held it and an order stamp; no work when
        // disabled.
        if (!eidOnly && !fromConvert && !keyW.empty() && keyW != L"None" &&
            coop::dev::join_window_pos_trace::IsEnabled()) {
            const bool hostHeld = ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, existing)
                               || coop::remote_prop::IsActorUnderAnyDrive(existing);
            coop::dev::join_window_pos_trace::NoteSnapshotExpression(
                keyW, static_cast<uint32_t>(payload.elementId), existing,
                ue_wrap::FVector{payload.locX, payload.locY, payload.locZ}, hostHeld);
        }
        // The local player's grab is authoritative for what they hold (symmetric with the remote
        // drive skip below): converging a held prop kinematic would break the live physics-handle
        // hold, so it is claimed and bound with no physics or transform touch.
        if (ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, existing)) {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' is held by the LOCAL player -- skipping reconcile/converge (local grab owns it)",
                    keyW.c_str());
            coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
            return;
        }
        // A prop under active kinematic drive (the host holds it) is not converged: the write would
        // stomp the drive for one frame, a visible pop until the next PropPose. The stream owns a
        // held prop's position.
        if (coop::remote_prop::IsActorUnderAnyDrive(existing)) {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' is under active kinematic drive (some slot) -- skipping convergence (PropPose owns position)",
                    keyW.c_str());
            // The mirror is still registered, so the sender's PropDestroy resolves by eid.
            coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
            return;
        }
        // An exact-key match is the same logical base-world prop, spawned by both peers at the same
        // position; a blind teleport to the host transform wakes its rigid body, and on a heavy
        // save the snapshot woke hundreds of resting bodies at once that never re-slept, a
        // permanent ~45 ms/frame physics cost (the client at ~20 fps against the host's 116). The
        // converge runs only when the prop actually moved (2 cm / 1 degree; deterministic base
        // spawns are sub-millimetre identical), so undisturbed scenery gets no write, no wake, no
        // spike.
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
            // Aligned, the save-transfer common case: no transform write and no physics touch,
            // since the client's save-loaded copy already carries the exact single-player physics
            // state (simulating, asleep), which keeps it grabbable; an unconditional kinematic
            // reconcile here made every bound prop ungrabbable until a host grab.
            UE_LOGI("remote_prop::OnSpawn: key '%ls' resolves to live actor %p -- already aligned (d=%.2fcm), skipping teleport (no physics wake)",
                    keyW.c_str(), existing, std::sqrt(dx * dx + dy * dy + dz * dz));
        } else {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' resolves to live actor %p -- diverged (d=%.1fcm), converging transform to host (loc=(%.1f,%.1f,%.1f))",
                    keyW.c_str(), existing, std::sqrt(dx * dx + dy * dy + dz * dz),
                    payload.locX, payload.locY, payload.locZ);
            // The kinematic converge: kinematic before the teleport so the move neither wakes nor
            // ejects the body, then the single-player physics state restored at the host's rest
            // pose, where the body re-settles instead of storming.
            ReconcileToHostPhysics(existing, payload.physFlags);
            ue_wrap::engine::SetActorLocation(existing,
                ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
            ue_wrap::engine::SetActorRotation(existing,
                ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
            RestoreSpParityPhysicsAfterConverge(existing, payload.physFlags);
        }
        // The client's copy may have come through spawnedNaturally() in its own spawner, leaving
        // the mesh with no collision; the converge reuses the actor without a fresh Init, so the
        // default collision is restored, or a later host release drops the body through the floor.
        RestoreCollisionIfNeeded(L"exact-key", classW, existing);
        // No sleep write after a converge: a simulating body teleported into a divergent layout
        // penetrates and PhysX ejects it, and PutRigidBodyToSleep mid-depenetration dereferences
        // null inside PhysX. The mirror binding lets a later PropDestroy and every eid lookup
        // resolve here.
        coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
        return;
    }
    // The keyless-pile position bind, in the join bracket only (only a snapshot expression has a
    // local twin; a mid-gameplay keyless spawn has none) and never for a convert (a convert-born
    // pile has no counterpart, and binding it to a nearby pile in a dense cluster would
    // mis-mirror): chipPile-lineage candidates only, the exact wire class and chipType, the nearest
    // within 30 cm. No match falls through to the fresh mirror; a local pile the host no longer has
    // stays unclaimed and is swept.
    if (eidOnly && coop::join_membership_sweep::IsClaimTrackingActive() && !fromConvert && payload.elementId != 0) {
        // The find-and-consume lives in coop/props/pile_spawn_bind: the matched native (removed
        // from the index), the squared distance and the per-bracket log seq; the registration and
        // the physics reconcile stay here.
        float bestD2 = 0.f;
        int   nBind  = 0;
        void* pile = coop::pile_spawn_bind::FindAndConsumeAdoptCandidate(
            payload, classW, coop::join_membership_sweep::ClaimedActors(), &bestD2, &nBind);
        if (pile) {
            coop::join_membership_sweep::RecordClaimIfTracking(pile);
            // Retire the client-local identity: the save-loaded pile was census-walked with a
            // client-minted eid, and from this bind its sole cross-peer identity is the host eid.
            // Left in place, a local grab morph would make the destroy observer broadcast a stray
            // PropDestroy under the superseded eid beside the death-watch's correct one; after the
            // Unmark the observer stays silent (keyless, no eid) and the watch is the sole destroy
            // speaker.
            coop::prop_element_tracker::UnmarkKnownKeyedProp(pile);
            // The exact-key path's reconcile: kinematic to the host physics, a converge only on
            // real divergence (save-aligned piles are sub-millimetre identical, so the common case
            // writes and wakes nothing).
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
            coop::prop_element_tracker::IndexActorKey(pile, keyW);  // a None-guarded no-op, as on the fresh path
            // No death-watch enrol here: the mirror binding above lets a later local grab resolve
            // the eid in the use observer, which tells the host through PropDestroy.
            if (nBind <= 3 || (nBind % 200) == 0) {
                UE_LOGI("remote_prop_spawn: pile-bind #%d -- eid=%u '%ls' chipType=%u -> OWN local pile %p (d=%.1fcm%s)",
                        nBind, payload.elementId, classW.c_str(),
                        static_cast<unsigned>(payload.chipType), pile,
                        std::sqrt(bestD2), aligned ? ", aligned" : ", converged");
            }
            return;
        }
    }
    // No exact key: the fuzzy dedupe for divergent-key, same-position spawns. The per-peer natural
    // spawners (mushrooms, underground garbage) place the same logical entity with different Keys
    // at slightly different positions on each peer, and without this the host's broadcast would
    // spawn a second prop beside the client's. Same class within 30 cm.
    constexpr float kFuzzyRadiusCm = 30.f;
    // A clump skips the fuzzy dedupe (its rekey through setKey is meaningless) and goes to a fresh
    // eid-bound spawn.
    const ue_wrap::FVector fuzzyAnchor{payload.locX, payload.locY, payload.locZ};
    const bool traceFuzzy = coop::dev::spawn_match_probe::IsEnabled();
    ue_wrap::prop::NearbyTrace fuzzyTrace;
    // The hand axis is player expression, never a world entity, and this scan is where it can do
    // the most damage. A peer's display mirror is a real actor of the held item's class riding
    // that peer's hands, and the release broadcast a drop sends goes out BEFORE the HandItem that
    // empties the hand -- so when this scan runs the mirror is alive, the right class and inches
    // from the anchor, while the local world copy died at the pickup and cannot answer by key.
    // Adopting it rekeys the mirror to the wire key and binds the wire eid to an actor the hand
    // lane destroys one message later: the dropped prop never appears here, and its pose stream
    // addresses an actor that is gone. The same set prop_census hoists out of its own walk.
    void* handAxis[1 + coop::players::kMaxPeers];
    const ue_wrap::prop::NearbyExclusion notAdoptable{
        handAxis, coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers)};
    void* fuzzy = eidOnly ? nullptr : ue_wrap::prop::FindNearbySameClass(
            classW,
            fuzzyAnchor,
            kFuzzyRadiusCm,
            propNameW == L"None" ? std::wstring() : propNameW,
            notAdoptable,
            traceFuzzy ? &fuzzyTrace : nullptr);
    if (traceFuzzy && !eidOnly) {
        coop::dev::spawn_match_probe::NoteFuzzyScan(payload.elementId, keyW, classW, propNameW,
                                                    fuzzyAnchor, fuzzyTrace);
    }
    // The kerfur anti-collision gate: a kerfur prop's Key is cross-peer stable (save-persisted), so
    // a fuzzy match whose key differs from the wire key is a different kerfur, never rekeyed or
    // stolen; the match is dropped and a fresh spawn follows. Kerfur only: the divergent-key
    // spawners keep the dedupe.
    if (fuzzy && classW.find(L"prop_kerfurOmega") != std::wstring::npos) {
        const std::wstring fuzzyKey = ue_wrap::prop::GetKeyString(fuzzy);
        if (!fuzzyKey.empty() && fuzzyKey != L"None" && fuzzyKey != keyW) {
            UE_LOGI("remote_prop::OnSpawn: kerfur fuzzy match (wire key '%ls') resolves to a neighbor with a "
                    "DIFFERENT key '%ls' -- anti-collision: not stealing it, fresh-spawning instead",
                    keyW.c_str(), fuzzyKey.c_str());
            fuzzy = nullptr;
        }
    }
    // The identity-steal gate: a fuzzy match already mirror-bound to a different eid is an
    // established cross-peer identity, a different prop within 30 cm (N same-spot placements arrive
    // back to back and the quiescence drain applies them in one batch); stealing it would chain N
    // placements onto one actor and leave N-1 host props invisible here. The steal is only correct
    // against a local actor with no cross-peer identity (the spawner twin, never wire-bound), so
    // only the wire-mirror rows are read: the manager mixes census-walk local rows with wire rows,
    // and an unfiltered scan would resolve a save-loaded mushroom's local eid and kill the
    // legitimate dedupe. A same-eid re-express keeps the converge.
    if (fuzzy) {
        const coop::element::ElementId mirrorEid =
            coop::remote_prop::ResolveMirrorEidByActor(fuzzy, /*wireMirrorOnly=*/true);
        if (mirrorEid != coop::element::kInvalidId &&
            static_cast<uint32_t>(mirrorEid) != payload.elementId) {
            UE_LOGI("remote_prop::OnSpawn: fuzzy match %p for wire key '%ls' is already mirror-bound to "
                    "eid=%u (wire eid=%u) -- an established identity is never position-stolen; "
                    "fresh-spawning instead",
                    fuzzy, keyW.c_str(), static_cast<unsigned>(mirrorEid), payload.elementId);
            fuzzy = nullptr;
        }
    }
    if (fuzzy) {
        // Host authority before the fuzzy rekey: the setKey below would rekey the host's own actor
        // to the peer's key, an identity theft no later wall can undo.
        if (HostAuthorityHandback_(fuzzy, keyW, classW, senderSlot, "fuzzy")) return;
        // The claim, as on the exact-key path; it covers the early returns below.
        coop::join_membership_sweep::RecordClaimIfTracking(fuzzy);
        // Past every gate: this actor is the adoption, whichever of the three bindings below runs.
        coop::dev::spawn_match_probe::NoteFuzzyBound(
            fuzzy, payload.elementId, keyW,
            fuzzyTrace.candidates.empty() ? -1.f : fuzzyTrace.candidates[0].distCm);
        // The local-held guard, as on the exact-key path (reached when an earlier bracket rekeyed
        // the held prop and this one fuzzy-matches it); the rekey is skipped too, and correctness
        // rides the eid binding.
        if (ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, fuzzy)) {
            UE_LOGI("remote_prop::OnSpawn: fuzzy match '%ls' is held by the LOCAL player -- skipping reconcile/converge/rekey (local grab owns it)",
                    classW.c_str());
            coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
            return;
        }
        // The exact-key drive guard's mirror: a match under active kinematic drive is not
        // converged.
        if (coop::remote_prop::IsActorUnderAnyDrive(fuzzy)) {
            UE_LOGI("remote_prop::OnSpawn: Gap-I-1 fuzzy match '%ls' -> active drive actor (some slot) -- skipping (PropPose owns position)",
                    classW.c_str());
            // The mirror binding on the drive-skip path too, or Registry::Get(eid) would never
            // resolve for a fuzzy, drive-skipped prop.
            coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
            return;
        }
        UE_LOGI("remote_prop::OnSpawn: Gap-I-1 FUZZY MATCH '%ls' (wire key '%ls') -> existing actor %p within %.1f cm -- de-duping, converging transform + rekeying",
                classW.c_str(), keyW.c_str(), fuzzy, kFuzzyRadiusCm);
        // The kinematic converge and restore, as on the exact-key diverged branch.
        ReconcileToHostPhysics(fuzzy, payload.physFlags);
        ue_wrap::engine::SetActorLocation(fuzzy,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(fuzzy,
            ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        RestoreSpParityPhysicsAfterConverge(fuzzy, payload.physFlags);
        // Rekey the matched actor to the wire Key: it otherwise keeps its client-local Key while
        // the host's PropPose carries the wire one, and the host's grab and move stream would be
        // dropped, a prop that de-duped but is invisible to the authoritative position stream.
        // Aprop_C.setKey updates the gamemode's key map; the old entry is harmless.
        if (void* propSetKeyFn = coop::prop_fresh_spawn::PropSetKeyFn()) {
            const R::FName keyFName = ue_wrap::fname_utils::StringToFName(keyW);
            if (keyFName.ComparisonIndex != 0) {
                ParamFrame sk(propSetKeyFn);
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
        // Index the matched actor under the wire key unconditionally: resolution binds through our
        // index, independent of the engine-level setKey, and a setKey failure once left a held prop
        // on a per-packet array scan. keyW is validated at entry; the match is live.
        coop::prop_element_tracker::IndexActorKey(fuzzy, keyW);
        // The collision restore, as on the exact-key path: the match came through the client's own
        // spawner with no collision, and the host's release would drop it through the floor.
        RestoreCollisionIfNeeded(L"fuzzy", classW, fuzzy);
        // The mirror binding: the actor now carries the wire Key, so PropPose and PropDestroy
        // resolve by key or eid.
        coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
        return;
    }
    // No exact and no fuzzy match. A kerfur prop's save-loaded twin may still be async-loading (the
    // snapshot arrives before the load tail materialises it), so a fresh spawn now would duplicate
    // it; it defers to the polled class-and-pose adoption, which binds the twin when it appears or
    // fresh-spawns at quiescence. deferKerfur is false on that fallback and on a convert.
    if (deferKerfur && classW.find(L"prop_kerfurOmega") != std::wstring::npos) {
        coop::kerfur_prop_adoption::Arm(payload);
        return;
    }
    // No local match anywhere: materialise a fresh mirror (prop_fresh_spawn); with skipBind,
    // OnConvert binds the element itself.
    void* spawned = coop::prop_fresh_spawn::Materialize(payload, senderSlot, classW, keyW,
                                                        propNameW, skipBind);
    if (spawned && outSpawned) *outSpawned = spawned;
}


}  // namespace coop::remote_prop_spawn

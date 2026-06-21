// coop/remote_prop.cpp -- v4 receiver implementation.

#include "coop/remote_prop.h"

#include "coop/prop_sound.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/kerfur_entity.h"  // K-5: NotifyKerfurPropMirrorBound (client held-pose eid map)
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/prop_echo_suppress.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_stick_sync.h"  // v68: stuck wall-attachable gates (unstick + release)
#include "coop/remote_prop_spawn.h"
#include "coop/trash_channel.h"  // docs/piles/08: per-eid sync-time-context (stale carry/convert drop)
#include "coop/trash_proxy.h"    // phase 1: the host-authoritative AStaticMeshActor trash mirror (dup fix)
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/hot_path_guard.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::remote_prop {

namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// State of the prop currently being driven on this receiver. cached.actor is
// the local Aprop_C* (NOT the sender's pointer -- those are in different
// processes); cached.mesh is its StaticMeshComponent (the UPrimitiveComponent
// physics ops target). lastKey caches the wire Key we resolved for `actor` so
// per-tick PropPose with the same Key skips the GUObjectArray walk.
//
// One ActiveDrive per peer slot so multiple clients can each kinematically
// drive their own held prop concurrently: if two clients each hold a
// different prop at once, their PropPose streams must not race-overwrite
// each other on the host.
struct ActiveDrive {
    void*        actor = nullptr;
    void*        mesh = nullptr;
    std::string  lastKey;        // ASCII (the Aprop_C save UUID format); empty for a clump
    uint32_t     lastEid = 0;    // v26: Prop Element id identity for the non-keyable clump
    uint64_t     lastApplyMs = 0;
    // Host-authoritative trash proxy: NO stream-stop timeout-release. A network gap
    // mid-km-walk must FREEZE the proxy at its last pose, never drop it to physics --
    // the carry ends ONLY on an explicit reliable edge (OnRelease throw / OnConvert
    // ToPile / disconnect), which the host-authoritative trash channel always sends.
    // Set at drive (re)start. A non-proxy Aprop_C held item keeps the 500 ms timeout
    // (it has no such reliable end-of-carry guarantee). [[feedback-follow-mta-architecture]]
    bool         isProxy = false;
    // MTA-style interpolation, SCOPED TO THE TRASH PROXY (the km-walk smoothness goal): a
    // ~sendHz pose stream renders smoothly at frame rate by lerping position + rotation between
    // consecutive host poses, instead of snapping to each packet. A non-proxy Aprop_C held item
    // is EXEMPT -- it keeps its proven teleport-to-latest behavior (interpolation latency would
    // regress that unrequested). For a proxy: the first pose snaps; subsequent poses lerp from the
    // CURRENT rendered transform to the new target over the measured interval; a far jump (gap /
    // teleport) snaps. (Vehicle/ped interp shape: reference/mtasa-blue CClientPed/CClientVehicle.)
    bool              lerpSeeded = false;  // identity seeded (first pose snapped)
    bool              lerpActive = false;  // a lerp in progress (false => frozen at target)
    ue_wrap::FVector  startLoc{}, targetLoc{}, renderedLoc{};
    ue_wrap::FRotator startRot{}, targetRot{}, renderedRot{};
    uint64_t          lerpStartMs = 0;
    uint64_t          lerpDurMs   = 0;     // measured pose interval (clamped)
    uint64_t          lastPoseMs  = 0;     // arrival time of the previous pose (interval measure)
};
std::array<ActiveDrive, coop::players::kMaxPeers> g_drives{};

// Interpolation tuning. The pose interval is measured per-stream and clamped: below
// kLerpMinMs a tick-fast stream effectively snaps; above kLerpMaxMs a slow/gappy stream
// caps the lerp window so a long stall doesn't produce a multi-second crawl. A jump
// larger than kSnapDist (cm) is a teleport / post-gap catch-up -> snap, don't crawl across.
constexpr uint64_t kLerpMinMs   = 16;     // ~one 60 fps frame
constexpr uint64_t kLerpMaxMs   = 200;    // ~5 Hz floor (a gappy stream still converges)
constexpr float    kSnapDist    = 500.f;  // 5 m: beyond this, snap instead of interpolate
constexpr float    kSnapDistSq  = kSnapDist * kSnapDist;
// Per-tick dirty-gate epsilons (atv_sync::ApplyMirror discipline): at 125 Hz the interp
// step is often sub-visible -- a held-but-still clump, or a sub-cm creep. Skip the engine
// write below these (the skipped delta accumulates until it crosses). Conservative so a
// slow drift still steps invisibly (sub-pixel at gameplay scale).
constexpr float    kLerpEpsLocSq = 0.25f * 0.25f;  // 0.25 cm
constexpr float    kLerpEpsRotDeg = 0.1f;          // summed |axis delta|, degrees

// v68 sustained-stream unstick gate (prop_stick_sync design note): when a
// PropPose stream targets a STUCK wall-attachable, 1-2 stale packets may
// merely be in flight from the moment between the sender's stick COMMIT and
// its hold-break -- they must not unstick the mirror. Only a SUSTAINED stream
// (a real re-grab) does: kUnstickStreak consecutive fresh poses for the same
// identity within the streak window. Per-slot, like the drive cache itself.
struct PendingUnstick {
    void*    actor = nullptr;
    int      streak = 0;
    uint64_t lastMs = 0;
};
std::array<PendingUnstick, coop::players::kMaxPeers> g_pendingUnstick{};
constexpr int      kUnstickStreak   = 5;
constexpr uint64_t kUnstickWindowMs = 400;  // streak resets after this gap (stale burst over)

// True if `actor` is the cached drive target of ANY slot's drive state.
}  // namespace [drive helpers part 1]

// Public accessor (M-1 2026-05-29 split): used by coop::remote_prop_spawn::
// OnSpawn convergence skip path. Predicate over the drive cache; tells the
// spawn receiver "this actor is currently being kinematically driven, so
// skip transform convergence (PropPose owns position)".
bool IsActorUnderAnyDrive(void* actor) {
    // g_drives is GT-only-by-convention (T-10, no mutex). Enforce it.
    UE_ASSERT_GAME_THREAD("g_drives (IsActorUnderAnyDrive)");
    if (!actor) return false;
    for (const auto& d : g_drives) {
        if (d.actor == actor) return true;
    }
    return false;
}

namespace {  // [drive helpers part 2]

// Cached UFunction pointers for the engine PrimComp ops we call per release.
// SetSimulatePhysics + SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
// live on UPrimitiveComponent. PropThrown is on Aprop_C (BP-class lookup).
// One-shot resolve; IsLive checks re-resolve if the UClass got GC'd at a
// level unload.
//
// v5 design (post-RE 2026-05-24): the launch energy is the body's inherited
// PhysX tracking velocity captured by the HOST at the release edge (see
// research/findings/votv-throw-release-pipeline-RE-2026-05-24.md). Receiver
// applies linear + angular velocity AFTER SetSimulatePhysics(true) so the
// body resumes dynamic sim with the matching launch state. No AddImpulse
// (the impulse was the v4 partial fix; replaced cleanly per RULE 2).
void*  g_primCompCls            = nullptr;
void*  g_setSimulateFn          = nullptr;
void*  g_setLinVelFn            = nullptr;
void*  g_setAngVelFn            = nullptr;
int32_t g_setSimulateFrameSize  = 0;
int32_t g_setSimulatePSim       = -1;  // offset of bSimulate bool in the frame
int32_t g_setLinVelFrameSize    = 0;
int32_t g_setLinVelPVec         = -1;  // FVector NewVel
int32_t g_setLinVelPBone        = -1;  // FName BoneName
int32_t g_setAngVelFrameSize    = 0;
int32_t g_setAngVelPVec         = -1;
int32_t g_setAngVelPBone        = -1;
bool    g_resolved = false;

// Separately resolved (different class). Aprop_C.thrown(Player) is the BP
// event the prop's graph wires to throw-whoosh sound + particle trail.
// Calling it on the receiver is the RULE 1 natural-dispatch path -- the
// same mechanism NPCs/local player use, so sound/effects fire identically.
void*   g_propThrownFn      = nullptr;
int32_t g_propThrownFrameSize = 0;
int32_t g_propThrownPPlayer  = -1;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Lazy resolver for Aprop_C.thrown. Called separately so primary-resolved
// callers can still drive AddImpulse even if prop_C hasn't loaded yet, and
// the next caller can pick up `thrown` once it does. Idempotent.
void TryResolvePropThrown() {
    if (g_propThrownFn) return;
    void* propCls = R::FindClass(P::name::PropClass);
    if (!propCls) return;
    g_propThrownFn = R::FindFunction(propCls, P::name::PropThrownFn);
    if (!g_propThrownFn) return;
    g_propThrownFrameSize = R::FunctionFrameSize(g_propThrownFn);
    g_propThrownPPlayer  = R::FindParamOffset(g_propThrownFn, L"Player");
    UE_LOGI("remote_prop: late-resolved Aprop_C.thrown frame=%d (player@%d)",
            g_propThrownFrameSize, g_propThrownPPlayer);
}

bool ResolveUFns() {
    // Always poke the thrown resolver -- it self-gates on g_propThrownFn and
    // costs one null-check on the steady state. If g_resolved already
    // succeeded but `prop_C` had not yet loaded, this picks up `thrown` the
    // next time a release fires.
    TryResolvePropThrown();
    if (g_resolved && R::IsLive(g_primCompCls)) return true;
    g_primCompCls = R::FindClass(P::name::PrimitiveComponentClass);
    if (!g_primCompCls) {
        UE_LOGW("remote_prop: PrimitiveComponent class not found");
        return false;
    }
    g_setSimulateFn = R::FindFunction(g_primCompCls, P::name::SetSimulatePhysicsFn);
    g_setLinVelFn   = R::FindFunction(g_primCompCls, P::name::SetPhysicsLinearVelocityFn);
    g_setAngVelFn   = R::FindFunction(g_primCompCls, P::name::SetPhysicsAngularVelocityInDegreesFn);
    if (!g_setSimulateFn || !g_setLinVelFn || !g_setAngVelFn) {
        UE_LOGW("remote_prop: UFunction lookup failed (sim=%p setLin=%p setAng=%p)",
                g_setSimulateFn, g_setLinVelFn, g_setAngVelFn);
        return false;
    }
    g_setSimulateFrameSize = R::FunctionFrameSize(g_setSimulateFn);
    // Param is `bSimulate` per Engine.hpp:17349 (Hungarian b- prefix for bool).
    g_setSimulatePSim = R::FindParamOffset(g_setSimulateFn, L"bSimulate");
    // UPrimitiveComponent.SetPhysicsLinearVelocity(FVector NewVel, bool
    // bAddToCurrent, FName BoneName). Per UE4 4.27 Engine.hpp; UFunction param
    // names match exactly. bAddToCurrent we leave 0 (overwrite, not accumulate).
    g_setLinVelFrameSize = R::FunctionFrameSize(g_setLinVelFn);
    g_setLinVelPVec   = R::FindParamOffset(g_setLinVelFn, L"NewVel");
    g_setLinVelPBone  = R::FindParamOffset(g_setLinVelFn, L"BoneName");
    g_setAngVelFrameSize = R::FunctionFrameSize(g_setAngVelFn);
    g_setAngVelPVec   = R::FindParamOffset(g_setAngVelFn, L"NewAngVel");
    g_setAngVelPBone  = R::FindParamOffset(g_setAngVelFn, L"BoneName");
    if (g_setSimulatePSim < 0 ||
        g_setLinVelPVec < 0 || g_setLinVelPBone < 0 ||
        g_setAngVelPVec < 0 || g_setAngVelPBone < 0) {
        UE_LOGW("remote_prop: param offsets failed (sim=%d / linVec=%d linBone=%d / angVec=%d angBone=%d)",
                g_setSimulatePSim,
                g_setLinVelPVec, g_setLinVelPBone,
                g_setAngVelPVec, g_setAngVelPBone);
        return false;
    }
    // Aprop_C.thrown is handled by the TryResolvePropThrown() call at the
    // top of this function (which retries every call to pick up `prop_C`
    // when it loads after PrimitiveComponent). No second call here -- the
    // duplicate would be a no-op (TryResolvePropThrown self-gates on
    // g_propThrownFn != nullptr) and audit-flagged as RULE 2 dual path
    // (audit fix 2026-05-24 post-Bug-B).
    UE_LOGI("remote_prop: resolved -- SetSimulatePhysics frame=%d (sim@%d), "
            "SetPhysicsLinearVelocity frame=%d (vec@%d bone@%d), "
            "SetPhysicsAngularVelocityInDegrees frame=%d (vec@%d bone@%d), "
            "Aprop_C.thrown frame=%d (player@%d) %s",
            g_setSimulateFrameSize, g_setSimulatePSim,
            g_setLinVelFrameSize, g_setLinVelPVec, g_setLinVelPBone,
            g_setAngVelFrameSize, g_setAngVelPVec, g_setAngVelPBone,
            g_propThrownFrameSize, g_propThrownPPlayer,
            g_propThrownFn ? "OK" : "(MISSING -- natural sound disabled)");
    g_resolved = true;
    return true;
}

// Fire Aprop_C.thrown(Player) on the prop actor so the BP's sound +
// particle-trail effects play. `propActor` is the Aprop_C; `localPlayer`
// is the local mainPlayer_C ptr (BP uses it for stats; semantically we're
// crediting local for a remote throw, accepted for natural-sound benefit).
//
// Skips silently when localPlayer is null -- a null Player would feed the
// BP graph a guaranteed-null cast, almost always producing a no-op or
// null-deref. Audit-found 2026-05-24 (RULE 1: degrade gracefully, never
// dispatch a known-null into BP we can't statically inspect).
//
// Frame buffer matches DriveSimulate/DriveSetLinearVelocity/DriveSetAngularVelocity
// (64 B). PropertiesSize covers params + locals, so a BP event with
// temporaries can exceed the 8 bytes the `Player` param alone takes.
void DrivePropThrown(void* propActor, void* localPlayer) {
    if (!propActor || !g_propThrownFn || !localPlayer) return;
    unsigned char frame[64] = {};
    if (g_propThrownFrameSize > static_cast<int32_t>(sizeof(frame))) return;
    if (g_propThrownPPlayer >= 0) {
        *reinterpret_cast<void**>(frame + g_propThrownPPlayer) = localPlayer;
    }
    R::CallFunction(propActor, g_propThrownFn, frame);
}

}  // namespace [drive helpers part 2]

// Public accessors (M-1 2026-05-29 split): UPrimitiveComponent ops used
// by both the drive Tick path (PropPose stream) and the spawn receiver
// (coop::remote_prop_spawn::OnSpawn). Game-thread only.
void DriveSimulate(void* mesh, bool simulate) {
    if (!mesh) return;
    if (!ResolveUFns()) return;
    unsigned char frame[64] = {};
    if (g_setSimulateFrameSize > static_cast<int32_t>(sizeof(frame))) return;
    *reinterpret_cast<bool*>(frame + g_setSimulatePSim) = simulate;
    R::CallFunction(mesh, g_setSimulateFn, frame);
}

void DriveSetLinearVelocity(void* mesh, float vx, float vy, float vz) {
    if (!mesh) return;
    if (!ResolveUFns()) return;
    unsigned char frame[64] = {};
    if (g_setLinVelFrameSize > static_cast<int32_t>(sizeof(frame))) return;
    *reinterpret_cast<ue_wrap::FVector*>(frame + g_setLinVelPVec) =
        ue_wrap::FVector{vx, vy, vz};
    *reinterpret_cast<R::FName*>(frame + g_setLinVelPBone) = R::FName{0, 0};
    R::CallFunction(mesh, g_setLinVelFn, frame);
}

void DriveSetAngularVelocity(void* mesh, float wx, float wy, float wz) {
    if (!mesh) return;
    if (!ResolveUFns()) return;
    unsigned char frame[64] = {};
    if (g_setAngVelFrameSize > static_cast<int32_t>(sizeof(frame))) return;
    *reinterpret_cast<ue_wrap::FVector*>(frame + g_setAngVelPVec) =
        ue_wrap::FVector{wx, wy, wz};
    *reinterpret_cast<R::FName*>(frame + g_setAngVelPBone) = R::FName{0, 0};
    R::CallFunction(mesh, g_setAngVelFn, frame);
}

// Extract null-terminated wstring from a WireKey for FindByKeyString.
// Public (M-1 2026-05-29 split): used by spawn receiver + drive subsystem.
std::wstring KeyToWString(const coop::net::WireKey& k) {
    std::wstring s;
    s.reserve(k.len);
    for (uint8_t i = 0; i < k.len && i < 31; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    }
    return s;
}

namespace {  // [drive helpers part 3]

// Compare WireKey vs the cached ASCII key for the drive at `slot`. Prop
// Keys are ASCII (base64-ish save UUIDs), so byte-by-byte comparison is
// correct.
bool KeyMatchesCache(int slot, const coop::net::WireKey& k) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    const auto& d = g_drives[slot];
    if (k.len != d.lastKey.size()) return false;
    return std::memcmp(k.data, d.lastKey.data(), k.len) == 0;
}

// Scan all slots for a drive whose cached lastKey matches `k`. Returns the
// slot index, or -1 if no slot is currently driving a prop with this key.
// Used by OnRelease (peer's release packet carries a key but not a slot --
// we find which slot's drive owned it).
int FindSlotByKey(const coop::net::WireKey& k) {
    for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (g_drives[slot].actor && KeyMatchesCache(slot, k)) return slot;
    }
    return -1;
}

// v26: resolve a Prop Element id to its live mirror actor. The trash clump is
// non-keyable (setKey doesn't stick), so it's identified by OUR eid instead of the
// BP Key (PropPoseSnapshot.elementId). null on miss/dead.
void* ResolveLiveActorByEid(uint32_t eid) {
    if (eid == 0 || eid == coop::element::kInvalidId) return nullptr;
    coop::element::Element* e = coop::element::Registry::Get().Get(eid);
    if (!e) return nullptr;
    void* actor = e->GetActor();
    // IsLiveByIndex (NOT IsLive): the mirror actor is engine-GC-owned + unrooted, so a
    // GC pass between the tick that queued the pose and this one could free it. IsLive
    // would deref the freed pointer to read its InternalIndex (UAF); IsLiveByIndex reads
    // only the cached GUObjectArray slot. Index captured at RegisterPropMirror (SetActor).
    // Audit finding 2026-06-03. [[feedback-islive-unsafe-on-freed-cached-pointer]]
    return (actor && R::IsLiveByIndex(actor, e->GetInternalIdx())) ? actor : nullptr;
}

// Toggle physics on a drive target. Aprop_C props go through their StaticMesh
// (DriveSimulate, the existing path). A non-Aprop_C clump has mesh==null (the
// GetStaticMesh-null 2a-safety gate) -- it uses the GENERIC root-component physics
// instead. UAF-safe: the mirror is OUR stable spawn (it never morphs/self-frees like
// the holder's source clump), so touching its root physics is safe; the gate still
// protects the SENDER's snapshot path. [[project-bug-trash-chippile-uaf-crash]]
void DriveTogglePhysics(void* actor, void* mesh, bool simulate) {
    if (mesh) DriveSimulate(mesh, simulate);
    else if (actor && R::IsLive(actor)) ue_wrap::engine::SetActorSimulatePhysics(actor, simulate);
}

// v68: every release-shaped physics re-enable (explicit PropRelease, the
// stream-stop timeout, the switched-prop implicit release) is gated on the
// stick state. A wall-attachable that got STUCK while held (the commit fires
// mid-hold; PropStickState applied it here) must stay frozen when the
// sender's hold breaks -- the unconditional re-enable was exactly the
// "host sees the camera fall" bug.
bool StickHoldsPhysicsOff(void* actor) {
    return actor && R::IsLive(actor) && ue_wrap::prop::IsDescendantOfProp(actor) &&
           (ue_wrap::prop::IsFrozen(actor) || ue_wrap::prop::IsStatic(actor));
}

void ResolveAndStartDrive(int slot, const coop::net::PropPoseSnapshot& pose) {
    // docs/piles/08: DROP a STALE trash carry pose -- one still in flight from before the entity's last
    // transition (re-pile/throw bumps ctx). Without this, a late clump-carry pose would re-drive the
    // re-skinned actor (now a SETTLED PILE) to the dead clump position -- the stale-pose half of the
    // cluster bug. ctx==0 / unknown eid (a non-trash keyed prop) -> always fresh, so Aprop poses pass.
    if (pose.elementId != 0 &&
        !coop::trash_channel::IsInboundStreamCtxFresh(pose.elementId, pose.ctx, /*requireCurrentGen=*/true)) {
        // Throttle: a short in-flight burst of same-(eid,ctx) stale poses collapses to ONE line.
        static uint32_t s_lastDropEid = 0; static uint8_t s_lastDropCtx = 0;
        if (pose.elementId != s_lastDropEid || pose.ctx != s_lastDropCtx) {
            UE_LOGI("[PILE] CLIENT HOLD carry pose eid=%u ctx=%u known=%u (not E's current generation -- either "
                    "AHEAD of its convert (would drive the pre-convert OLD rendering -> pile-jump + the triple "
                    "grab-cue) or STALE after a later transition; applies once the matching convert lands. "
                    "known=0 => the ToClump convert was NEVER adopted, so EVERY carry pose holds forever)",
                    pose.elementId, static_cast<unsigned>(pose.ctx),
                    static_cast<unsigned>(coop::trash_channel::CtxForEid(pose.elementId)));
            s_lastDropEid = pose.elementId; s_lastDropCtx = pose.ctx;
        }
        return;
    }
    const std::wstring keyW = KeyToWString(pose.key);
    // KEY first (Aprop_C), then EID fallback (the non-keyable clump streams key=None).
    void* prop = nullptr;
    if (!keyW.empty() && keyW != L"None")
        prop = coop::prop_element_tracker::ResolveLiveActorByKey(keyW);
    if (!prop && pose.elementId != 0)
        prop = ResolveLiveActorByEid(pose.elementId);
    if (!prop) {
        UE_LOGW("remote_prop: slot %d incoming PropPose key '%ls' eid=%u -- no local match (key or eid)",
                slot, keyW.c_str(), pose.elementId);
        return;
    }
    // v68: a STUCK wall-attachable (frozen/static -- the camera on a wall)
    // unsticks for an incoming drive only on a SUSTAINED stream (a real
    // re-grab). The 1-2 stale PropPose packets in flight between the sender's
    // stick commit and its hold-break land here with the drive cache freshly
    // cleared by OnStickState -- without the streak gate they would unstick
    // the mirror right back. A genuinely-static NON-attachable prop never
    // legitimately streams; skip it outright (no unstick, no drive).
    if (ue_wrap::prop::IsDescendantOfProp(prop) &&
        (ue_wrap::prop::IsFrozen(prop) || ue_wrap::prop::IsStatic(prop))) {
        if (!coop::prop_stick_sync::IsWallAttachable(prop)) {
            UE_LOGW("remote_prop: slot %d PropPose for frozen/static non-attachable %p -- ignored", slot, prop);
            return;
        }
        PendingUnstick& pu = g_pendingUnstick[slot];
        const uint64_t now = NowMs();
        if (pu.actor != prop || now - pu.lastMs > kUnstickWindowMs) {
            pu.actor = prop;
            pu.streak = 0;
        }
        pu.lastMs = now;
        if (++pu.streak < kUnstickStreak) return;  // not yet proven a real re-grab
        pu.actor = nullptr;
        pu.streak = 0;
        coop::prop_stick_sync::UnstickForDrive(prop);  // clears flags + simulate(true)/detach
        // fall through: start the kinematic drive on the now-free prop
    }
    // GetStaticMesh returns null for the non-Aprop_C clump by design (2a safety) --
    // it's driven via the generic root physics (DriveTogglePhysics). Only a true
    // Aprop_C with a missing mesh is an error worth bailing on.
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (!mesh && ue_wrap::prop::IsDescendantOfProp(prop)) {
        UE_LOGW("remote_prop: slot %d prop %p (Aprop_C) has null StaticMesh -- cannot drive", slot, prop);
        return;
    }
    UE_LOGI("remote_prop: slot %d GRAB-IN key='%ls' eid=%u -> local actor=%p mesh=%p (%s)",
            slot, keyW.c_str(), pose.elementId, prop, mesh,
            mesh ? "Aprop physics-off" : "clump kinematic (generic physics-off)");
    // Pick-up sounds (sounds RE 2026-06-11 + hands-on round 2): both native
    // grab sounds run only in the LOCAL grabber's input chain, so synthesize
    // them here. The `use` click is THE fixed always-the-same grab feedback
    // (useAction plays it 2D grabber-only after pickupObject); the material
    // soft cue is the secondary per-material thud. prop_C reads its cached
    // physSoundData; plain-Actor grabs (the trash clump) resolve root
    // material -> physmat -> the same physSound row (silent on row miss).
    coop::prop_sound::PlayUseClick(prop);
    coop::prop_sound::PlayGrabSound(prop);
    // Disable PhysX simulation so the per-packet SetActorLocation sticks (a clean
    // kinematic follow -- the clump uses the generic root toggle, NOT a fight-the-sim
    // crutch). The clump floats in front of the puppet exactly like the mannequin.
    DriveTogglePhysics(prop, mesh, false);
    g_drives[slot].actor = prop;
    g_drives[slot].mesh  = mesh;
    g_drives[slot].lastKey.assign(pose.key.data, pose.key.len);
    g_drives[slot].lastEid = pose.elementId;
    // Host-authoritative trash proxy? -> freeze-not-timeout end-of-carry policy (a
    // network gap mid-km-walk freezes; release only on the explicit reliable edge).
    g_drives[slot].isProxy = (pose.elementId != 0 && coop::trash_proxy::IsProxy(pose.elementId));
    // Fresh drive identity: the next pose SNAPS (no drift-in from the spawn/rest
    // position), then subsequent poses interpolate.
    g_drives[slot].lerpSeeded = false;
    g_drives[slot].lerpActive = false;
    // Seed the timeout clock with NOW. Without this, lastApplyMs stays at
    // zero (struct default); the stream-stop timeout at Tick() compares
    // (NowMs() - lastApplyMs > 500) and fires immediately, releasing the
    // grab on the very first packet drop / late-arrival after a fresh
    // grab. See research/findings/votv-coop-audit-post-pr4-7-2026-05-28.md.
    g_drives[slot].lastApplyMs = NowMs();
}

// Shortest-path angular interpolation (handles the +/-180 wrap so a yaw crossing
// 179 -> -179 lerps 2 deg, not 358).
float LerpAngle(float a, float b, float t) {
    return a + ue_wrap::NormalizeAxis(b - a) * t;
}

// Reset a drive slot to idle. ONE implementation -- every clear site funnels here, so
// the lerp + proxy state can never be left half-cleared (RULE 2). Does NOT touch
// physics; the caller decides whether to re-enable simulation (a release) or leave it
// off (a stick / a freeze).
void ResetDriveState(ActiveDrive& d) {
    d.actor      = nullptr;
    d.mesh       = nullptr;
    d.lastKey.clear();
    d.lastEid    = 0;
    d.isProxy    = false;
    d.lerpSeeded = false;
    d.lerpActive = false;
}

// Record a new host pose as the lerp TARGET. First pose for an identity (or a far jump)
// snaps; otherwise interpolate from the CURRENT rendered transform over the measured pose
// interval. Game thread.
void BeginLerpToPose(ActiveDrive& d, const coop::net::PropPoseSnapshot& pose, uint64_t nowMs) {
    const ue_wrap::FVector  nloc{pose.x, pose.y, pose.z};
    const ue_wrap::FRotator nrot{pose.pitch, pose.yaw, pose.roll};
    const float dx = nloc.X - d.renderedLoc.X, dy = nloc.Y - d.renderedLoc.Y, dz = nloc.Z - d.renderedLoc.Z;
    // Interpolation is scoped to the host-authoritative trash PROXY (the km-walk smoothness
    // requirement). A non-proxy Aprop_C held item keeps its proven teleport-to-latest behavior
    // (snap every pose) -- adding interpolation latency to it would regress unrequested, proven
    // sync. So a non-proxy ALWAYS snaps; a proxy snaps only on the first pose or a far jump.
    if (!d.isProxy || !d.lerpSeeded || (dx * dx + dy * dy + dz * dz) > kSnapDistSq) {
        // Snap: non-proxy (teleport), OR first pose (no drift-in from spawn/rest), OR a jump too
        // large to interpolate (post-gap catch-up / teleport -- crawling across the map would
        // look worse than a snap).
        d.startLoc = d.targetLoc = d.renderedLoc = nloc;
        d.startRot = d.targetRot = d.renderedRot = nrot;
        if (d.actor && R::IsLive(d.actor)) {
            E::SetActorLocation(d.actor, nloc);
            E::SetActorRotation(d.actor, nrot);
        }
        d.lerpActive = false;       // already at target (frozen until the next pose)
        d.lerpSeeded = true;
        d.lastPoseMs = nowMs;
        return;
    }
    uint64_t interval = nowMs - d.lastPoseMs;
    if (interval < kLerpMinMs) interval = kLerpMinMs;
    if (interval > kLerpMaxMs) interval = kLerpMaxMs;
    d.lastPoseMs  = nowMs;
    d.startLoc    = d.renderedLoc;  // continue from where we are actually rendering
    d.startRot    = d.renderedRot;
    d.targetLoc   = nloc;
    d.targetRot   = nrot;
    d.lerpStartMs = nowMs;
    d.lerpDurMs   = interval;
    d.lerpActive  = true;
}

// Advance an in-progress lerp one tick (called EVERY tick, not only on a new pose, so
// the follow is smooth and a stream gap FREEZES at the target rather than dropping).
// No-op when no lerp is active (frozen) or the actor died. Game thread.
void AdvanceLerp(ActiveDrive& d, uint64_t nowMs) {
    if (!d.lerpActive || !d.actor) return;
    if (!R::IsLive(d.actor)) { d.lerpActive = false; return; }
    float alpha = d.lerpDurMs
        ? static_cast<float>(nowMs - d.lerpStartMs) / static_cast<float>(d.lerpDurMs) : 1.f;
    if (alpha < 0.f) alpha = 0.f;
    if (alpha > 1.f) alpha = 1.f;
    const bool done = (alpha >= 1.f);
    const ue_wrap::FVector loc{
        d.startLoc.X + (d.targetLoc.X - d.startLoc.X) * alpha,
        d.startLoc.Y + (d.targetLoc.Y - d.startLoc.Y) * alpha,
        d.startLoc.Z + (d.targetLoc.Z - d.startLoc.Z) * alpha };
    const ue_wrap::FRotator rot{
        LerpAngle(d.startRot.Pitch, d.targetRot.Pitch, alpha),
        LerpAngle(d.startRot.Yaw,   d.targetRot.Yaw,   alpha),
        LerpAngle(d.startRot.Roll,  d.targetRot.Roll,  alpha) };
    // Dirty-gate: skip the SetActorLocation/Rotation (each a ParamFrame heap alloc +
    // ProcessEvent) when neither position nor any axis moved beyond an epsilon since the
    // LAST WRITTEN transform. A held-but-still clump (target == last pose) or a sub-cm
    // creep then costs ZERO engine writes; the skipped delta accumulates (renderedLoc/Rot
    // unchanged) until it crosses the epsilon, then one write catches up. Active motion
    // (> eps/tick) writes every tick -> still smooth. On the final frame we still skip if
    // already within epsilon (the actor is there) but ALWAYS deactivate.
    const float dl = (loc.X - d.renderedLoc.X) * (loc.X - d.renderedLoc.X)
                   + (loc.Y - d.renderedLoc.Y) * (loc.Y - d.renderedLoc.Y)
                   + (loc.Z - d.renderedLoc.Z) * (loc.Z - d.renderedLoc.Z);
    const float dr = std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - d.renderedRot.Pitch))
                   + std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - d.renderedRot.Yaw))
                   + std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - d.renderedRot.Roll));
    if (dl < kLerpEpsLocSq && dr < kLerpEpsRotDeg) {
        if (done) d.lerpActive = false;  // settled within epsilon -> stop (no redundant write)
        return;
    }
    E::SetActorLocation(d.actor, loc);
    E::SetActorRotation(d.actor, rot);
    d.renderedLoc = loc;
    d.renderedRot = rot;
    if (done) d.lerpActive = false;  // reached target -> freeze
}

}  // namespace

void Tick(coop::net::Session& session) {
    // Drives g_drives across all slots (T-10, GT-only). Called every game-
    // thread tick from net_pump::Tick.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::Tick)");
    if (!session.connected()) {
        ForceRelease();
        return;
    }
    // Per-slot drive iteration. On HOST: scan slots 1..kMaxPeers-1 (each
    // connected client can drive its own held prop independently). On
    // CLIENT: scan slot 0 only (the host's puppet is the only prop-pose
    // source from the client's POV).
    const bool isHost = (session.role() == coop::net::Role::Host);
    const int firstSlot = isHost ? 1 : 0;
    const int lastSlot  = isHost ? static_cast<int>(coop::players::kMaxPeers) : 1;
    const uint64_t nowMs = NowMs();
    for (int slot = firstSlot; slot < lastSlot; ++slot) {
        ActiveDrive& drive = g_drives[slot];
        coop::net::PropPoseSnapshot pose{};
        bool isNew = false;
        const bool have = session.TryGetRemotePropPose(slot, pose, &isNew);
        if (have && isNew) {
            // First snapshot OR identity changed (key OR eid) -> resolve + physics-off.
            // The eid check catches a re-grab where the clump's key stays None but a
            // NEW clump (new eid) is held -- otherwise the empty-key cache would keep
            // driving the OLD released clump.
            if (!drive.actor || !KeyMatchesCache(slot, pose.key) || drive.lastEid != pose.elementId) {
                if (drive.actor) {
                    // The peer at this slot switched to a different prop
                    // without sending Release -- implicit release (re-enable
                    // physics on the prior body; generic for a clump). v68:
                    // unless a stick froze it mid-hold (then physics stays off).
                    UE_LOGI("remote_prop: slot %d implicit release (peer switched to a new key/eid)", slot);
                    if (!StickHoldsPhysicsOff(drive.actor))
                        DriveTogglePhysics(drive.actor, drive.mesh, true);
                    ResetDriveState(drive);
                }
                ResolveAndStartDrive(slot, pose);
            }
            if (drive.actor && R::IsLive(drive.actor)) {
                // Record the pose as the new lerp TARGET (AdvanceLerp below moves the
                // actor toward it every tick -- a smooth follow, not a per-packet teleport).
                BeginLerpToPose(drive, pose, nowMs);
                drive.lastApplyMs = nowMs;
                // Throttled target log: first 3 + every 60th after per slot.
                static std::array<uint64_t, coop::players::kMaxPeers> sApplyCount{};
                const uint64_t n = ++sApplyCount[slot];
                if (n <= 3 || (n % 60) == 0) {
                    UE_LOGI("remote_prop: slot %d drive #%llu -> target(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f)%s",
                            slot, static_cast<unsigned long long>(n),
                            pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll,
                            drive.isProxy ? " [proxy]" : "");
                }
            } else if (drive.actor) {
                // The cached actor died (level unload / GC). Drop cleanly.
                UE_LOGW("remote_prop: slot %d cached actor no longer live -- dropping drive", slot);
                ResetDriveState(drive);
            }
        }
        // Advance the interpolation EVERY tick (whether or not a new pose arrived): a
        // smooth frame-rate follow between ~sendHz poses, and a stream gap FREEZES at the
        // last target instead of stalling at the last packet's raw position.
        AdvanceLerp(drive, nowMs);
        // Stream-stop implicit release -- ONLY for a non-proxy Aprop_C held item (which has
        // no reliable end-of-carry guarantee, so 500 ms of silence == released). A host-
        // authoritative trash PROXY is EXEMPT: a network gap must FREEZE it (AdvanceLerp
        // already holds it at the last target), and it releases only on the explicit reliable
        // edge (OnRelease throw / OnConvert ToPile / disconnect). THIS is the km-walk
        // robustness fix -- a hitch no longer drops the carried pile to physics mid-walk.
        if (drive.actor && !drive.isProxy && (nowMs - drive.lastApplyMs) > 500) {
            UE_LOGI("remote_prop: slot %d implicit release (%llu ms since last PropPose)",
                    slot, static_cast<unsigned long long>(nowMs - drive.lastApplyMs));
            if (!StickHoldsPhysicsOff(drive.actor))
                DriveTogglePhysics(drive.actor, drive.mesh, true);
            ResetDriveState(drive);
        }
    }
}

void OnRelease(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer) {
    // Reads + clears g_drives[senderSlot] (T-10, GT-only). Dispatched from
    // event_feed on the game thread.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnRelease)");
    const std::wstring keyW = KeyToWString(payload.key);
    const float linSpeedSq = payload.linVelX * payload.linVelX +
                             payload.linVelY * payload.linVelY +
                             payload.linVelZ * payload.linVelZ;
    const float linSpeed = std::sqrt(linSpeedSq);
    UE_LOGI("remote_prop: RELEASE wire '%ls' eid=%u ctx=%u linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f) deg/s",
            keyW.c_str(), payload.elementId, static_cast<unsigned>(payload.ctx),
            payload.linVelX, payload.linVelY, payload.linVelZ, linSpeed,
            payload.angVelX, payload.angVelY, payload.angVelZ);
    // docs/piles/08: a trash entity (keyless clump) is identified by EID, not key. DROP a STALE release
    // (ctx older than E's last transition) so a throw delayed past a re-pile / re-grab can never re-apply
    // velocity to the re-skinned entity. ctx==0 / eid==0 (a keyed Aprop release) -> always fresh, as before.
    if (payload.elementId != 0 &&
        !coop::trash_channel::IsInboundStreamCtxFresh(payload.elementId, payload.ctx, /*requireCurrentGen=*/false)) {
        UE_LOGI("[PILE] CLIENT DROP stale release eid=%u ctx=%u (older than E's last transition)",
                payload.elementId, static_cast<unsigned>(payload.ctx));
        return;
    }
    // Identify the releasing peer by sender slot (carried by the
    // ReliableMessage envelope) rather than by key. FindSlotByKey
    // linear-scans + returns first-match-wins which would clear the
    // wrong slot if two slots briefly held a prop with the same
    // lastKey (e.g., race on duplicated key, or a stale drive whose
    // owner already disconnected). Sender-attribution is exact.
    void* propActor = nullptr;
    void* meshToActOn = nullptr;
    int releasedSlot = -1;
    if (senderSlot >= 0 && senderSlot < static_cast<int>(coop::players::kMaxPeers)) {
        const ActiveDrive& d = g_drives[senderSlot];
        // Confirm the drive's lastKey matches this release's key. If it
        // doesn't, the sender's drive cache is stale (e.g., a release
        // arrived for a key we never saw a PropPose for, or out-of-order
        // after a disconnect cancel). Fall through to the FindByKeyString
        // fresh-resolve path below. docs/piles/08: a keyless clump (key=None)
        // matches ANY clump the slot carries, so ALSO require the slot's
        // driven eid == this release's eid -- else a late E1 throw lands on
        // E2 after a re-grab (the audit-flagged cross-slot mis-apply).
        if (d.actor && KeyMatchesCache(senderSlot, payload.key) &&
            (payload.elementId == 0 || d.lastEid == payload.elementId)) {
            releasedSlot = senderSlot;
            propActor = d.actor;
            meshToActOn = d.mesh;
        }
    }
    if (releasedSlot < 0) {
        if (void* prop = coop::prop_element_tracker::ResolveLiveActorByKey(keyW)) {
            // Release arrived without a matching drive cache entry --
            // resolve fresh from the live world (a keyed Aprop).
            propActor = prop;
            meshToActOn = ue_wrap::prop::GetStaticMesh(prop);
        } else if (payload.elementId != 0) {
            // docs/piles/08: a keyless trash clump whose slot already moved on (E still a live clump
            // elsewhere) -- resolve by eid so its throw still applies to the right entity.
            if (void* prop2 = ResolveLiveActorByEid(payload.elementId)) {
                propActor = prop2;
                meshToActOn = ue_wrap::prop::GetStaticMesh(prop2);
            }
        }
    }
    if (StickHoldsPhysicsOff(propActor)) {
        // v68: the prop got STUCK while this peer held it (PropStickState
        // landed before this release -- same reliable lane keeps the order).
        // The release must NOT re-enable physics / write velocity: the stuck
        // camera stays on the wall. Drive cache still clears below.
        UE_LOGI("remote_prop: RELEASE for stuck wall-attachable %p -- physics stays off (v68)",
                propActor);
        meshToActOn = nullptr;
        propActor = nullptr;
    }
    // Host-authoritative trash proxy throw: do NOT simulate locally (phase-1 NoCollision
    // follower -- local physics would both diverge from the host's authoritative trajectory
    // AND re-enable collision, breaking the phase-1 invariant). The proxy FREEZES at the
    // release pose; the host's reliable ToPile convert re-skins + repositions it to the
    // landed pile. The receiver-side swing still plays so the throw has audible feedback.
    // (Phase 2 may stream the flight for a smooth arc instead of the brief freeze.)
    if (payload.elementId != 0 && coop::trash_proxy::IsProxy(payload.elementId)) {
        if (propActor && linSpeed > coop::net::kThrownLinVelThreshold)
            coop::prop_sound::PlayThrowWhoosh(propActor);
        if (propActor) ClearAnyDriveFor(propActor);  // stop the carry drive; freeze in place
        UE_LOGI("[PILE] CLIENT proxy throw eid=%u |v|=%.1f cm/s -- frozen at release (no local physics); "
                "awaiting host ToPile convert to reposition to the landed pile",
                payload.elementId, linSpeed);
    } else if (meshToActOn) {
        // Order matters: SetSimulate(true) FIRST so the body re-enters
        // dynamic sim BEFORE we write velocity (a kinematic body's velocity
        // write would be ignored / cause a PhysX kinematic-target chase).
        DriveSimulate(meshToActOn, true);
        DriveSetLinearVelocity(meshToActOn, payload.linVelX, payload.linVelY, payload.linVelZ);
        DriveSetAngularVelocity(meshToActOn, payload.angVelX, payload.angVelY, payload.angVelZ);
        // Throw dispatch, gated on linear speed: a passive drop (residual walk
        // velocity ~30 cm/s) shouldn't whoosh; a deliberate throw (>200 cm/s,
        // per kThrownLinVelThreshold) should.
        //   - Aprop_C.thrown(Player): subclass hooks (particles etc.). The BASE
        //     prop_C thrown() is a NO-OP (uber @465 POP->ret, sounds RE
        //     2026-06-11 par.3) -- it never made the whoosh; the native whoosh
        //     is PlaySound2D(swing) in the THROWER's input chain, thrower-only.
        //   - PlayThrowWhoosh: the receiver-side spatialized swing (the audible
        //     part this branch was silently missing).
        if (propActor && linSpeed > coop::net::kThrownLinVelThreshold) {
            DrivePropThrown(propActor, localPlayer);
            coop::prop_sound::PlayThrowWhoosh(propActor);
            UE_LOGI("remote_prop: fired Aprop_C.thrown(player=%p) + swing whoosh -- launch speed %.1f cm/s > threshold %.1f",
                    localPlayer, linSpeed, coop::net::kThrownLinVelThreshold);
        }
    } else if (propActor && R::IsLive(propActor)) {
        // Non-Aprop_C clump (null mesh): re-enable physics + throw velocity via the GENERIC
        // root path so the mirror flies. Use PhysicsOnly collision (2, NOT QueryAndPhysics):
        // the PhysX contacts still make it fall + land + rest on the floor during its brief
        // flight, but the Query facet is DROPPED so the host's grab sphere-trace
        // (mainPlayer::useArm -> SphereTraceSingle, a query by trace channel) can NOT hit the
        // resting mirror. DUPE FIX (2026-06-08): a mirror is a PUPPET -- with QueryAndPhysics
        // the host could grab the resting mirror during the ~3 s window before the owner's
        // authoritative pile arrives, minting a real held clump ON TOP OF the pile (the user's
        // repro). The landed PILE is a SEPARATE spawn (trash_collect_sync -> remote_prop_spawn,
        // never through here) and keeps BP-default QueryAndPhysics, so both peers can still grab
        // the final pile. (We also do NOT prime the mirror to self-convert: the BP impulse/slope
        // gates made that unreliable ~3/10.) [[project-bug-trash-chippile-uaf-crash]] +
        // research/findings/votv-clump-mirror-grab-RE-2026-06-08.md
        ue_wrap::engine::SetActorRootCollisionEnabled(propActor, 2 /*PhysicsOnly -- lands but ungrabbable*/);
        ue_wrap::engine::SetActorSimulatePhysics(propActor, true);
        ue_wrap::engine::SetActorRootPhysicsVelocity(
            propActor,
            ue_wrap::FVector{payload.linVelX, payload.linVelY, payload.linVelZ},
            ue_wrap::FVector{payload.angVelX, payload.angVelY, payload.angVelZ});
        UE_LOGI("[PILE] CLIENT applied THROW eid=%u -> clump mirror physics on + velocity |v|=%.1f cm/s (actor=%p) "
                "-- it now flies + lands; the host's LAND convert will re-skin it to a pile",
                payload.elementId, linSpeed, propActor);
        // Throw WHOOSH: the clump is a plain AActor with no Aprop_C.thrown() to
        // fire. Same receiver-side swing as the Aprop_C branch (byte-exact RE
        // 2026-06-11: EVERY LMB throw plays `swing` regardless of prop type --
        // the earlier clump-only object_throw was a pre-RE guess, superseded
        // per RULE 2). Same speed gate so a passive drop is silent.
        if (linSpeed > coop::net::kThrownLinVelThreshold) {
            coop::prop_sound::PlayThrowWhoosh(propActor);
        }
    }
    // Clear only the matching slot's drive state -- other slots' active
    // drives (if any) stay intact.
    if (releasedSlot >= 0) {
        ResetDriveState(g_drives[releasedSlot]);
    }
}

namespace {

// PropSpawn UFunction resolution + OnSpawn impl moved to coop::
// remote_prop_spawn (M-1 2026-05-29 split). This TU owns the drive
// (PropPose stream) + OnRelease + OnDestroy + ForceRelease + per-slot
// disconnect. See coop/remote_prop_spawn.h for the receiver.

// A2 (2026-05-29) -- wire-received Prop mirrors. Each entry holds a Prop
// Element bound (via Registry::RegisterMirror) to the SENDER's elementId.
// The id is allocation-foreign to this peer (host range when we're client
// receiving from host; peer range when we're host receiving from client).
// Mirror destruction routes to Registry::UnregisterMirror (NOT FreeId)
// because the id belongs to the sender's allocator -- see [[feedback-
// registry-register-mirror-pattern]].
//
// Lifetime:
//   - Created in OnSpawn when the wire payload carries a non-zero
//     elementId AND we successfully resolved (or spawned) a local actor.
//   - Dropped in OnDestroy when the matching destroy packet arrives.
//   - Drained in ForceRelease() / on full session disconnect (we don't
//     try per-slot mirror eviction; convergent props persist across
//     per-slot disconnect just like remote_prop's actors do).
//
// Convergence case (local actor existed BEFORE the wire packet, exact-key
// or fuzzy match): we register a mirror at sender's eid pointing to the
// existing local actor. That actor is ALSO tracked by prop_element_tracker's
// LOCAL Prop Element in this peer's allocation range (host or peer). Two
// Elements per actor is acceptable: one is the LOCAL handle (m_mirror=false,
// our own alloc range), the other is the MIRROR handle (m_mirror=true,
// sender's alloc range). Both resolve to the same actor via GetActor(). The
// Registry's m_byId slots don't collide because host/peer ranges are disjoint.
// B2 (2026-05-29): per-type mirror map backed by the generic
// coop::element::MirrorManager<Prop>. The encapsulated 5-step pattern
// (alloc-under-lock -> RegisterMirror -> rollback-on-fail) lives in the
// template; this file's RegisterPropMirror/UnregisterPropMirror/
// DrainWirePropMirrors become thin wrappers that handle prop-specific
// concerns (name/typeName/actor stamping, log lines).
//
// PR-FOUNDATION-3 Inc3 (2026-05-30): this is now the SINGLE owner of ALL Prop
// Elements -- both these wire mirrors AND prop_element_tracker's locals (which
// it AllocAndInstall's into this same Instance()). So the disconnect drain
// must be SELECTIVE: DrainWirePropMirrors -> DrainMirrorsOnly() releases just
// the m_mirror=true entries; the locals (engine-state shadows of persistent
// world props) stay so they survive a reconnect.
inline coop::element::MirrorManager<coop::element::Prop>& PropMirrors() {
    return coop::element::MirrorManager<coop::element::Prop>::Instance();
}

// Lossy-narrow a wstring to ASCII for Element name/typeName storage. The
// VOTV Key strings + class names are ASCII in practice (BP-minted NewGuid
// + class identifiers like "Aprop_chipPile_C").
std::string NarrowAscii(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
    return s;
}

// Register a Prop mirror Element at `eid` bound to `actor`. Idempotent:
// if `eid` already has a mirror in our map, no-op (a re-spawn convergence
// packet for the same eid is silently absorbed). Validation:
//   - `eid == 0`: wire sentinel meaning "sender had no Element minted" --
//     skip (legacy / pre-v12 senders / unminted-on-sender props).
//   - `eid == kInvalidId`: defensive (the C++ sentinel shouldn't appear
//     on the wire, but reject loudly if it does).
//   - Registry::RegisterMirror enforces in-range; we don't pre-check.
//
// Mirror pattern (5-step protocol per [[feedback-registry-register-mirror-
// pattern]]):
//   1. make_unique<Prop>
//   2. emplace into our owner map FIRST (under g_propMirrorsMutex)
//   3. Registry::RegisterMirror SECOND
//   4. On RegisterMirror failure: drain from owner map outside the lock
//   5. (Teardown path lives in OnDestroy + ForceRelease)
}  // namespace [spawn helpers / mirror-manager]

// Public accessor (M-1 2026-05-29 split): used by coop::remote_prop_spawn::
// OnSpawn to bind a wire-received Prop Element after every successful
// spawn/converge path.
void RegisterPropMirror(coop::element::ElementId eid,
                        void* actor,
                        const std::wstring& key,
                        const std::wstring& cls,
                        int senderSlot,
                        bool rebindInPlace) {
    if (!actor) return;
    // Fork B 2b (2026-06-10): quiet idempotency. Under the relaxed snapshot
    // gate the OWNER client re-ingests its own entities at every re-bracket
    // -- the bind paths resolve its OWN element's actor and re-register the
    // same (eid, actor) pair. Short-circuit before Install so the manager's
    // duplicate path doesn't warn once per entity per re-bracket.
    if (auto* existing = coop::element::Registry::Get().Get(eid)) {
        if (existing->GetActor() == actor) return;
        // v81 MORPH V2: the SINGLE rebind entry point. Re-skin eid E onto the new rendering (pile-A ->
        // clump -> pile-B). HEAD keeps the existing live actor (a different live actor for the same eid
        // is a conflict to reject); the morph LEGITIMATELY swaps the actor (the old one is destroyed
        // right after). Route on the Element's AUTHORITATIVE m_mirror flag -- NOT a caller's runtime
        // guess (findings 3/6/7/15): a MIRROR rebinds via SetActor here; a LOCAL element (a host
        // applying a client's convert against its OWN pile) MUST go through RebindLocalElementActor so
        // the forward map g_actorToPropElementId stays consistent. Only the morph callers pass true.
        if (rebindInPlace) {
            if (existing->IsMirror()) {
                existing->SetActor(actor, R::InternalIndexOf(actor));
                // Keep the K-5 client kerfur held-pose map consistent if this is a kerfur mirror (it
                // won't be for a chipPile/clump morph -- self-filters on class).
                coop::kerfur_entity::NotifyKerfurPropMirrorBound(actor, eid);
                UE_LOGI("remote_prop::RegisterPropMirror: eid=%u REBOUND mirror in place -> actor=%p "
                        "cls='%ls' (morph re-skin)", eid, actor, cls.c_str());
            } else {
                coop::prop_element_tracker::RebindLocalElementActor(eid, actor);
            }
            return;
        }
        // else: fall through to Install, which rejects the duplicate eid (HEAD live-conflict guard).
    }
    // Tag with the originating peer slot for per-slot disconnect eviction
    // (D1-7). Out-of-range/unknown -> -1 (untagged; only drained on full
    // teardown). kMaxPeers bounds it to the 4-peer slot space.
    const int ownerSlot =
        (senderSlot >= 0 && senderSlot < static_cast<int>(coop::players::kMaxPeers))
            ? senderSlot : -1;
    // Build the Prop mirror with name/typeName/actor populated, then
    // hand off to MirrorManager::Install. The template encapsulates
    // the 5-step pattern (alloc-under-lock + RegisterMirror +
    // rollback-on-fail with dtor outside lock), so the wire-receiver
    // side just builds and ships.
    auto mirror = std::make_unique<coop::element::Prop>();
    if (!key.empty()) mirror->SetName(NarrowAscii(key));
    if (!cls.empty()) mirror->SetTypeName(NarrowAscii(cls));
    mirror->SetActor(actor, R::InternalIndexOf(actor));
    if (PropMirrors().Install(eid, std::move(mirror), ownerSlot)) {
        UE_LOGI("remote_prop::RegisterPropMirror: eid=%u bound to actor=%p "
                "key='%ls' cls='%ls' ownerSlot=%d",
                eid, actor, key.c_str(), cls.c_str(), ownerSlot);
        // K-5: if this is a CLIENT kerfur prop mirror, record actor->host-range-eid so local_streams
        // can stream the eid while it's carried (its random BP key won't resolve cross-peer; the host's
        // remote_prop receiver resolves the authoritative kerfur prop by this eid). Self-filters to
        // client + kerfur class (no-op for every ordinary prop / on the host). The single choke-point
        // every kerfur prop mirror bind funnels through (convert materialize, join-snapshot, fuzzy-adopt).
        coop::kerfur_entity::NotifyKerfurPropMirrorBound(actor, eid);
    }
    // On false: Install already logged the failure case (rejected
    // sentinel id, duplicate, or RegisterMirror failure).
}

// Reverse lookup: the eid bound to `actor` among the Prop Elements (mirrors + locals all live in this
// one manager). The forward map (prop_element_tracker) is the fast O(1) path for OWNED props; this is
// the MIRROR-side fallback the chipPile grab hook uses when a CLIENT grabs a host-owned pile (whose
// mirror is NOT in the local forward map). O(n) but reached only on the rare grab edge after the
// forward map missed. Snapshot copies the raw pointers under the manager lock, then we iterate
// lock-free (a concurrently-dropped element would just fail the GetActor compare -- never a UAF here
// since we only read GetActor/GetId, not the engine actor).
coop::element::ElementId ResolveMirrorEidByActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    std::vector<coop::element::Prop*> snap;
    PropMirrors().Snapshot(snap);
    for (coop::element::Prop* p : snap) {
        if (p && p->GetActor() == actor) return p->GetId();
    }
    return coop::element::kInvalidId;
}

namespace {  // [spawn helpers continued]

// Drop a Prop mirror by sender's eid. Drain pattern lives inside
// MirrorManager::Take (extract under lock, drained-destructs outside).
// Idempotent: silent no-op if eid is not in the map. Use Take (not Drop)
// so the "drained" log line only fires when a mirror actually was
// present -- avoids a misleading confirmation on PropDestroy bounces
// for eids that were never registered locally (audit fix 2026-05-29).
void UnregisterPropMirror(coop::element::ElementId eid) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    auto drained = PropMirrors().Take(eid);
    if (!drained) return;
    // K-5: evict the client held-pose map symmetrically (it is populated at RegisterPropMirror). This
    // is the general mirror-teardown choke-point -- covers a kerfur prop mirror dropped by a plain
    // PropDestroy (not just the conversion path's explicit evict). No-op for a non-kerfur mirror / on
    // the host (the map is empty there). GetKerfurMirrorEidForActor also self-heals, but evicting here
    // keeps the map tight + the lifecycle symmetric (RegisterPropMirror populates <-> this evicts).
    coop::kerfur_entity::ForgetKerfurPropMirror(drained->GetActor());
    UE_LOGI("remote_prop::UnregisterPropMirror: eid=%u drained", eid);
    // drained falls out of scope here; ~Prop -> ~Element ->
    // Registry::UnregisterMirror (via m_mirror=true).
}

// Drain the WIRE MIRRORS on full session teardown. Forwards to
// MirrorManager::DrainMirrorsOnly -- NOT DrainAll: since Inc3 the manager also
// owns prop_element_tracker's LOCAL Prop Elements (m_mirror=false, shadows of
// this peer's persistent world props), and DrainAll would wrongly destroy them
// (they must survive a reconnect -- the keyed-prop seed scan is one-shot per
// process). Only m_mirror=true entries are released here; each drained mirror's
// dtor calls Registry::UnregisterMirror sequentially as the vector releases.
size_t DrainWirePropMirrors() {
    return PropMirrors().DrainMirrorsOnly();
}


}  // namespace


namespace {
// Cached K2_DestroyActor UFunction for receiver-side destroys.
void* g_destroyActorFn = nullptr;
void* g_actorCls       = nullptr;

bool ResolveDestroyFn() {
    if (g_destroyActorFn && R::IsLive(g_actorCls)) return true;
    g_actorCls = R::FindClass(P::name::ActorClassName);
    if (!g_actorCls) return false;
    g_destroyActorFn = R::FindFunction(g_actorCls, P::name::DestroyActorFn);
    return g_destroyActorFn != nullptr;
}

}  // namespace

void ClearAnyDriveFor(void* actor) {
    // Clear every slot's kinematic-drive cache entry for `actor` so nothing
    // drives a destroyed actor next tick. Extracted from OnDestroy (Fork B
    // 2e, 2026-06-10) because the adoption sweep destroys actors through the
    // same teardown contract; one implementation (RULE 2).
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::ClearAnyDriveFor)");
    if (!actor) return;
    for (auto& d : g_drives) {
        if (d.actor == actor) {
            UE_LOGI("remote_prop: actor %p was under active kinematic drive (slot %td) -- clearing drive cache",
                    actor, std::distance(&g_drives[0], &d));
            ResetDriveState(d);
        }
    }
}

void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer) {
    // Clears g_drives[*] if the destroyed actor was under drive (T-10, GT-only).
    // Dispatched from event_feed::Update on the game thread (PropDestroy case).
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnDestroy)");
    // PROXY PATH (phase 1): a trash proxy mirror is retired through its OWN teardown (Destroy ->
    // RemoveFromRoot -> unbind). The rooted AStaticMeshActor MUST be un-rooted here or it leaks its
    // GUObjectArray slot forever. Trash rides key=None + an eid; return before the legacy keyed/BP path.
    if (payload.elementId != 0 && payload.elementId != coop::element::kInvalidId &&
        coop::trash_proxy::IsProxy(payload.elementId)) {
        coop::trash_proxy::RetireProxy(payload.elementId);
        return;
    }
    const std::wstring keyW = KeyToWString(payload.key);
    // Resolve the doomed actor BEFORE draining the mirror. KEYED props resolve by Key (a
    // separate key->actor index). The NON-KEYABLE trash clump rides key=None + an eid and
    // resolves THROUGH the mirror Registry (ResolveLiveActorByEid -> Registry::Get(eid)),
    // so UnregisterPropMirror MUST come AFTER this lookup -- draining first makes the eid
    // resolve null and the mirror is never destroyed (the 2026-06-03 clump-dupe
    // regression: "OnDestroy: eid=N has no local actor" while the ball kept piling up).
    // Symmetric with the v26 eid-SPAWN; MTA Packet_EntityRemove resolves by element ID
    // only. [[project-bug-trash-chippile-uaf-crash]]
    void* actor = nullptr;
    if (!keyW.empty()) {
        actor = coop::prop_element_tracker::ResolveLiveActorByKey(keyW);
    } else if (payload.elementId != 0 && payload.elementId != coop::element::kInvalidId) {
        actor = ResolveLiveActorByEid(payload.elementId);
    }
    // Now drain the wire-received mirror Element. It must vacate the Registry regardless
    // of whether the local actor still exists (echo bounce where we initiated the destroy
    // and the actor is already gone). Done AFTER the eid resolution above so that lookup
    // could still see it. Silent no-op for unknown eids (legacy elementId==0 senders).
    UnregisterPropMirror(payload.elementId);
    if (!actor) {
        if (keyW.empty() &&
            (payload.elementId == 0 || payload.elementId == coop::element::kInvalidId)) {
            UE_LOGW("remote_prop::OnDestroy: empty key AND no eid -- dropping");
        } else {
            UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u has no local actor (already destroyed or never spawned here)",
                    keyW.c_str(), payload.elementId);
        }
        return;
    }
    if (!ResolveDestroyFn()) {
        UE_LOGW("remote_prop::OnDestroy: K2_DestroyActor UFunction unresolved -- dropping");
        return;
    }
    UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u -> destroying local actor %p",
            keyW.c_str(), payload.elementId, actor);
    if (ue_wrap::prop::IsChipPile(actor) || ue_wrap::prop::IsGarbageClump(actor)) {
        UE_LOGI("[PILE] CLIENT destroy eid=%u -> mirror %p removed (the pile/clump vanished here too, "
                "matching the host)", payload.elementId, actor);
    }
    // If any slot was kinematically driving this prop, clear that slot's
    // cache so we don't try to drive a destroyed actor next tick.
    ClearAnyDriveFor(actor);
    // 2026-05-25 cross-peer destroy: if this peer's local mainPlayer is
    // currently grabbing the doomed actor (typical case: HOST holds the
    // food, CLIENT eats it, HOST receives PropDestroy from client and
    // must release before destroy), tear down the PHC grab cleanly
    // first. No-op for the common case where the actor isn't grabbed.
    // Audit fix #3 (2026-05-25): routed through ue_wrap::engine to keep
    // Principle 7 layering (coop/ does not touch engine struct offsets
    // directly).
    ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, actor);
    // Mark BEFORE the engine call so our K2_DestroyActor PRE observer sees
    // it and skips the broadcast (echo suppression).
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

void* OnConvert(const coop::net::PropConvertPayload& payload, void* localPlayer, int senderSlot) {
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnConvert)");
    // docs/piles/08 host-authoritative trash channel -- bind-model re-skin of eid E in place (oldEid ==
    // newEid == E). NO fresh eid, NO second entity. Resolve our current rendering of E (the SYNC-MIRROR
    // the host expects us to have), spawn the NEW rendering bound to the SAME E, rebind, echo-destroy old.
    const uint32_t E = payload.newEid;
    const bool wantClump = (payload.kind == coop::net::propconvert_kind::kToClump);
    const char* edge = wantClump ? "GRAB(pile->clump)" : "LAND(clump->pile)";
    if (E == 0u || E == coop::element::kInvalidId) {
        UE_LOGW("[PILE] CLIENT recv convert %s -- INVALID eid E=%u, dropping (no entity to re-skin)", edge, E);
        return nullptr;
    }
    // [diag 2026-06-21] RECEPTION marker -- logged BEFORE the ctx-gate so "the ToClump convert ARRIVED"
    // is explicit (its absence in the client log = the convert never reached us; e.g. the join race).
    // Shows known (our current ctx for E) + whether we have a proxy to re-skin. Event-driven, not hot.
    UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u known=%u isProxy=%d -- RECEPTION (pre-gate)",
            edge, E, static_cast<unsigned>(payload.ctx),
            static_cast<unsigned>(coop::trash_channel::CtxForEid(E)),
            coop::trash_proxy::IsProxy(E) ? 1 : 0);
    // docs/piles/08: adopt the host's authoritative sync-time-context for E, and DROP a stale/out-of-order
    // convert (a duplicate, or one older than a transition we already applied). ctx==0 = legacy/non-trash.
    if (!coop::trash_channel::AdoptInboundConvertCtx(E, payload.ctx)) return nullptr;
    // PROXY PATH (phase 1, THE dup fix): if E's mirror is our host-authoritative trash proxy, re-skin it
    // IN PLACE (pile<->clump). The eid->actor binding is NEVER touched -> no spawn-fresh, no orphan, no
    // dup; and a rooted proxy never goes stale, so the "mirror NOT-FOUND -> spawn fresh" path that caused
    // the dup is structurally unreachable. Non-proxy converts (Aprop_C / kerfur, or a rare trash convert
    // that beat its OnSpawn) keep the legacy spawn+rebind path below.
    if (coop::trash_proxy::IsProxy(E)) {
        void* proxy = coop::trash_proxy::ReskinProxy(E, payload.chipType, wantClump);
        // Reposition the proxy to the convert's authoritative transform + stop any active carry
        // drive. ESSENTIAL for ToPile (LAND): the carry stream stopped at the throw and the proxy
        // froze mid-air at release; this convert carries the LANDED rest position, so without the
        // reposition the re-skinned pile would float where the throw began. Harmless for ToClump
        // (GRAB): the incoming carry pose stream re-targets it immediately. ClearAnyDriveFor also
        // resets the lerp so a later stream re-snaps from the new position (no crawl from the stale
        // frozen pose).
        if (proxy) {
            ClearAnyDriveFor(proxy);
            E::SetActorLocation(proxy, ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
            E::SetActorRotation(proxy, ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        }
        UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -> PROXY re-skinned IN PLACE to %s chipType=%u "
                "[SYNC-MIRROR OK -- no spawn-fresh, no dup]",
                edge, E, static_cast<unsigned>(payload.ctx), wantClump ? "CLUMP" : "PILE",
                static_cast<unsigned>(payload.chipType));
        return proxy;
    }
    // HIGH-1: a trash convert that BEAT its OnSpawn (no proxy for E yet). Spawn the proxy HERE in the
    // unambiguous wantClump form (the convert's authoritative direction) and bind E, rather than falling
    // to the legacy path below -- that routes through remote_prop_spawn::OnSpawn, which derives the
    // proxy's form from the spawn CLASS (a fragile coupling). A trailing real PropSpawn then hits
    // SpawnProxy's convergence branch, which (by design) does NOT re-skin, so the form stays correct.
    // Non-trash converts (Aprop_C / kerfur) fall through to the legacy spawn+rebind path below.
    if (coop::trash_proxy::IsTrashProxyClass(remote_prop_spawn::ClassNameToWString(payload.pileClass))) {
        ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
        ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
        void* proxy = coop::trash_proxy::SpawnProxy(E, payload.chipType, /*isClump=*/wantClump,
                                                    senderSlot, loc, rot);
        if (proxy) {
            RegisterPropMirror(E, proxy, L"", R::ClassNameOf(proxy), senderSlot);
            UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -> proxy SPAWNED %s (convert beat its spawn) "
                    "[SYNC-MIRROR OK -- no dup]", edge, E, static_cast<unsigned>(payload.ctx),
                    wantClump ? "CLUMP" : "PILE");
        } else {
            UE_LOGW("[PILE] CLIENT recv convert %s eid=%u -- proxy spawn-on-convert FAILED (DESYNC)", edge, E);
        }
        return proxy;
    }
    void* cur = ResolveLiveActorByEid(E);
    const bool hadMirror = (cur != nullptr);   // was a SYNC-MIRROR of E present to re-skin?
    // Idempotency: if our rendering of E already matches the target edge (an echo, a duplicate, or a
    // grab-race loser's convert arriving after we already rendered the same class), this is a no-op
    // -- the winner's held-pose stream drives it. Prevents a spurious re-spawn + actor churn.
    if (cur && ue_wrap::prop::IsGarbageClump(cur) == wantClump) {
        UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -- already %s, idempotent no-op (echo/dup)",
                edge, E, static_cast<unsigned>(payload.ctx), wantClump ? "clump" : "pile");
        return cur;
    }
    // Spawn the NEW rendering bound to E. ToClump -> a kinematic clump (physFlags=0 -> not simulating;
    // OnSpawn also disarms the clump's self-convert hit-notify) the held-pose stream then drives;
    // ToPile -> a settled, grabbable pile (physFlags=0 -> resting, QueryAndPhysics kept). skipBind:
    // OnConvert binds E explicitly below; fromConvert: skip the eid-dedup so the still-live OLD rendering
    // of E doesn't converge the spawn.
    coop::net::PropSpawnPayload p{};
    p.className = payload.pileClass;
    p.key.len   = 0;                         // chipPile/clump are eid-only (Key=None)
    p.locX = payload.locX; p.locY = payload.locY; p.locZ = payload.locZ;
    p.rotPitch = payload.rotPitch; p.rotYaw = payload.rotYaw; p.rotRoll = payload.rotRoll;
    p.scaleX = p.scaleY = p.scaleZ = 1.f;
    p.physFlags = 0;
    p.chipType = payload.chipType;
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    p.elementId = E;
    void* next = nullptr;
    remote_prop_spawn::OnSpawn(p, senderSlot, localPlayer, /*fromConvert=*/true,
                              /*deferKerfur=*/true, &next, /*skipBind=*/true);
    if (!next) {
        UE_LOGW("[PILE] CLIENT recv convert %s eid=%u -- re-skin spawn FAILED, E left as the old actor (DESYNC)",
                edge, E);
        return nullptr;
    }
    // Rebind E onto the new rendering. RegisterPropMirror is the single rebind entry point -- it routes
    // on the Element's authoritative IsMirror() flag (a MIRROR -> SetActor; a host's OWN local element ->
    // RebindLocalElementActor, keeping the forward map consistent), so this is correct even when cur is
    // momentarily dead (findings 3/6/7/15 -- the old eIsLocal-from-live-cur guess silently mis-routed a
    // local element through the mirror path and desynced g_actorToPropElementId).
    const std::wstring cls = R::ClassNameOf(next);
    RegisterPropMirror(E, next, L"", cls, senderSlot, /*rebindInPlace=*/true);
    // Echo-destroy the OLD rendering AFTER the rebind (so E always resolves to a live actor -- no
    // flicker where E points at nothing). MarkIncomingDestroy suppresses our own destroy-broadcast.
    if (cur && cur != next) {
        ClearAnyDriveFor(cur);
        if (ResolveDestroyFn()) {
            coop::prop_echo_suppress::MarkIncomingDestroy(cur);
            R::CallFunction(cur, g_destroyActorFn, nullptr);
        }
    }
    UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -> mirror %s, re-skinned to %s cls='%ls' at "
            "(%.1f,%.1f,%.1f) variant=%u%s",
            edge, E, static_cast<unsigned>(payload.ctx),
            hadMirror ? "FOUND" : "NOT-FOUND",
            wantClump ? "CLUMP" : "PILE", cls.c_str(),
            payload.locX, payload.locY, payload.locZ, static_cast<unsigned>(payload.chipType),
            hadMirror ? " [SYNC-MIRROR OK]"
                      : " [WARN: no local mirror of E existed -- spawned fresh; was desynced pre-convert]");
    return next;
}

void ConsumeLocalActor(void* actor) {
    // Echo-suppressed local destroy. Used by OnSpawn to consume THIS peer's own copy of a
    // shared world chipPile when the other peer grabbed theirs (the pile is a separate
    // local UObject with no cross-peer id; we destroy it directly, by position). The
    // MarkIncomingDestroy makes our own K2_DestroyActor PRE observer skip re-broadcasting
    // it -- this is a local consume, not a new authoritative destroy to fan out.
    // [[project-bug-trash-chippile-uaf-crash]]
    UE_ASSERT_GAME_THREAD("ConsumeLocalActor (K2_DestroyActor)");
    if (!actor || !R::IsLive(actor)) return;
    if (!ResolveDestroyFn()) {
        UE_LOGW("remote_prop::ConsumeLocalActor: K2_DestroyActor unresolved -- cannot consume %p", actor);
        return;
    }
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

void ForceRelease() {
    // Clears every slot's g_drives state (T-10, GT-only). Called from
    // remote_prop::Tick on disconnect and from aggregate teardown (game thread).
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::ForceRelease)");
    // Force-release on aggregate disconnect/teardown clears every slot's
    // drive. Per-slot disconnect uses OnDisconnectForSlot instead.
    int released = 0;
    for (auto& d : g_drives) {
        if (!d.actor) continue;
        // Host-authoritative trash proxy: full teardown via RetireProxy (Destroy + RemoveFromRoot
        // + unbind). NEVER ConsumeLocalActor it -- that destroys the rooted AStaticMeshActor
        // WITHOUT RemoveFromRoot (a leaked GUObjectArray slot) and bypasses the proxy registry.
        // RetireProxy clears this drive (ClearAnyDriveFor) so d.actor is null after.
        if (d.isProxy) {
            coop::trash_proxy::RetireProxy(d.lastEid);
            ++released;
            continue;
        }
        // Normal prop -> release to physics (persists). Null-mesh clump mirror -> destroy
        // it (transient; the holder's death-watcher is gone on teardown -> would leak).
        // [[project-bug-trash-chippile-uaf-crash]]
        if (d.mesh) DriveSimulate(d.mesh, true);
        else        ConsumeLocalActor(d.actor);
        ResetDriveState(d);
        ++released;
    }
    if (released > 0) {
        UE_LOGI("remote_prop: force-release on disconnect/teardown (%d active drive(s) cleared)", released);
    }
    // A2 (2026-05-29): drain wire-received Prop mirrors on full session
    // teardown. Each mirror's dtor routes through Registry::UnregisterMirror
    // (m_mirror=true) so the Registry's m_byId slots in the foreign
    // allocation range are returned to nullptr. Per-slot disconnect does
    // NOT drain mirrors (mirrors aren't tagged with senderSlot yet -- a
    // future enhancement); convergent local actors persist across per-slot
    // disconnect just like remote_prop's drive cache does.
    // Inc3 (2026-05-30): mirror-ONLY drain -- the manager now also holds
    // prop_element_tracker's local Prop Elements, which must NOT be dropped on
    // disconnect (they shadow persistent world props + survive reconnect).
    const size_t mirrorsDrained = DrainWirePropMirrors();
    if (mirrorsDrained > 0) {
        UE_LOGI("remote_prop: force-release drained %zu wire-received Prop mirror(s)",
                mirrorsDrained);
    }
}

void OnDisconnectForSlot(int peerSlot) {
    // Clears g_drives[peerSlot] (T-10, GT-only). Called from net_pump::Tick's
    // per-slot disconnect edge (game thread).
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnDisconnectForSlot)");
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // D1-7: drain THIS peer's wire prop mirrors so they don't accumulate in the
    // Registry until full teardown (a reconnecting peer / recycled eid would
    // otherwise collide). Mirror-only + owner-slot-filtered: convergent locals
    // (m_mirror=false) and other peers' mirrors are untouched. The drained
    // Elements' dtors run outside the manager mutex (Registry::UnregisterMirror).
    // Done BEFORE the early-return below so a slot with mirrors but no held
    // drive still gets cleaned.
    const size_t drainedMirrors = PropMirrors().DrainMirrorsForSlot(peerSlot);
    if (drainedMirrors > 0) {
        UE_LOGI("remote_prop: peer slot %d disconnect -- drained %zu wire prop mirror(s)",
                peerSlot, drainedMirrors);
    }
    ActiveDrive& d = g_drives[peerSlot];
    if (!d.actor) return;
    // Host-authoritative trash proxy: full teardown via RetireProxy (Destroy + RemoveFromRoot
    // + unbind), never ConsumeLocalActor (a rooted-slot leak + registry bypass). Normally the
    // proxy is already retired by trash_proxy::OnDisconnectForSlot (called first in
    // subsystems::DisconnectSlot), so d.actor is null and we returned above -- this is the
    // belt-and-suspenders path. RetireProxy is idempotent.
    if (d.isProxy) {
        coop::trash_proxy::RetireProxy(d.lastEid);  // clears this drive via ClearAnyDriveFor
        UE_LOGI("remote_prop: peer slot %d disconnected -- retired held trash proxy eid=%u", peerSlot, d.lastEid);
        return;
    }
    if (d.mesh) {
        // Normal world prop: release to physics -- it persists (convergent world object).
        DriveSimulate(d.mesh, true);
        UE_LOGI("remote_prop: peer slot %d disconnected -- releasing held prop (key='%s')",
                peerSlot, d.lastKey.c_str());
    } else {
        // Null-mesh = the transient CLUMP mirror. The holder vanished mid-carry, so the
        // death-watcher that would despawn it (it lives on the HOLDER) is gone -- destroy
        // the mirror here or it leaks as a frozen floating ball on this peer.
        // ConsumeLocalActor is echo-suppressed + IsLive-gated. [[project-bug-trash-chippile-uaf-crash]]
        ConsumeLocalActor(d.actor);
        UE_LOGI("remote_prop: peer slot %d disconnected mid-carry -- destroyed held clump mirror %p (no leak)",
                peerSlot, d.actor);
    }
    ResetDriveState(d);
}

}  // namespace coop::remote_prop

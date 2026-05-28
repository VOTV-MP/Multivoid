// coop/remote_prop.cpp -- v4 receiver implementation.

#include "coop/remote_prop.h"

#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>

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
// PR-4.6 (2026-05-28, closes code-explorer Finding E): one ActiveDrive per
// peer slot so multiple clients can each kinematically drive their own held
// prop concurrently. Pre-PR-4.6 this was a single ActiveDrive instance --
// if two clients held different props at once, their PropPose streams
// would race-overwrite each other on the host.
struct ActiveDrive {
    void*        actor = nullptr;
    void*        mesh = nullptr;
    std::string  lastKey;        // ASCII (the Aprop_C save UUID format)
    uint64_t     lastApplyMs = 0;
};
std::array<ActiveDrive, coop::players::kMaxPeers> g_drives{};

// True if `actor` is the cached drive target of ANY slot's drive state.
// Used by OnSpawn convergence skip + OnDestroy clear paths -- they don't
// care which slot owns the drive, only that the actor is locked to the
// PropPose stream and shouldn't be stomped by a snapshot convergence.
bool IsActorUnderAnyDrive(void* actor) {
    if (!actor) return false;
    for (const auto& d : g_drives) {
        if (d.actor == actor) return true;
    }
    return false;
}

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
std::wstring KeyToWString(const coop::net::WireKey& k) {
    std::wstring s;
    s.reserve(k.len);
    for (uint8_t i = 0; i < k.len && i < 31; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    }
    return s;
}

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

void ResolveAndStartDrive(int slot, const coop::net::WireKey& k) {
    const std::wstring keyW = KeyToWString(k);
    void* prop = ue_wrap::prop::FindByKeyString(keyW);
    if (!prop) {
        UE_LOGW("remote_prop: slot %d incoming PropPose key '%ls' (len=%d) -- no local match in GUObjectArray",
                slot, keyW.c_str(), static_cast<int>(k.len));
        return;
    }
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (!mesh) {
        UE_LOGW("remote_prop: slot %d prop %p has null StaticMesh -- cannot drive", slot, prop);
        return;
    }
    UE_LOGI("remote_prop: slot %d GRAB-IN '%ls' -> local Aprop_C=%p mesh=%p (SetSimulatePhysics false)",
            slot, keyW.c_str(), prop, mesh);
    // Disable PhysX simulation so SetActorLocation we drive per packet sticks
    // (otherwise PhysX would yank the body back to its falling trajectory).
    DriveSimulate(mesh, false);
    g_drives[slot].actor = prop;
    g_drives[slot].mesh  = mesh;
    g_drives[slot].lastKey.assign(k.data, k.len);
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (!session.connected()) {
        ForceRelease();
        return;
    }
    // PR-4.6: per-slot drive iteration. On HOST: scan slots 1..kMaxPeers-1
    // (each connected client can drive its own held prop independently).
    // On CLIENT: scan slot 0 only (the host's puppet is the only prop-pose
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
            // First snapshot OR key changed -> resolve + SetSimulatePhysics(false).
            if (!drive.actor || !KeyMatchesCache(slot, pose.key)) {
                if (drive.actor) {
                    // The peer at this slot switched to a different prop
                    // without sending Release -- implicit release (re-enable
                    // physics on the prior body).
                    UE_LOGI("remote_prop: slot %d implicit release (peer switched to a new key)", slot);
                    DriveSimulate(drive.mesh, true);
                    drive.actor = nullptr;
                    drive.mesh = nullptr;
                    drive.lastKey.clear();
                }
                ResolveAndStartDrive(slot, pose.key);
            }
            if (drive.actor && R::IsLive(drive.actor)) {
                ue_wrap::FVector  loc{pose.x, pose.y, pose.z};
                ue_wrap::FRotator rot{pose.pitch, pose.yaw, pose.roll};
                E::SetActorLocation(drive.actor, loc);
                E::SetActorRotation(drive.actor, rot);
                drive.lastApplyMs = nowMs;
                // Throttled position log: first 3 applies + every 60th after
                // per slot. Helps debug cross-peer position parity.
                static std::array<uint64_t, coop::players::kMaxPeers> sApplyCount{};
                const uint64_t n = ++sApplyCount[slot];
                if (n <= 3 || (n % 60) == 0) {
                    UE_LOGI("remote_prop: slot %d drive #%llu -> world(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f)",
                            slot, static_cast<unsigned long long>(n),
                            loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
                }
            } else if (drive.actor) {
                // The cached actor died (level unload / GC). Drop cleanly.
                UE_LOGW("remote_prop: slot %d cached actor no longer live -- dropping drive", slot);
                drive.actor = nullptr;
                drive.mesh = nullptr;
                drive.lastKey.clear();
            }
            continue;
        }
        // No new packet for THIS slot -- check the stream-stop timeout
        // (treat as implicit release: peer stopped sending PropPose).
        if (drive.actor && (nowMs - drive.lastApplyMs) > 500) {
            UE_LOGI("remote_prop: slot %d implicit release (%llu ms since last PropPose)",
                    slot, static_cast<unsigned long long>(nowMs - drive.lastApplyMs));
            DriveSimulate(drive.mesh, true);
            drive.actor = nullptr;
            drive.mesh = nullptr;
            drive.lastKey.clear();
        }
    }
}

void OnRelease(const coop::net::PropReleasePayload& payload, void* localPlayer) {
    const std::wstring keyW = KeyToWString(payload.key);
    const float linSpeedSq = payload.linVelX * payload.linVelX +
                             payload.linVelY * payload.linVelY +
                             payload.linVelZ * payload.linVelZ;
    const float linSpeed = std::sqrt(linSpeedSq);
    UE_LOGI("remote_prop: RELEASE wire '%ls' linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f) deg/s",
            keyW.c_str(),
            payload.linVelX, payload.linVelY, payload.linVelZ, linSpeed,
            payload.angVelX, payload.angVelY, payload.angVelZ);
    // PR-4.6: find which slot's drive owns this key (peer's release packet
    // carries the prop key but not their peer slot -- the wire didn't add
    // sender attribution to the payload itself). Pre-PR-4.6 we only had
    // one global drive; now we scan all slots.
    void* propActor = nullptr;
    void* meshToActOn = nullptr;
    const int releasedSlot = FindSlotByKey(payload.key);
    if (releasedSlot >= 0) {
        propActor = g_drives[releasedSlot].actor;
        meshToActOn = g_drives[releasedSlot].mesh;
    } else if (void* prop = ue_wrap::prop::FindByKeyString(keyW)) {
        // Release arrived without a preceding PropPose stream (we missed/
        // dropped them) -- resolve fresh.
        propActor = prop;
        meshToActOn = ue_wrap::prop::GetStaticMesh(prop);
    }
    if (meshToActOn) {
        // Order matters: SetSimulate(true) FIRST so the body re-enters
        // dynamic sim BEFORE we write velocity (a kinematic body's velocity
        // write would be ignored / cause a PhysX kinematic-target chase).
        DriveSimulate(meshToActOn, true);
        DriveSetLinearVelocity(meshToActOn, payload.linVelX, payload.linVelY, payload.linVelZ);
        DriveSetAngularVelocity(meshToActOn, payload.angVelX, payload.angVelY, payload.angVelZ);
        // RULE 1 natural-sound dispatch: fire Aprop_C.thrown(Player) so the
        // prop's BP graph plays throw-whoosh sound + particle trail
        // identically to a local throw. Gated on linear speed: a passive drop
        // (residual walk velocity ~30 cm/s) shouldn't whoosh; a deliberate
        // throw (>200 cm/s, per kThrownLinVelThreshold) should. Without this
        // gate the BP fires on every release; without the call entirely
        // (Bug A) it never fires on the remote at all.
        if (propActor && linSpeed > coop::net::kThrownLinVelThreshold) {
            DrivePropThrown(propActor, localPlayer);
            UE_LOGI("remote_prop: fired Aprop_C.thrown(player=%p) -- launch speed %.1f cm/s > threshold %.1f",
                    localPlayer, linSpeed, coop::net::kThrownLinVelThreshold);
        }
    }
    // PR-4.6: clear only the matching slot's drive state (was: clear the
    // single global g_drive). Leaves other slots' active drives intact.
    if (releasedSlot >= 0) {
        g_drives[releasedSlot].actor = nullptr;
        g_drives[releasedSlot].mesh = nullptr;
        g_drives[releasedSlot].lastKey.clear();
    }
}

namespace {

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

// World context for the spawn -- the GameInstance is the long-lived UObject.
void* GetWorldContext() {
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) return gi;
    return R::FindObjectByClass(P::name::WorldClass);
}

// FRotator -> FQuat (UE4.27 stock formula, ZYX order, LEFT-HANDED coord
// system). Match EXACTLY the body of FRotator::Quaternion() from
// Engine/Source/Runtime/Core/Public/Math/Rotator.h:
//
//   RotationQuat.X =  CR*SP*SY - SR*CP*CY;
//   RotationQuat.Y = -CR*SP*CY - SR*CP*SY;
//   RotationQuat.Z =  CR*CP*SY - SR*SP*CY;
//   RotationQuat.W =  CR*CP*CY + SR*SP*SY;
//
// Note the negative Y term -- this is UE4's left-handed-Z-up convention
// (NOT a bug). General right-handed ZYX Euler-to-quat references will show
// the opposite signs; do NOT "correct" against those without checking the
// UE4 source. Audit 2026-05-24 flagged this as a critical bug; verified
// false-positive by reading UE4.27 source directly.
void RotatorToQuat(float pitchDeg, float yawDeg, float rollDeg,
                   float& qx, float& qy, float& qz, float& qw) {
    constexpr float kHalfDegToRad = 0.0087266462599716478846184538424431f;  // (pi/180)/2
    const float sp = std::sin(pitchDeg * kHalfDegToRad);
    const float cp = std::cos(pitchDeg * kHalfDegToRad);
    const float sy = std::sin(yawDeg   * kHalfDegToRad);
    const float cy = std::cos(yawDeg   * kHalfDegToRad);
    const float sr = std::sin(rollDeg  * kHalfDegToRad);
    const float cr = std::cos(rollDeg  * kHalfDegToRad);
    qx =  cr * sp * sy - sr * cp * cy;
    qy = -cr * sp * cy - sr * cp * sy;
    qz =  cr * cp * sy - sr * sp * cy;
    qw =  cr * cp * cy + sr * sp * sy;
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
// ==== RULE №1 RETIREMENT PLAN (audit fix 2026-05-25, issue #4) ====
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
//       PropFoodMushroomClass constant. Per RULE №2 the kerfur-of-the-day
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

}  // namespace

void OnSpawn(const coop::net::PropSpawnPayload& payload) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    const std::wstring classW = ClassNameToWString(payload.className);
    const std::wstring keyW   = KeyToWString(payload.key);
    UE_LOGI("remote_prop::OnSpawn: cls='%ls' key='%ls' loc=(%.1f, %.1f, %.1f) rot=(%.1f, %.1f, %.1f) physFlags=0x%02x",
            classW.c_str(), keyW.c_str(),
            payload.locX, payload.locY, payload.locZ,
            payload.rotPitch, payload.rotYaw, payload.rotRoll,
            static_cast<int>(payload.physFlags));
    if (classW.empty() || keyW.empty() || keyW == L"None") {
        // "None" = FName(NAME_None) stringified -- host shouldn't send these
        // (lifecycle + snapshot guards prevent it), but reject defensively
        // so any legacy/in-flight stragglers don't trigger the dedup spam
        // loop on the receiver.
        UE_LOGW("remote_prop::OnSpawn: unkeyed (cls='%ls' key='%ls') -- dropping",
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
    if (void* existing = ue_wrap::prop::FindByKeyString(keyW)) {
        // Audit I-1 (2026-05-24): skip the convergence write if THIS prop
        // is currently under active kinematic drive by the PropPose stream
        // (host is holding it). Otherwise the SetActorLocation here would
        // stomp the active drive for one frame, producing a visible
        // teleport-pop until the next PropPose Tick corrects it. The
        // PropPose stream is authoritative for held props; let it own
        // position while held.
        if (IsActorUnderAnyDrive(existing)) {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' is under active kinematic drive (some slot) -- skipping convergence (PropPose owns position)",
                    keyW.c_str());
            return;
        }
        UE_LOGI("remote_prop::OnSpawn: key '%ls' already resolves to live actor %p -- de-duping, converging transform to host (loc=(%.1f,%.1f,%.1f))",
                keyW.c_str(), existing, payload.locX, payload.locY, payload.locZ);
        ue_wrap::engine::SetActorLocation(existing,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(existing,
            ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        // 2026-05-25 mushroom fall-through fix: client's local copy may have
        // gone through spawnedNaturally() in its own AmushroomSpawner_C::Spawn
        // path, leaving NoCollision on the StaticMesh. Wire convergence
        // reuses the actor in-place (skipping a fresh Init); we must
        // explicitly restore default collision so subsequent host PropPose
        // releases don't drop the body into the void.
        RestoreCollisionIfNeeded(L"exact-key", classW, existing);
        return;
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
    if (void* fuzzy = ue_wrap::prop::FindNearbySameClass(
            classW,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ},
            kFuzzyRadiusCm)) {
        // Audit C-1 mirror of exact-Key guard (2026-05-24): if the fuzzy
        // match resolves to the actor currently under active kinematic
        // drive, skip convergence. Otherwise the SetActorLocation here
        // would stomp the PropPose stream for one frame -- same
        // teleport-pop bug as the exact-Key path.
        if (IsActorUnderAnyDrive(fuzzy)) {
            UE_LOGI("remote_prop::OnSpawn: Gap-I-1 fuzzy match '%ls' -> active drive actor (some slot) -- skipping (PropPose owns position)",
                    classW.c_str());
            return;
        }
        UE_LOGI("remote_prop::OnSpawn: Gap-I-1 FUZZY MATCH '%ls' (wire key '%ls') -> existing actor %p within %.1f cm -- de-duping, converging transform + rekeying",
                classW.c_str(), keyW.c_str(), fuzzy, kFuzzyRadiusCm);
        ue_wrap::engine::SetActorLocation(fuzzy,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(fuzzy,
            ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        // Audit C-2 (2026-05-24): REKEY the fuzzy-matched actor to the
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
        // 2026-05-25 mushroom fall-through fix (Gap-I-1 path): same reason as
        // exact-key. The fuzzy-matched local actor came from client's own
        // AmushroomSpawner_C::Spawn -> spawnedNaturally() -> NoCollision.
        // Without this restore, host's release-edge PropPose-driven mushroom
        // sits on top of the floor briefly then PhysX drops it through
        // because the body has NoCollision.
        RestoreCollisionIfNeeded(L"fuzzy", classW, fuzzy);
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
    void* worldCtx = GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("remote_prop::OnSpawn: no world context -- dropping");
        return;
    }
    // Build FTransform from wire rotation/scale/location. ue_wrap::FTransform
    // (types.h) is the canonical 48-byte layout matching engine FTransform;
    // RULE 2 (audit issue C-4): no parallel local FTransform48 type.
    ue_wrap::FTransform xform{};  // ctor defaults: identity rot, zero loc, unit scale
    RotatorToQuat(payload.rotPitch, payload.rotYaw, payload.rotRoll,
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
    // v5 Inc2 echo suppression: mark this actor as wire-induced BEFORE
    // FinishSpawningActor (which runs Aprop_C::Init via UserConstructionScript,
    // tripping our Init POST observer in harness.cpp). The observer's call
    // to ConsumeIncomingSpawn will then return true and skip the broadcast
    // back to the sender -> no echo loop.
    MarkIncomingSpawn(spawned);
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
    // Phase 4: physics state. The mesh is Aprop_C.StaticMesh.
    void* mesh = ue_wrap::prop::GetStaticMesh(spawned);
    if (mesh) {
        const bool sim = (payload.physFlags & coop::net::propspawn_flags::kSimulatePhysics) != 0;
        DriveSimulate(mesh, sim);
        const bool hasLinVel =
            payload.initLinVelX != 0.f || payload.initLinVelY != 0.f || payload.initLinVelZ != 0.f;
        const bool hasAngVel =
            payload.initAngVelX != 0.f || payload.initAngVelY != 0.f || payload.initAngVelZ != 0.f;
        if (hasLinVel) DriveSetLinearVelocity(mesh, payload.initLinVelX, payload.initLinVelY, payload.initLinVelZ);
        if (hasAngVel) DriveSetAngularVelocity(mesh, payload.initAngVelX, payload.initAngVelY, payload.initAngVelZ);
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
}

void* GetDriveActor() {
    // PR-4.6: returns the FIRST slot with an active drive (or nullptr).
    // Pre-PR-4.6 this returned the single global g_drive.actor; the
    // public API is unused outside this TU today (snapshot dedupe path
    // now uses IsActorUnderAnyDrive internally), but kept for header
    // compatibility -- a future caller can switch to the per-slot
    // overload below or to IsActorUnderAnyDrive directly.
    for (const auto& d : g_drives) {
        if (d.actor) return d.actor;
    }
    return nullptr;
}

namespace {
// v5 Inc2: echo-suppression sets. Populated by OnSpawn / OnDestroy before
// the engine call (BeginDeferred+Finish / K2_DestroyActor), consumed
// one-shot by the Aprop_C::Init POST observer + K2_DestroyActor PRE
// observer. Without this, our own receiver-applied spawn/destroy would
// re-broadcast to the original sender = packet ping-pong.
//
// Game-thread-only access (OnSpawn/OnDestroy and the observers all dispatch
// on the game thread). Capped at 256 to bound memory across a long session;
// on overflow we just clear -- a one-shot stale lookup on an entry that
// was supposed to be consumed but wasn't is harmless (it'd let a wire-induced
// spawn re-broadcast once, which the OTHER side de-dupes via FindByKeyString).
std::unordered_set<void*> g_incomingSpawns;
std::unordered_set<void*> g_incomingDestroys;
constexpr size_t kIncomingCap = 256;

template <class Set>
void InsertCapped(Set& s, void* actor) {
    if (s.size() >= kIncomingCap) s.clear();
    s.insert(actor);
}

template <class Set>
bool TakeOne(Set& s, void* actor) {
    auto it = s.find(actor);
    if (it == s.end()) return false;
    s.erase(it);
    return true;
}
}  // namespace

void MarkIncomingSpawn(void* actor)    { if (actor) InsertCapped(g_incomingSpawns, actor); }
bool ConsumeIncomingSpawn(void* actor) { return actor ? TakeOne(g_incomingSpawns, actor) : false; }
void MarkIncomingDestroy(void* actor)  { if (actor) InsertCapped(g_incomingDestroys, actor); }
bool ConsumeIncomingDestroy(void* actor){ return actor ? TakeOne(g_incomingDestroys, actor) : false; }

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

void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer) {
    const std::wstring keyW = KeyToWString(payload.key);
    if (keyW.empty()) {
        UE_LOGW("remote_prop::OnDestroy: empty key -- dropping");
        return;
    }
    void* actor = ue_wrap::prop::FindByKeyString(keyW);
    if (!actor) {
        UE_LOGI("remote_prop::OnDestroy: key '%ls' has no local actor (already destroyed or never spawned here)",
                keyW.c_str());
        return;
    }
    if (!ResolveDestroyFn()) {
        UE_LOGW("remote_prop::OnDestroy: K2_DestroyActor UFunction unresolved -- dropping");
        return;
    }
    UE_LOGI("remote_prop::OnDestroy: key '%ls' -> destroying local actor %p",
            keyW.c_str(), actor);
    // PR-4.6: if any slot was kinematically driving this prop, clear that
    // slot's cache so we don't try to drive a destroyed actor next tick.
    // Pre-PR-4.6 this only checked the single global g_drive.
    for (auto& d : g_drives) {
        if (d.actor == actor) {
            UE_LOGI("remote_prop::OnDestroy: actor was under active kinematic drive (slot %td) -- clearing drive cache",
                    std::distance(&g_drives[0], &d));
            d.actor = nullptr;
            d.mesh = nullptr;
            d.lastKey.clear();
        }
    }
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
    MarkIncomingDestroy(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

void ForceRelease() {
    // PR-4.6: force-release on disconnect/teardown clears every slot's
    // drive. Pre-PR-4.6 only the single g_drive was cleared.
    int released = 0;
    for (auto& d : g_drives) {
        if (!d.actor) continue;
        if (d.mesh) DriveSimulate(d.mesh, true);
        d.actor = nullptr;
        d.mesh = nullptr;
        d.lastKey.clear();
        ++released;
    }
    if (released > 0) {
        UE_LOGI("remote_prop: force-release on disconnect/teardown (%d active drive(s) cleared)", released);
    }
}

}  // namespace coop::remote_prop

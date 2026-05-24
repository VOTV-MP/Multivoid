// coop/remote_prop.cpp -- v4 receiver implementation.

#include "coop/remote_prop.h"

#include "coop/net/session.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

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
struct ActiveDrive {
    void*        actor = nullptr;
    void*        mesh = nullptr;
    std::string  lastKey;        // ASCII (the Aprop_C save UUID format)
    uint64_t     lastApplyMs = 0;
};
ActiveDrive g_drive;

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

// Compare WireKey vs cached ASCII key. Tiny helper -- prop Keys are ASCII
// (base64-ish save UUIDs), so comparing byte-by-byte vs the cached string
// is correct.
bool KeyMatchesCache(const coop::net::WireKey& k) {
    if (k.len != g_drive.lastKey.size()) return false;
    return std::memcmp(k.data, g_drive.lastKey.data(), k.len) == 0;
}

void ResolveAndStartDrive(const coop::net::WireKey& k) {
    const std::wstring keyW = KeyToWString(k);
    void* prop = ue_wrap::prop::FindByKeyString(keyW);
    if (!prop) {
        UE_LOGW("remote_prop: incoming PropPose key '%ls' (len=%d) -- no local match in GUObjectArray",
                keyW.c_str(), static_cast<int>(k.len));
        return;
    }
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (!mesh) {
        UE_LOGW("remote_prop: prop %p has null StaticMesh -- cannot drive", prop);
        return;
    }
    UE_LOGI("remote_prop: GRAB-IN '%ls' -> local Aprop_C=%p mesh=%p (SetSimulatePhysics false)",
            keyW.c_str(), prop, mesh);
    // Disable PhysX simulation so SetActorLocation we drive per packet sticks
    // (otherwise PhysX would yank the body back to its falling trajectory).
    DriveSimulate(mesh, false);
    g_drive.actor = prop;
    g_drive.mesh  = mesh;
    g_drive.lastKey.assign(k.data, k.len);
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (!session.connected()) {
        if (g_drive.actor) ForceRelease();
        return;
    }
    coop::net::PropPoseSnapshot pose{};
    bool isNew = false;
    const bool have = session.TryGetRemotePropPose(pose, &isNew);
    const uint64_t nowMs = NowMs();
    if (have && isNew) {
        // First snapshot OR key changed -> resolve + SetSimulatePhysics(false).
        if (!g_drive.actor || !KeyMatchesCache(pose.key)) {
            if (g_drive.actor) {
                // The peer switched to a different prop without sending Release
                // -- treat the prior as implicit release (re-enable physics).
                UE_LOGI("remote_prop: implicit release (peer switched to a new key)");
                DriveSimulate(g_drive.mesh, true);
                g_drive.actor = nullptr;
                g_drive.mesh = nullptr;
                g_drive.lastKey.clear();
            }
            ResolveAndStartDrive(pose.key);
        }
        if (g_drive.actor && R::IsLive(g_drive.actor)) {
            ue_wrap::FVector  loc{pose.x, pose.y, pose.z};
            ue_wrap::FRotator rot{pose.pitch, pose.yaw, pose.roll};
            E::SetActorLocation(g_drive.actor, loc);
            E::SetActorRotation(g_drive.actor, rot);
            g_drive.lastApplyMs = nowMs;
            // Throttled position log: first 3 applies + every 60th after.
            // Confirms per-tick drive arrival + lets us compare host-vs-client
            // world coords cross-peer.
            static uint64_t sApplyCount = 0;
            const uint64_t n = ++sApplyCount;
            if (n <= 3 || (n % 60) == 0) {
                UE_LOGI("remote_prop: drive #%llu -> world(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f)",
                        static_cast<unsigned long long>(n),
                        loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
            }
        } else if (g_drive.actor) {
            // The cached actor died (level unload / GC). Drop the drive cleanly.
            UE_LOGW("remote_prop: cached actor no longer live -- dropping drive");
            g_drive.actor = nullptr;
            g_drive.mesh = nullptr;
            g_drive.lastKey.clear();
        }
        return;
    }
    // No new packet -- check the stream-stop timeout (treat as implicit
    // release: peer stopped sending PropPose, e.g. their grab ended OR they
    // disconnected without RELEASE).
    if (g_drive.actor && (nowMs - g_drive.lastApplyMs) > 500) {
        UE_LOGI("remote_prop: implicit release (%llu ms since last PropPose)",
                static_cast<unsigned long long>(nowMs - g_drive.lastApplyMs));
        DriveSimulate(g_drive.mesh, true);
        g_drive.actor = nullptr;
        g_drive.mesh = nullptr;
        g_drive.lastKey.clear();
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
    // If we don't currently drive this prop (e.g. release arrived without a
    // preceding PropPose stream because we missed/dropped them), resolve fresh
    // so we can re-enable physics + apply velocity + fire thrown event.
    void* propActor = nullptr;
    void* meshToActOn = nullptr;
    if (g_drive.actor && KeyMatchesCache(payload.key)) {
        propActor = g_drive.actor;
        meshToActOn = g_drive.mesh;
    } else if (void* prop = ue_wrap::prop::FindByKeyString(keyW)) {
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
    g_drive.actor = nullptr;
    g_drive.mesh = nullptr;
    g_drive.lastKey.clear();
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
void* g_kslCdo           = nullptr;
void* g_convStrToNameFn  = nullptr;
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
    // Conv_StringToName on KismetStringLibrary CDO (UE4 stock, always loaded).
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(P::name::KismetStringLibraryClass);
    if (g_kslCdo && !g_convStrToNameFn) {
        if (void* kc = R::ClassOf(g_kslCdo)) {
            g_convStrToNameFn = R::FindFunction(kc, P::name::ConvStringToNameFn);
        }
    }
    g_spawnResolved = true;
    return true;
}

// Build a live FName from a wide string by dispatching
// UKismetStringLibrary::Conv_StringToName(FString) -> FName via ProcessEvent.
// Returns {0,0} on failure (the empty FName / NAME_None). The string is
// registered in the engine's FName pool with a fresh local index; subsequent
// FName equality compares with the same string will hit this index.
R::FName StringToFName(const std::wstring& s) {
    if (s.empty() || !g_kslCdo || !g_convStrToNameFn) return R::FName{0, 0};
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    // FString param: {Data ptr, Num including null, Max}. Copy to a local
    // buffer so the FString points at stable memory across the call.
    std::wstring buf(s);
    R::FString fs{};
    fs.Data = buf.data();
    fs.Num  = static_cast<int32_t>(buf.size()) + 1;
    fs.Max  = fs.Num;
    ParamFrame f(g_convStrToNameFn);
    // The param name is "InString" (UE4 stock); lowercase 'i' on some builds
    // (matches Conv_StringToText's "inString" pattern noted in
    // [[project-nameplate-widgetcomponent-plan]]). Try both for safety:
    // SetRaw with the lowercase name first, then fall back to UE4 stock case.
    if (!f.SetRaw(L"InString", &fs, sizeof(fs))) {
        if (!f.SetRaw(L"inString", &fs, sizeof(fs))) {
            UE_LOGW("StringToFName: Conv_StringToName param 'InString'/'inString' not found");
            return R::FName{0, 0};
        }
    }
    if (!Call(g_kslCdo, f)) {
        UE_LOGW("StringToFName: ProcessEvent call failed");
        return R::FName{0, 0};
    }
    return f.Get<R::FName>(L"ReturnValue");
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
    if (classW.empty() || keyW.empty()) {
        UE_LOGW("remote_prop::OnSpawn: empty class or key -- dropping");
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
    if (!g_propSetKeyFn) {
        // Late-resolve in case prop_C wasn't loaded earlier.
        if (void* propCls = R::FindClass(P::name::PropClass)) {
            g_propSetKeyFn = R::FindFunction(propCls, P::name::PropSetKeyFn);
        }
    }
    if (g_propSetKeyFn) {
        // Convert wire Key string -> live FName via Conv_StringToName, then
        // call Aprop_C.setKey(FName) so the receiver's prop carries the
        // SAME Key string as the sender's. Subsequent PropPose updates with
        // this key resolve via prop_wrap::FindByKeyString (which compares by
        // ToString, NOT by ComparisonIndex -- the cross-peer-stable path).
        const R::FName keyFName = StringToFName(keyW);
        if (keyFName.ComparisonIndex == 0) {
            UE_LOGW("remote_prop::OnSpawn: StringToFName('%ls') -> NAME_None; setKey skipped",
                    keyW.c_str());
        } else {
            ParamFrame sk(g_propSetKeyFn);
            if (!sk.SetRaw(L"Key", &keyFName, sizeof(keyFName))) {
                UE_LOGW("remote_prop::OnSpawn: setKey 'Key' param not found");
            } else if (!Call(spawned, sk)) {
                UE_LOGW("remote_prop::OnSpawn: setKey ProcessEvent call failed");
            } else {
                UE_LOGI("remote_prop::OnSpawn: setKey('%ls') ok (FName idx=%u)",
                        keyW.c_str(), keyFName.ComparisonIndex);
            }
        }
    } else {
        UE_LOGW("remote_prop::OnSpawn: prop_C.setKey UFunction not resolved -- spawn will use NewGuid'd Key");
    }
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
}

void ForceRelease() {
    if (!g_drive.actor) return;
    UE_LOGI("remote_prop: force-release on disconnect/teardown");
    if (g_drive.mesh) DriveSimulate(g_drive.mesh, true);
    g_drive.actor = nullptr;
    g_drive.mesh = nullptr;
    g_drive.lastKey.clear();
}

}  // namespace coop::remote_prop

// coop/remote_prop.cpp -- v4 receiver implementation.

#include "coop/remote_prop.h"

#include "coop/net/session.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <chrono>
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
// SetSimulatePhysics + AddImpulse live on UPrimitiveComponent. One-shot resolve;
// IsLive check re-resolves if the UClass got GC'd at a level unload.
void*  g_primCompCls       = nullptr;
void*  g_setSimulateFn     = nullptr;
void*  g_addImpulseFn      = nullptr;
int32_t g_setSimulateFrameSize = 0;
int32_t g_setSimulatePSim      = -1;  // offset of bSimulate bool in the frame
int32_t g_addImpulseFrameSize  = 0;
int32_t g_addImpulsePImpulse   = -1;
int32_t g_addImpulsePBone      = -1;
int32_t g_addImpulsePVelChange = -1;
bool    g_resolved = false;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool ResolveUFns() {
    if (g_resolved && R::IsLive(g_primCompCls)) return true;
    g_primCompCls = R::FindClass(P::name::PrimitiveComponentClass);
    if (!g_primCompCls) {
        UE_LOGW("remote_prop: PrimitiveComponent class not found");
        return false;
    }
    g_setSimulateFn = R::FindFunction(g_primCompCls, P::name::SetSimulatePhysicsFn);
    g_addImpulseFn  = R::FindFunction(g_primCompCls, P::name::AddImpulseFn);
    if (!g_setSimulateFn || !g_addImpulseFn) {
        UE_LOGW("remote_prop: UFunction lookup failed (sim=%p addImp=%p)",
                g_setSimulateFn, g_addImpulseFn);
        return false;
    }
    g_setSimulateFrameSize = R::FunctionFrameSize(g_setSimulateFn);
    // Param is `bSimulate` per Engine.hpp:17349 (Hungarian b- prefix for bool,
    // different convention than AddImpulse's lowercase `impulse`).
    g_setSimulatePSim = R::FindParamOffset(g_setSimulateFn, L"bSimulate");
    g_addImpulseFrameSize = R::FunctionFrameSize(g_addImpulseFn);
    g_addImpulsePImpulse  = R::FindParamOffset(g_addImpulseFn, L"impulse");
    g_addImpulsePBone     = R::FindParamOffset(g_addImpulseFn, L"BoneName");
    g_addImpulsePVelChange = R::FindParamOffset(g_addImpulseFn, L"bVelChange");
    if (g_setSimulatePSim < 0 || g_addImpulsePImpulse < 0 ||
        g_addImpulsePBone < 0 || g_addImpulsePVelChange < 0) {
        UE_LOGW("remote_prop: param offsets failed (sim=%d / imp=%d bone=%d vel=%d)",
                g_setSimulatePSim, g_addImpulsePImpulse, g_addImpulsePBone, g_addImpulsePVelChange);
        return false;
    }
    UE_LOGI("remote_prop: resolved -- SetSimulatePhysics frame=%d (sim@%d), "
            "AddImpulse frame=%d (imp@%d bone@%d vel@%d)",
            g_setSimulateFrameSize, g_setSimulatePSim,
            g_addImpulseFrameSize, g_addImpulsePImpulse, g_addImpulsePBone, g_addImpulsePVelChange);
    g_resolved = true;
    return true;
}

void DriveSimulate(void* mesh, bool simulate) {
    if (!mesh) return;
    if (!ResolveUFns()) return;
    unsigned char frame[64] = {};
    if (g_setSimulateFrameSize > static_cast<int32_t>(sizeof(frame))) return;
    *reinterpret_cast<bool*>(frame + g_setSimulatePSim) = simulate;
    R::CallFunction(mesh, g_setSimulateFn, frame);
}

void DriveAddImpulse(void* mesh, float ix, float iy, float iz) {
    if (!mesh) return;
    if (!ResolveUFns()) return;
    unsigned char frame[64] = {};
    if (g_addImpulseFrameSize > static_cast<int32_t>(sizeof(frame))) return;
    *reinterpret_cast<ue_wrap::FVector*>(frame + g_addImpulsePImpulse) =
        ue_wrap::FVector{ix, iy, iz};
    *reinterpret_cast<R::FName*>(frame + g_addImpulsePBone) = R::FName{0, 0};
    *reinterpret_cast<bool*>(frame + g_addImpulsePVelChange) = false;
    R::CallFunction(mesh, g_addImpulseFn, frame);
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

void OnRelease(const coop::net::PropReleasePayload& payload) {
    const std::wstring keyW = KeyToWString(payload.key);
    UE_LOGI("remote_prop: RELEASE wire '%ls' impulse=(%.1f, %.1f, %.1f)",
            keyW.c_str(), payload.impulseX, payload.impulseY, payload.impulseZ);
    // If we don't currently drive this prop (e.g. release arrived without a
    // preceding PropPose stream because we missed/dropped them), resolve fresh
    // so we can re-enable physics + apply the impulse.
    void* meshToActOn = nullptr;
    if (g_drive.actor && KeyMatchesCache(payload.key)) {
        meshToActOn = g_drive.mesh;
    } else {
        if (void* prop = ue_wrap::prop::FindByKeyString(keyW)) {
            meshToActOn = ue_wrap::prop::GetStaticMesh(prop);
        }
    }
    if (meshToActOn) {
        DriveSimulate(meshToActOn, true);
        const bool hasImpulse =
            payload.impulseX != 0.f || payload.impulseY != 0.f || payload.impulseZ != 0.f;
        if (hasImpulse) {
            DriveAddImpulse(meshToActOn, payload.impulseX, payload.impulseY, payload.impulseZ);
        }
    }
    g_drive.actor = nullptr;
    g_drive.mesh = nullptr;
    g_drive.lastKey.clear();
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

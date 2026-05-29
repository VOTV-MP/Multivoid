// coop/remote_prop.cpp -- v4 receiver implementation.

#include "coop/remote_prop.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/prop_echo_suppress.h"
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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
    std::string  lastKey;        // ASCII (the Aprop_C save UUID format)
    uint64_t     lastApplyMs = 0;
};
std::array<ActiveDrive, coop::players::kMaxPeers> g_drives{};

// True if `actor` is the cached drive target of ANY slot's drive state.
}  // namespace [drive helpers part 1]

// Public accessor (M-1 2026-05-29 split): used by coop::remote_prop_spawn::
// OnSpawn convergence skip path. Predicate over the drive cache; tells the
// spawn receiver "this actor is currently being kinematically driven, so
// skip transform convergence (PropPose owns position)".
bool IsActorUnderAnyDrive(void* actor) {
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
    // Seed the timeout clock with NOW. Without this, lastApplyMs stays at
    // zero (struct default); the stream-stop timeout at Tick() compares
    // (NowMs() - lastApplyMs > 500) and fires immediately, releasing the
    // grab on the very first packet drop / late-arrival after a fresh
    // grab. See research/findings/votv-coop-audit-post-pr4-7-2026-05-28.md.
    g_drives[slot].lastApplyMs = NowMs();
}

}  // namespace

void Tick(coop::net::Session& session) {
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

void OnRelease(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer) {
    const std::wstring keyW = KeyToWString(payload.key);
    const float linSpeedSq = payload.linVelX * payload.linVelX +
                             payload.linVelY * payload.linVelY +
                             payload.linVelZ * payload.linVelZ;
    const float linSpeed = std::sqrt(linSpeedSq);
    UE_LOGI("remote_prop: RELEASE wire '%ls' linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f) deg/s",
            keyW.c_str(),
            payload.linVelX, payload.linVelY, payload.linVelZ, linSpeed,
            payload.angVelX, payload.angVelY, payload.angVelZ);
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
        // fresh-resolve path below.
        if (d.actor && KeyMatchesCache(senderSlot, payload.key)) {
            releasedSlot = senderSlot;
            propActor = d.actor;
            meshToActOn = d.mesh;
        }
    }
    if (releasedSlot < 0) {
        if (void* prop = ue_wrap::prop::FindByKeyString(keyW)) {
            // Release arrived without a matching drive cache entry --
            // resolve fresh from the live world.
            propActor = prop;
            meshToActOn = ue_wrap::prop::GetStaticMesh(prop);
        }
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
    // Clear only the matching slot's drive state -- other slots' active
    // drives (if any) stay intact.
    if (releasedSlot >= 0) {
        g_drives[releasedSlot].actor = nullptr;
        g_drives[releasedSlot].mesh = nullptr;
        g_drives[releasedSlot].lastKey.clear();
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
// existing local actor. That actor may also be tracked by prop_lifecycle's
// own g_propElementsById in this peer's allocation range (host or peer).
// Two Elements per actor is acceptable: one is the LOCAL handle (our own
// alloc range), the other is the MIRROR handle (sender's alloc range).
// Both resolve to the same actor via GetActor(). The Registry's m_byId
// slots don't collide because host/peer ranges are disjoint.
// B2 (2026-05-29): per-type mirror map backed by the generic
// coop::element::MirrorManager<Prop>. The encapsulated 5-step pattern
// (alloc-under-lock -> RegisterMirror -> rollback-on-fail) lives in the
// template; this file's RegisterPropMirror/UnregisterPropMirror/
// DrainAllPropMirrors become thin wrappers that handle prop-specific
// concerns (name/typeName/actor stamping, log lines).
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
                        const std::wstring& cls) {
    if (!actor) return;
    // Build the Prop mirror with name/typeName/actor populated, then
    // hand off to MirrorManager::Install. The template encapsulates
    // the 5-step pattern (alloc-under-lock + RegisterMirror +
    // rollback-on-fail with dtor outside lock), so the wire-receiver
    // side just builds and ships.
    auto mirror = std::make_unique<coop::element::Prop>();
    if (!key.empty()) mirror->SetName(NarrowAscii(key));
    if (!cls.empty()) mirror->SetTypeName(NarrowAscii(cls));
    mirror->SetActor(actor);
    if (PropMirrors().Install(eid, std::move(mirror))) {
        UE_LOGI("remote_prop::RegisterPropMirror: eid=%u bound to actor=%p "
                "key='%ls' cls='%ls'",
                eid, actor, key.c_str(), cls.c_str());
    }
    // On false: Install already logged the failure case (rejected
    // sentinel id, duplicate, or RegisterMirror failure).
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
    UE_LOGI("remote_prop::UnregisterPropMirror: eid=%u drained", eid);
    // drained falls out of scope here; ~Prop -> ~Element ->
    // Registry::UnregisterMirror (via m_mirror=true).
}

// Bulk-drain on full session teardown. Forwards to MirrorManager;
// each Prop's dtor calls Registry::UnregisterMirror (via m_mirror=true)
// sequentially as the drained map releases.
size_t DrainAllPropMirrors() {
    return PropMirrors().DrainAll();
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

void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer) {
    const std::wstring keyW = KeyToWString(payload.key);
    if (keyW.empty()) {
        UE_LOGW("remote_prop::OnDestroy: empty key -- dropping");
        return;
    }
    // A2 (2026-05-29): drain the wire-received mirror Element FIRST,
    // before any actor lookup. The mirror is wire-identity bookkeeping;
    // it must vacate the Registry regardless of whether the local actor
    // still exists (e.g. echo bounce where we initiated the destroy and
    // the actor is already gone). UnregisterPropMirror is silent no-op
    // for unknown eids (legacy senders with elementId==0, or pre-A2
    // PropDestroy packets that never registered a mirror).
    UnregisterPropMirror(payload.elementId);
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
    // If any slot was kinematically driving this prop, clear that slot's
    // cache so we don't try to drive a destroyed actor next tick.
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
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

void ForceRelease() {
    // Force-release on aggregate disconnect/teardown clears every slot's
    // drive. Per-slot disconnect uses OnDisconnectForSlot instead.
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
    // A2 (2026-05-29): drain wire-received Prop mirrors on full session
    // teardown. Each mirror's dtor routes through Registry::UnregisterMirror
    // (m_mirror=true) so the Registry's m_byId slots in the foreign
    // allocation range are returned to nullptr. Per-slot disconnect does
    // NOT drain mirrors (mirrors aren't tagged with senderSlot yet -- a
    // future enhancement); convergent local actors persist across per-slot
    // disconnect just like remote_prop's drive cache does.
    const size_t mirrorsDrained = DrainAllPropMirrors();
    if (mirrorsDrained > 0) {
        UE_LOGI("remote_prop: force-release drained %zu wire-received Prop mirror(s)",
                mirrorsDrained);
    }
}

void OnDisconnectForSlot(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    ActiveDrive& d = g_drives[peerSlot];
    if (!d.actor) return;
    if (d.mesh) DriveSimulate(d.mesh, true);
    UE_LOGI("remote_prop: peer slot %d disconnected -- releasing held prop (key='%s')",
            peerSlot, d.lastKey.c_str());
    d.actor = nullptr;
    d.mesh = nullptr;
    d.lastKey.clear();
}

}  // namespace coop::remote_prop

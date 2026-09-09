// ue_wrap/actors/hook.cpp -- engine access and visual mirror management for VOTV
// grappling hooks and ropes (Ahook_C and derivatives).
// Principle-7 engine-wrapper layer (no network/coop state).

#include "ue_wrap/actors/hook.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace ue_wrap::hook {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

// UClasses
void* g_hookCls      = nullptr;  // hook_C
void* g_ropeCls      = nullptr;  // rope_C
void* g_hookChildCls = nullptr;  // hook_Child_C
void* g_hookFleshCls = nullptr;  // hook_flesh_C
void* g_cableCompCls = nullptr;  // CableComponent
void* g_sceneCompCls = nullptr;  // SceneComponent
void* g_primCompCls  = nullptr;  // PrimitiveComponent

// Offsets on Ahook_C
int32_t g_hookSingleOff   = 0x0278; // hook_single (Head A, UStaticMeshComponent*)
int32_t g_hookSingle1Off  = 0x0270; // hook_single1 (Head B, UStaticMeshComponent*)
int32_t g_cableOff        = 0x0290; // Cable (UCableComponent*)
int32_t g_distOff         = 0x02C0; // dist (float, cable length)
int32_t g_attachedAOff    = 0x02C4; // attached_a (bool)
int32_t g_attachedBOff    = 0x02C5; // attached_b (bool)
int32_t g_isThrownOff     = 0x02D8; // isThrown (bool)
int32_t g_playerHookedOff = 0x037C; // playerHooked (bool)

// Offsets on UCableComponent
int32_t g_cableLengthOff  = 0x04BC; // CableLength (float)
int32_t g_cableGravityOff = 0x04E4; // CableGravityScale (float)

// UFunctions
void* g_setWorldLocAndRotFn = nullptr; // SceneComponent::K2_SetWorldLocationAndRotation
void* g_setCollisionFn      = nullptr; // PrimitiveComponent::SetCollisionEnabled
void* g_setAttachEndFn      = nullptr; // CableComponent::SetAttachEndToComponent

template <typename T>
inline T ReadField(void* base, int32_t off) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(base) + off);
}

template <typename T>
inline void WriteField(void* base, int32_t off, const T& val) {
    *reinterpret_cast<T*>(reinterpret_cast<char*>(base) + off) = val;
}

bool SetComponentWorldLocAndRot(void* component, const FVector& loc, const FRotator& rot) {
    if (!component || !R::IsLive(component) || !g_setWorldLocAndRotFn) return false;
    ParamFrame f(g_setWorldLocAndRotFn);
    f.SetRaw(L"NewLocation", &loc, sizeof(loc));
    f.SetRaw(L"NewRotation", &rot, sizeof(rot));
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);
    return Call(component, f);
}

bool SetPrimCollisionEnabled(void* prim, uint8_t type) {
    if (!prim || !R::IsLive(prim) || !g_setCollisionFn) return false;
    ParamFrame f(g_setCollisionFn);
    f.Set<uint8_t>(L"NewType", type);
    return Call(prim, f);
}

bool SetCableAttachEnd(void* cableComp, void* endComp) {
    if (!cableComp || !endComp || !g_setAttachEndFn) return false;
    ParamFrame f(g_setAttachEndFn);
    f.Set<void*>(L"Component", endComp);
    R::FName noneSocket{0, 0};
    f.SetRaw(L"SocketName", &noneSocket, sizeof(noneSocket));
    return Call(cableComp, f);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* hookCls = R::FindClass(L"hook_C");
    if (!hookCls) return false;

    g_hookCls      = hookCls;
    g_ropeCls      = R::FindClass(L"rope_C");
    g_hookChildCls = R::FindClass(L"hook_Child_C");
    g_hookFleshCls = R::FindClass(L"hook_flesh_C");
    g_cableCompCls = R::FindClass(L"CableComponent");
    g_sceneCompCls = R::FindClass(L"SceneComponent");
    g_primCompCls  = R::FindClass(L"PrimitiveComponent");

    int32_t off;
    if ((off = R::FindPropertyOffset(hookCls, L"hook_single")) >= 0)   g_hookSingleOff = off;
    if ((off = R::FindPropertyOffset(hookCls, L"hook_single1")) >= 0)  g_hookSingle1Off = off;
    if ((off = R::FindPropertyOffset(hookCls, L"Cable")) >= 0)         g_cableOff = off;
    if ((off = R::FindPropertyOffset(hookCls, L"dist")) >= 0)          g_distOff = off;
    if ((off = R::FindPropertyOffset(hookCls, L"attached_a")) >= 0)    g_attachedAOff = off;
    if ((off = R::FindPropertyOffset(hookCls, L"attached_b")) >= 0)    g_attachedBOff = off;
    if ((off = R::FindPropertyOffset(hookCls, L"isThrown")) >= 0)      g_isThrownOff = off;
    if ((off = R::FindPropertyOffset(hookCls, L"playerHooked")) >= 0)  g_playerHookedOff = off;

    if (g_cableCompCls) {
        if ((off = R::FindPropertyOffset(g_cableCompCls, L"CableLength")) >= 0) {
            g_cableLengthOff = off;
        }
        if ((off = R::FindPropertyOffset(g_cableCompCls, L"CableGravityScale")) >= 0) {
            g_cableGravityOff = off;
        }
        g_setAttachEndFn = R::FindFunction(g_cableCompCls, L"SetAttachEndToComponent");
    }

    if (g_sceneCompCls) {
        g_setWorldLocAndRotFn = R::FindFunction(g_sceneCompCls, L"K2_SetWorldLocationAndRotation");
    }

    if (g_primCompCls) {
        g_setCollisionFn = R::FindFunction(g_primCompCls, L"SetCollisionEnabled");
    }

    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("hook: resolved hook_C=%p rope_C=%p single@0x%04X single1@0x%04X cable@0x%04X dist@0x%04X",
            g_hookCls, g_ropeCls, g_hookSingleOff, g_hookSingle1Off, g_cableOff, g_distOff);
    return true;
}

bool IsHook(void* obj) {
    if (!obj) return false;
    void* cls = R::ClassOf(obj);
    return IsHookClass(cls);
}

bool IsHookClass(void* cls) {
    if (!cls) return false;
    if (!EnsureResolved()) return false;
    void* bases[] = { g_hookCls, g_ropeCls, g_hookChildCls, g_hookFleshCls };
    size_t count = 1;
    if (g_ropeCls) bases[count++] = g_ropeCls;
    if (g_hookChildCls) bases[count++] = g_hookChildCls;
    if (g_hookFleshCls) bases[count++] = g_hookFleshCls;
    return R::IsDescendantOfAny(cls, bases, count);
}

uint8_t ClassTypeOf(void* actor) {
    if (!actor) return 0;
    void* cls = R::ClassOf(actor);
    if (!cls) return 0;
    if (cls == g_ropeCls) return 1;
    if (cls == g_hookChildCls) return 2;
    if (cls == g_hookFleshCls) return 3;
    return 0; // default hook_C
}

bool ReadHookSnapshot(void* hookActor, HookSnapshot& out) {
    if (!hookActor || !R::IsLive(hookActor)) return false;
    if (!EnsureResolved()) return false;

    out.classType    = ClassTypeOf(hookActor);
    out.attachedA    = ReadField<bool>(hookActor, g_attachedAOff);
    out.attachedB    = ReadField<bool>(hookActor, g_attachedBOff);
    out.playerHooked = ReadField<bool>(hookActor, g_playerHookedOff);
    out.isThrown     = ReadField<bool>(hookActor, g_isThrownOff);
    out.cableLength  = ReadField<float>(hookActor, g_distOff);

    void* compA = ReadField<void*>(hookActor, g_hookSingleOff);
    if (compA && R::IsLive(compA)) {
        out.posA = ue_wrap::engine::GetComponentLocation(compA);
        out.rotA = ue_wrap::engine::GetComponentWorldRotation(compA);
    } else {
        out.posA = ue_wrap::engine::GetActorLocation(hookActor);
        out.rotA = ue_wrap::engine::GetActorRotation(hookActor);
    }

    void* compB = ReadField<void*>(hookActor, g_hookSingle1Off);
    if (compB && R::IsLive(compB)) {
        out.posB = ue_wrap::engine::GetComponentLocation(compB);
        out.rotB = ue_wrap::engine::GetComponentWorldRotation(compB);
    } else {
        out.posB = out.posA;
        out.rotB = out.rotA;
    }

    out.valid = true;
    return true;
}

void* SpawnHookMirror(uint8_t classType, const HookSnapshot& initial) {
    if (!EnsureResolved()) return nullptr;

    void* cls = g_hookCls;
    if (classType == 1 && g_ropeCls) cls = g_ropeCls;
    else if (classType == 2 && g_hookChildCls) cls = g_hookChildCls;
    else if (classType == 3 && g_hookFleshCls) cls = g_hookFleshCls;

    if (!cls) return nullptr;

    void* mirror = ue_wrap::engine::SpawnActor(cls, initial.posA);
    if (!mirror) return nullptr;

    // Make completely non-colliding, kinematic, and disable BP tick to prevent
    // single-player ubergraph from overriding networked mirror transforms.
    ue_wrap::engine::SetActorTickEnabled(mirror, false);
    ue_wrap::engine::SetActorEnableCollision(mirror, false);
    // Do not dispatch SetSimulatePhysics here.  In this game build the call
    // consistently faults from the ProcessEvent outer callback while a hook
    // mirror is being constructed (see client log).  Collision and actor tick
    // are enough to keep this display-only actor outside gameplay simulation.

    void* compA = ReadField<void*>(mirror, g_hookSingleOff);
    void* compB = ReadField<void*>(mirror, g_hookSingle1Off);
    void* cableComp = ReadField<void*>(mirror, g_cableOff);

    if (compA && R::IsLive(compA)) {
        ue_wrap::engine::SetComponentMobility(compA, 2); // Movable
        SetPrimCollisionEnabled(compA, 0); // None
    }
    if (compB && R::IsLive(compB)) {
        ue_wrap::engine::SetComponentMobility(compB, 2); // Movable
        SetPrimCollisionEnabled(compB, 0); // None
    }

    if (cableComp && compB && R::IsLive(compB)) {
        SetCableAttachEnd(cableComp, compB);
    }

    ApplyMirrorSnapshot(mirror, initial, nullptr);
    return mirror;
}

void ApplyMirrorSnapshot(void* mirrorActor, const HookSnapshot& state, void* puppetActor) {
    if (!mirrorActor || !R::IsLive(mirrorActor)) return;

    void* compA = ReadField<void*>(mirrorActor, g_hookSingleOff);
    void* compB = ReadField<void*>(mirrorActor, g_hookSingle1Off);
    void* cableComp = ReadField<void*>(mirrorActor, g_cableOff);

    // Keep actor root near Head A so UE does not cull the actor
    ue_wrap::engine::SetActorLocation(mirrorActor, state.posA);

    if (compA && R::IsLive(compA)) {
        SetComponentWorldLocAndRot(compA, state.posA, state.rotA);
    }

    FVector posB = state.posB;
    FRotator rotB = state.rotB;

    // A rope with both ends attached is autonomous: Ahook_C may retain a
    // stale playerHooked flag from the preceding hold/throw transition, but
    // neither visual endpoint belongs to that player any more.  In that mode
    // the transmitted attachment locations are authoritative.
    if (state.playerHooked && !(state.attachedA && state.attachedB) &&
        puppetActor && R::IsLive(puppetActor)) {
        void* skel = ue_wrap::puppet::GetSkeletalMeshComponent(puppetActor);
        FVector handLoc{};
        if (skel && ue_wrap::engine::GetBoneWorldLocationByName(skel, L"hand_r", handLoc)) {
            posB = handLoc;
        } else {
            posB = ue_wrap::engine::GetActorLocation(puppetActor);
            posB.Z += 60.f;
        }
    }

    if (compB && R::IsLive(compB)) {
        SetComponentWorldLocAndRot(compB, posB, rotB);
    }

    // `dist` is the gameplay rope allowance, not necessarily the chord length
    // of its rendered endpoints.  Copying it directly leaves slack in a
    // CableComponent mirror, so a rope whose two ends are fixed visibly sags
    // on receivers.  A fixed/fixed rope has no permitted slack: make the
    // visual cable exactly the endpoint distance.  Dynamic states retain the
    // authoritative allowance and therefore keep the game's normal motion.
    float visualCableLength = state.cableLength;
    if (state.attachedA && state.attachedB) {
        const float dx = posB.X - state.posA.X;
        const float dy = posB.Y - state.posA.Y;
        const float dz = posB.Z - state.posA.Z;
        visualCableLength = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Update cable length on mirror.  Zero is valid for coincident endpoints;
    // reject only corrupt packet values before writing an engine property.
    if (std::isfinite(visualCableLength) && visualCableLength >= 0.f) {
        WriteField<float>(mirrorActor, g_distOff, visualCableLength);
        if (cableComp && R::IsLive(cableComp)) {
            WriteField<float>(cableComp, g_cableLengthOff, visualCableLength);
            // With no slack, gravity is still visible as numerical sag in the
            // cable solver.  A rope anchored at both ends is a static visual
            // segment on a mirror, so suppress that local-only simulation.
            if (state.attachedA && state.attachedB) {
                WriteField<float>(cableComp, g_cableGravityOff, 0.f);
            }
        }
    }
}

void DestroyHookMirror(void* mirrorActor) {
    if (!mirrorActor || !R::IsLive(mirrorActor)) return;
    ue_wrap::engine::DestroyActor(mirrorActor);
}

}  // namespace ue_wrap::hook

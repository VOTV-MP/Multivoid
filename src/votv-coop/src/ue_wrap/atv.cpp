// ue_wrap/atv.cpp -- see ue_wrap/atv.h. Engine access for the ATV/quadbike (AATV_C). Offsets
// resolved from the live class via reflection (version-portable); the Alpha 0.9.0-n values are
// logged fallbacks. Transform reads/writes go through ue_wrap::engine at the actor level (the
// root Mesh@0x0570 is the actor root, so the actor transform IS the physics body transform).

#include "ue_wrap/atv.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::atv {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};
void*   g_cls         = nullptr;  // ATV_C UClass
int32_t g_keyOff      = -1;       // Key      (Alpha 0.9.0-n: 0x0618, FName)
int32_t g_playerOff   = -1;       // Player   (0x05B0, AmainPlayer_C*)
int32_t g_isDrivenOff = -1;       // isDriven (0x05F7, bool)
int32_t g_fuelOff     = -1;       // fuel     (0x05D4, float)
int32_t g_healthOff   = -1;       // health   (0x05E4, float)
int32_t g_brakeOff    = -1;       // Brake    (0x05D9, bool)

int32_t ResolveOff(void* cls, const wchar_t* name, int32_t fallback) {
    int32_t o = R::FindPropertyOffset(cls, name);
    if (o < 0) {
        UE_LOGW("atv: %ls offset not found -- using fallback 0x%04X", name, fallback);
        o = fallback;
    }
    return o;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(L"ATV_C");
    if (!cls) return false;

    g_keyOff      = ResolveOff(cls, L"Key",      0x0618);
    g_playerOff   = ResolveOff(cls, L"Player",   0x05B0);
    g_isDrivenOff = ResolveOff(cls, L"isDriven", 0x05F7);
    g_fuelOff     = ResolveOff(cls, L"fuel",     0x05D4);
    g_healthOff   = ResolveOff(cls, L"health",   0x05E4);
    g_brakeOff    = ResolveOff(cls, L"Brake",    0x05D9);

    g_cls = cls;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("atv: resolved ATV_C=%p Key@0x%04X Player@0x%04X isDriven@0x%04X fuel@0x%04X "
            "health@0x%04X Brake@0x%04X",
            cls, g_keyOff, g_playerOff, g_isDrivenOff, g_fuelOff, g_healthOff, g_brakeOff);
    return true;
}

bool IsAtv(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_cls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* atv) {
    if (!atv || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(atv) + g_keyOff);
    return R::ToString(key);
}

bool GetRootTransform(void* atv, FVector& loc, FRotator& rot) {
    if (!atv) return false;
    loc = engine::GetActorLocation(atv);
    rot = engine::GetActorRotation(atv);
    return true;
}

bool GetRootVelocity(void* atv, FVector& vel) {
    if (!atv) return false;
    vel = engine::GetActorVelocity(atv);
    return true;
}

void* GetOccupantPlayer(void* atv) {
    if (!atv || g_playerOff < 0) return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(atv) + g_playerOff);
}

bool IsDriven(void* atv) {
    if (!atv || g_isDrivenOff < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(atv) + g_isDrivenOff);
}

float GetFuel(void* atv) {
    if (!atv || g_fuelOff < 0) return 0.f;
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(atv) + g_fuelOff);
}

float GetHealth(void* atv) {
    if (!atv || g_healthOff < 0) return 0.f;
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(atv) + g_healthOff);
}

bool GetBrake(void* atv) {
    if (!atv || g_brakeOff < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(atv) + g_brakeOff);
}

bool DriveMirrorTransform(void* atv, const FVector& loc, const FRotator& rot) {
    if (!atv) return false;
    bool ok = engine::SetActorLocation(atv, loc);
    ok = engine::SetActorRotation(atv, rot) && ok;
    return ok;
}

void PrepareMirror(void* atv) {
    if (!atv) return;
    engine::SetActorSimulatePhysics(atv, false);          // the rig must not integrate vs the stream
    engine::SetActorTickEnabled(atv, false);              // suppress the ATV BP ReceiveTick/AI
    engine::SetActorRootNotifyRigidBodyCollision(atv, false);  // mirror collisions don't fire BP hit events
}

void ReleaseMirror(void* atv) {
    if (!atv) return;
    engine::SetActorSimulatePhysics(atv, true);           // restore the local physics rig (this peer drives now)
    engine::SetActorTickEnabled(atv, true);               // restore the ATV BP tick (its own driving logic)
    engine::SetActorRootNotifyRigidBodyCollision(atv, true);
}

}  // namespace ue_wrap::atv

// ue_wrap/devices/atv_passenger_seat.cpp -- see ue_wrap/devices/atv_passenger_seat.h.

#include "ue_wrap/devices/atv_passenger_seat.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ue_wrap::atv_passenger_seat {
namespace {

namespace R = reflection;

float g_aheadCm = -60.f;     // `sitarrow` is under the seat's middle; the passenger sits on its back half
// The sit box's centre above `sitarrow`: where the game's own sit puts the capsule, which the passenger lane then raises
// or lowers to put the eye at kEyeAboveRootCm (the sit's crouch takes or not by timing, and moves the eye 45 cm).
float g_boxUpCm = 170.f;
constexpr float kBodyAheadCm = 25.f;  // a remote body's pivot ahead of `sitarrow`: the seat pose sits it behind the driver
constexpr float kBodyUpCm = 25.f;      // a remote body's pivot above `sitarrow`, the height the driver's body rides at
constexpr float kGetOffCm = 120.f;     // the box's get-off point, from its centre
constexpr float kGetOffDeg = 90.f;     // to the ATV's right

// An object field of `obj` by name, live or null. The offset is asked once per call site's class.
void* ObjField(void* obj, const wchar_t* name) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* o = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off);
    return o && R::IsLive(o) ? o : nullptr;
}

template <class T>
bool WriteField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(T));
    return true;
}

}  // namespace

void SetSeatOffsetsForDrill(float aheadCm, float upCm) {
    g_aheadCm = aheadCm;
    g_boxUpCm = upCm;
}

bool SitBoxTransform(void* atv, FVector& loc, FRotator& rot) {
    if (!atv || !R::IsLive(atv)) return false;
    void* sit = ObjField(atv, L"sitarrow");
    FRotator atvRot{};
    FVector at{};
    if (!sit || !engine::TryGetComponentLocation(sit, at) || !engine::TryGetActorRotation(atv, atvRot)) return false;
    const float yaw = atvRot.Yaw * 0.01745329252f;
    loc = FVector{at.X + std::cos(yaw) * g_aheadCm, at.Y + std::sin(yaw) * g_aheadCm, at.Z + g_boxUpCm};
    rot = FRotator{0.f, atvRot.Yaw, 0.f};
    return true;
}

bool BodyTransform(void* atv, FVector& loc, FRotator& rot) {
    void* sit = ObjField(atv, L"sitarrow");
    FRotator atvRot{};
    FVector at{};
    if (!atv || !R::IsLive(atv) || !sit || !engine::TryGetComponentLocation(sit, at) ||
        !engine::TryGetActorRotation(atv, atvRot))
        return false;
    const float yaw = atvRot.Yaw * 0.01745329252f;
    loc = FVector{at.X + std::cos(yaw) * kBodyAheadCm, at.Y + std::sin(yaw) * kBodyAheadCm, at.Z + kBodyUpCm};
    rot = FRotator{0.f, atvRot.Yaw, 0.f};
    return true;
}

void* SpawnSitBox(void* atv) {
    void* cls = R::FindClass(L"sitBox_C");
    FVector at{};
    FRotator rot{};
    if (!cls || !SitBoxTransform(atv, at, rot)) return nullptr;
    void* box = engine::BeginDeferredSpawn(cls, at, rot);
    if (!box) return nullptr;
    // Read by the box's construction script, which the finish runs: the get-off point's angle and distance.
    WriteField(box, L"distance", kGetOffCm);
    WriteField(box, L"getoffAngle", kGetOffDeg);
    WriteField(box, L"enableIcon", false);
    if (!engine::FinishDeferredSpawn(box, at, rot)) return nullptr;
    if (!engine::AttachActorToActorKeepWorld(box, atv, /*weld*/false)) {
        UE_LOGW("atv_passenger_seat: the sit box did not attach to ATV %p", atv);
        engine::DestroyActor(box);
        return nullptr;
    }
    return box;
}

bool Sit(void* player, void* atv, void* box) {
    if (!player || !atv || !box || !R::IsLive(player)) return false;
    void* f = R::FindFunction(R::ClassOf(player), L"sit");
    ParamFrame pf(f);
    return f && pf.valid() && pf.Set<void*>(L"actor", atv) && pf.Set<void*>(L"component", nullptr) &&
           pf.Set<void*>(L"place", box) && Call(player, pf);
}

bool Unsit(void* player) {
    if (!player || !R::IsLive(player)) return false;
    void* f = R::FindFunction(R::ClassOf(player), L"unsit");
    ParamFrame pf(f);
    return f && pf.valid() && Call(player, pf);
}

}  // namespace ue_wrap::atv_passenger_seat

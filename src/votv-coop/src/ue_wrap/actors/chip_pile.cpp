// ue_wrap/actors/chip_pile.cpp -- see chip_pile.h.

#include "ue_wrap/actors/chip_pile.h"

#include "ue_wrap/actors/prop.h"               // IsChipPile
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"             // TryGetActorRotation, GetComponentWorldRotation
#include "ue_wrap/engine/engine_component.h"   // SetComponentMobility

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ue_wrap::chip_pile {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;

constexpr uint8_t kMobilityStatic  = 0;
constexpr uint8_t kMobilityMovable = 2;

// The `StaticMesh` field's offset per pile class (the variants are subclasses with the same field;
// -1 = the class has none). A layout fact, the same in every world that loads the same cooked class.
std::mutex g_mu;
std::unordered_map<void*, int32_t> g_meshOffByClass;

int32_t MeshOffset(void* cls) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_meshOffByClass.find(cls);
    if (it != g_meshOffByClass.end()) return it->second;
    const int32_t off = R::FindPropertyOffset(cls, L"StaticMesh");
    g_meshOffByClass[cls] = off;
    return off;
}

}  // namespace

void* VisibleMesh(void* actor) {
    if (!actor || !R::IsLive(actor) || !ue_wrap::prop::IsChipPile(actor)) return nullptr;
    const int32_t off = MeshOffset(R::ClassOf(actor));
    if (off < 0) return nullptr;
    void* comp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + off);
    return (comp && R::IsLive(comp)) ? comp : nullptr;
}

bool VisibleMeshWorldRotation(void* actor, FRotator& out) {
    if (void* comp = VisibleMesh(actor)) {
        out = E::GetComponentWorldRotation(comp);
        return true;
    }
    return E::TryGetActorRotation(actor, out);
}

bool ReadLook(void* actor, Look& out) {
    void* comp = VisibleMesh(actor);
    if (!comp) return false;
    // Layout of the engine's own scene component: one resolve per process.
    static int32_t rotOff = -2, sclOff = -2;
    if (rotOff == -2) {
        void* sc = R::FindClass(P::name::SceneComponentClass);
        rotOff = sc ? R::FindPropertyOffset(sc, P::name::RelativeRotationProp) : -1;
        sclOff = sc ? R::FindPropertyOffset(sc, P::name::RelativeScale3DProp) : -1;
    }
    if (rotOff < 0 || sclOff < 0) return false;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(comp);
    out.relRotation = *reinterpret_cast<const FRotator*>(base + rotOff);
    out.relScale    = *reinterpret_cast<const FVector*>(base + sclOff);
    return true;
}

bool ApplyLook(void* actor, const Look& look) {
    void* comp = VisibleMesh(actor);
    if (!comp) return false;
    static void* rotFn = nullptr;
    static void* sclFn = nullptr;
    if (!rotFn || !sclFn) {
        if (void* sc = R::FindClass(P::name::SceneComponentClass)) {
            rotFn = R::FindFunction(sc, P::name::SetRelativeRotationFn);
            sclFn = R::FindFunction(sc, P::name::SetRelativeScale3DFn);
        }
    }
    if (!rotFn || !sclFn) return false;
    E::SetComponentMobility(comp, kMobilityMovable);
    ParamFrame fr(rotFn);
    fr.SetRaw(L"NewRotation", &look.relRotation, sizeof(look.relRotation));
    fr.Set<bool>(L"bSweep", false);
    fr.Set<bool>(L"bTeleport", true);
    const bool rotOk = Call(comp, fr);
    ParamFrame fs(sclFn);
    fs.SetRaw(L"NewScale3D", &look.relScale, sizeof(look.relScale));
    const bool sclOk = Call(comp, fs);
    E::SetComponentMobility(comp, kMobilityStatic);
    return rotOk && sclOk;
}

}  // namespace ue_wrap::chip_pile

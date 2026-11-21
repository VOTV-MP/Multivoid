// ue_wrap/actors/character_base.cpp -- see ue_wrap/actors/character_base.h.
//
// The layout, from the game's Engine.hpp dump (CXXHeaderDump): ACharacter::BasedMovement is an FBasedMovementInfo
// (Engine.hpp:6973) whose first member is `UPrimitiveComponent* MovementBase` (Engine.hpp:830-832). Both offsets are
// resolved by reflection here and logged, so a build that moved them says so.

#include "ue_wrap/actors/character_base.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::character_base {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

struct Layout {
    bool    resolved = false;
    bool    usable = false;
    void*   characterClass = nullptr;   // native, never collected
    int32_t baseOff = -1;               // BasedMovement in the character
    int32_t componentOff = -1;          // MovementBase in the struct
};
Layout g_layout;

void Resolve() {
    Layout& l = g_layout;
    l.resolved = true;
    l.characterClass = R::FindClass(L"Character");
    if (!l.characterClass) {
        UE_LOGE("character_base: the Character class did not resolve -- no movement base is read");
        return;
    }
    l.baseOff = R::FindPropertyOffset(l.characterClass, L"BasedMovement");
    void* const info = R::PropertyInnerStruct(l.characterClass, L"BasedMovement");
    l.componentOff = info ? R::FindPropertyOffset(info, L"MovementBase") : -1;
    l.usable = l.baseOff >= 0 && l.componentOff >= 0 &&
               l.baseOff + l.componentOff + static_cast<int32_t>(sizeof(void*)) <= R::StructSize(l.characterClass);
    if (!l.usable) {
        UE_LOGE("character_base: the engine's BasedMovement layout did not resolve (BasedMovement@%d MovementBase@%d) -- no "
                "movement base is read", l.baseOff, l.componentOff);
        return;
    }
    UE_LOGI("character_base: BasedMovement@0x%04X MovementBase@0x%04X", l.baseOff, l.componentOff);
}

bool IsCharacter(void* obj) {
    if (!g_layout.resolved) Resolve();
    void* cls = g_layout.characterClass;
    return obj && cls && R::IsLive(obj) && R::IsDescendantOfAny(R::ClassOf(obj), &cls, 1);
}

}  // namespace

bool ReadMovementBase(void* character, Base& out) {
    out = Base{};
    if (!IsCharacter(character) || !g_layout.usable) return false;
    void* const comp = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(character) + g_layout.baseOff +
                                                      g_layout.componentOff);
    if (!comp || !R::IsLive(comp)) return true;
    out.component = comp;
    void* const owner = R::OuterOf(comp);
    out.owner = owner && R::IsLive(owner) ? owner : nullptr;
    return true;
}

void* MovementBaseComponent(void* character) {
    if (!IsCharacter(character) || !g_layout.usable) return nullptr;
    void* const comp = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(character) + g_layout.baseOff +
                                                      g_layout.componentOff);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

void* CapsuleComponent(void* character) {
    if (!IsCharacter(character)) return nullptr;
    void* const capsule = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(character) +
                                                         P::off::ACharacter_CapsuleComponent);
    return capsule && R::IsLive(capsule) ? capsule : nullptr;
}

}  // namespace ue_wrap::character_base

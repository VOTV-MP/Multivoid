// ue_wrap/actors/character_floor.cpp -- see ue_wrap/actors/character_floor.h.

#include "ue_wrap/actors/character_floor.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace ue_wrap::character_floor {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

UE_FACT_FN(kRowFindFloor, "character_floor", L"CharacterMovementComponent", L"K2_FindFloor", Declared);

// What the engine's layout says, read once from the reflected class and function. FFindFloorResult is never mirrored by a
// hand-written struct: its size is the reflected struct's, and the same bytes move from the call's frame to CurrentFloor.
struct Layout {
    bool                 resolved = false;
    bool                 usable = false;
    void*                fn = nullptr;
    int32_t              locationOff = -1;      // CapsuleLocation in the frame
    int32_t              resultOff = -1;        // FloorResult in the frame
    int32_t              floorSize = 0;         // sizeof(FFindFloorResult), from the property's struct
    int32_t              currentFloorOff = -1;  // CurrentFloor in the movement component
    std::vector<uint8_t> frame;                 // the call's frame, kept
};
Layout g_layout;

void Resolve() {
    Layout& l = g_layout;
    l.resolved = true;
    void* const cls = R::FindClass(L"CharacterMovementComponent");
    const facts::Resolved fn = facts::Resolve(kRowFindFloor);
    if (!cls || !fn) {
        UE_LOGE("character_floor: CharacterMovementComponent or its K2_FindFloor did not resolve -- a mirror's floor is not refreshed");
        return;
    }
    l.fn = fn.Fn();
    l.locationOff = R::FindParamOffset(l.fn, L"CapsuleLocation");
    l.resultOff = R::FindParamOffset(l.fn, L"FloorResult");
    const int32_t resultSize = R::FindParamSize(l.fn, L"FloorResult");
    l.currentFloorOff = R::FindPropertyOffset(cls, L"CurrentFloor");
    l.floorSize = R::StructSize(R::PropertyInnerStruct(cls, L"CurrentFloor"));
    const int32_t frameSize = R::FunctionFrameSize(l.fn);
    const bool fits = l.locationOff >= 0 && l.resultOff >= 0 && l.floorSize > 0 && l.currentFloorOff >= 0 &&
                      l.locationOff + static_cast<int32_t>(sizeof(FVector)) <= frameSize && l.resultOff + l.floorSize <= frameSize &&
                      l.currentFloorOff + l.floorSize <= R::StructSize(cls);
    if (!fits || resultSize != l.floorSize) {
        UE_LOGE("character_floor: the engine's floor layout is not the one this reads (location@%d result@%d size=%d param=%d "
                "CurrentFloor@%d frame=%d) -- a mirror's floor is not refreshed",
                l.locationOff, l.resultOff, l.floorSize, resultSize, l.currentFloorOff, frameSize);
        return;
    }
    l.frame.assign(static_cast<size_t>(frameSize), 0);
    l.usable = true;
    UE_LOGI("character_floor: K2_FindFloor resolved (result %d bytes, CurrentFloor@0x%X)", l.floorSize, l.currentFloorOff);
}

}  // namespace

bool Refresh(void* characterActor, const FVector& capsuleAt) {
    if (!characterActor || !R::IsLive(characterActor)) return false;
    if (!g_layout.resolved) Resolve();
    Layout& l = g_layout;
    if (!l.usable) return false;
    void* const cmc = *reinterpret_cast<void**>(static_cast<uint8_t*>(characterActor) + P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc)) return false;
    // The engine writes the out parameter into this frame, and a fresh query leaves no earlier result in it.
    std::memset(l.frame.data(), 0, l.frame.size());
    std::memcpy(l.frame.data() + l.locationOff, &capsuleAt, sizeof(FVector));
    if (!R::CallFunction(cmc, l.fn, l.frame.data())) return false;
    std::memcpy(static_cast<uint8_t*>(cmc) + l.currentFloorOff, l.frame.data() + l.resultOff, static_cast<size_t>(l.floorSize));
    return true;
}

}  // namespace ue_wrap::character_floor

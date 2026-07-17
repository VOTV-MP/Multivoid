// ue_wrap/daily_task.h -- standalone engine access for saveSlot.taskNew (the daily
// tape/signal delivery task, Fstruct_taskNew @0x0CA8 size 0x48). Principle-7 engine-
// wrapper layer -- NO network logic; coop/daily_task_sync (the L7 host mirror) drives
// the struct through here.
//
// Layout (CXXHeaderDump/struct_taskNew.hpp, byte-validated in the L7 RE):
//   active            bool           @0x00
//   sigRequired       TArray<int32>  @0x08  (indexed by process LEVEL; fixed MakeArray)
//   sigCompleted      TArray<int32>  @0x18
//   requiredDishes    TArray<int32>  @0x28  (dish INDICES; Shuffle(gamemode.dishs) subset)
//   rewardSig         int32          @0x38
//   rewardSat         int32          @0x3C
//   reel_big          float          @0x40  (best-SENT -- written by sell/rollover only)
//   reel_small        float          @0x44
// The taskNew offset itself is reflected (FindPropertyOffset on saveSlot_C; fallback
// 0x0CA8); the INNER offsets are the measured struct layout (GUID-suffixed member names
// defeat exact-name reflection; the struct is byte-stable within the targeted game
// version, which the mod's version tag mirrors by policy).
//
// Design of record: votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md (D3).

#pragma once

#include <cstdint>

namespace ue_wrap::daily_task {

// A read-only VIEW of the live struct (array pointers alias engine memory --
// consume within the same game-thread slice, never store).
struct View {
    bool           active;
    int32_t        rewardSig;
    int32_t        rewardSat;
    float          reelBig;
    float          reelSmall;
    const int32_t* sigRequired;      int32_t sigRequiredNum;
    const int32_t* sigCompleted;     int32_t sigCompletedNum;
    const int32_t* requiredDishes;   int32_t requiredDishesNum;
};

// Array selector for Apply.
enum class Which : uint8_t { SigRequired = 0, SigCompleted = 1, RequiredDishes = 2 };

// Read the live taskNew. Returns false while saveSlot/taskNew is unresolvable
// (menu / booting). Game thread.
bool Read(View& out);

// Client-mirror writes (GT-atomic when called from one game-thread task).
bool WriteScalars(bool active, int32_t rewardSig, int32_t rewardSat,
                  float reelBig, float reelSmall);
// Overwrite one int32 array: in-place when count == Num; otherwise rebuild the
// engine allocation (R::EngineAlloc + copy + R::EngineFree of the old buffer +
// {ptr,num,max} write -- the ue_wrap/inventory.cpp precedent; int32 is POD).
bool WriteArray(Which which, const int32_t* vals, int32_t count);

}  // namespace ue_wrap::daily_task

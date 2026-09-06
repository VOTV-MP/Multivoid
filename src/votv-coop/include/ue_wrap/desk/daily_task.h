// ue_wrap/daily_task.h -- standalone engine access for saveSlot.taskNew (the daily
// tape/signal delivery task, Fstruct_taskNew). Principle-7 engine-wrapper layer --
// NO network logic; coop/daily_task_sync (the L7 host mirror) drives the struct
// through here.
//
// The struct carries: active; sigRequired, indexed by process LEVEL (a fixed MakeArray);
// sigCompleted; requiredDishes, holding dish INDICES (a Shuffle(gamemode.dishs) subset);
// rewardSig; rewardSat; and the reel_big / reel_small best-SENT pair, which only sell and
// rollover write.
//
// The taskNew offset itself is reflected (FindPropertyOffset on saveSlot_C, with a numeric
// fallback); the INNER field offsets are the measured struct layout and live as named
// constants in daily_task.cpp, because GUID-suffixed member names defeat exact-name
// reflection and the struct is byte-stable within the targeted game version.

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

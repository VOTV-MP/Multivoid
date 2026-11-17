// coop/dev/fireext_drill_detail.h -- co-located private header of the fire extinguisher drill (coop/dev/fireext_drill.h):
// what the runaway arm (fireext_drill_runaway.cpp) shares with the drill's other arms (fireext_drill.cpp).

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <string>
#include <vector>

namespace coop::dev::fireext_drill::detail {

enum class Step { WaitJoin, WalkTo, Grab, WalkAway, Rest, Damage, RunRest, WaitClient, Done, Invalid };

struct Watched {
    ue_wrap::CachedObjRef ref;
    std::wstring key;
    ue_wrap::FVector start{}, printed{};
    bool frozen = false, mounted = false, gone = false;
    bool unread = false;              // its location could not be read: said once, and no reader reads it again
    int thrusting = 0, spraying = 0;  // a drop hard enough starts the runaway thrust; -1 = unread
};

// The extinguishers both peers watch, the acting peer's step, and the target it took off its mount.
extern std::vector<Watched> g_watched;
extern Step g_step;
extern ue_wrap::CachedObjRef g_target;
extern std::wstring g_targetKey;

// The run's dead marker (an INVALID line, and the runaway arm's INVALID verdict), the move to another step, and the target's
// place logged under `what`. Game thread.
void Invalid(const char* why);
void Go(Step s);
void LogTarget(const char* what);

// The runaway arm: the host's steps once the extinguisher is dropped, the client's tick, and the session end.
void RunawayHostStep(void* player);
void RunawayClientTick();
void RunawayReset();

}  // namespace coop::dev::fireext_drill::detail

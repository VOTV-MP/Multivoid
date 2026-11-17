// coop/dev/drill_ground.h -- TEST-ONLY: the ground and the props a prop-rest drill sets down and finds again.
//
// A drill that spawns a machine for two peers to watch sets it on measured open ground (coop/dev/level_ground), with a
// footprint clear of other geometry, so it rests upright whatever its base, and the other peer finds its own copy of it by
// class near that spot. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

namespace coop::dev::drill_ground {

constexpr float kQuietCmS = 2.f;   // a body moving slower than this reads as quiet

// Level open ground round `around`, tried nearest first, whose square `halfCm` clear of the spot is open floor (nine traces
// from 400 cm above to 50 cm below the floor must each strike the floor itself, within 5 cm of its height, so a roof, a
// ledge or a static body inside the footprint turns the spot away) and with no instance of `crowdClass` within `crowdCm`:
// the other peer could not tell its copy from that one. `walker` is the actor the traces ignore. Logs each refusal under
// `tag`. False when no spot qualifies. Game thread.
bool FindSpot(void* walker, const ue_wrap::FVector& around, float halfCm, const wchar_t* crowdClass, float crowdCm,
              const char* tag, ue_wrap::FVector& spot);

// True when `actor`'s body moves slower than kQuietCmS: a body can stay awake with a rotation that never settles, so the
// engine's at-rest flag is not the test.
bool Quiet(void* actor);

// The live instance of `cls` nearest `at` within `maxCm`; null when none. Walks the object array: a few times a second
// at most.
void* NearestOf(const wchar_t* cls, const ue_wrap::FVector& at, float maxCm);

// A copy has rested when its pose stayed within kStableCm of where it was first seen for `needTicks` ticks in a row and its
// body reads quiet. A drill's rest is a state it reads, never a clock it waits out.
constexpr float kStableCm   = 0.5f;
constexpr float kStableDeg  = 0.5f;   // and, when a rotation is given, within this of where it first stood
constexpr int   kStableTicks = 30;
struct Settle {
    ue_wrap::FVector  anchor{};
    ue_wrap::FRotator rotAnchor{};
    int  ticks = 0;
    bool has = false;
};
bool Stable(Settle& s, const ue_wrap::FVector& at, bool bodyAtRest, int needTicks = kStableTicks,
            const ue_wrap::FRotator* rot = nullptr);
void Restart(Settle& s);

// The distance between two places.
float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

}  // namespace coop::dev::drill_ground

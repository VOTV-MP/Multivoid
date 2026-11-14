// coop/dev/atv_riders_probe_detail.h -- private to the ATV riders probe: the gate rows (atv_riders_probe_rows.cpp), the reads of
// each measurement (atv_riders_probe_acts.cpp) and the sequencer that runs both (atv_riders_probe.cpp). Every read is for the
// game thread.
#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::dev::atv_riders_probe::detail {

inline constexpr const char* kTag = "atv-probe";
inline constexpr uint8_t kDriverSlot = 1;      // the driving client's slot, as the host and the observer hold its body
inline constexpr float kDriveCm = 600.f;       // the host's ATV has moved this far from where the drive began

// ---- the gate rows ----

// Register this role's rows: the host's `sitOnCar` POST and `getOffCar` PRE, a client's `tryToOccupyCar` PRE. Any thread.
bool RegisterRows(bool host);
// Whether every row of the role is live: 1 yes, 0 not yet, -1 one is dead (said). Asks the gate to resolve its names. Game thread.
int RowsLive(bool host);
// HOST: the `getOffCar` PRE calls seen so far.
uint32_t GetOffSeen();

// ---- the reads ----

// The ATV this peer holds under `key`, or null.
void* AtvOf(const std::wstring& key);
// The kerfur mirror bound at `eid` on this peer, or null.
void* KerfurMirrorOf(uint32_t eid);
// M3: every NPC mirror that is a kerfur, as it stands against the ATV under `key`.
void LogMirrors(const std::wstring& key);
// The driver's body (slot kDriverSlot) is attached to the ATV and its AnimBP holds the seat pose.
bool BodySeated(const std::wstring& key);
// The kerfur at `eid` is attached to the ATV.
bool KerfurRides(const std::wstring& key, uint32_t eid);
// The ATV's root location.
bool ReadRoot(const std::wstring& key, ue_wrap::FVector& out);

// M2: the body's and the kerfur's distance from their seats, and whether each is attached to the ATV (-1 where not read).
struct Sample {
    float bodyCm = -1.f;
    int   bodyParent = -1;
    float kerfurCm = -1.f;
    int   kerfurParent = -1;
};
// `kerfurEid` 0 takes no kerfur read.
Sample TakeSample(const std::wstring& key, uint32_t kerfurEid);

// M15: the driver's body's seat pose before and after a re-skin to another builtin skin, and after the skin it wore.
void RunSkinProbe();

}  // namespace coop::dev::atv_riders_probe::detail

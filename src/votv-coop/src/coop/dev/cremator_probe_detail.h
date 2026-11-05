// coop/dev/cremator_probe_detail.h -- what the cremator probe's two files share: the property reads, the
// nearest-firebox search, the machine's reading and the host arm's hooks. Private to coop/dev:
// cremator_probe*.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::cremator_probe::detail {

// The arm the row selects: "1" runs the probe on the client's own copy, "placeonly" only places the client,
// "host" runs the lever on the host's copy once the client's puppet stands in a firebox.
enum class Mode : uint8_t { Off, Local, PlaceOnly, Host };
Mode ModeOf();

constexpr uint64_t kSampleMs = 1000;

void* ObjectProperty(void* object, const wchar_t* name);
bool ReadBool(void* object, const wchar_t* name, bool& value);
bool ReadFloat(void* object, const wchar_t* name, float& value);
// A boolean as the log prints it: -1 when it could not be read.
int Bit(void* object, const wchar_t* name);
bool CallNoParams(void* object, const wchar_t* fnName);

// `cremator.actors` Num and `cremator.health[0]`: -1 and kNoHealth when unreadable or empty.
constexpr float kNoHealth = -999.f;
void ReadMachine(void* cremator, int& actors, float& health0);

struct Nearest {
    ue_wrap::FVector from;
    float best;  // the squared distance to the closest firebox
    void* actor;
    ue_wrap::FVector at;
};
// The cremator_C whose `firebox` component is nearest `from`, no distance cap.
Nearest FindNearestFirebox(const ue_wrap::FVector& from);

// If the door is open, shuts it with its own close(); false when that call did not run. `opened` says what it was.
bool ShutDoor(void* cremator, bool& opened);
// If the door is shut, opens it with its own open(); false when that call did not run. `opened` is read after.
bool OpenDoor(void* cremator, bool& opened);

// The host arm, called once per pump tick on the host; and its reset.
void HostTick(coop::net::Session& session);
void HostReset();

}  // namespace coop::dev::cremator_probe::detail

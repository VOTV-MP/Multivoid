// coop/dev/cremator_probe_detail.h -- what the cremator probe's two files share: the arm the row selects, a flag as
// the log prints it and the host arm's hooks. Private to coop/dev: cremator_probe*.cpp. The machine's reads and
// calls are ue_wrap/devices/cremator.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::cremator_probe::detail {

// The arm the row selects: "1" runs the probe on the client's own copy, "placeonly" only places the client,
// "host" sends the client to the firebox by the admin verb's own teleport order and runs the lever on the host's copy
// once the cremator's begin-overlap sees the puppet.
enum class Mode : uint8_t { Off, Local, PlaceOnly, Host };
Mode ModeOf();

constexpr uint64_t kSampleMs = 1000;

// A flag as the log prints it: -1 when `read` could not read it.
inline int Flag(void* cremator, bool (*read)(void*, bool&)) {
    bool v = false;
    return read(cremator, v) ? (v ? 1 : 0) : -1;
}

// The host arm, called once per pump tick on the host; and its reset.
void HostTick(coop::net::Session& session);
void HostReset();
// The cremator's begin-overlap, seen on the host: `other` is the overlapping actor.
void HostOnOverlap(void* cremator, void* other);

}  // namespace coop::dev::cremator_probe::detail

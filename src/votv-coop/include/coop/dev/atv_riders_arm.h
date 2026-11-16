// coop/dev/atv_riders_arm.h -- the ATV riders drill's arms (the `atv_riders_drill` row), for the modules a red turns off. Each
// red is read once, at the one site it disables, as `static const bool off = coop::dev::atv_riders_arm::Is("<value>")`.
// Dev only: the row is off in every shipped run, so every red is false.

#pragma once

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include <string>

namespace coop::dev::atv_riders_arm {

// The row's value, resolved on the call.
inline std::string Value() { return coop::config::ResolveEnum(::coop::config_registry::rows::atv_riders_drill); }

// The row names `value` (a red, or `on`).
inline bool Is(const char* value) { return Value() == value; }

// The row runs step 0, the reveal route on the driver's first join: the values on and reveal_ride_off.
// Not under bind_ride_off: that red turns off the bind hook for every bind, the first join's included, so step 0 could not
// reach its route there; its own route is the rejoin's adoption at step 5.
inline bool RunsFirstJoin() { return Is("on") || Is("reveal_ride_off"); }

}  // namespace coop::dev::atv_riders_arm

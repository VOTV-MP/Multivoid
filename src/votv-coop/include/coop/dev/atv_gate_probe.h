// coop/dev/atv_gate_probe.h -- [dev] which of an ATV's own functions fire, on each peer, and through which seam.
//
// The functions are the ATV_C bodies a refuel, a repair, a module fit or take-off, the lights, brake, turbo and horn
// run through. Each has two OBSERVE-ONLY watches, so a line says which seam saw a call: a script-gate PRE watch (every
// route into a Blueprint body) and a ProcessEvent PRE observer (only a dispatched call). Neither refuses anything.
//   [ATVG] <host|client|solo> <fn> via=<script_gate|pe> self=<the ATV's key> mirror=<0|1> caller=<Class.fn|none>
// mirror=1 is a copy whose tick another peer owns (atv_sync::NonOwnerCopy). caller is the calling Blueprint frame, none
// for the ProcessEvent route and always for the pe line, which has no frame. The first 200 lines of each function and
// seam are printed, the rest counted. Ini atv_gate_probe=1 or VOTVCOOP_ATV_GATE_PROBE=1, on every peer to be read. Off,
// nothing is registered. Game thread.
#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::atv_gate_probe {

enum Fn : int {
    kGascanFuel, kFuelUp, kToolboxFix, kPlayerUsedOn, kUpgradeTake, kUpdLights, kSetBrake, kUnturbo,
    kLights, kBrake, kTurboPress, kTurboRelease, kHonkPress, kHonkRelease,
    kFnCount
};

// How often each function was seen this session, by seam.
struct Counts {
    uint32_t gate[kFnCount] = {};
    uint32_t pe[kFnCount] = {};
};

bool Enabled();                       // the row, latched
const char* NameOf(Fn fn);            // the function's short label

// Cache the session, for the role; register the watches while the row is on, retried each pump tick until each settles.
// A latched read when off.
void Install(coop::net::Session* session);

// Every script-gate watch live and every observer registered. Game thread.
bool AllLive();

// The counts so far, which coop/dev/atv_gate_drill reads around each step. Game thread.
Counts Snapshot();

// Session end: the totals are said and cleared.
void OnDisconnect();

}  // namespace coop::dev::atv_gate_probe

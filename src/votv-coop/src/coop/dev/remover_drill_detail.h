// coop/dev/remover_drill_detail.h -- sibling-internal state shared by the remover drill's two TUs
// (remover_drill.cpp, the client half and the entry points; remover_drill_host.cpp, the host half). The
// coop/items/hook_sync_detail.h shape: private to the src tree, never part of the include surface.

#pragma once

#include <cstdint>
#include <string>

#include "ue_wrap/core/cached_obj_ref.h"

namespace coop::net { class Session; }

namespace coop::dev::remover_drill::detail {

enum class Phase : uint8_t { Off, Granted, Withheld };

Phase PhaseOf();
const char* PhaseName();
std::string EndLine();

// The host's two lines of the hook steps: its own hook pulls the first crowbar; the client's hook pulls the second.
inline constexpr const char* kHostHookedSaid = "remover-drill host-hooked";
inline constexpr const char* kSelfHookedSaid = "remover-drill self-hooked";

inline constexpr int      kSweepFloor   = 10;   // the fewest ring props in sight that make the sweep a measurement
inline constexpr uint64_t kCheckEveryMs = 250;

// The session counter, the verdict latch and the step clocks of whichever half this peer runs.
extern int      g_session;
extern bool     g_done;
extern uint64_t g_stepMs;
extern uint64_t g_nextCheckMs;

bool Expired(uint64_t bound);
void End(const char* role, const char* word, const char* what, bool warn);

// The item's cancel for a hook this drill planted.
void ReleaseHook(ue_wrap::CachedObjRef& hook);

// The host half: one check of its steps, and its state back to the start.
void HostTick(coop::net::Session* s);
void HostReset();

}  // namespace coop::dev::remover_drill::detail

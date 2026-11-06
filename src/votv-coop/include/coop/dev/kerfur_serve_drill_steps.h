// coop/dev/kerfur_serve_drill_steps.h -- the client steps the Omega serve drill's legs share: a worker thread that
// waits on the game thread, the walk up to an Omega's mirror and the camera put on it. A player commands an Omega
// only through its radial, looking at it from within use reach, so every command of the drill is sent after these.
// Not for any other file.

#pragma once

#include <cstdint>
#include <functional>

namespace coop::net { class Session; }

namespace coop::dev::kerfur_serve_drill::steps {

// The session the client's worker thread is alive in. Called every pump tick; idempotent.
void SetSession(coop::net::Session* s);

// The session is up, connected and not shutting down. Any thread.
bool Alive();

// Poll `pred` on the game thread until it holds, the session ends or `boundMs` passes. Worker thread.
bool WaitGame(const std::function<bool()>& pred, uint64_t boundMs);

// Run `fn` once on the game thread and return what it returned. Worker thread.
bool Run(const std::function<bool()>& fn);

// A step that could not run: the drill's client INVALID under `tag`, unless the session already ended. Worker thread.
void Invalid(const char* why, const char* tag);

// The player's look-at: the interaction trace and the look-at name `mirror` and a radial action is selected. Game thread.
bool AimedAt(void* mirror);

// The camera turned at the mirror through a fan of headings, each held until AimedAt. False when none took. Worker thread.
bool AimFan(void* mirror, int32_t mirrorIdx);

// Walk the local player with the director to within `reachCm` (flat) of the mirror; nothing when already there. Null
// on arrival, else the reason it did not. Worker thread.
const char* WalkUpTo(void* mirror, float reachCm, int deadlineS);

}  // namespace coop::dev::kerfur_serve_drill::steps

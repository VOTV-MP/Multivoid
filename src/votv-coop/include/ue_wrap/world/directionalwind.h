// ue_wrap/directionalwind.h -- AdirectionalWind_C engine wrapper (principle 7).
//
// The wind actor is a singleton, also held by mainGamemode.directionalWind. This wraps the
// four persistent rain and background wind fields (WindState below) and windTarget's
// RelativeLocation, which is the GUST INPUT (ReadTarget/WriteTarget). What a player sees as
// leaves shaking hard is the per-tick spring `intensity`, a low-pass of windTarget's
// displacement from origin -- NOT the four fields, which the tick overwrites anyway
// (windStrength_background = intensity every frame). The gust is energised per peer by a
// `changeWindOrigin` timer on a random 1-60 s roll that re-rolls windTarget, and each peer
// rolls its own stream, so the host is gusty while the client is calm. The coop layer
// therefore syncs windTarget host to client AND suppresses the client's own changeWindOrigin,
// so the synced target holds; the four fields stay, being correct for rain wind and for
// particle, audio and engine SPEED. No network or gameplay logic here -- coop::weather_sync
// drives it. Resolved by class like the cycle (FindObjectByClass, cached and revalidated).
// Game thread only.

#pragma once

#include "ue_wrap/core/types.h"  // FVector

namespace ue_wrap::directionalwind {

// The 4 host-authoritative wind fields. Plain accumulators -- writing them directly is
// the canonical path (no BP fan-out, like the cycle clock); setWindParameters() only
// ever writes the rain pair from the cycle's per-peer `rain`, so it can't sync these.
struct WindState {
    float speedBg      = 0.f;  // windSpeed_background (ambient; default 5.0)
    float strengthBg   = 0.f;  // windStrength_background
    float speedRain    = 0.f;  // windSpeed_rain (= cycle rainWindSpeed)
    float strengthRain = 0.f;  // windStrength_rain (= (rainStrength+0.5)*rain)
};

// Read the 4 fields off the live AdirectionalWind_C. Returns false if the actor isn't
// live yet (nothing to read). Game thread.
bool Read(WindState& out);

// Overwrite the 4 fields on the live AdirectionalWind_C (host-authoritative apply on the
// client). No-op + false if the actor isn't live. The actor's own ReceiveTick + 1 s
// updateDirWind then converge the derived totals + the engine WindDirectionalSource.
bool Write(const WindState& in);

// Read windTarget's RelativeLocation, the gust input the spring low-passes into `intensity`.
// False if the actor or the windTarget component is not live; leaves `out` untouched on
// failure. Game thread.
bool ReadTarget(FVector& out);

// Overwrite windTarget's RelativeLocation on the client (host-authoritative). The actor's own
// ReceiveTick springs `windOffset` toward it next frame and reproduces the host's
// `intensity`, driving the foliage material parameter and the engine wind. A direct field
// write: the spring reads windTarget.RelativeLocation as a plain value, so no
// K2_SetRelativeLocation is needed. No-op and false if the actor or windTarget is not live.
// Game thread.
bool WriteTarget(const FVector& in);

// Disconnect hook: drop the cached actor pointer (re-resolved next Read/Write).
void OnDisconnect();

}  // namespace ue_wrap::directionalwind

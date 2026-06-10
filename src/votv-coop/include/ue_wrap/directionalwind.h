// ue_wrap/directionalwind.h -- AdirectionalWind_C engine wrapper (principle 7).
//
// The wind actor (singleton, also held by mainGamemode.directionalWind@0x0F70). Wraps:
//   - the 4 persistent rain/background wind fields (WindState below), and
//   - windTarget's RelativeLocation -- the GUST INPUT (ReadTarget/WriteTarget).
// The user-visible "strong leaves shaking" is the per-tick spring `intensity`, a low-pass
// of windTarget's displacement from origin -- NOT the 4 fields (the tick even overwrites
// windStrength_background = intensity every frame). The gust is energized per-peer by a
// `changeWindOrigin` RNG timer (random 1-60 s) that re-rolls windTarget; each peer rolls
// its own stream -> host gusty while client calm. So the coop layer syncs windTarget
// host->client AND suppresses the client's changeWindOrigin (its local roll) so the synced
// target holds. The 4 fields stay (correct for rain-wind + particle/audio/engine SPEED).
// NO network/gameplay logic here -- coop::weather_sync drives it. Resolved by class like
// the cycle (FindObjectByClass, cached + IsLive-revalidated). Game thread only.
//
// RE: votv-wind-basefog-RE-2026-06-08.md + votv-wind-event-driver-RE-2026-06-09.md.

#pragma once

#include "ue_wrap/types.h"  // FVector

namespace ue_wrap::directionalwind {

// The 4 host-authoritative wind fields. Plain accumulators -- writing them directly is
// the canonical path (no BP fan-out, like the cycle clock); setWindParameters() only
// ever writes the rain pair from the cycle's per-peer `rain`, so it can't sync these.
struct WindState {
    float speedBg      = 0.f;  // windSpeed_background    @0x02EC (ambient; default 5.0)
    float strengthBg   = 0.f;  // windStrength_background @0x02F0
    float speedRain    = 0.f;  // windSpeed_rain          @0x02E4 (= cycle rainWindSpeed)
    float strengthRain = 0.f;  // windStrength_rain       @0x02E8 (= (rainStrength+0.5)*rain)
};

// Read the 4 fields off the live AdirectionalWind_C. Returns false if the actor isn't
// live yet (nothing to read). Game thread.
bool Read(WindState& out);

// Overwrite the 4 fields on the live AdirectionalWind_C (host-authoritative apply on the
// client). No-op + false if the actor isn't live. The actor's own ReceiveTick + 1 s
// updateDirWind then converge the derived totals + the engine WindDirectionalSource.
bool Write(const WindState& in);

// Read windTarget's RelativeLocation (the gust input the spring low-passes into
// `intensity`). False if the actor / windTarget component isn't live; leaves `out`
// untouched on failure. Game thread. v50 (the leaf-shake sync).
bool ReadTarget(FVector& out);

// Overwrite windTarget's RelativeLocation on the client (host-authoritative). The actor's
// own ReceiveTick springs `windOffset` toward it next frame and reproduces the host's
// `intensity` -> the foliage MPC scalar + engine wind. Direct field write (the spring
// reads windTarget.RelativeLocation as a plain value; no K2_SetRelativeLocation needed).
// No-op + false if the actor / windTarget isn't live. Game thread. v50.
bool WriteTarget(const FVector& in);

// Disconnect hook: drop the cached actor pointer (re-resolved next Read/Write).
void OnDisconnect();

}  // namespace ue_wrap::directionalwind

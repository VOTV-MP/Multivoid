// coop/dev/remover_drill_layout.h -- the host's half of the remover drill that lays and re-lays its targets: the single
// prop, the mashed prop, the ring, the creature and the two crowbars the hook steps use, about the client's puppet. One concept: where the targets stand.
// The layout is laid only where each spot has a floor of its own, found by a trace, and the segment from the puppet's eye
// to it is clear (the trace the judge runs), in the first direction of a fan round the puppet's facing that satisfies
// both. The client, which judges its own sight, may ask for the next direction in a chat line; HandleRelay answers it.
// The drill (remover_drill.cpp) reads the props laid through Targets and keeps the verdict.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cmath>

namespace coop { class RemotePlayer; }

namespace coop::dev::remover_drill::layout {

// The classes of the three roles and the creature: the client finds each role by class, never by where physics left it.
inline constexpr int kRing = 28;
inline constexpr int kFanSteps = 14;   // the directions round the puppet's facing a layout can be laid in
inline constexpr const wchar_t* kSingleClass = L"prop_pointSack_m_C";
inline constexpr const wchar_t* kMashClass   = L"prop_pointSack_s_C";
inline constexpr const wchar_t* kSackClass   = L"prop_pointSack_C";
inline constexpr const wchar_t* kNpcClass    = L"kerfurOmega_C";
inline constexpr const wchar_t* kHookedClass = L"prop_crowbar_C";

inline float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// What the host laid; the drill's verdict reads it. Empty until a layout is laid.
struct Laid {
    ue_wrap::CachedObjRef single, mash, npc;
    ue_wrap::CachedObjRef hostHooked, selfHooked;   // crowbars: the host's hook pulls the first, the client's the second
    ue_wrap::CachedObjRef ring[kRing];
};
Laid& Targets();

// Lays the layout about the puppet `rp` at `at`: only the single prop unless `granted` (the creature and the crowbars
// are granted-only too). Null on success; else the reason
// nothing was laid. Game thread, the host.
const char* Lay(coop::RemotePlayer& rp, const ue_wrap::FVector& at, bool granted);

// What taking the client's next relay line did.
enum class Relayed { None, Laid, Failed, NoEye };

// Takes the client's next relay line, if any: logs the client's camera and the blocker it met against the host's own eye and
// position of that element, destroys the layout and lays it again from the next fan step. Laid: the layout stands again and
// the client has been told; Failed: the fan has no step left (the client has been told); NoEye: the sender's eye did not read.
// Game thread, the host.
Relayed HandleRelay(bool granted);

// Back to the first fan step, nothing laid and no relay answered. A session's end.
void Reset();

}  // namespace coop::dev::remover_drill::layout

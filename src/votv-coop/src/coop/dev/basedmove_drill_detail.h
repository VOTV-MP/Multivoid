// coop/dev/basedmove_drill_detail.h -- what the based-movement drill's files share (basedmove_drill.cpp, _host, _client). Dev only;
// see coop/dev/basedmove_drill.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::basedmove_drill::detail {

inline constexpr const char* kPrefix = "bmv ";
inline constexpr uint64_t kBoundMs = 180000;       // a step that waits this long for its event says INVALID and stops
inline constexpr uint64_t kWalkBoundMs = 480000;   // the client's walk onto the platform, the director's own deadline included
inline constexpr uint64_t kRideBoundMs = 150000;   // the lift's travel is about twelve seconds; a ride that lasts this long is INVALID

// A body stands on the platform when its feet (the capsule's centre less its half height) are within kFeetTolCm of the
// platform's walkable surface and it is no farther than kEdgeSlackCm past the platform's flat extent. Said once, here, for
// both peers: the client's boarding and the host's reading of the puppet.
inline constexpr float kFeetTolCm = 20.f;
inline constexpr float kEdgeSlackCm = 10.f;

bool Nocoll();                  // the ride_nocoll arm
const char* ArmName();          // "ride" or "ride_nocoll"

// ---- the log, the chat, the verdict and the bound ----
void Fact(const char* fmt, ...);
void Say(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);   // the text after `prefix` of the last chat line that starts with it
bool Seen(const std::string& line);
void Invalid(const char* why);
void Measured(const char* fmt, ...);                       // `rig: VERDICT basedmove-<arm> MEASURED -- <text>`, said once
bool Ended();
void Finish();                                             // this peer has nothing more to do
void Go();                                                 // a step began: its bound starts again
bool Late(const char* why, uint64_t boundMs = kBoundMs);   // true when the step's bound ran out (it has said INVALID)

// ---- reads ----
struct PlatformBox {
    ue_wrap::FVector centre{};
    ue_wrap::FVector extent{};   // half sizes of the colliding bounds, which take in what stands on the slab (rails, frame)
    float top = 0.f;             // the bounds' top: NOT a surface a body stands on
    float surface = 0.f;         // the platform's walkable surface at rest, from a down-trace over its centre
    float restZ = 0.f;           // the platform component's world Z when the surface was read; the surface follows it
};
// The platform's colliding bounds and its walkable surface: the lowest upward-facing hit of a down-trace over the bounds'
// centre that strikes the platform's own actor (`ctx` is the trace's world context, any live actor but the platform). Says its
// samples in the log. False when the bounds do not read or the trace strikes no upward-facing surface of the platform.
bool ReadPlatform(void* ctx, void* platformActor, float platformZ, PlatformBox& out);
// Whether a character's feet stand on the platform's surface (the surface moved with the platform: `platformZ` is its
// component's Z now), and the two numbers that say it: the flat distance past the platform's extent (negative inside) and
// the feet's height over the surface.
bool StandsOn(void* character, const PlatformBox& box, float platformZ, float* flatPast = nullptr, float* feetOver = nullptr);
// How many chat rows of this drill start with `prefix`.
int CountOf(const std::string& prefix);
// "<component>/<owner>" of a movement base, or "none"; `isPlatform` when the component or its owner is the platform's.
std::string BaseName(void* component, void* owner);
std::string NameOf(void* obj);

bool HostMayStart(coop::net::Session& s);
bool ClientMayStart();

// ---- each peer's steps, and the tick ----
void HostTick(coop::net::Session& s);
void ClientTick(coop::net::Session& s);

}  // namespace coop::dev::basedmove_drill::detail

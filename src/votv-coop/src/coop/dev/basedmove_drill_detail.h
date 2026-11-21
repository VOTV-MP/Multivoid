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

// How a standing body sits on a platform: its feet from the platform's top, and how far past the platform's flat extent it
// may stand. The feet are the capsule's centre less its half height. Said once, here, for both peers.
inline constexpr float kFeetBelowCm = 30.f;
inline constexpr float kFeetAboveCm = 120.f;
inline constexpr float kEdgeSlackCm = 40.f;

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
    ue_wrap::FVector extent{};   // half sizes
    float top = 0.f;
};
// The platform's colliding bounds. False when they do not read or hold no extent.
bool ReadPlatformBox(void* platformActor, PlatformBox& out);
// Whether a character's capsule stands over the platform's top, and the two numbers that say it: the flat distance past the
// platform's extent (negative inside) and the feet's height over the top.
bool StandsOn(void* character, const PlatformBox& box, float* flatPast = nullptr, float* feetOver = nullptr);
// "<component>/<owner>" of a movement base, or "none"; `isPlatform` when the component or its owner is the platform's.
std::string BaseName(void* component, void* owner);
std::string NameOf(void* obj);

bool HostMayStart(coop::net::Session& s);
bool ClientMayStart();

// ---- each peer's steps, and the tick ----
void HostTick(coop::net::Session& s);
void ClientTick(coop::net::Session& s);

}  // namespace coop::dev::basedmove_drill::detail

// coop/net/throw_bounds.h -- the bounds of a thrown prop's wire values, derived from the game's own throw.
//
// A client's release, drop intent, End and drive rows reach the host's physics and every other peer, so each
// meets a bound derived here and nowhere else. Clients only: the host may cheat and the relay holds. A velocity
// that is not finite is refused; a finite one is clamped in magnitude, direction kept, and counted by its caller.
// Every ceiling carries a 20 percent margin; the constants marked provisional wait for the proof's numbers.
// The sources, in the pawn's Blueprint (mainPlayer_C) as the 0.9.0n pak reflects it:
//  - Launch: traceThrow's camera-forward speed 1000 / (max(mass, 10) / 15), 1500 cm/s for the lightest mass,
//    plus the pawn's velocity, which traceThrow adds; throwShit sets the body to NewVel - fallVeloc with
//    bAddToCurrent, so the body's current velocity (a pawn's, when carried) is the second pawn term.
//  - The pawn's top speed: updateSpeed sets MaxWalkSpeed to defSpeed * 2 * (1 + agility / 100 * 0.25), with
//    defSpeed 400: 1000 cm/s at 100 agility.
//  - Spin: throwShit adds a unit vector times RandomFloatInRange(100, 300) / (max(m, 10) / 15): 450 deg/s.

#pragma once

#include <cmath>
#include <cstdint>

namespace coop::net::throw_bounds {

inline constexpr float kMargin = 1.2f;
inline constexpr float kGravityCmS2 = 980.f;

inline constexpr float kLightestThrowCmS = 1500.f;
inline constexpr float kPawnTopSpeedCmS = 1000.f;
inline constexpr float kPawnTermsInLaunch = 2.f;   // traceThrow's GetVelocity and throwShit's bAddToCurrent
inline constexpr float kRawLaunchCmS = kLightestThrowCmS + kPawnTermsInLaunch * kPawnTopSpeedCmS;

// A release's, a drop intent's launch speed (cm/s).
inline constexpr float kMaxLaunchSpeedCmS = kRawLaunchCmS * kMargin;

// PROVISIONAL: the longest flight the proof measures, in seconds; gravity over it adds to the launch ceiling.
inline constexpr float kProvisionalLongestFlightS = 5.f;
// A drive row's, an End's speed (cm/s): the launch ceiling plus gravity over the longest flight.
inline constexpr float kMaxFlightSpeedCmS = (kRawLaunchCmS + kGravityCmS2 * kProvisionalLongestFlightS) * kMargin;

// The spin ceiling (deg/s) for a release, a drop intent and an End: throwShit's largest, with the margin.
inline constexpr float kMaxLaunchAngDegS = 300.f / (10.f / 15.f) * kMargin;

// PROVISIONAL, from nothing measured yet: the proof's printed row jitter sets them.
inline constexpr float    kStepSlackCm = 50.f;       // a row's step beyond speed times time
inline constexpr uint32_t kJitterMs = 50;            // the host's arrival delta, allowed this much short of the sender's
inline constexpr int      kMaxRefusedRows = 5;       // consecutive refused rows that end an elected row (125 ms at 40 Hz)
inline constexpr uint32_t kGrantTimeoutMs = 1000;    // a throw's wait for its grant; the host's wait for a first row
inline constexpr uint32_t kMaxThrowFlightMs = 10000; // an elected row's longest life from the election
inline constexpr int      kMaxPendingThrows = 8;     // a peer's throws awaiting a grant
inline constexpr int      kMaxThrowRowsPerSlot = 4;  // elected rows one client holds at once

enum class Clamp : uint8_t { Ok, Clamped, NonFinite };

// Scale `v` to at most `cap` in magnitude, direction kept. NonFinite leaves it as it was.
inline Clamp ClampVec3(float v[3], float cap) {
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return Clamp::NonFinite;
    const double mag = std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] +
                                 static_cast<double>(v[2]) * v[2]);
    if (mag <= static_cast<double>(cap)) return Clamp::Ok;
    const double k = static_cast<double>(cap) / mag;
    for (int i = 0; i < 3; ++i) v[i] = static_cast<float>(v[i] * k);
    return Clamp::Clamped;
}

// One velocity pair against its two caps: NonFinite when either is, else Clamped when either was cut.
inline Clamp ClampPair(float lin[3], float linCap, float ang[3], float angCap) {
    const Clamp a = ClampVec3(lin, linCap);
    const Clamp b = ClampVec3(ang, angCap);
    if (a == Clamp::NonFinite || b == Clamp::NonFinite) return Clamp::NonFinite;
    return (a == Clamp::Clamped || b == Clamp::Clamped) ? Clamp::Clamped : Clamp::Ok;
}

// A drive row's test: its own velocity under the flight ceiling, and its step agreeing with it. `stepCm` is the
// distance from the last admitted row (for a first row, from the release); `dtRowMs` is the sender's own clock
// between the two; `arrivalDeltaMs` the host's between their arrivals. The sender's dt may not exceed the host's
// by more than kJitterMs, so a forged long dt buys no long step, nor a forged short one a free pass.
inline bool RowAdmitted(const float vel[3], float stepCm, int64_t dtRowMs, int64_t arrivalDeltaMs) {
    if (!std::isfinite(vel[0]) || !std::isfinite(vel[1]) || !std::isfinite(vel[2]) || !std::isfinite(stepCm))
        return false;
    const double speed = std::sqrt(static_cast<double>(vel[0]) * vel[0] + static_cast<double>(vel[1]) * vel[1] +
                                   static_cast<double>(vel[2]) * vel[2]);
    if (speed > static_cast<double>(kMaxFlightSpeedCmS)) return false;
    int64_t dt = dtRowMs;
    const int64_t hi = arrivalDeltaMs + static_cast<int64_t>(kJitterMs);
    if (dt > hi) dt = hi;
    if (dt < 1) dt = 1;
    return stepCm <= kMaxFlightSpeedCmS * static_cast<float>(dt) / 1000.f + kStepSlackCm;
}

}  // namespace coop::net::throw_bounds

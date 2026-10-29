// coop/net/throw_bounds.h -- the bounds of a thrown prop's wire values, derived from the game's own throw.
//
// A client's release, drop intent, End and drive rows reach the host's physics and every other peer, so each
// meets a bound derived here and nowhere else. Clients only: the host may cheat and the relay holds. A velocity
// that is not finite is refused; a finite one is clamped in magnitude, direction kept, and counted by its caller.
// A drive row whose step disagrees with its velocity is displayed and counted against its syncer's trust, and the
// right is revoked after a run of them; MTA bounds nothing on a syncer's packet (CUnoccupiedVehicleSync.cpp:263-340).
// Every ceiling carries a 20 percent margin; "p3" below is ignore_folder/lanes/ci-wp4/p3-<arm>, build 68fcff87.
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

// The longest release-to-rest the proof measured, in seconds (p3 throwjoin n1: 5.378 s); gravity over it adds to the
// launch ceiling, and the 20 percent margin on the sum covers a longer flight.
inline constexpr float kLongestFlightS = 5.378f;
// A drive row's, an End's speed (cm/s): the launch ceiling plus gravity over the longest flight.
inline constexpr float kMaxFlightSpeedCmS = (kRawLaunchCmS + kGravityCmS2 * kLongestFlightS) * kMargin;

// The spin ceiling (deg/s) of a hand throw's DROP INTENT (one that carries a throw sequence): throwShit's largest, with
// the margin. The dropped item is a fresh spawn with no spin of its own, so throwShit's added spin is all it has.
inline constexpr float kMaxLaunchAngDegS = 300.f / (10.f / 15.f) * kMargin;

// The spin ceiling (deg/s) of every other client input that reaches a body: every release, thrown or not (throwShit
// adds its spin to the held body's, so a grabbed prop leaves above kMaxLaunchAngDegS: 1905 deg/s measured, p3
// throwjoin), a drop intent that is no throw, every End. A whipped held prop or a tumble after a collision spins faster
// than a throw does. The value is
// the bound the per-axis check this replaced had (1e6 deg/s, headroom below anything that would teleport a body in
// one tick), here applied to the magnitude and clamped.
inline constexpr float kMaxBodyAngDegS = 1.0e6f;

// A row's step beyond speed times time (cm): 2 x the largest first-row jump of a green non-clamp throw, 50.1 cm
// (p3 hand-nov n1).
inline constexpr float    kStepSlackCm = 100.f;
// The host's arrival delta, allowed this much short of the sender's (ms): 2 x the largest sendTime jitter, 22 ms
// (p3 clamp n2).
inline constexpr uint32_t kJitterMs = 44;
// Design value: consecutive failed rows that revoke the thrower, 125 ms of failures at 40 Hz.
inline constexpr int      kMaxRefusedRows = 5;
// A client's wait for its throw's grant (the unmatched record and the mark): a design value, a second on any link a
// throw can be played on. The host's wait for a first row is per slot: GrantTimeoutMs below.
inline constexpr uint32_t kGrantTimeoutMs = 1000;
// The host's wait for a thrown prop's first admitted row after the election, from the link: the release-to-first-row
// delay is link-bound (43-83 ms on loopback, 160-182 ms at rtt ~120, p3 hand, hand-nov, hand-lag, grab-lag), so it is
// twice (the slot's rtt plus 83 ms, the loopback maximum), floor 250 ms; no rtt sample (-1) reads the floor. A late
// timeout only keeps the host's own simulation of the prop longer.
inline constexpr uint32_t kGrantTimeoutFloorMs = 250;
inline constexpr uint32_t kGrantLoopbackMaxMs = 83;
inline uint32_t GrantTimeoutMs(int rttMs) {
    if (rttMs < 0) return kGrantTimeoutFloorMs;
    const uint32_t t = 2u * (static_cast<uint32_t>(rttMs) + kGrantLoopbackMaxMs);
    return t < kGrantTimeoutFloorMs ? kGrantTimeoutFloorMs : t;
}
// An elected row's longest life from the election (ms): 2 x the longest release-to-rest, 5378 ms (p3 throwjoin n1).
inline constexpr uint32_t kMaxThrowFlightMs = 10756;
// How long an elected row may go without an admitted row before the host takes the prop back (prop_drive_host Tick):
// 2 x the largest host inter-arrival gap of an elected row, 57 ms (p3 clamp red run; 56 ms in the green runs and under
// net.fakelag_ms=50, since a fixed delay keeps the spacing), 114 ms.
inline constexpr uint32_t kThrowSilenceMs = 114;
// The silence above is a thrower gone only while the prop moves: once its rows say it rests (the last one slower than
// this), the client's next message is its rest End, which rides the reliable lane and may be resent past any bound
// fitted to the row stream; a resting prop hangs nowhere, so its silence is left to the pass's generic rule. A body the
// engine has settled reads well under it; a prop in flight reads hundreds of cm/s.
inline constexpr float    kThrowRestCmS = 10.f;
// Design values, counts rather than measurements. 8: the throws one player can have in flight before a grant, at the
// fastest throw rate. 4: the elected flights one client syncs at once.
inline constexpr int      kMaxPendingThrows = 8;
inline constexpr int      kMaxThrowRowsPerSlot = 4;

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

// A drive row's velocity, clamped to the flight ceiling (a value bound on client input, never a refusal). A velocity
// that is not finite becomes zero and reads NonFinite, so the caller counts it.
inline Clamp ClampRowVelocity(float vel[3]) {
    const Clamp c = ClampVec3(vel, kMaxFlightSpeedCmS);
    if (c == Clamp::NonFinite) vel[0] = vel[1] = vel[2] = 0.f;
    return c;
}

// A drive row's step test: its step agreeing with the speed a prop can fly. `stepCm` is the distance from the last
// PASSING row (for a first row, from the release); `dtRowMs` is the sender's own clock between the two;
// `arrivalDeltaMs` the host's between their arrivals. The sender's dt may not exceed the host's by more than
// kJitterMs, so a forged long dt buys no long step, nor a forged short one a free pass. Bounding a claimed dt by the
// receiver's own arrival delta is Source's shape (a client may not replay more time than the server's budget,
// Source's player.cpp:3392-3412), not MTA's, whose time context is an invalidation epoch (CElement.cpp:1281-1305).
// The row's velocity is clamped before this runs, so it is not tested here. false counts against the syncer's trust;
// it never refuses the display.
inline bool RowStepAgrees(float stepCm, int64_t dtRowMs, int64_t arrivalDeltaMs) {
    if (!std::isfinite(stepCm)) return false;
    int64_t dt = dtRowMs;
    const int64_t hi = arrivalDeltaMs + static_cast<int64_t>(kJitterMs);
    if (dt > hi) dt = hi;
    if (dt < 1) dt = 1;
    return stepCm <= kMaxFlightSpeedCmS * static_cast<float>(dt) / 1000.f + kStepSlackCm;
}

}  // namespace coop::net::throw_bounds

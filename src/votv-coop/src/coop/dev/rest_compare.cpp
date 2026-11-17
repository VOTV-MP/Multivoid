// coop/dev/rest_compare.cpp -- see coop/dev/rest_compare.h.

#include "coop/dev/rest_compare.h"

#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace coop::dev::rest_compare {

bool Read(void* actor, Pose& out) {
    return actor && ue_wrap::engine::TryGetActorLocation(actor, out.loc) &&
           ue_wrap::engine::TryGetActorRotation(actor, out.rot);
}

std::string Encode(const Pose& p) {
    char b[128];
    std::snprintf(b, sizeof(b), "%.2f %.2f %.2f %.2f %.2f %.2f", p.loc.X, p.loc.Y, p.loc.Z, p.rot.Pitch, p.rot.Yaw,
                  p.rot.Roll);
    return b;
}

bool Decode(const std::string& text, Pose& out) {
    return sscanf_s(text.c_str(), "%f %f %f %f %f %f", &out.loc.X, &out.loc.Y, &out.loc.Z, &out.rot.Pitch, &out.rot.Yaw,
                    &out.rot.Roll) == 6;
}

float Cm(const Pose& a, const Pose& b) {
    const float x = a.loc.X - b.loc.X, y = a.loc.Y - b.loc.Y, z = a.loc.Z - b.loc.Z;
    return std::sqrt(x * x + y * y + z * z);
}

namespace {

struct Quat {
    double x, y, z, w;
};

// UE's FRotator::Quaternion: yaw about Z, pitch about Y, roll about X.
Quat QuatOf(const ue_wrap::FRotator& r) {
    constexpr double kHalfRad = 3.14159265358979323846 / 360.0;
    const double sp = std::sin(r.Pitch * kHalfRad), cp = std::cos(r.Pitch * kHalfRad);
    const double sy = std::sin(r.Yaw * kHalfRad), cy = std::cos(r.Yaw * kHalfRad);
    const double sr = std::sin(r.Roll * kHalfRad), cr = std::cos(r.Roll * kHalfRad);
    return Quat{cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy,
                cr * cp * cy + sr * sp * sy};
}

}  // namespace

float Deg(const Pose& a, const Pose& b) {
    const Quat p = QuatOf(a.rot), q = QuatOf(b.rot);
    const double dot = std::fmin(1.0, std::fabs(p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w));
    return static_cast<float>(2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846);
}

bool Judge(const char* tag, const char* what, const Pose& host, const Pose& client, bool sayFail) {
    const float cm = Cm(host, client), deg = Deg(host, client);
    const bool within = cm <= kBoundCm && deg <= kBoundDeg;
    UE_LOGI("[REST-COMPARE] %s %s: the host's copy (%.1f, %.1f, %.1f | %.1f, %.1f, %.1f) and the client's (%.1f, %.1f, %.1f | "
            "%.1f, %.1f, %.1f) rest %.2f cm and %.2f deg apart (bound %.0f cm, %.0f deg) -- %s", tag, what, host.loc.X,
            host.loc.Y, host.loc.Z, host.rot.Pitch, host.rot.Yaw, host.rot.Roll, client.loc.X, client.loc.Y, client.loc.Z,
            client.rot.Pitch, client.rot.Yaw, client.rot.Roll, cm, deg, kBoundCm, kBoundDeg, within ? "within" : "OVER");
    if (!within && sayFail)
        coop::rig_verdict::Say(tag, coop::rig_verdict::Outcome::Fail, "%s: the copies rest %.1f cm and %.1f deg apart "
                               "(bound %.0f cm, %.0f deg)", what, cm, deg, kBoundCm, kBoundDeg);
    return within;
}

}  // namespace coop::dev::rest_compare

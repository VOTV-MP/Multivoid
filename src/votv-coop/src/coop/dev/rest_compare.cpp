// coop/dev/rest_compare.cpp -- see coop/dev/rest_compare.h.

#include "coop/dev/rest_compare.h"

#include "coop/props/prop_drive_rules.h"
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

float Deg(const Pose& a, const Pose& b) { return coop::prop_drive_rules::TurnDeg(a.rot, b.rot); }

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

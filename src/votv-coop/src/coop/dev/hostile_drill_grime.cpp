// coop/dev/hostile_drill_grime.cpp -- see coop/dev/hostile_drill_grime.h. The arm table, the lobby-chat lines, the verdict
// print and the family's entry points; the client's sender is hostile_drill_grime_client.cpp and the host's pick and judge
// hostile_drill_grime_host.cpp.

#include "coop/dev/hostile_drill_grime.h"

#include "hostile_drill_grime_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_chat.h"
#include "ue_wrap/core/log.h"

#include <cstdio>
#include <cstring>

namespace coop::dev::hostile_drill::grime {
namespace detail {
namespace {

// The arms: the decal the host picks for each, whether the client walks to it, holds a sponge, and the host's weather.
const Spec kSpecs[] = {
    // A real sponge on a real decal, several strokes, then one wall fixer use: logs the decals one overlap touches, the
    // destroy hold firing inside clean's call and wallFixer_fix's, and when the host's end arrives. PASS when it fired.
    {"grime_measure",  Arm::Measure,  Pick::Near, true,  true,  Weather::Any},
    // The host cleans its decal to where one stroke ends it; the client's real stroke does: its decal is held at zero until
    // the host's word, the host destroys at the granted zero, and the end reaches the client's copy.
    {"grime_last",     Arm::Last,     Pick::Near, true,  true,  Weather::Any},
    // A raw fall on a decal 3000 cm or more from the client, a sponge in hand, the host dry: refused for reach.
    {"grime_far",      Arm::Far,      Pick::Far,  false, true,  Weather::Dry},
    // A raw fall on a decal within reach, an empty hand, the host dry: refused, no tool.
    {"grime_notool",   Arm::Notool,   Pick::Near, true,  false, Weather::Dry},
    // A raw ended mark from a client: refused as no fall, and the host's decal stands.
    {"grime_gone",     Arm::Gone,     Pick::Any,  false, false, Weather::Any},
    // A raw fall to zero from a full decal within reach, a sponge in hand: granted cut to one step, and the asker reads the
    // host's value.
    {"grime_fast",     Arm::Fast,     Pick::Near, true,  true,  Weather::Any},
    // A raw fall of one rain step on a decal the host's own rain washes, an empty hand, the host raining: granted by the
    // rain route alone.
    {"grime_rain",     Arm::Rain,     Pick::Wash, false, false, Weather::Raining},
    // A raw fall on a decal within the rain's radius, an empty hand, the host dry: refused (the rain route's red arm is
    // grime_rain_open, which grants it).
    {"grime_dry",      Arm::Dry,      Pick::Wide, false, false, Weather::Dry},
    // The far arm, then the client leaves and rejoins: its decal at its world-ready must read the host's value from before
    // the refused fall (a fall lowered before it was judged would be in the snapshot).
    {"grime_joinleak", Arm::Joinleak, Pick::Far,  false, true,  Weather::Dry},
};

}  // namespace

const Spec* SpecOf(Arm a) {
    for (const Spec& s : kSpecs)
        if (s.arm == a) return &s;
    return nullptr;
}

// Lines no save can hold.
std::string Line(Arm a, const char* what) {
    const Spec* s = SpecOf(a);
    return std::string("hostile-drill ") + (s ? s->name : "off") + " " + what + " ";
}

void Verdict(Arm a, const char* what, const std::string& why) {
    const Spec* s = SpecOf(a);
    const char* name = s ? s->name : "off";
    if (std::strcmp(what, "PASS") == 0) UE_LOGI("[HOSTILE-DRILL] %s PASS -- %s", name, why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] %s %s: %s", name, what, why.c_str());
}

std::string FormatTarget(const Target& t) {
    char b[160];
    std::snprintf(b, sizeof(b), "%s %.1f %.1f %.1f %.2f", t.key.c_str(), t.at.X, t.at.Y, t.at.Z, t.process);
    return b;
}

bool ParseTarget(const std::string& text, Target& out) {
    char key[64] = {};
    float x = 0.f, y = 0.f, z = 0.f, p = 0.f;
    if (sscanf_s(text.c_str(), "%63s %f %f %f %f", key, static_cast<unsigned>(sizeof(key)), &x, &y, &z, &p) != 5) return false;
    out.key = key;
    out.at = {x, y, z};
    out.process = p;
    return !out.key.empty();
}

}  // namespace detail

Arm ArmOf(const std::string& value) {
    for (const detail::Spec& s : detail::kSpecs)
        if (value == s.name) return s.arm;
    return Arm::None;
}

const char* ArmName(Arm a) {
    const detail::Spec* s = detail::SpecOf(a);
    return s ? s->name : "off";
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (const detail::Spec* sp = detail::SpecOf(a)) detail::HostTick(s, *sp);
}

void TickClient(coop::net::Session* s, Arm a) {
    if (const detail::Spec* sp = detail::SpecOf(a)) detail::ClientTick(s, *sp);
}

void OnDisconnect() {
    detail::ClientReset();
    detail::HostReset();
}

}  // namespace coop::dev::hostile_drill::grime

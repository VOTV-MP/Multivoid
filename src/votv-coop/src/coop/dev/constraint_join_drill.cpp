// coop/dev/constraint_join_drill.cpp -- see coop/dev/constraint_join_drill.h. The entry and what both halves share.

#include "coop/dev/constraint_join_drill.h"

#include "constraint_join_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace coop::dev::constraint_join_drill {
namespace detail {
namespace {
bool g_over = false;
char g_lastWait[2][160] = {};   // the last wait said, per peer role (host, client)

std::string Format(const char* fmt, va_list ap) {
    char buf[300];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}
}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::constraint_join_drill);
        return v == "c1" ? Arm::C1 : v == "c2" ? Arm::C2 : v == "c3" ? Arm::C3 : Arm::Off;
    }();
    return a;
}

std::string ReadyLine(const SetInfo& s) {
    std::string out = std::string(kReadyPrefix) + std::to_string(s.n);
    for (size_t i = 0; i < s.n; ++i) out += " " + std::to_string(s.eid[i]);
    char at[96];
    std::snprintf(at, sizeof(at), " %.1f %.1f %.1f", static_cast<double>(s.a.X), static_cast<double>(s.a.Y),
                  static_cast<double>(s.a.Z));
    return out + at;
}

bool ParseReady(const std::string& rest, SetInfo& out) {
    const char* p = rest.c_str();
    int used = 0;
    unsigned n = 0;
    if (sscanf_s(p, "%u%n", &n, &used) != 1 || n < 2 || n > kMaxBoxes) return false;
    p += used;
    out = SetInfo{};
    out.n = n;
    for (unsigned i = 0; i < n; ++i) {
        unsigned eid = 0;
        if (sscanf_s(p, "%u%n", &eid, &used) != 1 || eid == 0u) return false;
        out.eid[i] = eid;
        p += used;
    }
    return sscanf_s(p, "%f %f %f", &out.a.X, &out.a.Y, &out.a.Z) == 3;
}

std::string FinalLine(size_t i, uint32_t eid, const ue_wrap::FVector& at) {
    char b[128];
    std::snprintf(b, sizeof(b), "%s%zu %u %.1f %.1f %.1f", kFinalPrefix, i, static_cast<unsigned>(eid),
                  static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z));
    return b;
}

bool ParseFinal(const std::string& rest, size_t& i, uint32_t& eid, ue_wrap::FVector& at) {
    unsigned idx = 0, e = 0;
    if (sscanf_s(rest.c_str(), "%u %u %f %f %f", &idx, &e, &at.X, &at.Y, &at.Z) != 5 || idx >= kMaxBoxes) return false;
    i = idx;
    eid = e;
    return true;
}

bool IsHost() { return coop::roster::LocalIsHost(); }

bool SecondLife() { return coop::config::ReadEnv("VOTVCOOP_CONSTRAINT_JOIN_LIFE") == "2"; }

bool Over() { return g_over; }

void Rearm() {
    g_over = false;
    g_lastWait[0][0] = g_lastWait[1][0] = '\0';
}

void Wait(char who, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string what = Format(fmt, ap);
    va_end(ap);
    char* last = g_lastWait[who == 'H' ? 0 : 1];
    if (what == last) return;
    std::snprintf(last, sizeof(g_lastWait[0]), "%s", what.c_str());
    UE_LOGI("[CONSTRAINT-JOIN-DRILL] [%c] waiting for %s", who, what.c_str());
}

void Invalid(char who, const char* fmt, ...) {
    if (g_over) return;
    g_over = true;
    va_list ap;
    va_start(ap, fmt);
    const std::string why = Format(fmt, ap);
    va_end(ap);
    UE_LOGW("[CONSTRAINT-JOIN-DRILL] [%c] INVALID %s", who, why.c_str());
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "%s", why.c_str());
}

void Fail(char who, const char* leg, const char* fmt, ...) {
    if (g_over) return;
    g_over = true;
    va_list ap;
    va_start(ap, fmt);
    const std::string what = Format(fmt, ap);
    va_end(ap);
    UE_LOGW("[CONSTRAINT-JOIN-DRILL] [%c] FAIL %s: %s", who, leg, what.c_str());
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, "%s: %s", leg, what.c_str());
}

void Pass(const char* fmt, ...) {
    if (g_over) return;
    g_over = true;
    va_list ap;
    va_start(ap, fmt);
    const std::string what = Format(fmt, ap);
    va_end(ap);
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass, "%s", what.c_str());
}

}  // namespace detail

bool IsEnabled() { return detail::ArmOf() != detail::Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected() || detail::Over()) return;
    if (detail::IsHost()) detail::host::Tick(*session);
    else detail::client::Tick(*session);
}

void OnDisconnect() {
    if (detail::IsHost() && detail::host::KeepsAcrossDisconnect()) return;
    detail::host::Reset();
    detail::client::Reset();
    detail::Rearm();
}

}  // namespace coop::dev::constraint_join_drill

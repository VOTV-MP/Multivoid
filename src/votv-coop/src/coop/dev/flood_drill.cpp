// coop/dev/flood_drill.cpp -- the flood drill's log, chat, verdict, reads and tick; see coop/dev/flood_drill.h. The host's
// steps are flood_drill_host.cpp, the client's flood_drill_client.cpp.
//
// Every verdict is decided by an event: the lane's own count at its judge, or a chat line the host said after the FloodState
// it covers (one ordered channel, so a peer that reads the line has applied what came before it). A bound only says INVALID,
// and a FAIL is said where it is decided with nothing after it.

#include "coop/dev/flood_drill.h"

#include "flood_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/flood_sync.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::flood_drill::detail {

namespace DC = coop::dev::drill_chat;
namespace FS = coop::flood_sync;
namespace RV = coop::rig_verdict;

namespace {

bool g_ended = false, g_armed = false;
uint64_t g_stepMs = 0;

char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

std::string Format(const char* fmt, va_list ap) {
    char buf[600];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

// `rig: VERDICT flood-<arm>`: the tag of the arm this peer runs.
const std::string& Tag() {
    static const std::string tag = std::string("flood-") + ArmName();
    return tag;
}

}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::flood_drill);
        if (v == "burst") return Arm::Burst;
        if (v == "mend") return Arm::Mend;
        if (v == "far") return Arm::Far;
        if (v == "join") return Arm::Join;
        if (v == "mendold") return Arm::MendOld;
        return Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::Burst: return "burst";
    case Arm::Mend:  return "mend";
    case Arm::Far:   return "far";
    case Arm::Join:  return "join";
    case Arm::MendOld: return "mendold";
    default:         return "off";
    }
}

// ---- the log, the chat, the verdict and the bound -------------------------------------------------------------------------

void Fact(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    UE_LOGI("[FLOOD-DRILL] [%c] %s", Who(), text.c_str());
}

void Say(const std::string& line) { DC::Say(std::string(kPrefix) + line); }
bool Seen(const std::string& line) { return DC::SeenHas(std::string(kPrefix) + line); }
bool Rest(const std::string& prefix, std::string* rest) { return DC::SeenRestOf(std::string(kPrefix) + prefix, rest); }

void Invalid(const char* why) {
    RV::Say(Tag().c_str(), RV::Outcome::Invalid, "%s", why);
    g_ended = true;
}

void Fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    RV::Say(Tag().c_str(), RV::Outcome::Fail, "%s", text.c_str());
    Say(coop::roster::LocalIsHost() ? "h-fail" : "c-fail");
    g_ended = true;
}

void Pass(const char* why) {
    RV::Say(Tag().c_str(), RV::Outcome::Pass, "%s", why);
    g_ended = true;
}

bool Ended() { return g_ended; }
void Finish() { g_ended = true; }
void Go() { g_stepMs = ::GetTickCount64(); }

bool Late(const char* why, uint64_t boundMs) {
    if (::GetTickCount64() - g_stepMs <= boundMs) return false;
    Invalid(why);
    return true;
}

bool PeerFailed() {
    if (!Seen(coop::roster::LocalIsHost() ? "c-fail" : "h-fail")) return false;
    Finish();
    return true;
}

// ---- what a peer reads ----------------------------------------------------------------------------------------------------

bool Read(Reading& out) {
    if (!FS::dev::PipeBits(out.bits)) {
        Invalid("the world holds no burst pipe");
        return false;
    }
    if (!FS::dev::LevelNow(out.level)) {
        Invalid("the world holds no flood volume");
        return false;
    }
    return true;
}

std::string Line(const char* head, const Reading& r) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s %s %.1f", head, r.bits.c_str(), r.level);
    return buf;
}

bool Parse(const std::string& rest, Reading& out) {
    const size_t gap = rest.find(' ');
    if (gap == std::string::npos || gap == 0 || gap > 63) return false;
    char* end = nullptr;
    const float level = std::strtof(rest.c_str() + gap + 1, &end);
    if (end == rest.c_str() + gap + 1 || !std::isfinite(level)) return false;
    out.bits = rest.substr(0, gap);
    out.level = level;
    return true;
}

bool Matches(const Reading& mine, const Reading& host, std::string& why) {
    char buf[200];
    if (mine.bits != host.bits) {
        std::snprintf(buf, sizeof(buf), "this copy's pipes read %s, the host's %s", mine.bits.c_str(), host.bits.c_str());
        why = buf;
        return false;
    }
    if (std::fabs(mine.level - host.level) > kLevelTolUu) {
        std::snprintf(buf, sizeof(buf), "this copy's level reads %.1f, the host's %.1f (more than %.1f apart)", mine.level,
                      host.level, kLevelTolUu);
        why = buf;
        return false;
    }
    return true;
}

// ---- the connection and the tick ------------------------------------------------------------------------------------------

bool HostMayStart(coop::net::Session& s) {
    return s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1) && !coop::join_window_baseline::IsLateWindowOpen(1);
}

bool ClientMayStart() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool RejoinedLife() {
    static const bool second = coop::config::ReadEnv("VOTVCOOP_FLOOD_DRILL_REJOIN") == "1";
    return second;
}

void ArmFirstTick() {
    if (g_armed) return;
    g_armed = true;
    Go();
    DC::SeenHas(kPrefix);   // the first read starts the chat history
}

void ResetAll() {
    g_ended = g_armed = false;
    g_stepMs = 0;
    ResetHost();
    ResetClient();
}

}  // namespace coop::dev::flood_drill::detail

namespace coop::dev::flood_drill {

void Tick(coop::net::Session* session) {
    namespace fd = detail;
    if (!session || fd::ArmOf() == fd::Arm::Off || fd::Ended()) return;
    fd::ArmFirstTick();
    if (coop::roster::LocalIsHost()) fd::HostTick(*session);
    else fd::ClientTick(*session);
}

void OnDisconnect() { detail::ResetAll(); }

}  // namespace coop::dev::flood_drill

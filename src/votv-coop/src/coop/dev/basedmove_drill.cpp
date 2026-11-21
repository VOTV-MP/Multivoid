// coop/dev/basedmove_drill.cpp -- the based-movement drill's arm, log, chat, verdict, platform reads and tick; see coop/dev/basedmove_drill.h.
// The host's steps are basedmove_drill_host.cpp, the client's basedmove_drill_client.cpp, the boarding _rider, the per-frame reads _watch.
//
// Every step is decided by an event: a chat line the other peer said, the walk's end, the puppet standing over the platform, the
// lift's own `moving` edge or a reversal of its `dir`. A bound only says INVALID.

#include "coop/dev/basedmove_drill.h"

#include "basedmove_drill_detail.h"

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

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::basedmove_drill::detail {

namespace DC = coop::dev::drill_chat;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
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

// `rig: VERDICT basedmove-<arm>`: the tag of the arm this peer runs.
const std::string& Tag() {
    static const std::string tag = std::string("basedmove-") + ArmName();
    return tag;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

bool Enabled() { return ArmOf() != Arm::Off; }

}  // namespace


Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::basedmove_drill);
        return v == "ride" ? Arm::Ride : v == "hostride" ? Arm::HostRide : v == "baseend" ? Arm::BaseEnd : v == "bad_base_xy" ? Arm::BadXy
             : v == "bad_base_vol" ? Arm::BadVol : Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::Ride:     return "ride";
    case Arm::HostRide: return "hostride";
    case Arm::BadXy:    return "bad_base_xy";
    case Arm::BadVol:   return "bad_base_vol";
    case Arm::BaseEnd:  return "baseend";
    default:            return "off";
    }
}

bool Hostile() { return ArmOf() == Arm::BadXy || ArmOf() == Arm::BadVol; }

// ---- the log, the chat, the verdict and the bound -------------------------------------------------------------------------

void Fact(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    UE_LOGI("[BMV-DRILL] [%c] %s", Who(), text.c_str());
}

void Say(const std::string& line) { DC::Say(std::string(kPrefix) + line); }
bool Seen(const std::string& line) { return DC::SeenHas(std::string(kPrefix) + line); }
bool Rest(const std::string& prefix, std::string* rest) { return DC::SeenRestOf(std::string(kPrefix) + prefix, rest); }

void Invalid(const char* why) {
    RV::Say(Tag().c_str(), RV::Outcome::Invalid, "%s", why);
    g_ended = true;
}

void Pass(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    RV::Say(Tag().c_str(), RV::Outcome::Pass, "%s", text.c_str());
    g_ended = true;
}

void Fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    RV::Say(Tag().c_str(), RV::Outcome::Fail, "%s", text.c_str());
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

// ---- reads ----------------------------------------------------------------------------------------------------------------

bool ReadPlatform(void* ctx, void* platformActor, float platformZ, PlatformBox& out) {
    if (!platformActor || !E::GetActorBounds(platformActor, true, out.centre, out.extent)) return false;
    if (!(out.extent.X > 0.f) || !(out.extent.Y > 0.f) || !(out.extent.Z > 0.f)) return false;
    out.top = out.centre.Z + out.extent.Z;
    out.restZ = platformZ;
    out.floorZ = 0.f;
    // Down from just over the bounds' top, hit after hit: each next trace starts a hair under the last hit, so a rail or a
    // frame over the slab is passed, the first upward-facing hit on the platform's actor is its walkable surface (nothing
    // stands over its centre), and the floor under the platform (another actor) ends the descent: the floor it rests on.
    const uint8_t types[] = {0, 1, 3};   // WorldStatic, WorldDynamic, PhysicsBody (trace.h)
    const float bottom = out.centre.Z - out.extent.Z - 30.f;
    float z = out.top + 30.f;
    bool found = false;
    std::string samples;
    for (int i = 0; i < 6; ++i) {
        ue_wrap::trace::Hit hit;
        if (!ue_wrap::trace::LineHitForObjects(ctx, {out.centre.X, out.centre.Y, z}, {out.centre.X, out.centre.Y, bottom}, types, 3,
                                               nullptr, 0, &hit)) {
            samples += " [trace unresolved]";
            break;
        }
        if (!hit.blocked) {
            samples += " [clear]";
            break;
        }
        const bool mine = hit.actor == platformActor;
        char buf[160];
        std::snprintf(buf, sizeof(buf), " [z=%.1f nz=%.2f %s%s]", hit.point.Z, hit.normal.Z, mine ? "platform:" : "other:",
                      NameOf(hit.component).c_str());
        samples += buf;
        if (!mine) {
            out.floorZ = hit.point.Z;
            break;
        }
        if (!found && hit.normal.Z >= 0.7f) {
            found = true;
            out.surface = hit.point.Z;
        }
        z = hit.point.Z - 2.f;
    }
    Fact("platform bounds top %.1f bottom %.1f, down-trace over its centre from the top:%s", out.top, bottom + 30.f,
         samples.c_str());
    if (found && out.floorZ == 0.f) out.floorZ = out.surface;   // no floor under it was struck: the slab's own height
    return found;
}

bool StandsOn(void* character, const PlatformBox& box, float platformZ, float* flatPast, float* feetOver) {
    ue_wrap::FVector at{};
    if (!character || !E::TryGetActorLocation(character, at)) return false;
    const float past = std::fmax(std::fabs(at.X - box.centre.X) - box.extent.X, std::fabs(at.Y - box.centre.Y) - box.extent.Y);
    const float feet = at.Z - E::GetActorCharacterHalfHeight(character) - (box.surface + platformZ - box.restZ);
    if (flatPast) *flatPast = past;
    if (feetOver) *feetOver = feet;
    return past <= kEdgeSlackCm && std::fabs(feet) <= kFeetTolCm;
}

int CountOf(const std::string& prefix) {
    std::vector<std::string> rests;
    DC::SeenRestsOf(std::string(kPrefix) + prefix, &rests);
    return static_cast<int>(rests.size());
}

std::string NameOf(void* obj) { return obj && R::IsLive(obj) ? Narrow(R::ToString(R::NameOf(obj))) : std::string("?"); }

std::string BaseName(void* component, void* owner) {
    if (!component) return "none";
    return NameOf(component) + "/" + NameOf(owner);
}

// ---- the connection and the tick ------------------------------------------------------------------------------------------

bool HostMayStart(coop::net::Session& s) {
    return s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1) && !coop::join_window_baseline::IsLateWindowOpen(1);
}

bool ClientMayStart() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

}  // namespace coop::dev::basedmove_drill::detail

namespace coop::dev::basedmove_drill {

void Tick(coop::net::Session* session) {
    namespace bd = detail;
    if (!session || !bd::Enabled() || bd::Ended()) return;
    static bool armed = false;
    if (!armed) {
        armed = true;
        bd::Go();
        bd::Seen(bd::kPrefix);   // the first read starts the chat history
    }
    if (coop::roster::LocalIsHost()) bd::HostTick(*session);
    else bd::ClientTick(*session);
}

}  // namespace coop::dev::basedmove_drill

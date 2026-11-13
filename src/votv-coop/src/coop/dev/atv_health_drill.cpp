// coop/dev/atv_health_drill.cpp -- see coop/dev/atv_health_drill.h. The arm table, the phase clock, the verdict and chat
// lines and the entry points; the arms' phases are atv_health_drill_arms.cpp (the client's acts, the silent syncer),
// atv_health_drill_host.cpp (the arms where the host acts), atv_health_drill_seated.cpp (the client walk and sit the
// client halves share), atv_health_drill_damage.cpp (gate, owner, trap), atv_health_drill_spawned.cpp (spawned and the
// host spawn step) and atv_health_drill_peers.cpp (carry_still, synced_peer, join, stream_pace).

#include "coop/dev/atv_health_drill.h"

#include "atv_health_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"

#include <string>

namespace coop::dev::atv_health_drill {
namespace detail {
namespace {

constexpr Spec kSpecs[] = {
    {Arm::Off,  "off",  "", "", ""},
    {Arm::Sit,  "sit",  "seated", "sit",  "dismount"},
    {Arm::Grab, "grab", "synced", "grab", "release"},
    {Arm::Occupied, "occupied", "seated",   "send", ""},
    {Arm::Synced,   "synced",   "carrying", "send", ""},
    {Arm::Silent,   "silent",   "seated",   "sit",  "mute"},
    {Arm::Gate,     "gate",     "blasted",  "sit",  ""},
    {Arm::Owner,    "owner",    "blown",    "sit",  ""},
    {Arm::Trap,     "trap",     "armed",    "sit",  ""},
    {Arm::Spawned,  "spawned",  "parked",   "sit",  ""},
    {Arm::CarryStill, "carry_still", "parked", "grab", "release"},
    {Arm::SyncedPeer, "synced_peer", "parked", "grab", "release"},
    {Arm::Join,       "join",        "parked", "sit",  ""},
    {Arm::StreamPace, "stream_pace", "parked", "sit",  ""},
};

}  // namespace

const Spec& SpecOf(Arm a) {
    for (const Spec& s : kSpecs)
        if (s.arm == a) return s;
    return kSpecs[0];
}

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::atv_health_drill);
        if (v == "sit") return Arm::Sit;
        if (v == "grab") return Arm::Grab;
        if (v == "occupied") return Arm::Occupied;
        if (v == "synced") return Arm::Synced;
        if (v == "silent") return Arm::Silent;
        if (v == "gate") return Arm::Gate;
        if (v == "owner") return Arm::Owner;
        if (v == "trap") return Arm::Trap;
        if (v == "spawned") return Arm::Spawned;
        if (v == "carry_still") return Arm::CarryStill;
        if (v == "synced_peer") return Arm::SyncedPeer;
        if (v == "join") return Arm::Join;
        if (v == "stream_pace") return Arm::StreamPace;
        return Arm::Off;
    }();
    return a;
}

void Enter(Run& r, int phase, const char* name) {
    r.phase = phase;
    r.name = name;
    r.at = Clock::now();
}

bool Past(const Run& r, std::chrono::seconds bound) { return Clock::now() - r.at > bound; }

long long Ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

void Fail(const char* role, Arm a, Run& r, const std::string& what, const std::string& got, const std::string& want) {
    UE_LOGW("[ATV-HEALTH-DRILL] FAIL %s arm=%s phase=%s: %s, got %s, want %s", role, SpecOf(a).name, r.name,
            what.c_str(), got.c_str(), want.c_str());
    r.done = true;
}

// A run that could not decide: no event came and a bound ran out. Neither green nor red; the rig ends on the line.
void Invalid(const char* role, Arm a, Run& r, const std::string& what, const std::string& got, const std::string& want) {
    UE_LOGW("[ATV-HEALTH-DRILL] INVALID %s arm=%s phase=%s: %s, got %s, want %s", role, SpecOf(a).name, r.name,
            what.c_str(), got.c_str(), want.c_str());
    coop::rig_verdict::Say("atv-health-drill", coop::rig_verdict::Outcome::Invalid, "%s arm=%s phase=%s: %s", role,
                           SpecOf(a).name, r.name, what.c_str());
    r.done = true;
}

void Pass(const char* role, Arm a, Run& r, const std::string& numbers) {
    UE_LOGI("[ATV-HEALTH-DRILL] %s DONE PASS arm=%s%s%s", role, SpecOf(a).name, numbers.empty() ? "" : " ",
            numbers.c_str());
    r.done = true;
}

// Lines no save can hold, as a prop class or a place could.
std::string ReadyLine(Arm a, const char* role) {
    return std::string("ATVHD ") + role + " ready " + SpecOf(a).name;
}
std::string PhaseLine(const char* name) { return std::string("ATVHD phase ") + name; }
std::string ClientLine(const char* what) { return std::string("ATVHD client ") + what; }

}  // namespace detail

void Tick(coop::net::Session* s) {
    const detail::Arm a = detail::ArmOf();
    if (a == detail::Arm::Off || !s) return;
    const bool damage = detail::IsDamageArm(a);
    const bool spawned = detail::IsSpawnedArm(a);
    const bool peers = detail::IsPeersArm(a);
    if (s->role() == coop::net::Role::Host) {
        if (damage) detail::DamageHostTick(s, a);
        else if (spawned) detail::SpawnedHostTick(s, a);
        else if (peers) detail::PeersHostTick(s, a);
        else detail::HostTick(s, a);
    } else if (s->connected()) {
        if (damage) detail::DamageClientTick(s, a);
        else if (spawned) detail::SpawnedClientTick(s, a);
        else if (peers) detail::PeersClientTick(s, a);
        else detail::ClientTick(s, a);
    }
}

void OnDisconnect() {
    if (detail::ArmOf() == detail::Arm::Off) return;
    detail::HostReset();
    detail::ClientReset();
    detail::DamageReset();
    detail::SpawnedReset();
    detail::PeersReset();
}

}  // namespace coop::dev::atv_health_drill

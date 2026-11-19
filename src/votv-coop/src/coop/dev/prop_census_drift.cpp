// coop/dev/prop_census_drift.cpp -- [dev] the keyed props' places on every peer. See the header.
//
// EVERY PEER, once its session connects, at each VOTVCOOP_PROP_DRIFT_PERIOD ms (default 5000) boundary of the wall
// clock -- the machine's, which the rig's peers share, so every peer samples at one instant: each keyed prop (the key
// index's entries) within VOTVCOOP_PROP_DRIFT_RADIUS cm (default 4000) of VOTVCOOP_PROP_DRIFT_AT ("x;y;z", default the
// rig's base spawn) is a row of `prop_drift_<role>.csv` in VOTVCOOP_CAN_DIR: the wall instant, the key, the class, the
// place, the rotation and the speed. The rig's drift script joins the peers' files by instant and key.

#include "coop/dev/prop_census_drift.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::prop_census_drift {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_PROP_DRIFT") == "1";
    return v;
}

uint64_t WallMs() {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return ((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ull;
}

int EnvInt(const char* name, int def) {
    const std::string v = coop::config::ReadEnv(name);
    return v.empty() ? def : std::atoi(v.c_str());
}

ue_wrap::FVector Center() {
    ue_wrap::FVector c{-1044.f, 576.f, 6651.f};
    const std::string v = coop::config::ReadEnv("VOTVCOOP_PROP_DRIFT_AT");
    if (!v.empty()) std::sscanf(v.c_str(), "%f;%f;%f", &c.X, &c.Y, &c.Z);
    return c;
}

FILE* g_file = nullptr;
uint64_t g_lastSlot = 0;
std::vector<coop::prop_key_index::DebugEntry> g_entries;

FILE* File(bool host) {
    if (g_file) return g_file;
    const std::string dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (dir.empty()) return nullptr;
    const std::string path = dir + "\\prop_drift_" + (host ? "host" : "client" + std::to_string(
                                 coop::players::Registry::Get().LocalPeerId())) + ".csv";
    g_file = std::fopen(path.c_str(), "w");
    if (g_file) {
        std::fprintf(g_file, "wall,key,class,x,y,z,pitch,yaw,roll,speed\n");
        UE_LOGI("[PROP-DRIFT] census armed -> %s", path.c_str());
    }
    return g_file;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const uint64_t period = static_cast<uint64_t>(EnvInt("VOTVCOOP_PROP_DRIFT_PERIOD", 5000));
    const uint64_t slot = WallMs() / period;
    if (slot == g_lastSlot) return;
    g_lastSlot = slot;
    FILE* f = File(session->role() == coop::net::Role::Host);
    if (!f) return;
    const ue_wrap::FVector c = Center();
    const float r = static_cast<float>(EnvInt("VOTVCOOP_PROP_DRIFT_RADIUS", 4000));
    g_entries.clear();
    coop::prop_key_index::DebugCollect(g_entries);
    int rows = 0;
    for (const auto& e : g_entries) {
        if (!e.actor || !R::IsLiveByIndex(e.actor, e.idx)) continue;
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(e.actor, at)) continue;
        const float dx = at.X - c.X, dy = at.Y - c.Y, dz = at.Z - c.Z;
        if (dx * dx + dy * dy + dz * dz > r * r) continue;
        ue_wrap::FRotator rot{};
        E::TryGetActorRotation(e.actor, rot);
        const ue_wrap::FVector v = E::GetActorVelocity(e.actor);
        const std::string key(e.key.begin(), e.key.end());
        const std::wstring cls = R::ClassNameOf(e.actor);
        std::fprintf(f, "%llu,%s,%ls,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f\n",
                     static_cast<unsigned long long>(slot * period), key.c_str(), cls.c_str(), at.X, at.Y, at.Z,
                     rot.Pitch, rot.Yaw, rot.Roll, std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z));
        ++rows;
    }
    std::fflush(f);
    if (slot % 6 == 0) UE_LOGI("[PROP-DRIFT] sample %llu: %d props", static_cast<unsigned long long>(slot), rows);
}

}  // namespace coop::dev::prop_census_drift

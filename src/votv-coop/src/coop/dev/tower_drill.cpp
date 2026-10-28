// coop/dev/tower_drill.cpp -- see coop/dev/tower_drill.h. The dispatcher and what the arms share; the coordinate arms
// are in tower_drill_coord.cpp and the radio arms in tower_drill_radio.cpp.

#include "coop/dev/tower_drill.h"

#include "tower_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/world/fuse_tower_rows.h"

#include <chrono>
#include <cmath>
#include <string>

namespace coop::dev::tower_drill {
namespace detail {

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::tower_drill);
        if (v == "run") return Arm::Run;
        if (v == "red") return Arm::Red;
        if (v == "join") return Arm::Join;
        if (v == "joinred") return Arm::JoinRed;
        if (v == "radioreach") return Arm::RadioReach;
        if (v == "radio") return Arm::Radio;
        if (v == "radiored") return Arm::RadioRed;
        if (v == "radioreset") return Arm::RadioReset;
        if (v == "radiojoin") return Arm::RadioJoin;
        if (v == "radiojoinred") return Arm::RadioJoinRed;
        if (v == "radioresetred") return Arm::RadioResetRed;
        if (v == "radiofuse") return Arm::RadioFuse;
        if (v == "radiofusered") return Arm::RadioFuseRed;
        return Arm::Off;
    }();
    return a;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool FindRow(uint8_t kind, int32_t id, coop::net::FuseTowerRow& out) {
    coop::net::FuseTowerPayload rows{};
    if (!coop::fuse_tower_rows::LastRows(rows)) return false;
    for (int i = 0; i < rows.count && i < coop::net::kFuseTowers; ++i)
        if (rows.rows[i].kind == kind && rows.rows[i].id == id) {
            out = rows.rows[i];
            return true;
        }
    return false;
}

}  // namespace detail

void Tick(coop::net::Session* session) {
    const detail::Arm arm = detail::ArmOf();
    if (arm == detail::Arm::Off || !session) return;
    const bool host = session->role() == coop::net::Role::Host;
    // The join arms' host breaks its tower at its own world-ready, before any peer has joined: its leg runs with no
    // peer connected. Every other leg waits for the link.
    if (!session->connected() && !(host && detail::IsRadioJoin(arm))) return;
    if (detail::IsRadio(arm)) {
        if (host) detail::RadioHostTick(*session);
        else detail::RadioClientTick();
        return;
    }
    if (host) detail::CoordHostTick(*session);
    else detail::CoordClientTick();
}

void OnDisconnect() {
    detail::CoordReset();
    detail::RadioReset();
}

}  // namespace coop::dev::tower_drill

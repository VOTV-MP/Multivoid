// coop/dev/atv_riders_drill_seat_offset.cpp -- the ATV riders drill's measurement of where each peer puts the driver: one line
// per seating, every point in the ATV root's own axes (the offset from the root, rotated by the root's rotation undone), so the
// lines of different peers compare. No verdict is read from them.

#include "atv_riders_drill_detail.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>

namespace coop::dev::atv_riders_drill::detail {
namespace {

namespace E = ue_wrap::engine;
using ue_wrap::FRotator;
using ue_wrap::FVector;

// `p` from the root's place, in the root's axes (UE's rotation matrix of the root's FRotator, undone).
FVector InRootFrame(const FVector& p, const FVector& root, const FRotator& rot) {
    constexpr float kRad = 3.14159265358979f / 180.f;
    const float sp = std::sin(rot.Pitch * kRad), cp = std::cos(rot.Pitch * kRad);
    const float sy = std::sin(rot.Yaw * kRad), cy = std::cos(rot.Yaw * kRad);
    const float sr = std::sin(rot.Roll * kRad), cr = std::cos(rot.Roll * kRad);
    const float dx = p.X - root.X, dy = p.Y - root.Y, dz = p.Z - root.Z;
    const FVector ax{cp * cy, cp * sy, sp};
    const FVector ay{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    const FVector az{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    return {dx * ax.X + dy * ax.Y + dz * ax.Z, dx * ay.X + dy * ay.Y + dz * ay.Z, dx * az.X + dy * az.Y + dz * az.Z};
}

std::string Triple(const FVector& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.0f,%.0f,%.0f)", static_cast<double>(v.X), static_cast<double>(v.Y), static_cast<double>(v.Z));
    return buf;
}

// The point as a triple in the root's axes, or "(unread)".
std::string Rooted(bool read, const FVector& world, const FVector& root, const FRotator& rot) {
    return read ? Triple(InRootFrame(world, root, rot)) : "(unread)";
}

}  // namespace

void SayLocalSeatOffset() {
    void* atv = AD::Atv(g_key);
    FVector root{}, player{}, camera{}, sit{};
    FRotator rot{};
    if (!atv || !ue_wrap::atv::GetRootTransform(atv, root, rot)) {
        UE_LOGI("atv-riders: seat offset (local driver) on the %s: the ATV's root does not read", RoleName(MyRole()));
        return;
    }
    const bool p = E::TryGetActorLocation(AD::Me(), player);
    const bool c = ue_wrap::atv::GetCameraLocation(atv, camera);
    const bool s = ue_wrap::atv::GetSitArrowLocation(atv, sit);
    UE_LOGI("atv-riders: seat offset (local driver) on the %s: player=%s camera=%s sitarrow=%s", RoleName(MyRole()),
            Rooted(p, player, root, rot).c_str(), Rooted(c, camera, root, rot).c_str(), Rooted(s, sit, root, rot).c_str());
}

void SayRemoteSeatOffset(uint8_t slot) {
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(slot);
    void* body = puppet && puppet->valid() ? puppet->GetActor() : nullptr;
    void* atv = AD::Atv(g_key);
    FVector root{}, at{}, seat{}, sit{};
    FRotator rot{}, seatRot{};
    if (!body || !atv || !ue_wrap::atv::GetRootTransform(atv, root, rot)) {
        UE_LOGI("atv-riders: seat offset (remote driver slot %u) on the %s: the body or the ATV's root does not read",
                static_cast<unsigned>(slot), RoleName(MyRole()));
        return;
    }
    const bool b = E::TryGetActorLocation(body, at);
    const bool t = ue_wrap::atv::GetSeatTransform(atv, seat, seatRot);
    const bool s = ue_wrap::atv::GetSitArrowLocation(atv, sit);
    UE_LOGI("atv-riders: seat offset (remote driver slot %u) on the %s: body=%s seatTransform=%s sitarrow=%s",
            static_cast<unsigned>(slot), RoleName(MyRole()), Rooted(b, at, root, rot).c_str(), Rooted(t, seat, root, rot).c_str(),
            Rooted(s, sit, root, rot).c_str());
}

}  // namespace coop::dev::atv_riders_drill::detail

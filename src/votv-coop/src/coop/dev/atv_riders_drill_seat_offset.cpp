// coop/dev/atv_riders_drill_seat_offset.cpp -- the ATV riders drill's measurement of where each peer puts the driver: one line
// per seating, every point in the ATV root's own axes (the offset from the root, rotated by the root's rotation undone), so the
// lines of different peers compare. The local driver's line is a measurement only; a remote driver's body is also judged against
// the ATV's `playerHit` (JudgeRemoteSeat).

#include "atv_riders_drill_detail.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>
#include <string>

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

namespace {

// One remote driver's seating, said as a line and measured: the body's place against the ATV's `playerHit` (where the game's
// own sit puts its player; GetSeatTransform reads it), both in the ATV root's axes. `read` is false when the body, the ATV's
// root or `playerHit` does not read.
struct RemoteSeat {
    bool read = false;
    float offCm = -1.f;
    std::string body, hit;
};

RemoteSeat MeasureRemoteSeat(uint8_t slot) {
    RemoteSeat m;
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(slot);
    void* body = puppet && puppet->valid() ? puppet->GetActor() : nullptr;
    void* atv = AD::Atv(g_key);
    FVector root{}, at{}, seat{}, sit{};
    FRotator rot{}, seatRot{};
    if (!body || !atv || !ue_wrap::atv::GetRootTransform(atv, root, rot)) {
        UE_LOGI("atv-riders: seat offset (remote driver slot %u) on the %s: the body or the ATV's root does not read",
                static_cast<unsigned>(slot), RoleName(MyRole()));
        return m;
    }
    const bool b = E::TryGetActorLocation(body, at);
    const bool t = ue_wrap::atv::GetSeatTransform(atv, seat, seatRot);
    const bool s = ue_wrap::atv::GetSitArrowLocation(atv, sit);
    m.body = Rooted(b, at, root, rot);
    m.hit = Rooted(t, seat, root, rot);
    UE_LOGI("atv-riders: seat offset (remote driver slot %u) on the %s: body=%s playerHit=%s sitarrow=%s",
            static_cast<unsigned>(slot), RoleName(MyRole()), m.body.c_str(), m.hit.c_str(), Rooted(s, sit, root, rot).c_str());
    if (b && t) {
        m.read = true;
        const float dx = at.X - seat.X, dy = at.Y - seat.Y, dz = at.Z - seat.Z;
        m.offCm = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return m;
}

}  // namespace

void SayRemoteSeatOffset(uint8_t slot) { MeasureRemoteSeat(slot); }

bool JudgeRemoteSeat(uint8_t slot, int step) {
    RemoteSeat m;
    OnGt([&] { m = MeasureRemoteSeat(slot); });
    if (!m.read) {
        Invalid("the remote driver's body or the ATV's playerHit did not read (slot " + std::to_string(slot) + ")");
        return false;
    }
    if (m.offCm > kRigidCm) {
        Fail(step, "remote_seat_off",
             "the remote driver's body " + m.body + " is " + Num("%.0f", m.offCm) + " cm from the ATV's playerHit " + m.hit +
                 ", the seat the game's own sit uses (at most " + Num("%.0f", kRigidCm) + " cm)");
        return false;
    }
    return true;
}

}  // namespace coop::dev::atv_riders_drill::detail

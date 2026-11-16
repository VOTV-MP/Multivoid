// coop/dev/atv_riders_drill_drive.cpp -- the drive leg of the ATV riders drill's step 7 and the control run's idle verb: the
// host backs its own ATV kDriveCm away with its kerfur riding and judges where the kerfur stands, and says what the ATV, the
// kerfur and the seat table read while it drives. Called from the drill's own thread; every read hops to the game thread.

#include "atv_riders_drill_detail.h"

#include "coop/dev/atv_riders_arm.h"
#include "coop/element/mirror_managers.h"
#include "coop/interactables/atv_sync.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_nav.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::atv_riders_drill::detail {
namespace {

namespace E = ue_wrap::engine;
namespace K = ue_wrap::kerfur;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr float kWedgedUpZ = 0.9f;    // an ATV whose up vector points lower than this is tipped or climbing what it struck
constexpr int kDriveLogGapS = 2;      // at most one drive line per this many seconds of block events
constexpr int kProbeS = 3;            // the throttle's first seconds: the root must have left its start by kProbeCm in them
constexpr float kProbeCm = 20.f;

constexpr uint8_t kStateIdle = 1;   // enum_kerfurCommand: idle (0 follows the player, 6 goes to an ATV)

// The host's kerfur, as the drive line reads it: its distance to the ATV's sitarrow (cm, -1 unread), its CharacterMovement's
// mode and speed (-1 unread), its command state (-1 unread), what its last move() went for, and the path-following status
// of its AI controller (EPathFollowingStatus, -1 unread).
enum class Target { Unread, None, Host, Other };
struct KerfurDrive {
    float sitarrowCm = -1.f, speed = -1.f;
    int mode = -1, state = -1, path = -1;
    Target target = Target::Unread;
};

// What step 7's drive leg reads of the host's ATV in one game-thread hop, and the kerfur's seat beside it. `ok` is false when the
// root or the up vector does not read; `speed` is the root body's linear speed (cm/s), -1 when unread; `occupant` and `syncer`
// are the table's slots, -1 when the table does not read.
struct DriveRead {
    bool ok = false;
    FVector root{};
    float upZ = 0.f, yaw = 0.f, speed = -1.f, fuel = 0.f;
    bool brake = false, turbo = false, kerfurOn = false;
    int occupant = -1, syncer = -1;
    KerfurDrive kerfur;
};

// The kerfur of g_eid on this peer (the host's own, on the host), or null. Game thread.
void* KerfurActor() {
    coop::element::Npc* npc = coop::element::NpcMirrors().Get(g_eid);
    void* actor = npc ? npc->LiveActor() : nullptr;
    return actor && K::IsKerfurActor(actor) ? actor : nullptr;
}

void ReadKerfurDrive(KerfurDrive& k) {
    OnGt([&] {
        void* actor = KerfurActor();
        if (!actor) return;
        uint8_t mode = 0, state = 0, face = 0, status = 0;
        bool spooky = false;
        FVector vel{};
        if (ue_wrap::puppet::ReadCharacterMovement(actor, mode, vel)) {
            k.mode = mode;
            k.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z);
        }
        if (K::ReadKerfurState(actor, state, spooky, face)) k.state = state;
        void* target = K::ReadTargetActor(actor);
        k.target = !target ? Target::None : target == AD::Me() ? Target::Host : Target::Other;
        if (E::ReadMoveStatus(actor, &status)) k.path = status;
    });
}

DriveRead ReadDrive() {
    DriveRead d;
    OnGt([&] {
        void* atv = AD::Atv(g_key);
        FRotator rot{};
        if (!atv || !ue_wrap::atv::GetRootTransform(atv, d.root, rot)) return;
        const FVector up = E::GetActorUpVector(atv);
        d.ok = up.X != 0.f || up.Y != 0.f || up.Z != 0.f;   // a unit vector is never zero: a zero is an unread one
        d.upZ = up.Z;
        d.yaw = rot.Yaw;
        FVector lin{}, ang{};
        if (E::GetActorRootPhysicsVelocity(atv, lin, ang)) d.speed = std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z);
        d.fuel = ue_wrap::atv::GetFuel(atv);
        d.brake = ue_wrap::atv::GetBrake(atv);
        d.turbo = ue_wrap::atv::GetTurbo(atv);
        coop::atv_sync::TableView tv;
        if (coop::atv_sync::ReadTable(g_key, tv)) {
            d.occupant = tv.occupantSlot;
            d.syncer = tv.syncerSlot;
        }
    });
    const KerfurRead kerfur = ReadKerfur(g_eid);
    d.kerfurOn = kerfur.parentIsAtv;
    d.kerfur.sitarrowCm = kerfur.sitarrowCm;
    ReadKerfurDrive(d.kerfur);
    return d;
}

// The status of the path following as the line says it: Idle and Moving by name, anything else (waiting for a path, paused)
// or unread as `?`.
const char* PathName(int status) { return status == 0 ? "idle" : status == 3 ? "moving" : "?"; }

const char* TargetName(Target t) {
    return t == Target::None ? "none" : t == Target::Host ? "host" : t == Target::Other ? "other" : "?";
}

std::string DriveText(const DriveRead& d) {
    char buf[416];
    std::snprintf(buf, sizeof(buf),
                  "root=(%.0f,%.0f,%.0f) yaw=%.0f upZ=%.2f v=%.0f fuel=%.1f brake=%d turbo=%d seat=%d/%d kerfur=%s kd=%.0f "
                  "kmode=%d kv=%.0f kstate=%d ktarget=%s kpath=%s",
                  static_cast<double>(d.root.X), static_cast<double>(d.root.Y), static_cast<double>(d.root.Z),
                  static_cast<double>(d.yaw), static_cast<double>(d.upZ), static_cast<double>(d.speed), static_cast<double>(d.fuel), d.brake ? 1 : 0,
                  d.turbo ? 1 : 0, d.occupant, d.syncer, d.kerfurOn ? "on" : "off", static_cast<double>(d.kerfur.sitarrowCm),
                  d.kerfur.mode, static_cast<double>(d.kerfur.speed), d.kerfur.state, TargetName(d.kerfur.target),
                  PathName(d.kerfur.path));
    return buf;
}

// Reads the drive leg's state, says it, and returns its text.
std::string SayDrive() {
    const std::string text = DriveText(ReadDrive());
    UE_LOGI("atv-riders: step 7 drive (host): %s", text.c_str());
    return text;
}

}  // namespace

// Step 7's second leg on the host: it backs its own ATV kDriveCm (the root's distance, whatever the direction) away from where
// step 3 stopped it, with its kerfur riding, lets the reverse key go, reads where its kerfur stands (JudgeKerfurSeat) and says
// `drive7-done` for the clients' reads. An ATV that stands tipped, or that the throttle does not move kProbeCm in the kProbeS
// seconds after the reverse key is down, ends the leg INVALID at once as wedged. In the control the kerfur is idle and off the ATV, and the leg ends on the
// 600 cm alone.
bool HostDriveSeated() {
    const DriveRead first = ReadDrive();
    if (!first.ok) {
        Invalid("the ATV's root or up vector does not read");
        return false;
    }
    UE_LOGI("atv-riders: step 7 drive (host): %s", DriveText(first).c_str());
    // A tipped ATV cannot drive: the leg would measure something other than the host's own drive.
    if (first.upZ < kWedgedUpZ) {
        Invalid("the ATV is wedged before the host's drive (upZ=" + Num("%.2f", first.upZ) + ", yaw " + Num("%.0f", first.yaw) + ")");
        return false;
    }
    const FVector start = first.root;
    std::string why;
    // The leg backs away: step 3's drive ends against what it struck, ahead of the ATV, so the reverse key has the lane clear.
    if (!DriveBack(g_key, why)) {
        DriveBackOff(g_key);
        Invalid(why + "; " + SayDrive());
        return false;
    }
    // DriveBack returns with the key down and the driver's fuel fallen: the wedge probe's window and its root start here.
    const auto driveStart = std::chrono::steady_clock::now();
    FVector probeRoot = start;
    ReadRoot(probeRoot);
    SayDrive();
    // The leg's line is said at each block of the key (the host's own built, or a client's applied) and at most once per
    // kDriveLogGapS of them, so a drive that does not move shows where it stands without a line per block.
    const int32_t tag = Tag();
    uint32_t builtCursor = DE::Count(DE::kStampBuilt), appliedCursor = DE::Count(DE::kPoseApplied);
    std::chrono::steady_clock::time_point lastSaid = std::chrono::steady_clock::now();
    float movedCm = 0.f;
    bool probed = false;
    const bool arrived = PollFn("the host's own 600 cm drive", kDriveS, [&]() -> int {
        bool block = false;
        DE::Rec r;
        while (DE::Next(DE::kStampBuilt, builtCursor, r)) block = block || r.i0 == tag;
        while (DE::Next(DE::kPoseApplied, appliedCursor, r)) block = block || r.i0 == tag;
        const auto now = std::chrono::steady_clock::now();
        if (block && now - lastSaid >= std::chrono::seconds(kDriveLogGapS)) {
            lastSaid = now;
            SayDrive();
        }
        FVector at{};
        const bool read = ReadRoot(at);
        if (read) movedCm = std::sqrt((at.X - start.X) * (at.X - start.X) + (at.Y - start.Y) * (at.Y - start.Y));
        if (movedCm >= kDriveCm) return 1;
        // The throttle's first seconds moved the root nowhere: the ATV is held (a wall, a rock), not slow; said at once. The window
        // starts at DriveBack's return, after the key went down and the fuel fell (it exposes no key-down), so it only errs late.
        if (read && !probed && now - driveStart >= std::chrono::seconds(kProbeS)) {
            probed = true;
            const float probeCm = std::sqrt((at.X - probeRoot.X) * (at.X - probeRoot.X) + (at.Y - probeRoot.Y) * (at.Y - probeRoot.Y));
            if (probeCm < kProbeCm) {
                Invalid("the ATV is wedged before the host's drive: the first " + std::to_string(kProbeS) + " s of reverse moved the root " +
                        Num("%.0f", probeCm) + " cm; " + SayDrive());
                return -1;
            }
        }
        return 0;
    }, [] { return "at the bound: " + SayDrive(); });
    if (arrived) SayDrive();
    DriveBackOff(g_key);
    if (!arrived) return false;
    if (Control()) {
        // The control measures the drive with no kerfur on the ATV: a kerfur that rode anyway says nothing about it.
        if (ReadKerfur(g_eid).parentIsAtv) {
            Invalid("control: the host's kerfur rode the ATV although it was idle");
            return false;
        }
        StepPass(7, "control: the host drove " + Num("%.0f", kDriveCm) + " cm with its kerfur idle and off the ATV");
    } else if (!JudgeKerfurSeat('b', "at the end of its own 600 cm drive")) {
        return false;
    }
    PS::Mark("drive7-done");
    return true;
}

bool HostKerfurIdle() {
    bool ran = false;
    OnGt([&] {
        void* kerfur = KerfurActor();
        void* me = AD::Me();
        ran = kerfur && me && K::RunActionName(kerfur, me, L"idle");
    });
    if (!ran) {
        Invalid("control: the game's 'idle' verb did not run on the host's kerfur");
        return false;
    }
    // The verb is recorded by the radial's watch and run on the next net-pump tick (coop/creatures/kerfur_command), so the
    // state is read until it shows.
    if (!PollFn("the host's kerfur idle after the 'idle' verb", kEventS, [&]() -> int {
            KerfurDrive k;
            ReadKerfurDrive(k);
            return k.state == kStateIdle ? 1 : 0;
        }))
        return false;
    UE_LOGI("atv-riders: step 7 control: the host's kerfur eid %u took the game's 'idle' verb before the host sat", g_eid);
    return true;
}

}  // namespace coop::dev::atv_riders_drill::detail

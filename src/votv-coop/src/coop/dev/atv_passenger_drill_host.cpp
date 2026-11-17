// coop/dev/atv_passenger_drill_host.cpp -- see coop/dev/atv_passenger_drill.h: the host's half. It spawns the ATV, logs
// its geometry, drives it with the client aboard, rides the client's ATV in turn and judges the red arms by the host's
// counters.

#include "coop/dev/atv_passenger_drill_detail.h"  // shared by the drill's two files

namespace coop::dev::atv_passenger_drill::detail {
namespace {

enum class H : int {
    WaitClient, Spawn, Park, Sit, WaitBoard, Judge1, DriveStart, Drive, DriverLeaves, WaitStayed, WaitLeft, Judge2,
    ClientDrives, WaitClientSat, HostBoard, HostRideWait, HostLeave, ClientUndrove,
    Sit2, WaitBoard2, Judge3,
    RedDupGo, RedDupWait, RedLeave, RedLeaveWait, FullArm, FullWait, ReachArm, ReachWait, Finish
};

// The stage: open ground outside the base, the host facing away from it.
constexpr float kStageX = -1692.f, kStageY = 560.f, kStageZ = 6318.f, kStageYaw = 180.f;

struct Host {
    Shared                g;
    void*                 atv = nullptr;
    std::wstring          key;
    uint64_t              spawnAt = 0;
    FVector               lastAt{};
    uint64_t              lastAtMs = 0;
    int                   calm = 0;
    bool                  spawned = false;
    bool                  staged = false;   // the host went to the stage
    atv_seat::SitTry      sit;
    FVector               driveFrom{};
    float                 maxPuppetErr = 0.f;
    float                 clientRideErr = -1.f;
    int                   inputOff = -1;
    uint8_t               inputMask = 0;
    void*                 atvClass = nullptr;
    AU::Counters          base{};
    uint64_t              redAt = 0;
    coop::director::ClearHand clear;   // the sit verb wants empty hands: the save's shovel is dropped first
    int                   presses = 0;
    uint64_t              pressAt = 0;
    LeaveCheck            leave;
    bool                  cleared = false;
    uint64_t              lookStartMs = 0, lookAimedMs = 0;   // the host's look at the seat after the driver left
    bool                  lookDone = false, lookAimed = false;
    int                   lookShot = 0;
};
Host gh;

// The throttle: ATV_C::input_forward, the field the game's own key handler writes.
void Throttle(void* atv, bool on) {
    if (gh.inputOff < 0) {
        gh.atvClass = R::ClassOf(atv);
        if (!gh.atvClass || !R::FindBoolProperty(gh.atvClass, L"input_forward", gh.inputOff, gh.inputMask)) {
            gh.inputOff = -1;
            return;
        }
    }
    uint8_t* b = reinterpret_cast<uint8_t*>(atv) + gh.inputOff;
    if (on) *b |= gh.inputMask; else *b &= static_cast<uint8_t>(~gh.inputMask);
}

// The handbrake: ATV_C::brake and the game's own setBrake, which applies it to the rig.
void SetBrake(void* atv, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    void* cls = R::ClassOf(atv);
    if (!cls || !R::FindBoolProperty(cls, L"brake", off, mask)) return;
    uint8_t* b = reinterpret_cast<uint8_t*>(atv) + off;
    if (((*b & mask) != 0) == on) return;
    if (on) *b |= mask; else *b &= static_cast<uint8_t>(~mask);
    if (void* fn = R::FindFunction(cls, L"setBrake")) {
        ue_wrap::ParamFrame pf(fn);
        if (pf.valid()) ue_wrap::Call(atv, pf);
    }
}

bool HostLiveAtv() {
    if (gh.atv && AtvOf(gh.key, &gh.atv)) return true;
    return false;
}

}  // namespace

void HostTick(coop::net::Session* s) {
    Shared& g = gh.g;
    if (Seen("atvpass client FAIL")) { Fail(g, "host", "the client reported a failed arm (its own line has why)"); return; }
    void* me = coop::players::Registry::Get().Local();
    switch (static_cast<H>(g.step)) {
    case H::WaitClient:
        // The drive needs open ground: the host goes to the stage outside the base, facing away from it, before the client
        // joins at its place.
        if (!gh.staged && me) {
            gh.staged = true;
            const bool moved = coop::teleport_client::ApplyLocally({kStageX, kStageY, kStageZ, 0.f, kStageYaw, 0.f});
            UE_LOGI("%s host: goes to the stage at (%.0f,%.0f,%.0f): teleport %s", kTag, kStageX, kStageY, kStageZ,
                    moved ? "dispatched" : "NOT dispatched");
        }
        if (!s->IsSlotWorldReady(1) || !me) { if (g.stepAt == 0) g.stepAt = Now(); return; }
        if (Seen("atvpass client-ready")) { DC::Say("atvpass host-ready"); Next(g); }
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never said it was ready");
        return;

    case H::Spawn: {
        if (!gh.spawned) {
            // Noon, so the screenshots of the seats show the rig and not a night sky.
            int hour = 0, minute = 0, day = 1;
            float sun = 0.f;
            if (coop::dev::set_clock::ReadCurrent(hour, minute, day, sun)) coop::dev::set_clock::SetClock(day, 12, 0);
            FVector at{};
            if (!E::TryGetActorLocation(me, at)) { Fail(g, "host", "the host's player does not read"); return; }
            const FVector fwd = E::GetActorForwardVector(me);
            SN::Spawned sp;
            gh.spawnAt = Now();
            if (!SN::SpawnAtNav(me, {at.X + fwd.X * kAheadCm, at.Y + fwd.Y * kAheadCm, at.Z + SN::kLiftCm}, sp)) {
                Fail(g, "host", std::string(sp.what) + ": " + sp.got);
                return;
            }
            gh.atv = sp.actor;
            gh.spawned = true;
        }
        const char* why = nullptr;
        const SN::Key k = SN::KeyOf(gh.atv, &gh.key, &why);
        if (k == SN::Key::Bad) { Fail(g, "host", std::string("the ATV's key cannot ride a chat line: ") + why); return; }
        if (k == SN::Key::Ok) {
            DC::Say("atvpass key " + std::string(gh.key.begin(), gh.key.end()));
            UE_LOGI("%s host: the spawned ATV is '%ls'", kTag, gh.key.c_str());
            Next(g);
        } else if (Timed(g, kShortMs * 3)) {
            Fail(g, "host", "the spawned ATV never entered the host's table");
        }
        return;
    }

    case H::Park: {
        FVector at{};
        if (!HostLiveAtv() || !E::TryGetActorLocation(gh.atv, at)) { Fail(g, "host", "the spawned ATV is gone"); return; }
        const uint64_t now = Now();
        if (gh.lastAtMs == 0 || now - gh.lastAtMs >= 250) {
            if (gh.lastAtMs != 0) {
                const float secs = static_cast<float>(now - gh.lastAtMs) / 1000.f;
                gh.calm = Dist(at, gh.lastAt) / secs < kParkedCmS ? gh.calm + 1 : 0;
            }
            gh.lastAt = at;
            gh.lastAtMs = now;
        }
        // Parked, and the client's body is on this machine (a board before its first pose is refused no_body).
        if (gh.calm >= 2 && coop::players::Registry::Get().LifeOf(1) == coop::players::PuppetLife::Alive) {
            LogGeometry(gh.atv, me);
            DC::Say("atvpass parked");
            Next(g);
        } else if (Timed(g, kShortMs * 3)) {
            Fail(g, "host", "the spawned ATV never came to rest");
        }
        return;
    }

    case H::Sit:
    case H::Sit2:
        if (!HostLiveAtv()) { Fail(g, "host", "the ATV is gone before the sit"); return; }
        if (!gh.cleared) {
            const coop::director::ClearState cs = gh.clear.Tick();
            if (cs == coop::director::ClearState::Failed) { Fail(g, "host", std::string("the host's hand could not be cleared: ") + gh.clear.Why()); return; }
            if (cs == coop::director::ClearState::Working) { if (Timed(g, kShortMs * 3)) Fail(g, "host", "the host's hand never cleared"); return; }
            gh.cleared = true;
            g.stepAt = Now();
        }
        if (!g.flag) { gh.sit = atv_seat::SitTry{}; g.flag = true; }
        if (!atv_seat::TrySit(me, gh.atv, gh.sit)) {
            if (Timed(g, kShortMs * 3)) Fail(g, "host", "the host's sit never finished");
            return;
        }
        if (coop::atv_sync::ActorSeatedBy(0) != gh.atv) { Fail(g, "host", "the host's sit did not take"); return; }
        DC::Say(static_cast<H>(g.step) == H::Sit ? "atvpass host-sat" : "atvpass host-sat2");
        UE_LOGI("%s host: sat as the driver", kTag);
        LogDriverSeat(gh.atv, me);
        Next(g);
        return;

    case H::WaitBoard:
    case H::WaitBoard2: {
        const bool second = static_cast<H>(g.step) == H::WaitBoard2;
        if (Seen(second ? "atvpass client-boarded2" : "atvpass client-boarded")) { gh.base = AU::ReadCounters(); Next(g); }
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never boarded");
        return;
    }

    case H::Judge1:
    case H::Judge3: {
        // The host's own table holds the client in seat 0, the host's copy of the rig carries its puppet at the seat.
        if (!HostLiveAtv()) { Fail(g, "host", "the ATV is gone"); return; }
        bool attached = false;
        const int seat = coop::atv_passenger::SeatOfSlot(gh.key, 1, &attached);
        const coop::atv_sync::TableView t = TableOf(gh.key);
        FVector seatAt{}, puppetAt{};
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
        const bool placed = AP::SeatWorldLocation(gh.atv, 0, seatAt) && rp && rp->valid() && rp->TryGetLocation(puppetAt);
        if (seat != 0 || !attached || !placed || t.passengerSlot[0] != 1) {
            if (Timed(g, kShortMs)) {
                Fail(g, "host", "the client's seat: table seat " + std::to_string(seat) + ", puppet attached " + std::to_string(attached ? 1 : 0) +
                                    ", puppet read " + std::to_string(placed ? 1 : 0));
            }
            return;
        }
        const float err = Dist(puppetAt, seatAt);
        if (err > kRideTolCm) {
            if (Timed(g, kShortMs)) Fail(g, "host", "the client's puppet is " + Num(err) + " cm from seat 0 (tolerance " + Num(kRideTolCm) + ")");
            return;
        }
        if (!g.logged) {
            g.logged = true;
            UE_LOGI("%s host: the client holds passenger seat 0, its puppet sits %.0f cm from it on the host's copy, speed shown seated=%d",
                    kTag, err, coop::puppet_seat::HoldsBody(1) ? 1 : 0);
        }
        if (!coop::puppet_seat::HoldsBody(1)) { Fail(g, "host", "the body's owner does not hold the seated puppet"); return; }
        // The look of the seat, for a person: the host's view (the driver's chase camera) of the rig with its rider on it.
        if (static_cast<H>(g.step) == H::Judge1) {
            if (!g.flag) { g.flag = true; g.flagAt = Now(); return; }
            if (Now() - g.flagAt < 1500) return;
            harness::screenshot::Capture(L"atvpass-rest");
        }
        DC::Say(static_cast<H>(g.step) == H::Judge1 ? "atvpass host-sees-rider" : "atvpass host-sees-rider2");
        Next(g);
        return;
    }

    case H::DriveStart:
        if (!Seen("atvpass client-armed")) { if (Timed(g, kBoundMs)) Fail(g, "host", "the client never armed its ride samples"); return; }
        if (!HostLiveAtv() || !AtvOf(gh.key, &gh.atv, &gh.driveFrom)) { Fail(g, "host", "the ATV is gone before the drive"); return; }
        SetBrake(gh.atv, false);
        Throttle(gh.atv, true);
        gh.maxPuppetErr = 0.f;
        DC::Say("atvpass drive-start");
        UE_LOGI("%s host: driving from (%.0f,%.0f,%.0f)", kTag, gh.driveFrom.X, gh.driveFrom.Y, gh.driveFrom.Z);
        Next(g);
        return;

    case H::Drive: {
        FVector root{}, seatAt{}, puppetAt{};
        if (!HostLiveAtv() || !AtvOf(gh.key, &gh.atv, &root)) { Fail(g, "host", "the ATV is gone during the drive"); return; }
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
        if (rp && rp->valid() && rp->TryGetLocation(puppetAt) && AP::SeatWorldLocation(gh.atv, 0, seatAt)) {
            const float err = Dist(puppetAt, seatAt);
            if (err > gh.maxPuppetErr) gh.maxPuppetErr = err;
        }
        const float moved = Dist(root, gh.driveFrom);
        // A rig that will not take the throttle is pushed along its own forward axis: the drill measures the ride, not the engine.
        if (Now() - g.stepAt > 5000 && moved < 100.f) {
            const FVector fwd = E::GetActorForwardVector(gh.atv);
            E::SetActorRootPhysicsVelocity(gh.atv, {fwd.X * 500.f, fwd.Y * 500.f, 0.f}, {});
        }
        if (moved >= kDriveCm * 0.5f && !g.flag) { g.flag = true; harness::screenshot::Capture(L"atvpass-drive"); }
        if (moved >= kDriveCm) {
            Throttle(gh.atv, false);
            UE_LOGI("%s host: drove %.0f cm; the client's puppet stayed within %.0f cm of its seat (tolerance %.0f)", kTag, moved,
                    gh.maxPuppetErr, kRideTolCm);
            if (gh.maxPuppetErr > kRideTolCm) { Fail(g, "host", "the client's puppet strayed " + Num(gh.maxPuppetErr) + " cm from its seat during the drive"); return; }
            DC::Say("atvpass driven " + Num(moved));
            Next(g);
        } else if (Timed(g, kShortMs * 3)) {
            Throttle(gh.atv, false);
            Fail(g, "host", "the ATV moved only " + Num(moved) + " cm of " + Num(kDriveCm));
        }
        return;
    }

    case H::DriverLeaves: {
        if (!Seen("atvpass client-rode")) { if (Timed(g, kBoundMs)) Fail(g, "host", "the client never reported its ride"); return; }
        if (!HostLiveAtv()) { Fail(g, "host", "the ATV is gone"); return; }
        if (!g.flag) {
            g.flag = true;
            if (!A::Dismount(gh.atv)) { Fail(g, "host", "the ATV's own dismount did not run"); return; }
            SetBrake(gh.atv, true);   // a rig left rolling would carry the next boarding out of reach
            return;
        }
        const coop::atv_sync::TableView t = TableOf(gh.key);
        if (t.occupantSlot != 0xFF || coop::atv_sync::ActorSeatedBy(0) == gh.atv) {
            if (Timed(g, kShortMs)) Fail(g, "host", "the driver's seat did not clear on the host's table");
            return;
        }
        // The look of the seat, for a person: the host's own eyes, put a few metres from the rig (first its side, then its rear),
        // aimed at the rider.
        if (!gh.lookDone) {
            if (gh.lookStartMs == 0) gh.lookStartMs = Now();
            if (Now() - gh.lookStartMs < 1200) return;
            FVector root{}, seatAt{};
            FRotator rot{};
            if (!me || !A::GetRootTransform(gh.atv, root, rot) || !AP::SeatWorldLocation(gh.atv, 0, seatAt)) { gh.lookDone = true; return; }
            const FVector f = E::GetActorForwardVector(gh.atv), u = E::GetActorUpVector(gh.atv);
            const FVector r{u.Y * f.Z - u.Z * f.Y, u.Z * f.X - u.X * f.Z, u.X * f.Y - u.Y * f.X};
            if (gh.lookAimedMs == 0) {
                // The side view stands 3.3 m off the left; the rear view 3.3 m behind and a little right.
                const bool rear = gh.lookShot == 1;
                const FVector at{root.X + (rear ? -f.X * 330.f + r.X * 60.f : -r.X * 330.f), root.Y + (rear ? -f.Y * 330.f + r.Y * 60.f : -r.Y * 330.f),
                                 root.Z + 40.f};
                E::TeleportTo(me, at, FRotator{0.f, rot.Yaw, 0.f});
                gh.lookAimedMs = Now();
                gh.lookAimed = false;
                return;
            }
            if (!gh.lookAimed && Now() - gh.lookAimedMs >= 900) {
                E::SetControlRotation(E::GetController(me), coop::director::LookAt(E::GetCameraLocation(), seatAt));
                gh.lookAimed = true;
                gh.lookAimedMs = Now();
                return;
            }
            if (!gh.lookAimed || Now() - gh.lookAimedMs < 700) return;
            harness::screenshot::Capture(gh.lookShot == 0 ? L"atvpass-seat-side" : L"atvpass-seat-rear");
            if (++gh.lookShot >= 2) gh.lookDone = true;
            gh.lookAimedMs = 0;
            return;
        }
        bool attached = false;
        if (coop::atv_passenger::SeatOfSlot(gh.key, 1, &attached) != 0 || !attached) {
            Fail(g, "host", "the driver leaving ejected the passenger from the host's table or its puppet");
            return;
        }
        UE_LOGI("%s host: the driver left; the client still holds seat 0 and its puppet is still on the rig", kTag);
        DC::Say("atvpass driver-left");
        Next(g);
        return;
    }

    case H::WaitStayed:
        if (Seen("atvpass client-stayed")) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never confirmed it stayed seated");
        return;

    case H::WaitLeft:
        if (Seen("atvpass client-left ")) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never left its seat");
        return;

    case H::Judge2: {
        bool attached = true;
        const int seat = coop::atv_passenger::SeatOfSlot(gh.key, 1, &attached);
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
        if (seat != -1 || attached || coop::puppet_seat::HoldsBody(1)) {
            if (Timed(g, kShortMs)) Fail(g, "host", "the client's leave did not clear its seat on the host: seat " + std::to_string(seat) + ", puppet attached " +
                                                          std::to_string(attached ? 1 : 0));
            return;
        }
        UE_LOGI("%s host: the client's seat is free and its puppet follows the pose stream again", kTag);
        DC::Say("atvpass host-sees-left");
        Next(g);
        return;
    }

    // The host rides: the client drives the braked rig and the host's own player boards by the use key, judged by the same table.
    case H::ClientDrives:
        DC::Say("atvpass drive-by-client go");
        Next(g);
        return;
    case H::WaitClientSat:
        if (Seen("atvpass client-sat") && TableOf(gh.key).occupantSlot == 1) { gh.presses = 0; gh.pressAt = 0; Next(g); }
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never sat in the ATV as the driver");
        return;
    case H::HostBoard: {
        if (!HostLiveAtv()) { Fail(g, "host", "the ATV is gone"); return; }
        if (coop::atv_passenger::ReadLocal().seated) {
            bool attached = false;
            const int seat = coop::atv_passenger::SeatOfSlot(gh.key, 0, &attached);
            if (seat != 0 || !attached || TableOf(gh.key).passengerSlot[0] != 0) {
                if (Timed(g, kShortMs * 3)) Fail(g, "host", "the host's board: table seat " + std::to_string(seat) + ", body attached " + std::to_string(attached ? 1 : 0));
                return;
            }
            UE_LOGI("%s host: boarded passenger seat 0 of the client's ATV through the use key (the host's own table)", kTag);
            DC::Say("atvpass host-boarded");
            Next(g);
            return;
        }
        if (Now() - gh.pressAt > 3000) {
            if (gh.presses >= 3) { Fail(g, "host", "three use presses at the client's ATV did not board the host"); return; }
            ++gh.presses;
            gh.pressAt = Now();
            FVector root{};
            AtvOf(gh.key, &gh.atv, &root);
            E::SetControlRotation(E::GetController(me), coop::director::LookAt(E::GetCameraLocation(), root));
            if (!PressUse(me, gh.atv)) Fail(g, "host", "the use input event did not dispatch");
        }
        return;
    }
    case H::HostRideWait:
        if (Seen("atvpass client-sees-host-rider")) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never saw the host's body on its copy of the rig");
        return;
    case H::HostLeave: {
        if (!g.flag) {
            if (Now() - g.stepAt < 1000) return;
            g.flag = true;
            gh.leave = LeaveCheck{};
            if (!PressUse(me, nullptr)) Fail(g, "host", "the use input event did not dispatch for the leave");
            return;
        }
        if (coop::atv_passenger::LocalIsPassenger()) { if (Timed(g, kShortMs)) Fail(g, "host", "the use key did not leave the seat"); return; }
        const int r = CheckLeft(gh.leave, me, gh.atv);
        if (r < 0) { Fail(g, "host", gh.leave.why); return; }
        if (r == 0) return;
        UE_LOGI("%s host: left its passenger seat through the use key and stood %.0f cm from the ATV, %.0f cm above its root", kTag, gh.leave.horiz, gh.leave.up);
        DC::Say("atvpass host-left");
        Next(g);
        return;
    }
    case H::ClientUndrove:
        if (Seen("atvpass client-undrove") && TableOf(gh.key).occupantSlot == 0xFF) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never got out of the driver's seat");
        return;

    // The red arms, each a raw request the client sends and the host's counters judge.
    case H::RedDupGo:
        if (Now() - g.stepAt < kBucketGapMs) return;
        gh.base = AU::ReadCounters();
        DC::Say("atvpass red-dup go");
        Next(g);
        return;
    case H::RedDupWait: {
        const AU::Counters c = AU::ReadCounters();
        const unsigned idx = static_cast<unsigned>(AtvPassengerRefusal::AlreadySeated);
        if (c.passengerRefused[idx] > gh.base.passengerRefused[idx]) {
            UE_LOGI("%s host: a duplicate board from a rider was refused already_seated (%llu)", kTag, c.passengerRefused[idx]);
            Next(g);
        } else if (Timed(g, kShortMs)) {
            Fail(g, "host", "the duplicate board while seated was not refused already_seated");
        }
        return;
    }

    case H::RedLeave:
        DC::Say("atvpass red-leave go");
        Next(g);
        return;
    case H::RedLeaveWait: {
        bool attached = true;
        if (Seen("atvpass client-left2") && coop::atv_passenger::SeatOfSlot(gh.key, 1, &attached) == -1) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "host", "the client never left the second time");
        return;
    }

    case H::FullArm:
        if (!HostLiveAtv()) { Fail(g, "host", "the ATV is gone"); return; }
        if (Now() - g.stepAt < kBucketGapMs) return;
        AU::DrillSetPassengerGhosts(gh.key, true);
        gh.base = AU::ReadCounters();
        DC::Say("atvpass red-full go");
        Next(g);
        return;
    case H::FullWait: {
        const AU::Counters c = AU::ReadCounters();
        const unsigned idx = static_cast<unsigned>(AtvPassengerRefusal::Full);
        if (c.passengerRefused[idx] > gh.base.passengerRefused[idx]) {
            AU::DrillSetPassengerGhosts(gh.key, false);
            UE_LOGI("%s host: a board with every seat taken was refused full (%llu)", kTag, c.passengerRefused[idx]);
            Next(g);
        } else if (Timed(g, kShortMs)) {
            AU::DrillSetPassengerGhosts(gh.key, false);
            Fail(g, "host", "the board with every seat taken was not refused full");
        }
        return;
    }

    case H::ReachArm: {
        if (!Seen("atvpass raw full") || Now() - g.stepAt < kBucketGapMs) { if (Timed(g, kBoundMs)) Fail(g, "host", "the client never sent its second raw request"); return; }
        if (!HostLiveAtv()) { Fail(g, "host", "the ATV is gone"); return; }
        FVector root{};
        FRotator rot{};
        if (!A::GetRootTransform(gh.atv, root, rot) || !A::TeleportRig(gh.atv, {root.X + kAwayCm, root.Y, root.Z + 40.f}, rot)) {
            Fail(g, "host", "the host could not move its ATV away");
            return;
        }
        gh.base = AU::ReadCounters();
        DC::Say("atvpass red-reach go");
        Next(g);
        return;
    }
    case H::ReachWait: {
        const AU::Counters c = AU::ReadCounters();
        const unsigned idx = static_cast<unsigned>(AtvPassengerRefusal::OutOfReach);
        if (c.passengerRefused[idx] > gh.base.passengerRefused[idx]) {
            UE_LOGI("%s host: a board out of reach was refused out_of_reach (%llu)", kTag, c.passengerRefused[idx]);
            Next(g);
        } else if (Timed(g, kShortMs)) {
            Fail(g, "host", "the board out of reach was not refused out_of_reach");
        }
        return;
    }

    case H::Finish: {
        const AU::Counters c = AU::ReadCounters();
        DC::Say("atvpass done");
        char nums[200];
        std::snprintf(nums, sizeof(nums), "admitted=%llu refused: already_seated=%llu full=%llu out_of_reach=%llu paced=%llu",
                      c.passengerAdmitted, c.passengerRefused[static_cast<unsigned>(AtvPassengerRefusal::AlreadySeated)],
                      c.passengerRefused[static_cast<unsigned>(AtvPassengerRefusal::Full)],
                      c.passengerRefused[static_cast<unsigned>(AtvPassengerRefusal::OutOfReach)],
                      c.passengerRefused[static_cast<unsigned>(AtvPassengerRefusal::Paced)]);
        Pass(g, "host", std::string("the client boarded seat 0 and rode the drive within tolerance (puppet max ") + Num(gh.maxPuppetErr) +
                            " cm), stayed when the driver left, left beside the ATV; the host rode the client's ATV the same way; the client boarded again and an teleport took it off the seat, and the host refused its duplicate, full and out-of-reach boards -- " +
                            nums);
        return;
    }
    }
}

bool HostDone() { return gh.g.done; }

void HostReset() { gh = Host{}; }

}  // namespace coop::dev::atv_passenger_drill::detail

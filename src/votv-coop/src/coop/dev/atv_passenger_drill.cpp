// coop/dev/atv_passenger_drill.cpp -- see coop/dev/atv_passenger_drill.h: the client's half and the entry points; the
// host's half is atv_passenger_drill_host.cpp.

#include "coop/dev/atv_passenger_drill.h"

#include "coop/dev/atv_passenger_drill_detail.h"  // shared by the drill's two files

namespace coop::dev::atv_passenger_drill {
namespace detail {
namespace {

enum class C : int {
    Ready, WaitKey, WaitAtv, Walk, WaitHostSat, Board, Armed, Ride, Stay, Leave, WaitDriveCue, SitDriver, WaitHostBoard, WaitHostLeft, WaitSit2, Walk2,
    Board2, DupGo, DupSent,
    Leave2Go, Leave2, FullGo, FullSent, ReachGo, ReachSent, Finish
};

struct Client {
    Shared                                          g;
    std::wstring                                    key;
    void*                                           atv = nullptr;
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    FVector                                         boardedAt{};
    float                                           maxErr = 0.f;
    float                                           moved = 0.f;
    int                                             presses = 0;
    uint64_t                                        pressAt = 0;
    bool                                            readySaid = false;
    coop::director::ClearHand                       clear;
    bool                                            cleared = false;
    atv_seat::SitTry                                sit;
    LeaveCheck                                      leave;
};
Client gc;

void WalkCancel() {
    if (gc.walk && gc.walk->state.load() == 0) coop::director::CancelBackgroundWalk(*gc.walk);
    gc.walk.reset();
}

bool StreamsRun() { return coop::net_pump::HasAnnouncedWorldReady() && coop::players::Registry::Get().Local() != nullptr; }

bool SendRaw(coop::net::Session& s, bool board, uint8_t seq) {
    coop::net::AtvPassengerRequestPayload p{};
    coop::net::WireKeyFromString(gc.key, p.key);
    p.action = board ? 1 : 0;
    p.seq = seq;
    return s.SendReliable(coop::net::ReliableKind::AtvPassengerRequest, &p, sizeof(p));
}

// Is this machine's body a passenger of the ATV, on the seat the table names, attached?
bool Seated(int* seatOut = nullptr) {
    const coop::atv_passenger::LocalView v = coop::atv_passenger::ReadLocal();
    if (!v.seated || v.key != gc.key) return false;
    bool attached = false;
    const int seat = coop::atv_passenger::SeatOfSlot(gc.key, coop::players::Registry::Get().LocalPeerId(), &attached);
    if (seatOut) *seatOut = seat;
    return attached && seat == v.seat && seat >= 0;
}
}  // namespace

void ClientTick(coop::net::Session* s) {
    Shared& g = gc.g;
    if (Seen("atvpass host FAIL")) { WalkCancel(); Fail(g, "client", "the host reported a failed arm (its own line has why)"); return; }
    void* player = coop::players::Registry::Get().Local();
    if (!player || !StreamsRun()) return;
    if (g.stepAt == 0) g.stepAt = Now();
    switch (static_cast<C>(g.step)) {
    case C::Ready:
        if (!gc.readySaid) { gc.readySaid = true; DC::Say("atvpass client-ready"); }
        if (Seen("atvpass host-ready")) Next(g);
        return;

    case C::WaitKey: {
        std::string rest;
        if (DC::SeenRestOf("atvpass key ", &rest) && Seen("atvpass parked")) {
            gc.key.assign(rest.begin(), rest.end());
            UE_LOGI("%s client: the host's ATV is '%ls'", kTag, gc.key.c_str());
            Next(g);
        } else if (Timed(g, kBoundMs)) {
            Fail(g, "client", "the host never named its ATV");
        }
        return;
    }

    case C::WaitAtv: {
        FVector root{};
        if (AtvOf(gc.key, &gc.atv, &root)) {
            gc.walk = coop::director::StartBackgroundWalk(root, kWalkReachCm, kWalkDeadlineS);
            UE_LOGI("%s client: walking to the ATV at (%.0f,%.0f,%.0f)", kTag, root.X, root.Y, root.Z);
            Next(g);
        } else if (Timed(g, kBoundMs)) {
            Fail(g, "client", "the host's ATV never entered this table");
        }
        return;
    }

    case C::Walk:
    case C::Walk2: {
        const int st = gc.walk ? gc.walk->state.load() : 2;
        if (st == 2) { Fail(g, "client", "the director's walk to the ATV did not arrive"); return; }
        if (st == 1) { gc.walk.reset(); Next(g); }
        return;
    }

    // The host rides: this client drives the braked rig, and the host boards a passenger seat of it.
    case C::WaitDriveCue:
        if (Seen("atvpass drive-by-client go")) { gc.sit = atv_seat::SitTry{}; Next(g); }
        else if (Timed(g, kBoundMs)) Fail(g, "client", "the host never cued the drive by the client");
        return;
    case C::SitDriver:
        if (!AtvOf(gc.key, &gc.atv)) { Fail(g, "client", "the ATV is gone"); return; }
        if (!atv_seat::TrySit(player, gc.atv, gc.sit)) { if (Timed(g, kShortMs * 3)) Fail(g, "client", "the client's sit as the driver never finished"); return; }
        if (coop::atv_sync::ActorSeatedBy(coop::players::Registry::Get().LocalPeerId()) != gc.atv) { Fail(g, "client", "the client's sit as the driver did not take"); return; }
        UE_LOGI("%s client: sat as the driver", kTag);
        DC::Say("atvpass client-sat");
        Next(g);
        return;
    case C::WaitHostBoard: {
        bool attached = false;
        if (Seen("atvpass host-boarded")) {
            const int seat = coop::atv_passenger::SeatOfSlot(gc.key, 0, &attached);
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(0);
            if (seat == 0 && attached && rp && rp->valid() && coop::puppet_seat::HoldsBody(0)) {
                UE_LOGI("%s client: the host's puppet sits at seat 0 on this machine's copy of the rig", kTag);
                DC::Say("atvpass client-sees-host-rider");
                Next(g);
                return;
            }
            if (Timed(g, kShortMs * 3)) Fail(g, "client", "the host's board: table seat " + std::to_string(seat) + ", its puppet attached " + std::to_string(attached ? 1 : 0));
        } else if (Timed(g, kBoundMs)) {
            Fail(g, "client", "the host never boarded");
        }
        return;
    }
    case C::WaitHostLeft: {
        if (!Seen("atvpass host-left")) { if (Timed(g, kBoundMs)) Fail(g, "client", "the host never left its seat"); return; }
        if (!g.flag) {
            bool attached = true;
            if (coop::atv_passenger::SeatOfSlot(gc.key, 0, &attached) != -1 || attached) { if (Timed(g, kShortMs)) Fail(g, "client", "the host's leave did not clear its seat on this machine's table or puppet"); return; }
            UE_LOGI("%s client: the host's seat is free and its puppet follows the pose stream again; getting out of the driver's seat", kTag);
            g.flag = true;
            if (!A::Dismount(gc.atv)) Fail(g, "client", "the ATV's own dismount did not run");
            return;
        }
        if (coop::atv_sync::ActorSeatedBy(coop::players::Registry::Get().LocalPeerId()) == gc.atv) { if (Timed(g, kShortMs)) Fail(g, "client", "the client is still the driver"); return; }
        DC::Say("atvpass client-undrove");
        Next(g);
        return;
    }

    case C::WaitHostSat:
    case C::WaitSit2:
        if (!AtvOf(gc.key, &gc.atv)) { Fail(g, "client", "the ATV is gone"); return; }
        if (Seen(static_cast<C>(g.step) == C::WaitHostSat ? "atvpass host-sat" : "atvpass host-sat2") &&
            TableOf(gc.key).occupantSlot == 0) {
            gc.presses = 0;
            gc.pressAt = 0;
            if (static_cast<C>(g.step) == C::WaitSit2) {
                // The rig may have rolled after the first ride: the second boarding starts from a walk to where it is now.
                FVector root{};
                AtvOf(gc.key, &gc.atv, &root);
                gc.walk = coop::director::StartBackgroundWalk(root, kWalkReachCm, kWalkDeadlineS);
            }
            Next(g);
        } else if (Timed(g, kBoundMs)) {
            Fail(g, "client", "the host never sat in the ATV (its seat never reached this table)");
        }
        return;

    case C::Board:
    case C::Board2: {
        const bool second = static_cast<C>(g.step) == C::Board2;
        if (!AtvOf(gc.key, &gc.atv)) { Fail(g, "client", "the ATV is gone"); return; }
        int seat = -1;
        if (Seated(&seat)) {
            // The use key took the seat on this machine and the table (the answer or the stamp) agrees.
            const coop::atv_sync::TableView t = TableOf(gc.key);
            if (t.passengerSlot[seat] != coop::players::Registry::Get().LocalPeerId()) return;
            if (second ? seat > 1 : seat != 0) { Fail(g, "client", "the first board took seat " + std::to_string(seat) + ", not 0"); return; }
            E::TryGetActorLocation(player, gc.boardedAt);
            UE_LOGI("%s client: boarded seat %d through the use key (the host's table agrees)", kTag, seat);
            DC::Say(second ? "atvpass client-boarded2" : "atvpass client-boarded");
            Next(g);
            return;
        }
        if (!gc.cleared) {
            const coop::director::ClearState cs = gc.clear.Tick();
            if (cs == coop::director::ClearState::Failed) { Fail(g, "client", std::string("the client's hand could not be cleared: ") + gc.clear.Why()); return; }
            if (cs == coop::director::ClearState::Working) return;
            gc.cleared = true;
        }
        if (Now() - gc.pressAt > 3000) {
            if (gc.presses >= 3) { Fail(g, "client", "three use presses at the ATV did not board a passenger seat"); return; }
            ++gc.presses;
            gc.pressAt = Now();
            FVector root{};
            AtvOf(gc.key, &gc.atv, &root);
            E::SetControlRotation(E::GetController(player), coop::director::LookAt(E::GetCameraLocation(), root));
            UE_LOGI("%s client: the use key at the ATV (press %d)", kTag, gc.presses);
            if (!PressUse(player, gc.atv)) Fail(g, "client", "the use input event did not dispatch");
        }
        return;
    }

    case C::Armed:
        gc.maxErr = 0.f;
        DC::Say("atvpass client-armed");
        Next(g);
        return;

    case C::Ride: {
        FVector seatAt{}, at{};
        int seat = -1;
        if (!Seated(&seat)) { Fail(g, "client", "the rider lost its seat during the ride"); return; }
        if (AP::SeatWorldLocation(gc.atv, seat, seatAt) && E::TryGetActorLocation(player, at)) {
            const float err = Dist(at, seatAt);
            if (err > gc.maxErr) gc.maxErr = err;
            gc.moved = Dist(at, gc.boardedAt);
        }
        std::string rest;
        if (Seen("atvpass driven") && DC::SeenRestOf("atvpass driven ", &rest)) {
            const float driven = static_cast<float>(std::atof(rest.c_str()));
            UE_LOGI("%s client: the host drove %.0f cm; this body moved %.0f cm and stayed within %.0f cm of its seat (tolerance %.0f)",
                    kTag, driven, gc.moved, gc.maxErr, kRideTolCm);
            if (gc.maxErr > kRideTolCm) { Fail(g, "client", "the body strayed " + Num(gc.maxErr) + " cm from its seat during the ride"); return; }
            if (gc.moved < driven * kFollowFrac) { Fail(g, "client", "the body moved " + Num(gc.moved) + " cm of the " + Num(driven) + " the rig was driven"); return; }
            DC::Say("atvpass client-rode " + Num(gc.maxErr));
            Next(g);
        } else if (Timed(g, kBoundMs)) {
            Fail(g, "client", "the host never finished its drive");
        }
        return;
    }

    case C::Stay:
        if (!Seen("atvpass driver-left")) { if (Timed(g, kBoundMs)) Fail(g, "client", "the driver never left"); return; }
        if (!g.flag) { g.flag = true; g.flagAt = Now(); return; }
        if (Now() - g.flagAt < 2500) {
            if (!Seated()) Fail(g, "client", "the driver leaving ejected this passenger");
            return;
        }
        if (TableOf(gc.key).passengerSlot[0] != coop::players::Registry::Get().LocalPeerId()) { Fail(g, "client", "the table no longer seats this passenger"); return; }
        UE_LOGI("%s client: still seated two seconds after the driver left", kTag);
        DC::Say("atvpass client-stayed");
        Next(g);
        return;

    case C::Leave:
    case C::Leave2: {
        const bool second = static_cast<C>(g.step) == C::Leave2;
        if (!g.flag) {
            g.flag = true;
            g.flagAt = Now();
            gc.leave = LeaveCheck{};
            if (second) {
                // The second leave is a teleport of the body, 150 cm up from where it sits, through the teleports' one entry.
                ue_wrap::FVector at{};
                ue_wrap::FRotator rot{};
                E::TryGetActorLocation(player, at);
                E::TryGetActorRotation(player, rot);
                UE_LOGI("%s client: a teleport while seated", kTag);
                coop::teleport_client::ApplyLocally({at.X, at.Y, at.Z + 150.f, rot.Pitch, rot.Yaw, rot.Roll});
            } else if (!PressUse(player, nullptr)) {
                Fail(g, "client", "the use input event did not dispatch for the leave");
            }
            return;
        }
        if (Seated() || coop::atv_passenger::LocalIsPassenger()) { if (Timed(g, kShortMs)) Fail(g, "client", second ? "the teleport did not take the body off its seat" : "the use key did not leave the seat"); return; }
        if (second) {
            // Off the seat on this machine, and the host's table agrees (the host's arm judges its side); the body lifted and landed on a floor.
            if (Now() - g.flagAt < 2500) return;
            FVector root{}, at{};
            FRotator rot{};
            E::FloorRead fr;
            const bool floor = E::ReadCurrentFloor(player, &fr) && fr.blocking && fr.walkable;
            if (!A::GetRootTransform(gc.atv, root, rot) || !E::TryGetActorLocation(player, at)) { Fail(g, "client", "the ATV or the body does not read after the a teleport"); return; }
            if (AP::IsRiderOf(player, gc.atv) || !floor || HorizDist(at, root) > 500.f) {
                Fail(g, "client", "after the teleport the body is attached " + std::to_string(AP::IsRiderOf(player, gc.atv) ? 1 : 0) + ", on a floor " + std::to_string(floor ? 1 : 0) + ", " + Num(HorizDist(at, root)) + " cm from the ATV");
                return;
            }
            UE_LOGI("%s client: the teleport left the seat and the body stands on a floor %.0f cm from the ATV", kTag, HorizDist(at, root));
            DC::Say("atvpass client-left2");
            Next(g);
            return;
        }
        const int r = CheckLeft(gc.leave, player, gc.atv);
        if (r < 0) { Fail(g, "client", gc.leave.why); return; }
        if (r == 0) return;
        UE_LOGI("%s client: left through the use key and stood %.0f cm from the ATV, %.0f cm above its root, on a walkable floor (floor at leave %d)", kTag,
                gc.leave.horiz, gc.leave.up, gc.leave.floorAtLeave ? 1 : 0);
        DC::Say(second ? "atvpass client-left2" : "atvpass client-left " + Num(gc.leave.horiz));
        Next(g);
        return;
    }

    case C::DupGo:
        if (Seen("atvpass red-dup go")) {
            if (!Seated() || !SendRaw(*s, true, kRawSeqBase)) { Fail(g, "client", "the duplicate board could not be sent from a seated body"); return; }
            UE_LOGI("%s client: sent a duplicate board while seated", kTag);
            DC::Say("atvpass raw dup");
            Next(g);
        } else if (Timed(g, kBoundMs)) {
            Fail(g, "client", "the host never cued the duplicate board");
        }
        return;
    case C::DupSent:
        if (Seen("atvpass red-leave go")) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "client", "the host never judged the duplicate board");
        return;
    case C::Leave2Go:
        g.flag = false;
        Next(g);
        return;
    case C::FullGo:
        if (Seen("atvpass red-full go")) {
            if (Seated() || !SendRaw(*s, true, kRawSeqBase + 1)) { Fail(g, "client", "the board for the full arm could not be sent"); return; }
            UE_LOGI("%s client: sent a board with every seat taken", kTag);
            DC::Say("atvpass raw full");
            Next(g);
        } else if (Timed(g, kBoundMs)) {
            Fail(g, "client", "the host never cued the full arm");
        }
        return;
    case C::FullSent:
        if (Seen("atvpass red-reach go")) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "client", "the host never moved its ATV");
        return;
    case C::ReachGo:
        if (Seated() || !SendRaw(*s, true, kRawSeqBase + 2)) { Fail(g, "client", "the board for the reach arm could not be sent"); return; }
        UE_LOGI("%s client: sent a board while the ATV is %.0f cm away", kTag, kAwayCm);
        DC::Say("atvpass raw reach");
        Next(g);
        return;
    case C::ReachSent:
        if (Seen("atvpass done")) Next(g);
        else if (Timed(g, kBoundMs)) Fail(g, "client", "the host never finished");
        return;
    case C::Finish:
        Pass(g, "client", "boarded through the use key twice, rode within tolerance, stayed when the driver left, left beside the ATV, saw the host ride its rig and left a seat by a teleport");
        return;
    }
}

bool ClientDone() { return gc.g.done; }

void ClientReset() {
    WalkCancel();
    gc = Client{};
}

}  // namespace detail

void Tick(coop::net::Session* session) {
    if (!detail::Enabled() || !session || !session->running()) return;
    if (session->role() == coop::net::Role::Host) {
        if (!detail::HostDone()) detail::HostTick(session);
    } else if (!detail::ClientDone()) {
        detail::ClientTick(session);
    }
}

void OnDisconnect() {
    detail::ClientReset();
    detail::HostReset();
}

}  // namespace coop::dev::atv_passenger_drill

// harness/autotest/autotest_driveslot_host.cpp -- the host's half of the drive-slot test (see autotest_driveslot.cpp):
// it empties the play slot, births a drive carrying the seeded signal, seats it, waits for the client's take and judges
// its own copy and the act lane's counters.

#include "harness/autotest/autotest_driveslot_detail.h"  // shared by the drill's two files

namespace harness::autotest::driveslot_detail {
namespace {

// The host's copy of the drive as the wait sees it, logged whenever it changes: where it is, what it is attached to,
// whether it simulates and is frozen, and whether the slot still holds it. A drive that breaks or vanishes leaves the
// last state it was seen in.
void TraceDrive(const Subject& sb, std::string& last) {
    char buf[300];
    const bool live = R::IsLiveByIndex(sb.drive, sb.driveIdx);
    const bool empty = DC::SlotDrive(sb.slot) == nullptr;
    if (!live) {
        std::snprintf(buf, sizeof(buf), "slotEmpty=%d live=0", empty ? 1 : 0);
    } else {
        void* parent = nullptr;
        E::GetAttachParentActor(sb.drive, parent);
        ue_wrap::FVector at{};
        const bool read = E::TryGetActorLocation(sb.drive, at);
        std::snprintf(buf, sizeof(buf), "slotEmpty=%d live=1 attachedTo=%ls sim=%d frozen=%d at=(%.0f,%.0f,%.0f)%s",
                      empty ? 1 : 0, parent ? R::ClassNameOf(parent).c_str() : L"none",
                      E::IsActorRootSimulatingPhysics(sb.drive) ? 1 : 0, ue_wrap::prop::IsFrozen(sb.drive) ? 1 : 0,
                      at.X, at.Y, at.Z, read ? "" : " (unread)");
    }
    if (last == buf) return;
    last = buf;
    UE_LOGI("driveslot: HOST trace eid=%u %s", sb.eid, buf);
}

// WaitFor on the host, sampling every 30 ms and tracing the drive, so the moments around the client's press show.
template <class Pred>
bool WaitForTraced(const Subject& sb, int ms, Pred pred) {
    std::string last;
    for (int t = 0; t < ms; t += 17) {
        bool ok = false;
        RunGT([&](std::atomic<int>& d) {
            ok = pred();
            TraceDrive(sb, last);
            d.store(1);
        });
        if (ok) return true;
        ::Sleep(1);
    }
    return false;
}

}  // namespace

void RunHost(Arm arm) {
    const char* const an = ArmName(arm);
    if (!WaitForPeerWorld("HOST")) return;
    auto sb = std::make_shared<Subject>();
    if (RunGT([sb, arm](std::atomic<int>& d) {
            if (!DC::EnsureResolved()) { UE_LOGW("driveslot: drive chain unresolved"); d.store(2); return; }
            sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
            if (!sb->slot) { UE_LOGW("driveslot: no desk play slot"); d.store(2); return; }
            // A save that already has a drive in the port would otherwise end the run here, and
            // worse, the client's wait for "a frozen drive in the port" would be answered by THAT
            // drive rather than by the insert under test. So the port is emptied first, the way a
            // player empties it -- the drive's own eject, then the grab's unfreeze -- and the drive
            // is left where it lies.
            void* me = coop::players::Registry::Get().Local();
            if (!me) { UE_LOGW("driveslot: no local player"); d.store(2); return; }
            ue_wrap::FVector at{};   // read before the slot is cleared: a host it fails on changes nothing
            if (!E::TryGetActorLocation(me, at)) {
                UE_LOGW("driveslot: the host's location could not be read, so the drive has nowhere to spawn");
                d.store(2);
                return;
            }
            if (void* sitting = DC::SlotDrive(sb->slot)) {
                CallWithPlayer(sitting, L"playerTryToGrab", me);
                CallWithPlayer(sitting, L"playerGrabbed_pre", me);
                UE_LOGI("driveslot: HOST cleared a drive the save left in the play slot (empty=%d)",
                        DC::SlotDrive(sb->slot) == nullptr ? 1 : 0);
                if (DC::SlotDrive(sb->slot)) {
                    UE_LOGW("driveslot: the play slot is taken and did not clear"); d.store(2); return;
                }
            }
            // Born at the host's feet, away from the port: a drive born in the port's reach is taken
            // by the slot's own overlap on every peer, before the insert under test.
            at.Z += 100.f;
            void* drive = E::BeginDeferredSpawn(DC::DriveClass(), at, ue_wrap::FRotator{});
            if (!drive || !E::FinishDeferredSpawn(drive, at, ue_wrap::FRotator{})) {
                UE_LOGW("driveslot: the drive did not spawn"); d.store(2); return;
            }
            // The signal, written as a game writer does (row, then the drive's own upd), before the drive is enrolled:
            // the row lane sends a drive's row at its enrolment when it is not the class default's.
            if (!DC::WriteDriveRow(drive, SeedRow(arm)) || !DC::CallDriveUpd(drive)) {
                UE_LOGW("driveslot: the seeded row could not be written into the drive"); d.store(2); return;
            }
            sb->drive = drive;
            sb->driveIdx = R::InternalIndexOf(drive);
            d.store(1);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- could not set up the drive", an);
        return;
    }
    // The fresh drive has to be an ENTITY before it goes in: a slot line names its drive by element
    // id, and one sent for a drive that has not been enrolled yet names nothing the far peer can
    // ever resolve. Enrolment happens on the tracker's own pass, a frame or two behind the spawn.
    if (!WaitFor(15000, [sb] {
            const uint32_t e = EidOf(sb->drive);
            return e != 0u && e != static_cast<uint32_t>(coop::element::kInvalidId);
        })) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- the fresh drive never took an element id", an);
        return;
    }
    // Straight into the port: an insert that overtakes its drive's birth broadcast is parked by the
    // receiver (a line held in bind_wait until the drive binds), so no wait is owed here.
    const bool portRead = RunGT([sb](std::atomic<int>& d) {
        DC::CallPutDriveIn(sb->slot, sb->drive);
        sb->eid = EidOf(sb->drive);
        const bool read = E::TryGetActorLocation(sb->drive, sb->port);
        sb->pre = LiveDrives();
        UE_LOGI("driveslot: HOST inserted eid=%u in the play slot at (%.0f,%.0f,%.0f)%s frozen=%d",
                sb->eid, sb->port.X, sb->port.Y, sb->port.Z, read ? "" : " (unread)",
                ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0);
        d.store(read ? 1 : 2);
    }) == 1;
    if (!portRead) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- the port's location could not be read, so no carry "
                "off it can be measured", an);
        return;
    }

    // The act lane's counters at the start, for the line that says what the host answered.
    const uint32_t grants0 = PAH::AnsweredFor(1, coop::net::PropActReason::Granted);
    const uint32_t destroys0 = PAH::CommitsOf(1, PAH::internal::CommitStep::Destroy);
    uint32_t tally0[static_cast<int>(PAH::Tally::kCount)] = {};
    for (int t = 0; t < static_cast<int>(PAH::Tally::kCount); ++t) tally0[t] = PAH::TallyOf(static_cast<PAH::Tally>(t));

    const bool out = WaitForTraced(*sb, 90000, [sb] { return DC::SlotDrive(sb->slot) == nullptr; });
    if (!out) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- the client's eject never reached the host (slot still holds the drive)", an);
        return;
    }
    UE_LOGI("driveslot: HOST's slot is empty -- waiting for the %hs to land", an);

    auto followed = std::make_shared<float>(0.f);   // grab: the furthest the copy moved from where the first pose put it
    if (arm == Arm::Grab) {
        // The evidence, not a clock: the copy here is unfrozen and has left the port under the client's poses, and
        // then goes on following the carry (it moves a further kFollowCm from where the first pose put it) with the
        // drive still standing -- a copy that breaks on its way is the failure this wait is for. It ends the moment
        // both hold, and the readout below reports whatever is true when the wait returns, pass or timeout.
        auto first = std::make_shared<ue_wrap::FVector>();
        auto haveFirst = std::make_shared<bool>(false);
        WaitForTraced(*sb, 40000, [sb, first, haveFirst, followed] {
            if (!R::IsLiveByIndex(sb->drive, sb->driveIdx)) return true;   // gone: the readout says so
            if (ue_wrap::prop::IsFrozen(sb->drive)) return false;
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(sb->drive, at)) return false;
            if (!*haveFirst) {
                if (Distance(at, sb->port) < kOffPortCm) return false;
                *first = at;
                *haveFirst = true;
                return false;
            }
            *followed = std::fmax(*followed, Distance(at, *first));
            return *followed >= kFollowCm;
        });
    } else {
        // This peer's copy destroyed by the grant's commit, and for a hold the client's hand shown here.
        WaitForTraced(*sb, 30000, [sb, arm] {
            if (R::IsLiveByIndex(sb->drive, sb->driveIdx)) return false;
            return arm != Arm::Hold || (HI::HeldClassIs(1, kDriveLeaf) && HI::MirrorActorForSlot(1) != nullptr);
        });
        ::Sleep(3000);   // a stray, or a second copy, would show by now
    }

    RunGT([sb, arm, an, grants0, destroys0, tally0, followed](std::atomic<int>& d) {
        Checks ck;
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        ck.Need(empty, "this slot still holds a drive");
        const uint32_t grants = PAH::AnsweredFor(1, coop::net::PropActReason::Granted) - grants0;
        const uint32_t destroys = PAH::CommitsOf(1, PAH::internal::CommitStep::Destroy) - destroys0;
        std::string tallies;
        for (int t = 0; t < static_cast<int>(PAH::Tally::kCount); ++t) {
            const uint32_t n = PAH::TallyOf(static_cast<PAH::Tally>(t)) - tally0[t];
            if (n) tallies += std::string(" ") + PAH::TallyName(static_cast<PAH::Tally>(t)) + "=" + std::to_string(n);
        }
        char tail[360];
        std::snprintf(tail, sizeof(tail), "act lane: grants=%u destroy-commits=%u tallies:%s", grants, destroys,
                      tallies.empty() ? " none" : tallies.c_str());
        if (arm == Arm::Grab) {
            const bool frozen = live && ue_wrap::prop::IsFrozen(sb->drive);
            ue_wrap::FVector at{};
            const bool atRead = live && E::TryGetActorLocation(sb->drive, at);
            const float moved = atRead ? Distance(at, sb->port) : 0.f;
            ck.Need(live, "the drive was destroyed (a grab keeps the actor)");
            ck.Need(!frozen, "the drive is still frozen");
            ck.Need(atRead && moved >= kOffPortCm, "the drive did not follow the carry off the port");
            ck.Need(*followed >= kFollowCm, "the drive did not keep following the carry");
            UE_LOGI("driveslot: VERDICT host grab %s -- eid=%u live=%d slotEmpty=%d frozen=%d at (%.0f,%.0f,%.0f)%s, "
                    "%.0f cm off the port (needs %.0f), followed the carry %.0f cm (needs %.0f)%s%s; %s",
                    ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, frozen ? 1 : 0, at.X, at.Y,
                    at.Z, (live && !atRead) ? " (unread)" : "", moved, kOffPortCm, static_cast<double>(*followed),
                    kFollowCm, ck.Pass() ? "" : " FAILED: ",
                    ck.failed.c_str(), tail);
            d.store(1);
            return;
        }
        const Census cen = TakeCensus(arm, sb->slot, sb->pre, sb->port);
        ck.Need(!live, "the host's copy of the drive is still standing");
        ck.Need(cen.signalWorld == 0, "a world drive with the signal is left on the host");
        ck.Need(cen.newNearPort == 0, "a drive that was not here before stands at the port");
        void* const me = coop::players::Registry::Get().Local();
        ck.Need(!MainHandDrive(me), "the host's own hand holds a drive");
        const bool puppetDrive = HI::HeldClassIs(1, kDriveLeaf);
        void* const mirror = HI::MirrorActorForSlot(1);
        const bool mirrorDrive = mirror && R::IsLive(mirror) && DC::IsDriveClass(R::ClassOf(mirror));
        if (arm == Arm::Hold) {
            ck.Need(puppetDrive, "slot 1's hand state is not a drive");
            ck.Need(mirrorDrive, "the host's puppet for slot 1 shows no drive mirror");
        } else {
            ck.Need(!puppetDrive, "slot 1's hand shows a drive after a pocket");
        }
        UE_LOGI("driveslot: VERDICT host %hs %s -- eid=%u copyLive=%d slotEmpty=%d worldSignal=%d newNearPort=%d%s "
                "puppetHand=%d mirror=%d hostHandDrive=%d%s%s; %s",
                an, ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, cen.signalWorld,
                cen.newNearPort, cen.detail.c_str(), puppetDrive ? 1 : 0, mirrorDrive ? 1 : 0,
                MainHandDrive(me) ? 1 : 0, ck.Pass() ? "" : " FAILED: ", ck.failed.c_str(), tail);
        d.store(1);
    });
}

}  // namespace harness::autotest::driveslot_detail

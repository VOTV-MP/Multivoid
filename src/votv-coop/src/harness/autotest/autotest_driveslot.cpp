// harness/autotest/autotest_driveslot.cpp -- a drive taken out of the desk's play slot by one peer, three ways
// (VOTVCOOP_RUN_DRIVESLOT=grab|hold|pocket on both peers; "1" is grab): the entry points (harness/autotest/driveslot.h)
// and the client's half; the host's is autotest_driveslot_host.cpp.
// What a player does with a drive in the play slot, from the 0.10 bytecode (capture-0.10.0-b284.txt):
//   grab    prop_drive_C.playerTryToGrab only calls slot.drivePulledOut and keeps the actor: a physics grab, the drive
//           carried on the handle and dropped where the player lets go.
//   hold    playerTryToHold -> driveAction(player, collect=false): pulls the drive out and then calls player.Add Object to
//           Free Hotbar Slot(Self, ...), which DESTROYS the world actor on success; the hand's actor is respawned from the
//           slot's own data.
//   pocket  playerTryToCollect -> driveAction(collect=true), then addObjectToInventory: the same, into the bag.
// In every arm the HOST births a drive, writes a recognisable signal into its data_0 and seats it in the play slot. The
// CLIENT, once the drive is in its slot with the signal on it, drives the action as a player's press does; hold and pocket
// go through the act lane (coop/props/prop_act_request): the client asks the host, the host grants, the client replays
// under the latch and commits, the host destroys its copy. Each peer judges its own copy and says one "driveslot: VERDICT
// <host|client> <arm> PASS|FAIL" line.

#include "harness/autotest/driveslot.h"

#include "harness/autotest/autotest_driveslot_detail.h"  // shared by the drill's two files

namespace harness::autotest {

bool DriveSlotArmed() {
    const std::string v = coop::config::ReadEnv("VOTVCOOP_RUN_DRIVESLOT");
    return v == "1" || v == "grab" || v == "hold" || v == "pocket";
}

namespace driveslot_detail {
namespace {

// ---- the client ----------------------------------------------------------------------------------------------------

// Where the carry goes. The host's copy follows the client's poses as a body that is moved and cannot be pushed back,
// so whatever lies in its way takes an impact the game reads as damage: a drive has little health, and the desk's
// surroundings on the rig save hold a pile of loose drives, one of which broke the carried drive on its first pose
// (impulse 14000 against 20 for a drive at rest). The hand and the far end of the carry are therefore picked, among
// eight directions round the player, as far from every other drive as can be found.
void ChooseCarry(const ue_wrap::FVector& player, const ue_wrap::FVector& fwd, void* subject, ue_wrap::FVector& hand,
                 ue_wrap::FVector& side) {
    std::vector<ue_wrap::FVector> others;
    for (void* d : LiveDrives()) {
        ue_wrap::FVector at{};
        if (d != subject && E::TryGetActorLocation(d, at)) others.push_back(at);
    }
    float best = -1.f;
    for (int k = 0; k < 8; ++k) {
        const float a = static_cast<float>(k) * 0.7853982f;
        const float c = std::cos(a), s = std::sin(a);
        const ue_wrap::FVector dir{fwd.X * c - fwd.Y * s, fwd.X * s + fwd.Y * c, 0.f};
        const ue_wrap::FVector h{player.X + dir.X * 60.f, player.Y + dir.Y * 60.f, player.Z + 80.f};
        const ue_wrap::FVector sd{-dir.Y, dir.X, 0.f};
        float clear = 1e9f;
        for (const auto& o : others)
            for (int step = 0; step <= 3; ++step) {
                const float t = kCarryAsideCm * static_cast<float>(step) / 3.f;
                const ue_wrap::FVector at{h.X + sd.X * t, h.Y + sd.Y * t, h.Z};
                clear = std::fmin(clear, Distance(at, o));
            }
        if (clear > best) {
            best = clear;
            hand = h;
            side = sd;
        }
    }
    UE_LOGI("driveslot: CLIENT carry placed %.0f cm from the nearest other drive (%zu others in the world)", best,
            others.size());
}

void RunClientGrab(Arm arm, const std::shared_ptr<Subject>& sb) {
    struct Grab {
        void* player = nullptr;
        void* handle = nullptr;
        void* mesh = nullptr;
        void* setTargetFn = nullptr;
        void* releaseFn = nullptr;
        ue_wrap::FVector hand{}, side{};
    };
    auto g = std::make_shared<Grab>();
    if (RunGT([sb, g](std::atomic<int>& d) {
            g->player = coop::players::Registry::Get().Local();
            if (!g->player) { UE_LOGW("driveslot: no local player"); d.store(2); return; }
            // Where the hand goes, read before the grab starts: a take that cannot place the drive leaves it in the
            // slot.
            ue_wrap::FVector p{};
            if (!E::TryGetActorLocation(g->player, p)) {
                UE_LOGW("driveslot: the player's location could not be read"); d.store(2); return; }
            // The game's grab of a slotted drive: the drive ejects itself, then the grab unfreezes it.
            const bool took = CallWithPlayer(sb->drive, L"playerTryToGrab", g->player);
            const bool pre = CallWithPlayer(sb->drive, L"playerGrabbed_pre", g->player);
            g->handle = E::ReadMainPlayerGrabHandle(g->player);
            g->mesh = ue_wrap::prop::GetStaticMesh(sb->drive);
            void* phc = R::FindClass(P::name::PhysicsHandleComponentClass);
            void* grabFn = phc ? R::FindFunction(phc, P::name::GrabComponentAtLocationFn) : nullptr;
            g->setTargetFn = phc ? R::FindFunction(phc, P::name::SetTargetLocationFn) : nullptr;
            g->releaseFn = phc ? R::FindFunction(phc, P::name::ReleaseComponentFn) : nullptr;
            if (!g->handle || !g->mesh || !grabFn || !g->setTargetFn || !g->releaseFn) {
                UE_LOGW("driveslot: the physics handle did not resolve"); d.store(2); return;
            }
            const ue_wrap::FVector fwd = E::GetActorForwardVector(g->player);
            ChooseCarry(p, fwd, sb->drive, g->hand, g->side);
            E::SetActorLocation(sb->drive, g->hand);
            ue_wrap::ParamFrame f(grabFn);
            f.Set<void*>(L"Component", g->mesh);
            f.Set<ue_wrap::FVector>(L"GrabLocation", g->hand);
            const bool grabbed = f.valid() && ue_wrap::Call(g->handle, f);
            E::WriteMainPlayerGrabbingPair(g->player, sb->drive, g->mesh);
            UE_LOGI("driveslot: CLIENT took eid=%u out (playerTryToGrab=%d playerGrabbed_pre=%d "
                    "grab=%d) slotEmpty=%d frozen=%d", sb->eid, took ? 1 : 0, pre ? 1 : 0,
                    grabbed ? 1 : 0, DC::SlotDrive(sb->slot) == nullptr ? 1 : 0,
                    ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0);
            d.store(1);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT client grab FAIL -- could not take the drive");
        return;
    }

    // Carry it aside over three seconds, then let go.
    for (int i = 1; i <= 12; ++i) {
        ::Sleep(250);
        const float k = kCarryAsideCm * static_cast<float>(i) / 12.f;
        RunGT([g, k](std::atomic<int>& d) {
            ue_wrap::ParamFrame f(g->setTargetFn);
            f.Set<ue_wrap::FVector>(L"NewLocation", ue_wrap::FVector{g->hand.X + g->side.X * k,
                                                                      g->hand.Y + g->side.Y * k,
                                                                      g->hand.Z});
            if (f.valid()) ue_wrap::Call(g->handle, f);
            d.store(1);
        });
    }
    RunGT([g](std::atomic<int>& d) {
        ue_wrap::ParamFrame f(g->releaseFn);
        if (f.valid()) ue_wrap::Call(g->handle, f);
        E::WriteMainPlayerGrabbingPair(g->player, nullptr, nullptr);
        d.store(1);
    });
    // Let it come to rest, measured rather than waited out: two samples a quarter second apart
    // within a centimetre of each other is the drop settling. The readout follows either way.
    ue_wrap::FVector last{};
    WaitFor(15000, [sb, &last] {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(sb->drive, at)) return false;   // unread: not a settled sample
        const bool still = Distance(at, last) < 1.f;
        last = at;
        return still;
    });
    RunGT([sb, arm](std::atomic<int>& d) {
        Checks ck;
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        const bool frozen = live && ue_wrap::prop::IsFrozen(sb->drive);
        ue_wrap::FVector at{};
        const bool atRead = live && E::TryGetActorLocation(sb->drive, at);
        const float moved = atRead ? Distance(at, sb->port) : -1.f;
        ck.Need(empty, "this slot still holds a drive");
        ck.Need(live, "the drive was destroyed (a grab keeps the actor)");
        ck.Need(!frozen, "the drive is still frozen");
        ck.Need(atRead && moved >= kOffPortCm, "the drive is not off the port");
        UE_LOGI("driveslot: VERDICT client %hs %s -- eid=%u live=%d slotEmpty=%d frozen=%d at (%.0f,%.0f,%.0f)%s, "
                "%.0f cm off the port (needs %.0f)%s%s",
                ArmName(arm), ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, frozen ? 1 : 0,
                at.X, at.Y, at.Z, (live && !atRead) ? " (unread)" : "", moved, kOffPortCm,
                ck.Pass() ? "" : " FAILED: ", ck.failed.c_str());
        d.store(1);
    });
}

// The hold and the pocket: the player's own press verb on the drive, whose hotbar / inventory call the act watch asks
// the host about. The client's hand or bag then carries the drive, respawned or recorded from the slot's data.
void RunClientTake(Arm arm, const std::shared_ptr<Subject>& sb) {
    const char* const an = ArmName(arm);
    struct Base { uint32_t refused = 0; bool opened = false; bool inUse = false; };
    auto base = std::make_shared<Base>();
    auto refusedTotal = [] {
        uint32_t n = 0;
        for (uint8_t r = 1; r < coop::net::kPropActReasons; ++r) n += PAR::dev::RefusedCount(static_cast<coop::net::PropActReason>(r));
        return n;
    };
    if (RunGT([sb, arm, base, refusedTotal](std::atomic<int>& d) {
            void* const player = coop::players::Registry::Get().Local();
            if (!player) { d.store(2); return; }
            sb->pre = LiveDrives();
            base->refused = refusedTotal();
            // The press: the drive's own verb. Its driveAction ejects the drive from the slot here, lifts it and
            // gives it physics, then calls the player's Add Object to Free Hotbar Slot / addObjectToInventory, which
            // the act watch cancels in order to ask the host.
            const bool ran = CallWithPlayer(sb->drive, arm == Arm::Hold ? L"playerTryToHold" : L"playerTryToCollect", player);
            base->opened = PAR::dev::PendingId() != 0;
            UE_LOGI("driveslot: CLIENT pressed %hs on eid=%u (ran=%d) -- slotEmpty=%d requestOpen=%d",
                    arm == Arm::Hold ? "playerTryToHold" : "playerTryToCollect", sb->eid, ran ? 1 : 0,
                    DC::SlotDrive(sb->slot) == nullptr ? 1 : 0, base->opened ? 1 : 0);
            d.store(ran ? 1 : 2);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT client %hs FAIL -- the drive's press verb could not run", an);
        return;
    }
    // The host's answer and the replay: no request pending is the lane's own end, then the verb's result lands.
    WaitFor(20000, [] { return PAR::dev::PendingId() == 0; });
    WaitFor(20000, [sb, arm] {
        void* const player = coop::players::Registry::Get().Local();
        if (arm == Arm::Hold) {
            void* const hand = MainHandDrive(player);
            SD::Row row;
            return hand && DC::ReadDriveRow(hand, row) && RowIsSignal(row, arm);
        }
        std::vector<ue_wrap::inventory_item::Item> bag;
        return ue_wrap::inventory::ReadLiveItems(nullptr, &bag) && DriveRecordsWithSignal(bag, arm) > 0;
    });
    ::Sleep(3000);   // a second copy, a late respawn or a stray would show by now

    RunGT([sb, arm, an, base, refusedTotal](std::atomic<int>& d) {
        Checks ck;
        void* const player = coop::players::Registry::Get().Local();
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const Census cen = TakeCensus(arm, sb->slot, sb->pre, sb->port);
        const uint32_t refused = refusedTotal() - base->refused;
        ck.Need(base->opened, "the press opened no request (the watch did not ask the host)");
        ck.Need(refused == 0, "the host refused the request");
        ck.Need(empty, "this slot still holds a drive");
        ck.Need(cen.signalWorld == 0, "a world drive with the signal is left on the client");
        ck.Need(cen.newNearPort == 0, "a drive that was not here before stands at the port");
        char what[260];
        what[0] = '\0';
        if (arm == Arm::Hold) {
            void* const hand = MainHandDrive(player);
            SD::Row row;
            const bool handRead = hand && DC::ReadDriveRow(hand, row);
            const bool handSig = handRead && RowIsSignal(row, arm);
            std::vector<ue_wrap::inventory_item::Item> hotbar, bag;
            ue_wrap::inventory::ReadLiveItems(&hotbar, &bag);
            const std::wstring held = ue_wrap::inventory::ReadHeldItemName();
            ck.Need(hand != nullptr, "the main hand holds no drive");
            ck.Need(handSig, "the hand's drive does not carry the seeded signal");
            ck.Need((handSig ? 1 : 0) + cen.signalWorld == 1, "the drive with the signal is not exactly one across hand and world");
            ck.Need(DriveRecordsWithSignal(hotbar, arm) == 1, "the hotbar holds no record of the drive with its data");
            ck.Need(DriveRecords(bag) == 0 || DriveRecordsWithSignal(bag, arm) == 0, "the bag holds a second copy of the drive");
            std::snprintf(what, sizeof(what), "hand=%p row='%ls' size=%.0f holdingObjectName='%ls' hotbarRecords=%d bagCopies=%d",
                          hand, handRead ? row.name.c_str() : L"<unread>", handRead ? row.size : -1.f, held.c_str(),
                          DriveRecordsWithSignal(hotbar, arm), DriveRecordsWithSignal(bag, arm));
        } else {
            std::vector<ue_wrap::inventory_item::Item> hotbar, bag;
            const bool read = ue_wrap::inventory::ReadLiveItems(&hotbar, &bag);
            const int withSig = DriveRecordsWithSignal(bag, arm);
            ck.Need(read, "the inventory could not be read");
            ck.Need(withSig == 1, "the bag does not hold exactly one drive record carrying the signal");
            ck.Need(DriveRecordsWithSignal(hotbar, arm) == 0, "the hotbar holds a copy of the pocketed drive");
            ck.Need(!MainHandDrive(player), "the main hand holds a drive after a pocket");
            std::snprintf(what, sizeof(what), "bagDriveRecords=%d bagWithSignal=%d hotbarWithSignal=%d bagSize=%zu",
                          DriveRecords(bag), withSig, DriveRecordsWithSignal(hotbar, arm), bag.size());
        }
        UE_LOGI("driveslot: VERDICT client %hs %s -- eid=%u mirrorLive=%d slotEmpty=%d requestOpened=%d refused=%u "
                "worldSignal=%d newNearPort=%d%s %s%s%s",
                an, ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, base->opened ? 1 : 0, refused,
                cen.signalWorld, cen.newNearPort, cen.detail.c_str(), what, ck.Pass() ? "" : " FAILED: ",
                ck.failed.c_str());
        d.store(1);
    });
}

}  // namespace

void RunClient(Arm arm) {
    const char* const an = ArmName(arm);
    if (!WaitForPeerWorld("CLIENT")) return;
    // A drive carrying the seeded signal, frozen in the play slot. The signal and not "a different drive", because
    // the host empties the port and inserts a fresh drive, and the lane carries idempotent slot STATE rather than
    // edges: the two moves land here as one line naming the new drive, so a peer watching for the port to go empty
    // would wait forever. Frozen, because the slot freezes what it takes, and without that the take below would have
    // nothing to undo.
    auto sb = std::make_shared<Subject>();
    UE_LOGI("driveslot: CLIENT -- waiting for the seeded drive '%ls' frozen in the play slot", SignalName(arm).c_str());
    const bool in = WaitFor(180000, [sb, arm] {
        if (!DC::EnsureResolved()) return false;
        if (!sb->slot) sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
        sb->drive = sb->slot ? DC::SlotDrive(sb->slot) : nullptr;
        if (!sb->drive || !ue_wrap::prop::IsFrozen(sb->drive)) return false;
        SD::Row row;
        return DC::ReadDriveRow(sb->drive, row) && RowIsSignal(row, arm);
    });
    if (!in) {
        std::string why = "no drive reached the play slot";
        RunGT([sb, &why](std::atomic<int>& d) {
            void* const dr = sb->slot ? DC::SlotDrive(sb->slot) : nullptr;
            SD::Row row;
            if (dr) {
                why = DC::ReadDriveRow(dr, row) ? "a drive reached the play slot but its row is not the seeded signal (name '" +
                                                       NarrowAscii(row.name) + "')"
                                                 : "a drive reached the play slot, its row unreadable";
                if (!ue_wrap::prop::IsFrozen(dr)) why += ", not frozen";
            }
            d.store(1);
        });
        UE_LOGW("driveslot: VERDICT client %hs FAIL -- %hs", an, why.c_str());
        return;
    }

    // The player first stands at the port, as a player taking a disc out does. On 0.10 the save's spawn is far from
    // the desk (measured 5.5 km), and a hand carried there in one frame would drag the drive that far in one frame: the
    // host's copy then follows at 1.8e5 cm/s and the game destroys it on impact, which reads as a lost eject. The
    // host's reach test for a hold or a pocket reads this standing too.
    if (RunGT([sb](std::atomic<int>& d) {
            void* const me = coop::players::Registry::Get().Local();
            ue_wrap::FVector port{};
            if (!me || !sb->drive || !E::TryGetActorLocation(sb->drive, port)) { d.store(2); return; }
            sb->port = port;
            sb->eid = EidOf(sb->drive);
            sb->driveIdx = R::InternalIndexOf(sb->drive);
            E::SetActorLocation(me, ue_wrap::FVector{port.X + 150.f, port.Y, port.Z - 60.f});
            d.store(1);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT client %hs FAIL -- could not stand the player at the port", an);
        return;
    }
    ::Sleep(3000);   // the puppet's pose reaches the host before the action
    if (arm == Arm::Grab) {
        RunGT([sb](std::atomic<int>& d) { sb->pre = LiveDrives(); d.store(1); });
        RunClientGrab(arm, sb);
    } else {
        RunClientTake(arm, sb);
    }
}

}  // namespace driveslot_detail

void RunDriveSlotTest() {
    using namespace driveslot_detail;
    const Arm arm = ArmFromEnv();
    UE_LOGI("driveslot: arm=%hs role=%hs", ArmName(arm), IsClientRole() ? "client" : "host");
    if (IsClientRole()) RunClient(arm);
    else RunHost(arm);
    UE_LOGI("driveslot: done");
}

DWORD WINAPI DriveSlotTestThread(LPVOID) {
    RunDriveSlotTest();
    return 0;
}

}  // namespace harness::autotest

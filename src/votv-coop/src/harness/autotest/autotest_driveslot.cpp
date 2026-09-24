// harness/autotest/autotest_driveslot.cpp -- a drive taken out of the desk's play slot by one peer
// is out, and in that peer's hand, on the other (VOTVCOOP_RUN_DRIVESLOT=1 on both peers). The
// entry points are in harness/autotest/driveslot.h.
//
// The host births a drive and puts it in the play slot. The client, once the insert has reached
// it, takes it out as a player's grab does -- the drive's playerTryToGrab, which ejects it from the
// slot, then playerGrabbed_pre, which unfreezes it, then the physics handle -- carries it a metre
// and a half aside and lets go. The HOST's verdict is the test: its slot empty, its copy of the
// drive not frozen, and carried off the port by the client's poses. A slot freezes the drive it
// takes, so an eject that crosses without the grab's unfreeze leaves the host's copy frozen in the
// port, where the prop lane drops every pose of the carry.
//
// Every wait here is on a state one of the peers reaches: the client's world coming up, the drive
// standing frozen in the port, the far copy leaving it, the dropped drive coming to rest. The
// carry itself is the one timed thing, because a carry is a movement over time.

#include "harness/autotest/driveslot.h"

#include "harness/autotest.h"  // IsClientRole

#include "coop/element/registry.h"
#include "coop/net/session.h"                // IsSlotWorldReady: the host's half of the shared origin
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"      // the client's join being over, not just announced
#include "coop/session/net_pump.h"           // HasAnnouncedWorldReady: the client's half
#include "harness/session_runtime.h"         // Session()
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace DC = ue_wrap::drive_chain;
namespace P  = ue_wrap::profile;

constexpr float kCarryAsideCm = 150.f;  // how far the client carries the drive
constexpr float kOffPortCm    = 75.f;   // the host's copy has left the port by at least this
constexpr DWORD kReplayWindowMs = 10000;  // the host's connect replay to a fresh joiner

// Run a game-thread closure and block until it stores into `done` (1 ok, 2 fail).
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

// Poll `pred` on the game thread every quarter second until it holds or `ms` pass.
template <class Pred>
bool WaitFor(int ms, Pred pred) {
    for (int t = 0; t < ms; t += 250) {
        if (RunGT([pred](std::atomic<int>& d) { d.store(pred() ? 1 : 2); }) == 1) return true;
        ::Sleep(250);
    }
    return false;
}

// The shared origin both peers wait on, the broom drill's: the client's world coming up, which the
// client announces and the host learns as that slot going world-ready. A run reaches it when it
// reaches it -- a fixed wait here measured nothing when the load ran long and burned the rest of
// the window when it did not.
bool WaitForPeerWorld(const char* who) {
    const bool isClient = IsClientRole();
    UE_LOGI("driveslot: %hs -- waiting for the client's world", who);
    for (int i = 0; i < 1800; ++i) {
        // The client's own join being OVER, not merely announced: its phase returns to Idle when
        // the host's snapshot has been applied and the cover is down. The host has no predicate for
        // the other end of its own replay, so it waits out that window -- it is the one thing here
        // that is not a state either peer publishes. Acting inside it loses: the replay's seed
        // carries the slot as it was BEFORE the drill touched it, and it lands after the drill's
        // own edges and puts the old drive back.
        const bool ready = isClient ? (coop::net_pump::HasAnnouncedWorldReady() &&
                                       coop::join_progress::CurrentPhase() ==
                                           coop::join_progress::Phase::Idle)
                                    : harness::session_runtime::Session().IsSlotWorldReady(1);
        if (ready) {
            if (!isClient) ::Sleep(kReplayWindowMs);
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("driveslot: %hs never saw the client's world -- aborting", who);
    return false;
}

uint32_t EidOf(void* actor) {
    return static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor));
}

float Distance(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Call a Blueprint verb on `obj` with the local player as its `player` argument.
bool CallWithPlayer(void* obj, const wchar_t* verb, void* player) {
    void* fn = R::FindDispatchFunction(R::ClassOf(obj), verb, nullptr);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(obj, f);
}

struct Subject {
    void* slot = nullptr;
    void* drive = nullptr;
    int32_t driveIdx = 0;
    uint32_t eid = 0;
    ue_wrap::FVector port{};
};

void RunHost() {
    if (!WaitForPeerWorld("HOST")) return;
    auto sb = std::make_shared<Subject>();
    if (RunGT([sb](std::atomic<int>& d) {
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
            sb->drive = drive;
            sb->driveIdx = R::InternalIndexOf(drive);
            d.store(1);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT host FAIL -- could not set up the drive");
        return;
    }
    // The fresh drive has to be an ENTITY before it goes in: a slot line names its drive by element
    // id, and one sent for a drive that has not been enrolled yet names nothing the far peer can
    // ever resolve. Enrolment happens on the tracker's own pass, a frame or two behind the spawn.
    if (!WaitFor(15000, [sb] {
            const uint32_t e = EidOf(sb->drive);
            return e != 0u && e != static_cast<uint32_t>(coop::element::kInvalidId);
        })) {
        UE_LOGW("driveslot: VERDICT host FAIL -- the fresh drive never took an element id");
        return;
    }
    // Straight into the port: an insert that overtakes its drive's birth broadcast is parked by the
    // receiver (drive_sync's pending list, retried until the eid resolves), so no wait is owed here.
    const bool portRead = RunGT([sb](std::atomic<int>& d) {
        DC::CallPutDriveIn(sb->slot, sb->drive);
        sb->eid = EidOf(sb->drive);
        const bool read = E::TryGetActorLocation(sb->drive, sb->port);
        UE_LOGI("driveslot: HOST inserted eid=%u in the play slot at (%.0f,%.0f,%.0f)%s frozen=%d",
                sb->eid, sb->port.X, sb->port.Y, sb->port.Z, read ? "" : " (unread)",
                ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0);
        d.store(read ? 1 : 2);
    }) == 1;
    if (!portRead) {
        UE_LOGW("driveslot: VERDICT host FAIL -- the port's location could not be read, so no carry "
                "off it can be measured");
        return;
    }

    const bool out = WaitFor(90000, [sb] { return DC::SlotDrive(sb->slot) == nullptr; });
    if (!out) {
        UE_LOGW("driveslot: VERDICT host FAIL -- the client's eject never reached the host");
        return;
    }
    // The evidence, not a clock: the copy here is unfrozen and has left the port under the client's
    // poses. It ends the moment both hold -- mid-carry is already proof the poses drove it -- and
    // the readout below reports whatever is true when the wait returns, pass or timeout.
    UE_LOGI("driveslot: HOST's slot is empty -- watching for the carry to move this copy");
    WaitFor(30000, [sb] {
        if (!R::IsLiveByIndex(sb->drive, sb->driveIdx)) return false;
        if (ue_wrap::prop::IsFrozen(sb->drive)) return false;
        ue_wrap::FVector at{};
        return E::TryGetActorLocation(sb->drive, at) && Distance(at, sb->port) >= kOffPortCm;
    });
    RunGT([sb](std::atomic<int>& d) {
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        const bool frozen = live && ue_wrap::prop::IsFrozen(sb->drive);
        ue_wrap::FVector at{};
        const bool atRead = live && E::TryGetActorLocation(sb->drive, at);
        const float moved = atRead ? Distance(at, sb->port) : 0.f;
        const bool pass = atRead && empty && !frozen && moved >= kOffPortCm;
        UE_LOGI("driveslot: VERDICT host %s -- eid=%u live=%d slotEmpty=%d frozen=%d at "
                "(%.0f,%.0f,%.0f)%s, %.0f cm off the port (needs %.0f)",
                pass ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, frozen ? 1 : 0, at.X,
                at.Y, at.Z, (live && !atRead) ? " (unread)" : "", moved, kOffPortCm);
        d.store(1);
    });
}

void RunClient() {
    if (!WaitForPeerWorld("CLIENT")) return;
    // A DIFFERENT drive from whatever the save left in the port, frozen. Different, because the
    // host empties the port and inserts a fresh drive, and the lane carries idempotent slot STATE
    // rather than edges: the two moves land here as one line naming the new drive, so a peer
    // watching for the port to go empty would wait forever. Frozen, because the slot freezes what
    // it takes, and without that the grab below would have nothing to undo.
    auto sb = std::make_shared<Subject>();
    void* before = nullptr;
    RunGT([sb, &before](std::atomic<int>& d) {
        if (DC::EnsureResolved()) sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
        before = sb->slot ? DC::SlotDrive(sb->slot) : nullptr;
        d.store(1);
    });
    UE_LOGI("driveslot: CLIENT -- waiting for a fresh frozen drive in the play slot (the save left %p)",
            before);
    const bool in = WaitFor(180000, [sb, &before] {
        if (!DC::EnsureResolved()) return false;
        if (!sb->slot) sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
        sb->drive = sb->slot ? DC::SlotDrive(sb->slot) : nullptr;
        return sb->drive != nullptr && sb->drive != before && ue_wrap::prop::IsFrozen(sb->drive);
    });
    if (!in) {
        UE_LOGW("driveslot: VERDICT client FAIL -- no drive reached the play slot frozen");
        return;
    }

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
            sb->eid = EidOf(sb->drive);
            if (!E::TryGetActorLocation(sb->drive, sb->port)) {
                UE_LOGW("driveslot: the drive's location in the port could not be read"); d.store(2); return; }
            g->player = coop::players::Registry::Get().Local();
            if (!g->player) { UE_LOGW("driveslot: no local player"); d.store(2); return; }
            // Where the hand goes, read before the grab starts: a take that cannot place the drive
            // leaves it in the slot.
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
            g->hand = {p.X + fwd.X * 60.f, p.Y + fwd.Y * 60.f, p.Z + 80.f};
            g->side = {-fwd.Y, fwd.X, 0.f};
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
        UE_LOGW("driveslot: VERDICT client FAIL -- could not take the drive");
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
    RunGT([sb](std::atomic<int>& d) {
        ue_wrap::FVector at{};
        const bool atRead = E::TryGetActorLocation(sb->drive, at);
        UE_LOGI("driveslot: VERDICT client DONE -- eid=%u slotEmpty=%d frozen=%d at (%.0f,%.0f,%.0f)%s, "
                "%.0f cm off the port", sb->eid, DC::SlotDrive(sb->slot) == nullptr ? 1 : 0,
                ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0, at.X, at.Y, at.Z, atRead ? "" : " (unread)",
                atRead ? Distance(at, sb->port) : -1.f);
        d.store(1);
    });
}

}  // namespace

void RunDriveSlotTest() {
    if (IsClientRole()) RunClient();
    else RunHost();
    UE_LOGI("driveslot: done");
}

DWORD WINAPI DriveSlotTestThread(LPVOID) {
    RunDriveSlotTest();
    return 0;
}

}  // namespace harness::autotest

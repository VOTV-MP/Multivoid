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

#include "harness/autotest/driveslot.h"

#include "harness/autotest.h"  // IsClientRole

#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
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
    UE_LOGI("driveslot: HOST -- waiting 75s for the client's world, then a drive into the play slot");
    ::Sleep(75000);
    auto sb = std::make_shared<Subject>();
    if (RunGT([sb](std::atomic<int>& d) {
            if (!DC::EnsureResolved()) { UE_LOGW("driveslot: drive chain unresolved"); d.store(2); return; }
            sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
            if (!sb->slot) { UE_LOGW("driveslot: no desk play slot"); d.store(2); return; }
            if (DC::SlotDrive(sb->slot)) { UE_LOGW("driveslot: the play slot is taken already"); d.store(2); return; }
            // Born at the host's feet, away from the port: a drive born in the port's reach is taken
            // by the slot's own overlap on every peer, before the insert under test.
            void* player = coop::players::Registry::Get().Local();
            if (!player) { UE_LOGW("driveslot: no local player"); d.store(2); return; }
            ue_wrap::FVector at = E::GetActorLocation(player);
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
    ::Sleep(3000);  // the birth reaches the client before the insert does
    RunGT([sb](std::atomic<int>& d) {
        DC::CallPutDriveIn(sb->slot, sb->drive);
        sb->eid = EidOf(sb->drive);
        sb->port = E::GetActorLocation(sb->drive);
        UE_LOGI("driveslot: HOST inserted eid=%u in the play slot at (%.0f,%.0f,%.0f) frozen=%d",
                sb->eid, sb->port.X, sb->port.Y, sb->port.Z,
                ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0);
        d.store(1);
    });

    const bool out = WaitFor(90000, [sb] { return DC::SlotDrive(sb->slot) == nullptr; });
    if (!out) {
        UE_LOGW("driveslot: VERDICT host FAIL -- the client's eject never reached the host");
        return;
    }
    UE_LOGI("driveslot: HOST's slot is empty -- waiting out the client's carry and drop");
    ::Sleep(9000);
    RunGT([sb](std::atomic<int>& d) {
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        const bool frozen = live && ue_wrap::prop::IsFrozen(sb->drive);
        const ue_wrap::FVector at = live ? E::GetActorLocation(sb->drive) : ue_wrap::FVector{};
        const float moved = live ? Distance(at, sb->port) : 0.f;
        const bool pass = live && empty && !frozen && moved >= kOffPortCm;
        UE_LOGI("driveslot: VERDICT host %s -- eid=%u live=%d slotEmpty=%d frozen=%d at "
                "(%.0f,%.0f,%.0f), %.0f cm off the port (needs %.0f)",
                pass ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, frozen ? 1 : 0, at.X,
                at.Y, at.Z, moved, kOffPortCm);
        d.store(1);
    });
}

void RunClient() {
    UE_LOGI("driveslot: CLIENT -- waiting for the host's drive in the play slot");
    auto sb = std::make_shared<Subject>();
    const bool in = WaitFor(180000, [sb] {
        if (!DC::EnsureResolved()) return false;
        sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
        sb->drive = sb->slot ? DC::SlotDrive(sb->slot) : nullptr;
        return sb->drive != nullptr;
    });
    if (!in) {
        UE_LOGW("driveslot: VERDICT client FAIL -- no drive reached the play slot");
        return;
    }
    ::Sleep(2000);

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
            sb->port = E::GetActorLocation(sb->drive);
            g->player = coop::players::Registry::Get().Local();
            if (!g->player) { UE_LOGW("driveslot: no local player"); d.store(2); return; }
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
            const ue_wrap::FVector p = E::GetActorLocation(g->player);
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
    ::Sleep(4000);
    RunGT([sb](std::atomic<int>& d) {
        const ue_wrap::FVector at = E::GetActorLocation(sb->drive);
        UE_LOGI("driveslot: VERDICT client DONE -- eid=%u slotEmpty=%d frozen=%d at (%.0f,%.0f,%.0f), "
                "%.0f cm off the port", sb->eid, DC::SlotDrive(sb->slot) == nullptr ? 1 : 0,
                ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0, at.X, at.Y, at.Z, Distance(at, sb->port));
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

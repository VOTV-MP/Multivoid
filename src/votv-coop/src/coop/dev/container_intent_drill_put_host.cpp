// coop/dev/container_intent_drill_put_host.cpp -- the HOST's half of the container intent drill's put arms; see
// container_intent_drill_put.cpp for the arms and coop/dev/container_intent_drill.h for the drill. It spawns the arm's
// fixture beside the client's puppet (a target container 600 cm ahead, the item the client takes), says the element ids
// on drill chat, then judges its own copy once the client says it is done.

#include "container_intent_drill_put.h"
#include "container_route_drill_detail.h"

#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace ci = ue_wrap::container_inventory;
namespace DC = coop::dev::drill_chat;
namespace CC = coop::props::container_contents_sync;
namespace RD = coop::dev::container_route_drill::detail;
using ue_wrap::FVector;

constexpr const wchar_t* kFridgePath = L"/Game/objects/prop_container_minifridge.prop_container_minifridge_C";
constexpr const wchar_t* kFoodPath   = L"/Game/objects/prop_food.prop_food_C";

constexpr float    kTargetAheadCm = 600.f;   // the target, before the client's puppet: the client must walk
constexpr float    kItemAheadCm   = 100.f;   // the item the client takes, beside the puppet
constexpr float    kItemSideCm    = 80.f;
constexpr uint64_t kHostBindMs    = 15000;   // the host's spawn to its ids, inventories and first publications
constexpr uint64_t kHostWaitMs    = 300000;  // the host's fixture line to the client's done line
constexpr uint64_t kHostJudgeMs   = 5000;    // the done line to the target's count reading what the arm expects


// ---- the host -----------------------------------------------------------------------------------------------------

enum class HStep : uint8_t { WaitPeer, Fill, Bind, WaitClient, Judge, Done };
HStep    g_hStep = HStep::WaitPeer;
uint64_t g_hMs = 0;          // the time the step was entered
uint64_t g_hNextMs = 0;      // the host checks four times a second, not every frame
ue_wrap::CachedObjRef g_hTarget, g_hItem;
int      g_hCount0 = -1;     // the target's record count at the fixture line
int      g_hClientOk = -1;
int      g_hClientStray = -1;

void HNext(HStep s) { g_hStep = s; g_hMs = ::GetTickCount64(); }
bool HExpired(uint64_t bound) { return ::GetTickCount64() - g_hMs > bound; }

FVector Place(const FVector& at, const FVector& fwd, float ahead, float side) {
    return {at.X + fwd.X * ahead + fwd.Y * side, at.Y + fwd.Y * ahead - fwd.X * side, at.Z + RD::kDropCm};
}

bool EidBound(uint32_t eid) { return eid != static_cast<uint32_t>(coop::element::kInvalidId); }

// The host's fixture beside the client's puppet: the target 600 cm ahead, then what the arm needs the client to take.
// False, with the FAIL printed, when a class does not load or an actor does not spawn.
bool SpawnFixture(const FVector& at, const FVector& fwd) {
    const Arm arm = ArmOf();
    void* targetCls = ue_wrap::asset_load::LoadObjectByPath(arm == Arm::PutFood ? kFridgePath : RD::kPackPath);
    if (!targetCls) {
        Fail("the target's class does not load (%ls)", arm == Arm::PutFood ? kFridgePath : RD::kPackPath);
        return false;
    }
    void* target = E::SpawnActor(targetCls, Place(at, fwd, kTargetAheadCm, 0.f));
    if (!target || !ci::IsContainer(target)) {
        Fail("the target did not spawn as a container");
        return false;
    }
    g_hTarget.Set(target);
    void* itemCls = nullptr;
    switch (arm) {
    case Arm::PutRecord: itemCls = R::FindClass(RD::kDriveClass); break;
    case Arm::PutFood:   itemCls = ue_wrap::asset_load::LoadObjectByPath(kFoodPath); break;
    case Arm::PutNested:
    case Arm::PutEmpty:  itemCls = targetCls; break;   // the backpack the client pockets
    default: return true;
    }
    void* item = itemCls ? E::SpawnActor(itemCls, Place(at, fwd, kItemAheadCm, kItemSideCm)) : nullptr;
    if (!item) {
        Fail("the item the client takes did not spawn");
        return false;
    }
    g_hItem.Set(item);
    return true;
}

// A drive into `pack` by the backpack's own addObject, the drive's actor destroyed after the put that took. The pack's
// inventory must be bound (ContentsSlot). Returns false when the drive did not spawn or the pack refused it.
bool FillWithDrive(void* pack) {
    void* cls = R::FindClass(RD::kDriveClass);
    FVector put{};
    if (!cls || !E::TryGetActorLocation(pack, put)) return false;
    void* drive = E::SpawnActor(cls, {put.X, put.Y, put.Z + 60.f});
    if (!drive || !RD::PutIn(pack, drive)) return false;
    E::DestroyActor(drive);
    return true;
}

// The pack the arm fills: puthidden's target (it needs a slot to hover), putnested's item (the contents that must not
// travel). Null for the other arms.
void* FilledPack() {
    if (ArmOf() == Arm::PutHidden) return g_hTarget.Get();
    if (ArmOf() == Arm::PutNested) return g_hItem.Get();
    return nullptr;
}

void StepHostWaitPeer(const coop::net::Session& s) {
    int slot = 0;
    for (int i = 1; i < coop::net::kMaxPeers && !slot; ++i)
        if (s.IsSlotWorldReady(i)) slot = i;
    if (!slot) return;
    void* puppet = coop::puppet_drive::Puppet(slot).GetActor();   // spawned once the peer's pose arrives
    FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
    FVector fwd = E::GetActorForwardVector(puppet);
    if (fwd.X == 0.f && fwd.Y == 0.f) fwd = {1.f, 0.f, 0.f};
    if (!SpawnFixture(at, fwd)) return;
    HNext(HStep::Fill);
}

void StepHostFill() {
    void* pack = FilledPack();
    if (pack) {
        void* inv = ci::InventoryOf(pack);
        if (!inv || !ci::ContentsSlot(inv)) {   // its inventory's own init binds the save slot's contents first
            if (HExpired(kHostBindMs)) Fail("the pack's inventory never bound its contents slot");
            return;
        }
        if (!FillWithDrive(pack)) {
            Fail("the pack did not take its drive");
            return;
        }
    }
    HNext(HStep::Bind);
}

void StepHostBind() {
    void* target = g_hTarget.Get();
    void* item = g_hItem.Get();
    if (!target || (ArmOf() != Arm::PutHand && ArmOf() != Arm::PutHidden && !item)) {
        Fail("a fixture actor died before the client took it");
        return;
    }
    const uint32_t te = RD::EidOf(target);
    const uint32_t ie = item ? RD::EidOf(item) : 0;
    void* filled = FilledPack();
    const uint32_t fe = filled ? RD::EidOf(filled) : 0;
    const bool bound = EidBound(te) && (!item || EidBound(ie)) && (!filled || (EidBound(fe) && CC::SentBaselineKnown(fe)));
    const int count = bound ? CountOf(te) : -1;
    if (!bound || count < 0) {
        if (HExpired(kHostBindMs)) Fail("the fixture was never bound and published (target eid %u, item eid %u, count %d)", te, ie, count);
        return;
    }
    g_hCount0 = count;
    char say[64];
    std::snprintf(say, sizeof(say), "%spack=%u item=%u", kPutFixtureLine, te, item ? ie : 0u);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] host %s fixture: target eid=%u %ls holds %d record(s), item eid=%u", ArmName(), te,
            R::ClassNameOf(target).c_str(), count, item ? ie : 0u);
    HNext(HStep::WaitClient);
}

void StepHostWaitClient() {
    std::string rest;
    if (DC::LiveRestOf(kPutDoneLine, &rest) && RD::IntAfter(rest, "ok", g_hClientOk)) {
        if (!RD::IntAfter(rest, "stray", g_hClientStray)) g_hClientStray = -1;
        UE_LOGI("[CI-DRILL] host %s: the client is done (%s)", ArmName(), rest.c_str());
        HNext(HStep::Judge);
        return;
    }
    if (HExpired(kHostWaitMs)) Fail("the client never said it was done within %llu s", static_cast<unsigned long long>(kHostWaitMs / 1000));
}

// The container record the target holds after a putempty, read raw: the stack index it carries (-1 when none is read).
int InsertedStackIndex(void* target, std::wstring& cls) {
    void* inv = ci::InventoryOf(target);
    std::vector<ue_wrap::save_record::SaveRecord> recs;
    if (!inv || !coop::props::container_contents_io::ReadRecords(inv, recs, /*neuterNested=*/false)) return -2;
    for (const auto& r : recs) {
        int32_t idx = 0;
        if (ci::IsContainerRecord(r) && ci::ContainerStackIndex(r, idx)) {
            cls = r.className;
            return idx;
        }
    }
    return -3;
}

void StepHostJudge() {
    const Arm arm = ArmOf();
    void* target = g_hTarget.Get();
    if (g_hClientOk == 0) {
        Fail("the client's arm failed (its FAIL line says why)");
        return;
    }
    const int count = CountOf(RD::EidOf(target));
    const int want = PutIsNoPutArm(arm) ? g_hCount0 : g_hCount0 + 1;
    if (g_hClientOk == 1 && count != want && !HExpired(kHostJudgeMs)) return;   // the commit may still be arriving
    if (g_hClientOk == 2) {
        UE_LOGI("[CI-DRILL] host %s NOT MEASURED (the client's fixture did not hold what the arm needs)", ArmName());
        Finish();
        return;
    }
    const wchar_t* cls = PutIsNoPutArm(arm) ? nullptr : PutItemClass(arm);   // no put, nothing to strew
    const int stray = cls ? PutStrayCount(cls, target) : -1;
    UE_LOGI("[CI-DRILL] host %s target %d -> %d record(s)", ArmName(), g_hCount0, count);
    if (cls) UE_LOGI("[CI-DRILL] host %s stray %ls host=%d client=%d", ArmName(), cls, stray, g_hClientStray);
    if (count != want) {
        Fail("the target went %d -> %d record(s), expected %d", g_hCount0, count, want);
        return;
    }
    if (stray > 0 || g_hClientStray > 0) {
        Fail("stray %ls props near the target: host=%d client=%d", cls, stray, g_hClientStray);
        return;
    }
    if (arm == Arm::PutEmpty) {
        std::wstring inserted;
        const int idx = InsertedStackIndex(target, inserted);
        UE_LOGI("[CI-DRILL] host putempty inserted record class=%ls stack index=%d", inserted.c_str(), idx);
        if (idx != -1) {
            Fail("the inserted container record carries stack index %d, expected -1", idx);
            return;
        }
    }
    UE_LOGI("[CI-DRILL] host %s PASS", ArmName());
    Finish();
}

}  // namespace

void PutHostTick(const coop::net::Session& session) {
    if (!g_hMs) HNext(HStep::WaitPeer);   // the first step's clock starts when the arm does, never at reset
    const uint64_t now = ::GetTickCount64();
    if (now < g_hNextMs) return;
    g_hNextMs = now + RD::kCheckEveryMs;
    switch (g_hStep) {
    case HStep::WaitPeer:   StepHostWaitPeer(session); return;
    case HStep::Fill:       StepHostFill(); return;
    case HStep::Bind:       StepHostBind(); return;
    case HStep::WaitClient: StepHostWaitClient(); return;
    case HStep::Judge:      StepHostJudge(); return;
    case HStep::Done:       return;
    }
}

void PutHostReset() {
    g_hStep = HStep::WaitPeer;
    g_hMs = g_hNextMs = 0;
    g_hTarget.Reset();
    g_hItem.Reset();
    g_hCount0 = g_hClientOk = g_hClientStray = -1;
}

}  // namespace coop::dev::container_intent_drill::detail

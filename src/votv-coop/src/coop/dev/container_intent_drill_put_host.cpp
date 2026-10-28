// coop/dev/container_intent_drill_put_host.cpp -- the HOST's half of the container intent drill's put arms; see
// container_intent_drill_put.cpp for the arms and coop/dev/container_intent_drill.h for the drill. It spawns the arm's
// fixture beside the client's puppet (a target container 600 cm ahead, the item the client takes), says the element ids
// on drill chat, then judges its own copy once the client says it is done. The arms that interleave (putreserved,
// putdepart, putdeadline, putowed) have the host do their half between the fixture line and that end.

#include "container_intent_drill_put.h"
#include "container_route_drill_detail.h"
#include "drive_drill_verbs.h"  // Pocket: the host player's own verb (co-located private header)

#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop_gather.h"
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
namespace AH = coop::props::container_act_host;
namespace AS = coop::props::container_act_state;
namespace CV = ue_wrap::container_view;
namespace DV = coop::dev::drive_drill_verbs;
using ue_wrap::FVector;

constexpr const wchar_t* kFridgePath = L"/Game/objects/prop_container_minifridge.prop_container_minifridge_C";
constexpr const wchar_t* kFoodPath   = L"/Game/objects/prop_food.prop_food_C";

constexpr float    kTargetAheadCm = 600.f;   // the target, before the client's puppet: the client must walk
constexpr float    kItemAheadCm   = 100.f;   // the item the client takes, beside the puppet
constexpr float    kItemSideCm    = 80.f;
constexpr uint64_t kHostBindMs    = 15000;   // the host's spawn to its ids, inventories and first publications
constexpr uint64_t kHostWaitMs    = 300000;  // the host's fixture line to the client's done line
constexpr uint64_t kHostJudgeMs   = 5000;    // the done line to the target's count reading what the arm expects
constexpr uint64_t kHostViewMs    = 10000;   // putreserved: the host's openPropInv to the screen showing the target
constexpr uint64_t kHostReplyMs   = 8000;    // putreserved: the grant to the held replies going out (the client waits 10 s)
constexpr uint64_t kReleaseMs     = 10000;   // putdepart: the sender's departure to its reservation being released
constexpr uint64_t kLapseMinMs    = 29000;   // putdeadline: the grant to the reservation lapsing, at least
constexpr uint64_t kLapseMaxMs    = 36000;   // and at most
constexpr uint64_t kOwedBoundMs   = 90000;   // putowed: the grant to the owed put being given up (30 tries at 1 Hz)
constexpr int      kBrimMax       = 120;     // putfull: drives put in before the pack is declared never to refuse one
constexpr const char* kBusyLine   = "Another player is using that item.";


// ---- the host -----------------------------------------------------------------------------------------------------

enum class HStep : uint8_t { WaitPeer, Fill, Bind, Interleave, WaitClient, Judge, Done };
HStep    g_hStep = HStep::WaitPeer;
uint64_t g_hMs = 0;          // the time the step was entered
uint64_t g_hNextMs = 0;      // the host checks four times a second, not every frame
int      g_hSub = 0;         // the interleave's own step
uint64_t g_hSubMs = 0;       // and the time it was entered (the grant's, for putdeadline and putowed)
ue_wrap::CachedObjRef g_hTarget, g_hItem, g_hItem2;
int      g_hCount0 = -1;     // the target's record count at the fixture line
int      g_hClientOk = -1;
int      g_hClientStray = -1;
float    g_hDriveVolume = -1.f, g_hPackMax = -1.f;   // the fill's drive Get Volume and the pack's maxVol, -1 when not read

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
    const wchar_t* itemPath = RD::kDriveClass;   // a class name for the drive, a path for the others
    switch (arm) {
    case Arm::PutFood:   itemPath = kFoodPath; itemCls = ue_wrap::asset_load::LoadObjectByPath(itemPath); break;
    case Arm::PutNested:
    case Arm::PutEmpty:  itemPath = kBoxPath; itemCls = ue_wrap::asset_load::LoadObjectByPath(itemPath); break;   // the gembox
    default:
        if (!PutHasItem(arm)) return true;   // the hand arms take a drive by the player's own verb
        itemCls = R::FindClass(RD::kDriveClass);
        break;
    }
    if (!itemCls) {
        Fail("the item's class does not load (%ls)", itemPath);
        return false;
    }
    void* item = E::SpawnActor(itemCls, Place(at, fwd, kItemAheadCm, kItemSideCm));
    if (!item) {
        Fail("the item the client takes did not spawn");
        return false;
    }
    g_hItem.Set(item);
    if (arm == Arm::PutDeadline) {   // the next put of the lapsed sender needs a second item
        void* item2 = E::SpawnActor(itemCls, Place(at, fwd, kItemAheadCm, -kItemSideCm));
        if (!item2) {
            Fail("the second item the client takes did not spawn");
            return false;
        }
        g_hItem2.Set(item2);
    }
    return true;
}

// A drive into `pack` by the backpack's own addObject, the drive's actor destroyed after the put that took. The pack's
// inventory must be bound (ContentsSlot). Returns false when the drive did not spawn or the pack refused it.
bool FillWithDrive(void* pack) {
    void* cls = R::FindClass(RD::kDriveClass);
    FVector put{};
    if (!cls || !E::TryGetActorLocation(pack, put)) return false;
    void* drive = E::SpawnActor(cls, {put.X, put.Y, put.Z + 60.f});
    float bound = 0.f;   // the volume is the game's Get Volume on the drive's mesh; the room is the pack's own maxVol
    if (drive) ue_wrap::prop_gather::ReadVolumeAndBound(drive, g_hDriveVolume, bound);
    if (void* inv = ci::InventoryOf(pack)) ci::MaxVolume(inv, g_hPackMax);
    if (!drive || !RD::PutIn(pack, drive)) return false;
    E::DestroyActor(drive);
    return true;
}

// putfull: drives into `pack` by its own addObject until it refuses the next one (its volume, as the game counts it),
// each accepted drive's actor destroyed. The refused drive is destroyed too, so none is left strewn. False, with the
// FAIL printed, when a drive did not spawn, the first was refused, or the pack never refused one.
bool FillToBrim(void* pack) {
    void* cls = R::FindClass(RD::kDriveClass);
    FVector put{};
    void* inv = ci::InventoryOf(pack);
    if (!cls || !inv || !E::TryGetActorLocation(pack, put)) {
        Fail("the pack could not be filled (class %p, inventory %p)", cls, inv);
        return false;
    }
    ci::MaxVolume(inv, g_hPackMax);
    int n = 0;
    for (; n < kBrimMax; ++n) {
        void* drive = E::SpawnActor(cls, {put.X, put.Y, put.Z + 60.f});
        if (!drive) {
            Fail("a drive did not spawn for the fill (%d put in)", n);
            return false;
        }
        float bound = 0.f;
        if (!n) ue_wrap::prop_gather::ReadVolumeAndBound(drive, g_hDriveVolume, bound);
        const bool took = RD::PutIn(pack, drive);
        E::DestroyActor(drive);
        if (!took) break;
    }
    float curr = -1.f;
    ci::CurrentVolume(inv, curr);
    UE_LOGI("[CI-DRILL] host %s filled %d, currVol/maxVol %.1f/%.1f, drive volume %.1f", ArmName(), n,
            static_cast<double>(curr), static_cast<double>(g_hPackMax), static_cast<double>(g_hDriveVolume));
    if (n == 0 || n >= kBrimMax) {
        Fail("the pack %s (%d drives put in)", n == 0 ? "took no drive" : "never refused a drive", n);
        return false;
    }
    return true;
}

// putreserved: the host's own drive in its own inventory, put there by the player's own verb, so the host has an item to
// put into the target while the client's grant stands.
bool HostPocketDrive() {
    void* cls = R::FindClass(RD::kDriveClass);
    void* player = coop::players::Registry::Get().Local();
    FVector at{};
    void* target = g_hTarget.Get();
    if (!cls || !player || !target || !E::TryGetActorLocation(target, at)) {
        Fail("the host's own drive could not be placed (class %p, player %p, target %p)", cls, player, target);
        return false;
    }
    void* drive = E::SpawnActor(cls, {at.X, at.Y, at.Z + 60.f});
    const int before = ue_wrap::inventory::LivePersonalStoreCount();
    if (!drive || !DV::Pocket(player, drive) || ue_wrap::inventory::LivePersonalStoreCount() != before + 1) {
        Fail("the host could not pocket its own drive (own inventory %d -> %d)", before,
             ue_wrap::inventory::LivePersonalStoreCount());
        return false;
    }
    UE_LOGI("[CI-DRILL] host putreserved: the host pocketed its own drive, own inventory %d -> %d", before, before + 1);
    return true;
}

// The pack the arm fills: the target of puthidden and puthover (a slot to hover), of putfull (to its volume), putnested's
// item (the contents that must not travel). Null for the other arms.
void* FilledPack() {
    if (PutTargetFilled(ArmOf())) return g_hTarget.Get();
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
        if (PutFillsToBrim(ArmOf())) {
            if (!FillToBrim(pack)) return;
        } else if (!FillWithDrive(pack)) {
            Fail("the pack did not take its drive (drive volume %.1f, pack maxVol %.1f)", static_cast<double>(g_hDriveVolume),
                 static_cast<double>(g_hPackMax));
            return;
        }
    }
    if (ArmOf() == Arm::PutReserved && !HostPocketDrive()) return;
    HNext(HStep::Bind);
}

void StepHostBind() {
    void* target = g_hTarget.Get();
    void* item = g_hItem.Get();
    void* item2 = g_hItem2.Get();
    if (!target || (PutHasItem(ArmOf()) && !item) || (ArmOf() == Arm::PutDeadline && !item2)) {
        Fail("a fixture actor died before the client took it");
        return;
    }
    const uint32_t te = RD::EidOf(target);
    const uint32_t ie = item ? RD::EidOf(item) : 0;
    const uint32_t i2e = item2 ? RD::EidOf(item2) : 0;
    void* filled = FilledPack();
    const uint32_t fe = filled ? RD::EidOf(filled) : 0;
    const bool bound = EidBound(te) && (!item || EidBound(ie)) && (!item2 || EidBound(i2e)) &&
                       (!filled || (EidBound(fe) && CC::SentBaselineKnown(fe)));
    const int count = bound ? CountOf(te) : -1;
    if (!bound || count < 0) {
        if (HExpired(kHostBindMs)) Fail("the fixture was never bound and published (target eid %u, item eid %u, count %d)", te, ie, count);
        return;
    }
    g_hCount0 = count;
    if (ArmOf() == Arm::PutReserved) AH::dev::HoldReplies(true);   // before the client can ask: its grant is held
    char say[112];
    std::snprintf(say, sizeof(say), "%spack=%u item=%u item2=%u count=%d", kPutFixtureLine, te, item ? ie : 0u, item2 ? i2e : 0u,
                  count);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] host %s fixture: target eid=%u %ls holds %d record(s), item eid=%u, item2 eid=%u, fill: drive "
            "volume %.1f in pack maxVol %.1f", ArmName(), te, R::ClassNameOf(target).c_str(), count, item ? ie : 0u,
            item2 ? i2e : 0u, static_cast<double>(g_hDriveVolume), static_cast<double>(g_hPackMax));
    HNext(PutHostInterleaves(ArmOf()) ? HStep::Interleave : HStep::WaitClient);
}

// ---- the arms whose host does something between the fixture and the client's end -------------------------------------

bool AnyPeerWorldReady(const coop::net::Session& s) {
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (s.IsSlotWorldReady(i)) return true;
    return false;
}

// The client's grants and Gone answers for the target so far (the host counts a queued, held grant as sent).
void Verdicts(uint32_t& grants, uint32_t& gones) { AH::dev::Verdicts(RD::EidOf(g_hTarget.Get()), grants, gones); }

// putreserved: the client's grant stands (its reply held), the host's own player presses em_take on the same target and is
// refused; then the replies go out and the client's put commits.
void HostReserved() {
    void* target = g_hTarget.Get();
    uint32_t grants = 0, gones = 0;
    const uint64_t now = ::GetTickCount64();
    switch (g_hSub) {
    case 0:   // the grant
        Verdicts(grants, gones);
        if (gones) {
            Fail("the client's put was answered Gone (%u)", gones);
            return;
        }
        if (!grants) {
            if (HExpired(kHostWaitMs)) Fail("the client never asked within %llu s", static_cast<unsigned long long>(kHostWaitMs / 1000));
            return;
        }
        UE_LOGI("[CI-DRILL] host putreserved: the client's grant is held (grants=%u), holds=%zu, a hold stands on the target=%d",
                grants, AS::Live().Count(), AS::Live().HoldOn(RD::EidOf(target)) ? 1 : 0);
        g_hSubMs = now;
        g_hSub = 1;
        return;
    case 1:   // the screen
        if (!CV::Open(target)) {
            Fail("openPropInv did not dispatch");
            return;
        }
        g_hSub = 2;
        return;
    case 2: {  // the press
        if (CV::Viewed() != target) {
            if (now - g_hSubMs >= kHostViewMs) Fail("the host's screen does not show the target %llu s after openPropInv", static_cast<unsigned long long>(kHostViewMs / 1000));
            return;
        }
        void* const ui = RD::Screen();
        int32_t sel = -1;
        char why[240];
        if (!ui || !PutSelectOwnItem(ui, sel, why, sizeof(why))) {
            Fail("%s", ui ? why : "the inventory screen does not resolve");
            return;
        }
        const int own0 = ue_wrap::inventory::LivePersonalStoreCount();
        if (!CV::RunPut(ui)) {
            Fail("em_take did not dispatch");
            return;
        }
        const bool busy = DC::ChatHasLive(kBusyLine);
        const int own1 = ue_wrap::inventory::LivePersonalStoreCount();
        UE_LOGI("[CI-DRILL] host putreserved: the host presses em_take on the reserved target after %llu ms: busy line=%d "
                "own %d -> %d", static_cast<unsigned long long>(now - g_hSubMs), busy ? 1 : 0, own0, own1);
        AH::dev::ReleaseReplies();
        AH::dev::HoldReplies(false);
        if (own1 != own0 || !busy) {
            Fail("the host's own put was not refused under the client's reservation: busy line=%d, own %d -> %d",
                 busy ? 1 : 0, own0, own1);
            return;
        }
        if (now - g_hSubMs >= kHostReplyMs) {
            Fail("the held reply went out %llu ms after the grant, past the client's reply wait", static_cast<unsigned long long>(now - g_hSubMs));
            return;
        }
        HNext(HStep::WaitClient);
        return;
    }
    default:
        return;
    }
}

// putdepart: the client's grant stands (its replay parked); the client leaves; the reservation is released within
// kReleaseMs of the departure, and the target keeps what it had.
void HostDepart(const coop::net::Session& s) {
    const uint64_t now = ::GetTickCount64();
    uint32_t grants = 0, gones = 0;
    switch (g_hSub) {
    case 0:
        Verdicts(grants, gones);
        if (!grants) {
            if (HExpired(kHostWaitMs)) Fail("the client never asked within %llu s", static_cast<unsigned long long>(kHostWaitMs / 1000));
            return;
        }
        UE_LOGI("[CI-DRILL] host putdepart: the client's grant stands (grants=%u), holds=%zu", grants, AS::Live().Count());
        g_hSub = 1;
        return;
    case 1:
        if (AnyPeerWorldReady(s)) return;
        g_hSubMs = now;
        UE_LOGI("[CI-DRILL] host putdepart: the client is gone, holds=%zu", AS::Live().Count());
        g_hSub = 2;
        return;
    case 2: {
        const size_t holds = AS::Live().Count();
        if (holds) {
            if (now - g_hSubMs >= kReleaseMs)
                Fail("the reservation stood %llu ms after the sender left (holds %zu)", static_cast<unsigned long long>(now - g_hSubMs), holds);
            return;
        }
        const int count = CountOf(RD::EidOf(g_hTarget.Get()));
        const int stray = PutStrayCount(PutItemClass(ArmOf()), g_hTarget.Get());
        UE_LOGI("[CI-DRILL] host putdepart: the reservation was released %llu ms after the sender left; target %d -> %d, "
                "stray %ls host=%d", static_cast<unsigned long long>(now - g_hSubMs), g_hCount0, count, PutItemClass(ArmOf()), stray);
        if (count != g_hCount0) {
            Fail("the target went %d -> %d record(s), expected no change", g_hCount0, count);
            return;
        }
        if (stray > 0) {
            Fail("stray %ls props near the target: host=%d", PutItemClass(ArmOf()), stray);
            return;
        }
        UE_LOGI("[CI-DRILL] host %s PASS", ArmName());
        Finish();
        return;
    }
    default:
        return;
    }
}

// putdeadline: the reservation lapses about 30 s after the grant; the rest is the client's, judged at the end.
void HostDeadline() {
    const uint64_t now = ::GetTickCount64();
    uint32_t grants = 0, gones = 0;
    if (g_hSub == 0) {
        Verdicts(grants, gones);
        if (!grants) {
            if (HExpired(kHostWaitMs)) Fail("the client never asked within %llu s", static_cast<unsigned long long>(kHostWaitMs / 1000));
            return;
        }
        g_hSubMs = now;
        g_hSub = 1;
        UE_LOGI("[CI-DRILL] host putdeadline: the client's grant stands (grants=%u)", grants);
        return;
    }
    const uint64_t since = now - g_hSubMs;
    if (!AH::TallyOf(AH::Tally::ReservationLapsed)) {
        if (since > kLapseMaxMs + 4000) Fail("the reservation never lapsed (%llu ms after the grant)", static_cast<unsigned long long>(since));
        return;
    }
    UE_LOGI("[CI-DRILL] host putdeadline: the reservation lapsed %llu ms after the grant", static_cast<unsigned long long>(since));
    if (since < kLapseMinMs || since > kLapseMaxMs) {
        Fail("the reservation lapsed %llu ms after the grant, expected %llu..%llu", static_cast<unsigned long long>(since),
             static_cast<unsigned long long>(kLapseMinMs), static_cast<unsigned long long>(kLapseMaxMs));
        return;
    }
    HNext(HStep::WaitClient);
}

// putowed: the put's insert answers NotReady, so it is owed and retried at 1 Hz, then given up and lost; the host's truth
// goes back to the sender.
void HostOwed() {
    const uint64_t now = ::GetTickCount64();
    uint32_t grants = 0, gones = 0;
    if (g_hSub == 0) {
        Verdicts(grants, gones);
        if (!grants) {
            if (HExpired(kHostWaitMs)) Fail("the client never asked within %llu s", static_cast<unsigned long long>(kHostWaitMs / 1000));
            return;
        }
        g_hSubMs = now;
        g_hSub = 1;
        UE_LOGI("[CI-DRILL] host putowed: the client's grant stands (grants=%u)", grants);
        return;
    }
    const uint64_t since = now - g_hSubMs;
    if (!AH::TallyOf(AH::Tally::OwedLost)) {
        if (since > kOwedBoundMs)
            Fail("the owed put was never given up %llu s after the grant (restore_owed=%u)", static_cast<unsigned long long>(since / 1000),
                 AH::TallyOf(AH::Tally::RestoreOwed));
        return;
    }
    UE_LOGI("[CI-DRILL] host putowed: the owed put was given up %llu ms after the grant (restore_owed=%u owed_lost=%u)",
            static_cast<unsigned long long>(since), AH::TallyOf(AH::Tally::RestoreOwed), AH::TallyOf(AH::Tally::OwedLost));
    HNext(HStep::WaitClient);
}

void StepHostInterleave(const coop::net::Session& s) {
    switch (ArmOf()) {
    case Arm::PutReserved: HostReserved(); return;
    case Arm::PutDepart:   HostDepart(s); return;
    case Arm::PutDeadline: HostDeadline(); return;
    case Arm::PutOwed:     HostOwed(); return;
    default:               HNext(HStep::WaitClient); return;
    }
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
    const int want = g_hCount0 + PutHostDelta(arm);
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
    if (arm == Arm::PutDeadline) {   // the late done inserted once, its second send a bad commit, one lapse
        const uint32_t late = AH::TallyOf(AH::Tally::LateDone), bad = AH::TallyOf(AH::Tally::BadCommit);
        const uint32_t lapsed = AH::TallyOf(AH::Tally::ReservationLapsed);
        UE_LOGI("[CI-DRILL] host putdeadline late_done=%u bad_commit=%u reservation_lapsed=%u", late, bad, lapsed);
        if (late != 1 || bad != 1 || lapsed != 1) {
            Fail("late_done=%u bad_commit=%u reservation_lapsed=%u, expected 1 each", late, bad, lapsed);
            return;
        }
    }
    if (arm == Arm::PutOwed) {   // the put was owed once and given up, and nothing late arrived
        const uint32_t lost = AH::TallyOf(AH::Tally::OwedLost), late = AH::TallyOf(AH::Tally::LateDone);
        UE_LOGI("[CI-DRILL] host putowed owed_lost=%u late_done=%u", lost, late);
        if (lost != 1 || late != 0) {
            Fail("owed_lost=%u late_done=%u, expected 1 and 0", lost, late);
            return;
        }
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
    PutArmHostKnobs();
    if (!g_hMs) HNext(HStep::WaitPeer);   // the first step's clock starts when the arm does, never at reset
    const uint64_t now = ::GetTickCount64();
    if (now < g_hNextMs) return;
    g_hNextMs = now + RD::kCheckEveryMs;
    switch (g_hStep) {
    case HStep::WaitPeer:   StepHostWaitPeer(session); return;
    case HStep::Fill:       StepHostFill(); return;
    case HStep::Bind:       StepHostBind(); return;
    case HStep::Interleave: StepHostInterleave(session); return;
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
    g_hItem2.Reset();
    g_hSub = 0;
    g_hSubMs = 0;
    g_hCount0 = g_hClientOk = g_hClientStray = -1;
    g_hDriveVolume = g_hPackMax = -1.f;
}

}  // namespace coop::dev::container_intent_drill::detail

// coop/dev/atv_trunk_drill.cpp -- see coop/dev/atv_trunk_drill.h.

#include "coop/dev/atv_trunk_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_pick.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/atv_trunk_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::atv_trunk_drill {
namespace {

namespace A   = ue_wrap::atv;
namespace CI  = ue_wrap::container_inventory;
namespace CV  = ue_wrap::container_view;
namespace CP  = coop::dev::container_pick;
namespace DC  = coop::dev::drill_chat;
namespace II  = ue_wrap::inventory_item;
namespace OI  = ue_wrap::object_index;
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace ats = coop::atv_trunk_sync;

constexpr const char* kSeededPrefix = "atvtrunk host seeded ";    // + "<records> <trunk id>"
constexpr const char* kTakenLine    = "atvtrunk client taken";    // the client's take stood on its own copy
constexpr const char* kTakeOkLine   = "atvtrunk host take ok";    // the host read the take and holds it: the client may put
constexpr const char* kPutLine      = "atvtrunk client put";      // the client's put stood on its own copy
constexpr const char* kChangedPrefix = "atvtrunk host changed ";  // + the record count after the host's own add
constexpr const char* kDoneLine     = "atvtrunk client done";
constexpr uint64_t kSettleMs  = 4000;   // the host's answer, and the truth it would send after a refusal, are in by then
constexpr uint64_t kRecheckMs = 8000;   // a refusal's give-back would have landed by here
constexpr uint64_t kBoundMs   = 120000; // a step that waits on the other peer or the world gives up after this
constexpr uint64_t kOpenWaitMs = 3000;  // the widgets of a freshly opened screen are found within this

int NonEmpty(const std::vector<II::Item>& recs) {
    int n = 0;
    for (const II::Item& r : recs) n += II::IsEmpty(r) ? 0 : 1;
    return n;
}

// ---- the host ----------------------------------------------------------------------------------------------------

enum class HPhase { Seed, AwaitTake, AwaitPut, AwaitDone, Judged };
HPhase   g_hPhase = HPhase::Seed;
uint64_t g_hSince = 0;
uint32_t g_hEid = 0;
int      g_seeded = 0;
int      g_hostSeen = -1;
uint64_t g_hostNextMs = 0;
uint64_t g_wordAtMs = 0;

void HostFail(const std::string& why) {
    g_hPhase = HPhase::Judged;
    UE_LOGW("[ATV-TRUNK-DRILL] host FAIL: %s", why.c_str());
}

void HostSeed(coop::net::Session& s) {
    const uint64_t now = ::GetTickCount64();
    if (!g_hSince) g_hSince = now;
    if (now - g_hSince > kBoundMs) return HostFail("no seed: no ATV with a readable trunk, a world container record or the trunk write never came");
    if (!s.IsSlotWorldReady(1)) return;
    struct Pick { uint32_t eid = 0; void* atv = nullptr; } atv;
    ats::ForEachTrunk([](void* ctx, uint32_t eid, void* a) {
        Pick& p = *static_cast<Pick*>(ctx);
        if (!p.atv) { p.eid = eid; p.atv = a; }
    }, &atv);
    if (!atv.atv) return;
    CP::Want want;
    want.minRecords = 1;
    CP::Pick pick;
    if (!CP::PickContainer(want, pick)) return;
    void* const inv = CI::InventoryOf(pick.actor);
    std::vector<II::Item> recs;
    if (!inv || !CI::ReadContents(inv, recs, 4096)) return;
    II::Item first;
    bool have = false;
    for (const II::Item& r : recs)
        if (!II::IsEmpty(r)) { first = r; have = true; break; }
    if (!have) return;
    II::Item a = first, b = first;
    a.amount = 1;
    b.amount = 2;     // a different record by its hash, so the two entries are told apart by the host's judge
    b.posX = a.posX + 80.f;
    const std::vector<II::Item> items{a, b};
    switch (A::WriteTrunk(atv.atv, items)) {
    case A::TrunkWrite::NotReady: return;
    case A::TrunkWrite::Refused:  return HostFail("the ATV's trunk write was refused");
    case A::TrunkWrite::Written:  break;
    }
    // The ATV stands where it was parked, usually far from the rig's players. Reach is judged on the HOST's copy of the ATV
    // against the client's puppet, and the rig's client starts where the host stands (join_at_host), so the ATV is set
    // down beside the host's player.
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!me || !E::TryGetActorLocation(me, at) || !A::GetRootTransform(atv.atv, loc, rot)) return;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
    const ue_wrap::FVector put{at.X + fwd.X * 300.f, at.Y + fwd.Y * 300.f, at.Z + 60.f};
    if (!A::TeleportRig(atv.atv, put, rot)) return HostFail("the ATV could not be set down beside the host");
    g_seeded = 2;
    g_hEid = atv.eid;
    UE_LOGI("[ATV-TRUNK-DRILL] host set the ATV (trunk %08x) down at (%.0f,%.0f,%.0f) and seeded its trunk with %d records of %ls "
            "(copied off eid=%u)", g_hEid, put.X, put.Y, put.Z, g_seeded, II::ClassLeaf(first).c_str(), pick.eid);
    DC::Say(std::string(kSeededPrefix) + std::to_string(g_seeded) + " " + std::to_string(g_hEid));
    g_wordAtMs = 0;
    g_hPhase = HPhase::AwaitTake;
}

int HostCount() { return ats::TrunkCount(g_hEid); }

// The host reads the trunk 2 s after the client's word, once the judge has had its turn.
bool HostWordDue(const char* line) {
    const uint64_t now = ::GetTickCount64();
    if (!g_wordAtMs) {
        if (DC::PollDue() && DC::SeenHas(line)) g_wordAtMs = now;
        return false;
    }
    if (now - g_wordAtMs < 2000) return false;
    g_wordAtMs = 0;
    return true;
}

void HostAwait() {
    const uint64_t now = ::GetTickCount64();
    const int n = HostCount();
    if (now >= g_hostNextMs) {
        g_hostNextMs = now + 500;
        if (n != g_hostSeen) {
            UE_LOGI("[ATV-TRUNK-DRILL] host trunk records %d -> %d", g_hostSeen, n);
            g_hostSeen = n;
        }
    }
    if (g_hPhase == HPhase::AwaitTake) {
        if (!HostWordDue(kTakenLine)) return;
        if (n != g_seeded - 1) return HostFail("after the client's take the trunk holds " + std::to_string(n) + " records, want " + std::to_string(g_seeded - 1));
        UE_LOGI("[ATV-TRUNK-DRILL] host take PASS: the trunk holds %d of %d records after the client's take", n, g_seeded);
        DC::Say(kTakeOkLine);
        g_hPhase = HPhase::AwaitPut;
        return;
    }
    if (g_hPhase == HPhase::AwaitPut) {
        if (!HostWordDue(kPutLine)) return;
        if (n != g_seeded) return HostFail("after the client's put the trunk holds " + std::to_string(n) + " records, want " + std::to_string(g_seeded));
        UE_LOGI("[ATV-TRUNK-DRILL] host put PASS: the trunk holds %d records again after the client's put", n);
        // The host's own change: one more record, read off the trunk's own first entry.
        void* const atv = ats::AtvOfEid(g_hEid);
        std::vector<II::Item> recs;
        if (!atv || !A::ReadTrunk(atv, recs, 4096)) return HostFail("the trunk does not read for the host's own add");
        II::Item extra;
        bool have = false;
        for (const II::Item& r : recs)
            if (!II::IsEmpty(r)) { extra = r; have = true; break; }
        if (!have) return HostFail("the trunk holds no record to copy for the host's own add");
        extra.amount = 3;
        extra.posX += 200.f;
        recs.push_back(extra);
        if (A::WriteTrunk(atv, recs) != A::TrunkWrite::Written) return HostFail("the host's own add was not written");
        const int want = NonEmpty(recs);
        UE_LOGI("[ATV-TRUNK-DRILL] host added a record itself: %d records", want);
        DC::Say(std::string(kChangedPrefix) + std::to_string(want));
        g_hPhase = HPhase::AwaitDone;
        g_hSince = now;
        return;
    }
    if (g_hPhase == HPhase::AwaitDone) {
        if (DC::PollDue() && DC::SeenHas(kDoneLine)) {
            g_hPhase = HPhase::Judged;
            UE_LOGI("[ATV-TRUNK-DRILL] host PASS: the client's take and put stood on the host and the host's own add reached the client");
            return;
        }
        if (now - g_hSince > 3 * kBoundMs) HostFail("the client never finished");
    }
}

void HostTick(coop::net::Session& s) {
    if (g_hPhase == HPhase::Seed) HostSeed(s);
    else if (g_hPhase != HPhase::Judged) HostAwait();
}

// ---- the client --------------------------------------------------------------------------------------------------

enum class Phase { Wait, Mirror, Placed, TakeSettle, TakeRecheck, AwaitHost, PutOpen, PutSettle, PutRecheck, AwaitChange, Done };
Phase    g_phase = Phase::Wait;
uint32_t g_eid = 0;
int      g_want = 0;      // the records the host said it put in
int      g_trunk0 = -1;
int      g_bag0 = -1;
int      g_changedWant = -1;
II::Item g_taken;
uint64_t g_atMs = 0;
uint64_t g_startMs = 0;

void Done(const char* verdict) {
    g_phase = Phase::Done;
    CV::Close();
    UE_LOGI("[ATV-TRUNK-DRILL] client DONE %s", verdict);
}

int TrunkNow() { return ats::TrunkCount(g_eid); }

// The screen's drag out of the trunk: the bag's copy first, then the trunk's removal.
void Take() {
    void* const atv = ats::AtvOfEid(g_eid);
    void* const inv = atv ? A::InventoryOf(atv) : nullptr;
    void* const bag = CV::PlayerComponent();
    std::vector<II::Item> recs;
    if (!inv || !bag || !A::ReadTrunk(atv, recs, 4096)) {
        Done("the ATV's trunk or the player's bag component is unreadable -- INCONCLUSIVE");
        return;
    }
    int32_t idx = -1;
    for (size_t i = 0; i < recs.size(); ++i)
        if (!II::IsEmpty(recs[i])) { idx = static_cast<int32_t>(i); break; }
    g_trunk0 = NonEmpty(recs);
    g_bag0 = ue_wrap::inventory::LivePersonalStoreCount();
    if (idx < 0 || g_trunk0 < 2 || g_bag0 < 0) {
        Done("nothing to take or the bag is unreadable -- INCONCLUSIVE");
        return;
    }
    g_taken = recs[static_cast<size_t>(idx)];
    UE_LOGI("[ATV-TRUNK-DRILL] client takes record %d (%ls) of %d from the trunk; the bag holds %d", idx,
            II::ClassLeaf(g_taken).c_str(), g_trunk0, g_bag0);
    if (!CI::AddItemAdvanced(bag, g_taken)) {
        Done("the bag refused the copy -- INCONCLUSIVE");
        return;
    }
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(inv), L"removeInventoryItem");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<int32_t>(L"indexToRemove", idx) || !f.Set<bool>(L"slotbar", false) ||
        !ue_wrap::Call(inv, f)) {
        Done("removeInventoryItem did not run -- FAIL");
        return;
    }
    g_atMs = ::GetTickCount64();
    g_phase = Phase::TakeSettle;
}

bool JudgeTake(const char* when) {
    const int trunk = TrunkNow();
    const int bag = ue_wrap::inventory::LivePersonalStoreCount();
    UE_LOGI("[ATV-TRUNK-DRILL] client take %s: trunk %d -> %d record(s), bag %d -> %d", when, g_trunk0, trunk, g_bag0, bag);
    return trunk == g_trunk0 - 1 && bag == g_bag0 + 1;
}

// The bag's item widget for the record that was taken, on the open screen.
void* BagWidget() {
    void* const cls = OI::ClassByName(L"uicomp_FLI_item_C");
    void* const bag = CV::PlayerComponent();
    if (!cls || !bag) return nullptr;
    struct Ctx { void* bag; const II::Item* want; void* found; } x{bag, &g_taken, nullptr};
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        Ctx& c = *static_cast<Ctx*>(ctx);
        if (c.found) return;
        CV::WidgetEntry w;
        if (CV::ReadWidget(obj, w) && w.owner == c.bag && !II::IsEmpty(w.item) && w.item.classPath == c.want->classPath &&
            w.item.name == c.want->name && w.item.amount == c.want->amount)
            c.found = obj;
    }, &x);
    return x.found;
}

// The put the mouse release makes: the bag's widget dropped on the trunk's canvas.
void Put() {
    void* const atv = ats::AtvOfEid(g_eid);
    void* const inv = atv ? A::InventoryOf(atv) : nullptr;
    if (!inv) { Done("the ATV is gone -- INCONCLUSIVE"); return; }
    if (CV::ViewedInventory() != inv) {
        if (!CV::OpenInventory(inv) || CV::ViewedInventory() != inv) {
            Done("the screen does not show the ATV's trunk after begin(component) -- FAIL");
            return;
        }
        g_atMs = ::GetTickCount64();
        return;   // the widgets of a freshly opened screen are found within kOpenWaitMs
    }
    void* const widget = BagWidget();
    if (!widget) {
        if (::GetTickCount64() - g_atMs > kOpenWaitMs) Done("no bag widget for the taken record on the open screen -- INCONCLUSIVE");
        return;
    }
    UE_LOGI("[ATV-TRUNK-DRILL] client drops the bag's %ls on the open trunk (the screen's enddrag)", II::ClassLeaf(g_taken).c_str());
    if (!CV::DropOnViewed(widget)) { Done("the screen's drop did not run -- INCONCLUSIVE"); return; }
    g_atMs = ::GetTickCount64();
    g_phase = Phase::PutSettle;
}

bool JudgePut(const char* when) {
    const int trunk = TrunkNow();
    const int bag = ue_wrap::inventory::LivePersonalStoreCount();
    UE_LOGI("[ATV-TRUNK-DRILL] client put %s: trunk %d -> %d record(s), bag %d -> %d", when, g_trunk0 - 1, trunk, g_bag0 + 1, bag);
    return trunk == g_trunk0 && bag == g_bag0;
}

void ClientTick() {
    const uint64_t now = ::GetTickCount64();
    if (!g_startMs) g_startMs = now;
    if (g_phase == Phase::Wait) {
        std::string rest;
        if (DC::SeenRestOf(kSeededPrefix, &rest)) {
            char* tail = nullptr;
            const int n = static_cast<int>(std::strtol(rest.c_str(), &tail, 10));
            const unsigned long eid = std::strtoul(tail, nullptr, 10);
            if (n <= 0 || eid == 0) {
                Done("the host's seeded line did not parse -- INCONCLUSIVE");
                return;
            }
            g_want = n;
            g_eid = static_cast<uint32_t>(eid);
            g_phase = Phase::Mirror;
            return;
        }
        if (now - g_startMs > 2 * kBoundMs) Done("the host never said it seeded a trunk -- INCONCLUSIVE");
        return;
    }
    if (g_phase == Phase::Mirror) {
        if (g_want > 0 && TrunkNow() >= g_want) {
            g_atMs = now;   // the host's pose and its ledger of this player settle before the take is asked
            g_phase = Phase::Placed;
            return;
        }
        if (now - g_startMs > 3 * kBoundMs) Done("the mirror of the trunk never held the seeded records -- FAIL");
        return;
    }
    if (g_phase == Phase::Placed) {
        if (now - g_atMs < 4000) return;
        Take();
        return;
    }
    if (g_phase == Phase::PutOpen) { Put(); return; }
    const uint64_t since = now - g_atMs;
    if (g_phase == Phase::TakeSettle && since >= kSettleMs) {
        if (!JudgeTake("settled")) { Done("the take did not stand on this client -- FAIL"); return; }
        g_phase = Phase::TakeRecheck;
        return;
    }
    if (g_phase == Phase::TakeRecheck && since >= kRecheckMs) {
        if (!JudgeTake("rechecked")) { Done("the take was undone after the settle -- FAIL"); return; }
        UE_LOGI("[ATV-TRUNK-DRILL] client take PASS -- the trunk is one record fewer and the bag one more");
        DC::Say(kTakenLine);
        g_atMs = now;
        g_phase = Phase::AwaitHost;
        return;
    }
    if (g_phase == Phase::AwaitHost) {
        if (DC::PollDue() && DC::SeenHas(kTakeOkLine)) {
            g_atMs = now;
            g_phase = Phase::PutOpen;
            return;
        }
        if (since > kBoundMs) Done("the host never confirmed the take -- FAIL");
        return;
    }
    if (g_phase == Phase::PutSettle && since >= kSettleMs) {
        if (!JudgePut("settled")) { Done("the put did not stand on this client -- FAIL"); return; }
        g_phase = Phase::PutRecheck;
        return;
    }
    if (g_phase == Phase::PutRecheck && since >= kRecheckMs) {
        if (!JudgePut("rechecked")) { Done("the put was undone after the settle -- FAIL"); return; }
        UE_LOGI("[ATV-TRUNK-DRILL] client put PASS -- the trunk holds %d records again and the bag is as it was", g_trunk0);
        CV::Close();
        DC::Say(kPutLine);
        g_atMs = now;
        g_phase = Phase::AwaitChange;
        return;
    }
    if (g_phase == Phase::AwaitChange) {
        if (g_changedWant < 0) {
            std::string rest;
            if (DC::PollDue() && DC::SeenRestOf(kChangedPrefix, &rest)) g_changedWant = std::atoi(rest.c_str());
            if (g_changedWant < 0 && since > kBoundMs) Done("the host never added a record of its own -- FAIL");
            if (g_changedWant >= 0) g_atMs = now;
            return;
        }
        const int n = TrunkNow();
        if (n == g_changedWant) {
            DC::Say(kDoneLine);
            Done("the take and the put stood, and the host's own add reached this copy -- PASS");
            return;
        }
        if (since > 10000) {
            UE_LOGI("[ATV-TRUNK-DRILL] client: the mirror holds %d records, the host's add makes %d", n, g_changedWant);
            Done("the host's own add never reached this copy -- FAIL");
        }
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::atv_trunk_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled()) return;
    if (!session || !session->connected()) return;
    if (coop::roster::LocalIsHost()) {
        HostTick(*session);
        return;
    }
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    if (g_phase != Phase::Done) ClientTick();
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    g_phase = Phase::Wait;
    g_hPhase = HPhase::Seed;
    g_hSince = 0;
    g_hEid = 0;
    g_seeded = 0;
    g_hostSeen = -1;
    g_hostNextMs = 0;
    g_wordAtMs = 0;
    g_eid = 0;
    g_want = 0;
    g_trunk0 = g_bag0 = -1;
    g_changedWant = -1;
    g_taken = II::Item{};
    g_atMs = g_startMs = 0;
}

}  // namespace coop::dev::atv_trunk_drill

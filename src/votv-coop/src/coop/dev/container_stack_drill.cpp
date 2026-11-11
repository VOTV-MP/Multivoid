// coop/dev/container_stack_drill.cpp -- see coop/dev/container_stack_drill.h.

#include "coop/dev/container_stack_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_pick.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_place.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/container_record_edit.h"
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
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coop::dev::container_stack_drill {
namespace {

namespace CI  = ue_wrap::container_inventory;
namespace CV  = ue_wrap::container_view;
namespace CP  = coop::dev::container_pick;
namespace DC  = coop::dev::drill_chat;
namespace DR  = coop::director;
namespace II  = ue_wrap::inventory_item;
namespace OI  = ue_wrap::object_index;
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace cre = coop::props::container_record_edit;

constexpr const char* kSeededPrefix = "cstack host seeded ";    // + the container's element id
constexpr const char* kTakeDoneLine = "cstack client take done";
constexpr const char* kMergeDoneLine = "cstack client merge done";
constexpr const char* kTakeOkLine = "cstack host take ok";   // the host read the take and holds it: the client may merge
constexpr int      kBigStack = 6, kSmallStack = 3;
constexpr uint64_t kSettleMs  = 4000;
constexpr uint64_t kRecheckMs = 8000;
constexpr uint64_t kBoundMs   = 120000;
constexpr uint64_t kOpenWaitMs = 3000;   // the widgets of a freshly opened screen are found within this
// Where both peers stand for the pick and the walk: where the container take drill's passing runs walked from.
constexpr drill_place::Spot kStandSpot{-535.f, 310.f, 6665.f, 0.f};

// The non-empty entries' amounts, in array order; the array's indexes alongside.
struct Stacks {
    std::vector<int> amount;
    std::vector<int32_t> index;
};
bool ReadStacks(void* inv, Stacks& out) {
    std::vector<II::Item> recs;
    out = Stacks{};
    if (!inv || !CI::ReadContents(inv, recs, 4096)) return false;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (II::IsEmpty(recs[i])) continue;
        out.amount.push_back(recs[i].amount);
        out.index.push_back(static_cast<int32_t>(i));
    }
    return true;
}
std::string Text(const std::vector<int>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + std::to_string(v[i]);
    return s + "]";
}
std::vector<int> Sorted(std::vector<int> v) {
    std::sort(v.begin(), v.end());
    return v;
}
const std::vector<int> kAfterSeed = {kSmallStack, kSmallStack, kBigStack};
const std::vector<int> kAfterTake = {kSmallStack, kSmallStack, kBigStack - 1};
const std::vector<int> kAfterMerge = {kBigStack - 1, 2 * kSmallStack};

// ---- the host ----------------------------------------------------------------------------------------------------

enum class HPhase { Seed, AwaitTake, AwaitMerge, Judged };
HPhase   g_hPhase = HPhase::Seed;
uint64_t g_hSince = 0;
uint64_t g_hAskMs = 0;
uint32_t g_hEid = 0;
uint64_t g_wordAtMs = 0;
drill_place::Placement g_hPlace;   // the host's placement before the pick
drill_place::Placement g_cPlace;   // the client's before its walk

void HostFail(const std::string& why) {
    g_hPhase = HPhase::Judged;
    UE_LOGW("[CSTACK-DRILL] host FAIL: %s", why.c_str());
}

bool HostStacks(Stacks& st) {
    std::vector<II::Item> recs;
    st = Stacks{};
    if (cre::ReadRecords(g_hEid, recs) != cre::EditResult::Done) return false;
    for (size_t i = 0; i < recs.size(); ++i)
        if (!II::IsEmpty(recs[i])) { st.amount.push_back(recs[i].amount); st.index.push_back(static_cast<int32_t>(i)); }
    return true;
}

void HostSeed(coop::net::Session& s) {
    const uint64_t now = ::GetTickCount64();
    if (!g_hSince) g_hSince = now;
    if (now - g_hSince > kBoundMs) return HostFail("no seed: no world container with a record, or its write never came");
    if (!s.IsSlotWorldReady(1)) return;
    // The pick ranks the containers within 5000 cm of where this player stands, and where it stands after the join is the
    // save's (the rig save was rewritten, and now starts the host on another floor than the walk's): both peers stand at the
    // spot the verified runs used (drill_place.cpp), so the container is one the client's walk reaches. A locked container is
    // not a candidate: the client's openContainer would show the hint "Cannot open" and nothing else.
    const drill_place::Result placed = drill_place::Run(g_hPlace, coop::players::Registry::Get().Local(), kStandSpot,
                                                        now, "the container pick");
    if (placed == drill_place::Result::Pending) return;
    if (placed == drill_place::Result::Failed) return HostFail("the host's placement before the container pick did not land");
    CP::Want want;
    want.minRecords = 1;
    want.skips = &CP::SkipLocked;
    CP::Pick pick;
    if (!CP::PickContainer(want, pick)) return;
    std::vector<II::Item> recs;
    if (cre::ReadRecords(pick.eid, recs) != cre::EditResult::Done) return;
    II::Item base;
    bool have = false;
    for (const II::Item& r : recs)
        if (!II::IsEmpty(r)) { base = r; have = true; break; }
    if (!have) return;
    size_t cleared = 0;
    if (cre::dev::Clear(pick.eid, cleared) != cre::EditResult::Done) return;
    II::Item big = base, a = base, b = base;
    big.amount = static_cast<uint8_t>(kBigStack);
    a.amount = b.amount = static_cast<uint8_t>(kSmallStack);
    a.posX = base.posX + 120.f;
    b.posX = base.posX + 240.f;
    const II::Item seeds[3] = {big, a, b};
    for (const II::Item& it : seeds) {
        if (cre::InsertAt(pick.eid, 255, it) != cre::EditResult::Done) return HostFail("the seed's insert was refused");
    }
    g_hEid = pick.eid;
    UE_LOGI("[CSTACK-DRILL] host cleared eid=%u (%zu records) and seeded it with stacks of %d, %d and %d of %ls", g_hEid,
            cleared, kBigStack, kSmallStack, kSmallStack, II::ClassLeaf(base).c_str());
    DC::Say(std::string(kSeededPrefix) + std::to_string(g_hEid));
    g_hAskMs = now;
    g_hPhase = HPhase::AwaitTake;
}

void HostAwait() {
    const uint64_t now = ::GetTickCount64();
    if (now - g_hAskMs > 6 * kBoundMs) return HostFail("the client never finished");
    const bool take = g_hPhase == HPhase::AwaitTake;
    if (!DC::PollDue() && !g_wordAtMs) return;
    if (!g_wordAtMs) {
        if (!DC::SeenHas(take ? kTakeDoneLine : kMergeDoneLine)) return;
        g_wordAtMs = now;
        return;
    }
    if (now - g_wordAtMs < 1500) return;
    g_wordAtMs = 0;
    Stacks st;
    if (!HostStacks(st)) return HostFail("the container's records do not read");
    const std::vector<int>& want = take ? kAfterTake : kAfterMerge;
    if (Sorted(st.amount) != Sorted(want))
        return HostFail(std::string(take ? "after the take" : "after the merge") + " the container holds " +
                        Text(Sorted(st.amount)) + ", want " + Text(Sorted(want)));
    UE_LOGI("[CSTACK-DRILL] host %s PASS: the container holds %s", take ? "take" : "merge", Text(Sorted(st.amount)).c_str());
    if (take) {
        DC::Say(kTakeOkLine);
        g_hPhase = HPhase::AwaitMerge;
        return;
    }
    g_hPhase = HPhase::Judged;
    UE_LOGI("[CSTACK-DRILL] host PASS: the part of a stack and the merge of two stacks both stand on the host");
}

void HostTick(coop::net::Session& s) {
    if (g_hPhase == HPhase::Seed) HostSeed(s);
    else if (g_hPhase == HPhase::AwaitTake || g_hPhase == HPhase::AwaitMerge) HostAwait();
}

// ---- the client --------------------------------------------------------------------------------------------------

enum class Phase { Wait, Mirror, Walking, TakeOpen, TakeSettle, TakeRecheck, AwaitHost, MergeOpen, MergeSettle, MergeRecheck, Done };
struct WalkOutcome {
    uint32_t gen = 0;
    int result = 0;   // 0 walking, 1 arrived, 2 no arrival
};
std::mutex  g_walkMu;
WalkOutcome g_walk;

Phase    g_phase = Phase::Wait;
uint32_t g_eid = 0;
void*    g_container = nullptr;
uint64_t g_atMs = 0, g_startMs = 0;
int      g_bag0 = -1;
int32_t  g_idxBig = -1, g_idxA = -1, g_idxB = -1;

void Done(const char* verdict) {
    g_phase = Phase::Done;
    CV::Close();
    UE_LOGI("[CSTACK-DRILL] client DONE %s", verdict);
}

struct WalkArg {
    std::shared_ptr<DR::DirectorGoal> goal;
    uint32_t gen;
};
DWORD WINAPI WalkerThread(LPVOID arg) {
    const WalkArg a = *std::unique_ptr<WalkArg>(static_cast<WalkArg*>(arg));
    DR::ControlManager mgr;
    DR::AddWalkToProcesses(mgr, *a.goal);
    const bool arrived = mgr.Run(*a.goal, 300) && a.goal->reached;
    std::lock_guard<std::mutex> lk(g_walkMu);
    if (g_walk.gen == a.gen) g_walk.result = arrived ? 1 : 2;
    return 0;
}
bool StartWalk(void* actor) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(actor, at)) return false;
    auto goal = std::make_shared<DR::DirectorGoal>();
    goal->reachCm = CP::kStandCm;
    goal->epoch = DR::WalkEpoch();
    goal->targetActor = actor;
    goal->targetPos = at;
    uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        g_walk.result = 0;
        gen = g_walk.gen;
    }
    UE_LOGI("[CSTACK-DRILL] client walks to eid=%u at (%.0f,%.0f,%.0f)", g_eid, at.X, at.Y, at.Z);
    auto* arg = new WalkArg{goal, gen};
    HANDLE h = ::CreateThread(nullptr, 0, &WalkerThread, arg, 0, nullptr);
    if (!h) {
        delete arg;
        return false;
    }
    ::CloseHandle(h);
    return true;
}

// The item widget the open screen holds for entry `index` of `inv`.
void* FindWidget(void* inv, int32_t index) {
    void* const cls = OI::ClassByName(L"uicomp_FLI_item_C");
    if (!cls) return nullptr;
    struct Ctx { void* inv; int32_t index; void* found; } x{inv, index, nullptr};
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        Ctx& c = *static_cast<Ctx*>(ctx);
        if (c.found) return;
        CV::WidgetEntry w;
        if (CV::ReadWidget(obj, w) && w.owner == c.inv && w.index == c.index) c.found = obj;
    }, &x);
    return x.found;
}

bool SetScreenFlag(void* screen, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(screen), name, off, mask)) return false;
    uint8_t* const b = static_cast<uint8_t*>(screen) + off;
    *b = on ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
    return true;
}

// The right-button drag of the stack of 6: the screen's own beginDrag on that stack's widget takes one unit.
void DoTake() {
    void* const inv = CI::InventoryOf(g_container);
    void* const screen = CV::Screen();
    Stacks st;
    if (!inv || !screen || !ReadStacks(inv, st)) { Done("the container or the screen is unreadable -- INCONCLUSIVE"); return; }
    g_idxBig = -1;
    for (size_t i = 0; i < st.amount.size(); ++i)
        if (st.amount[i] == kBigStack) g_idxBig = st.index[i];
    if (g_idxBig < 0 || Sorted(st.amount) != Sorted(kAfterSeed)) {
        Done(("the mirror does not hold the seeded stacks, it holds " + Text(st.amount) + " -- INCONCLUSIVE").c_str());
        return;
    }
    void* const widget = FindWidget(inv, g_idxBig);
    if (!widget) {
        if (::GetTickCount64() - g_atMs > kOpenWaitMs) Done("no item widget for the stack on the open screen -- INCONCLUSIVE");
        return;
    }
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(screen), L"beginDrag");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"item", widget)) { Done("beginDrag does not resolve -- FAIL"); return; }
    if (!SetScreenFlag(screen, L"dragRMB", true) && !SetScreenFlag(screen, L"dragrmb", true)) {
        Done("the screen's dragRMB does not resolve -- INCONCLUSIVE");
        return;
    }
    g_bag0 = ue_wrap::inventory::LivePersonalStoreCount();
    UE_LOGI("[CSTACK-DRILL] client drags one unit off the stack of %d (entry %d) with the right button; stacks %s, bag %d",
            kBigStack, g_idxBig, Text(st.amount).c_str(), g_bag0);
    const bool ran = ue_wrap::Call(screen, f);
    if (!SetScreenFlag(screen, L"dragRMB", false)) SetScreenFlag(screen, L"dragrmb", false);
    if (!ran) { Done("beginDrag did not run -- FAIL"); return; }
    g_atMs = ::GetTickCount64();
    g_phase = Phase::TakeSettle;
}

bool JudgeTake(const char* when) {
    Stacks st;
    const bool ok = ReadStacks(CI::InventoryOf(g_container), st);
    const int bag = ue_wrap::inventory::LivePersonalStoreCount();
    UE_LOGI("[CSTACK-DRILL] client take %s: stacks %s, bag %d -> %d", when, ok ? Text(st.amount).c_str() : "unreadable",
            g_bag0, bag);
    return ok && Sorted(st.amount) == Sorted(kAfterTake) && bag == g_bag0 + 1;
}

// The merge the canvas's combine action makes: addToStack on the stack that stays, then the other leaves.
void DoMerge() {
    void* const inv = CI::InventoryOf(g_container);
    std::vector<II::Item> recs;
    if (!inv || !CI::ReadContents(inv, recs, 4096)) { Done("the container is unreadable -- INCONCLUSIVE"); return; }
    g_idxA = g_idxB = -1;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (II::IsEmpty(recs[i]) || recs[i].amount != kSmallStack) continue;
        if (g_idxA < 0) g_idxA = static_cast<int32_t>(i);
        else if (g_idxB < 0) g_idxB = static_cast<int32_t>(i);
    }
    if (g_idxA < 0 || g_idxB < 0) { Done("the two stacks of 3 are not both there -- INCONCLUSIVE"); return; }
    void* const widget = FindWidget(inv, g_idxA);
    if (!widget) {
        if (::GetTickCount64() - g_atMs > kOpenWaitMs) Done("no item widget for the stack that stays -- INCONCLUSIVE");
        return;
    }
    void* const addFn = R::FindDispatchFunctionCached(R::ClassOf(widget), L"addToStack");
    ue_wrap::ParamFrame af(addFn);
    const int32_t dataOff = addFn && af.valid() ? af.ParamOffset(L"itemData") : -1;
    if (!addFn || dataOff < 0 || !af.Set<uint8_t>(L"add", static_cast<uint8_t>(kSmallStack)) ||
        !II::Write(static_cast<uint8_t*>(af.data()) + dataOff, recs[static_cast<size_t>(g_idxB)])) {
        Done("addToStack does not resolve or its entry did not build -- FAIL");
        return;
    }
    UE_LOGI("[CSTACK-DRILL] client merges entry %d into entry %d (addToStack, then the other leaves)", g_idxB, g_idxA);
    if (!ue_wrap::Call(widget, af)) { Done("addToStack did not run -- FAIL"); return; }
    void* const rmFn = R::FindDispatchFunctionCached(R::ClassOf(inv), L"removeInventoryItem");
    ue_wrap::ParamFrame rf(rmFn);
    if (!rmFn || !rf.valid() || !rf.Set<int32_t>(L"indexToRemove", g_idxB) || !rf.Set<bool>(L"slotbar", false) ||
        !ue_wrap::Call(inv, rf)) {
        Done("removeInventoryItem did not run -- FAIL");
        return;
    }
    g_atMs = ::GetTickCount64();
    g_phase = Phase::MergeSettle;
}

bool JudgeMerge(const char* when) {
    Stacks st;
    const bool ok = ReadStacks(CI::InventoryOf(g_container), st);
    UE_LOGI("[CSTACK-DRILL] client merge %s: stacks %s", when, ok ? Text(st.amount).c_str() : "unreadable");
    return ok && Sorted(st.amount) == Sorted(kAfterMerge);
}

void ClientTick() {
    const uint64_t now = ::GetTickCount64();
    if (!g_startMs) g_startMs = now;
    if (g_phase == Phase::Wait) {
        std::string rest;
        if (DC::SeenRestOf(kSeededPrefix, &rest)) {
            g_eid = static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 10));
            g_phase = Phase::Mirror;
            return;
        }
        if (now - g_startMs > 2 * kBoundMs) Done("the host never said it seeded a container -- INCONCLUSIVE");
        return;
    }
    if (g_phase == Phase::Mirror) {
        void* const actor = g_eid ? coop::element::LivePropActor(g_eid) : nullptr;
        Stacks st;
        if (actor && ReadStacks(CI::InventoryOf(actor), st) && Sorted(st.amount) == Sorted(kAfterSeed)) {
            g_container = actor;
            const drill_place::Result placed = drill_place::Run(g_cPlace, coop::players::Registry::Get().Local(), kStandSpot,
                                                                now, "the container walk");
            if (placed == drill_place::Result::Pending) return;
            if (placed == drill_place::Result::Failed) { Done("the placement before the container walk did not land -- INCONCLUSIVE"); return; }
            if (!StartWalk(actor)) { Done("the walk did not start -- INCONCLUSIVE"); return; }
            g_phase = Phase::Walking;
            return;
        }
        if (now - g_startMs > 3 * kBoundMs) Done("the mirror of the container never held the seeded stacks -- FAIL");
        return;
    }
    if (g_phase == Phase::Walking) {
        int result = 0;
        {
            std::lock_guard<std::mutex> lk(g_walkMu);
            result = g_walk.result;
        }
        if (result == 0) return;
        if (result == 2) { Done("the walk to the container did not arrive -- INCONCLUSIVE"); return; }
        if (!CV::Open(g_container) || CV::Viewed() != g_container) {
            Done("the inventory screen does not show the container after openContainer -- FAIL");
            return;
        }
        g_atMs = now;
        g_phase = Phase::TakeOpen;
        return;
    }
    if (g_phase == Phase::TakeOpen) { DoTake(); return; }
    if (g_phase == Phase::MergeOpen) { DoMerge(); return; }
    const uint64_t since = now - g_atMs;
    if (g_phase == Phase::TakeSettle && since >= kSettleMs) {
        if (!JudgeTake("settled")) {
            // A stack the screen's takeFromStack leaves whole is an item that does not stack: not the port's.
            Stacks st;
            if (ReadStacks(CI::InventoryOf(g_container), st) && Sorted(st.amount) == Sorted(kAfterSeed)) {
                Done("the stack was not lowered -- INCONCLUSIVE if the item does not stack, FAIL if the seat refused it");
                return;
            }
            Done("the part of the stack did not stand on this client -- FAIL");
            return;
        }
        g_phase = Phase::TakeRecheck;
        return;
    }
    if (g_phase == Phase::TakeRecheck && since >= kRecheckMs) {
        if (!JudgeTake("rechecked")) { Done("the part of the stack was undone after the settle -- FAIL"); return; }
        UE_LOGI("[CSTACK-DRILL] client take PASS -- the stack reads %d, the bag one more", kBigStack - 1);
        DC::Say(kTakeDoneLine);
        CV::Close();
        g_atMs = now;
        g_phase = Phase::AwaitHost;
        return;
    }
    if (g_phase == Phase::AwaitHost) {
        if (DC::PollDue() && DC::SeenHas(kTakeOkLine)) {
            if (!CV::Open(g_container)) { Done("the screen did not reopen for the merge -- FAIL"); return; }
            g_atMs = now;
            g_phase = Phase::MergeOpen;
            return;
        }
        if (since > kBoundMs) Done("the host never confirmed the take -- FAIL");
        return;
    }
    if (g_phase == Phase::MergeSettle && since >= kSettleMs) {
        if (!JudgeMerge("settled")) { Done("the merge did not stand on this client -- FAIL"); return; }
        g_phase = Phase::MergeRecheck;
        return;
    }
    if (g_phase == Phase::MergeRecheck && since >= kRecheckMs) {
        if (!JudgeMerge("rechecked")) { Done("the merge was undone after the settle -- FAIL"); return; }
        DC::Say(kMergeDoneLine);
        Done("the part of a stack and the merge of two stacks both stood -- PASS");
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::container_stack_drill);
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
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        ++g_walk.gen;
        g_walk.result = 0;
    }
    g_phase = Phase::Wait;
    g_hPhase = HPhase::Seed;
    g_hSince = g_hAskMs = g_wordAtMs = 0;
    g_hEid = 0;
    g_hPlace = drill_place::Placement{};
    g_cPlace = drill_place::Placement{};
    g_eid = 0;
    g_container = nullptr;
    g_atMs = g_startMs = 0;
    g_bag0 = -1;
    g_idxBig = g_idxA = g_idxB = -1;
}

}  // namespace coop::dev::container_stack_drill

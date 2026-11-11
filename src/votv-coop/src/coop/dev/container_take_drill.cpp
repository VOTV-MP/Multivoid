// coop/dev/container_take_drill.cpp -- see coop/dev/container_take_drill.h.

#include "coop/dev/container_take_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_pick.h"
#include "coop/dev/drill_place.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/container_contents_sync.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace coop::dev::container_take_drill {
namespace {

namespace CC  = coop::props::container_contents_sync;
namespace CI  = ue_wrap::container_inventory;
namespace CV  = ue_wrap::container_view;
namespace CP  = coop::dev::container_pick;
namespace DR  = coop::director;
namespace R   = ue_wrap::reflection;
namespace II  = ue_wrap::inventory_item;
namespace E   = ue_wrap::engine;

constexpr uint64_t kSettleMs  = 4000;   // the host's answer, and the truth it would send after a refusal, are in by then
constexpr uint64_t kRecheckMs = 8000;   // a refusal's give-back would have landed by here

enum class Phase { Unpicked, Walking, Settling, Rechecking, PutHeld, PutSettling, PutRechecking, Done };

// What the walker thread hands the game thread, under one lock with the session generation it was started in.
struct WalkOutcome {
    uint32_t gen = 0;
    int result = 0;   // 0 walking, 1 arrived, 2 no arrival
};
std::mutex g_walkMu;
WalkOutcome g_walk;

Phase g_phase = Phase::Unpicked;
drill_place::Placement g_place;   // the client's placement before the pick and the walk (see drill_place.cpp)
uint32_t g_eid = 0;
void* g_container = nullptr;
int32_t g_idx = -1;
int g_records0 = -1;
int g_bag0 = -1;
uint64_t g_atMs = 0;
std::map<uint32_t, int32_t> g_hostSeen;   // the host's last logged record count per container
uint64_t g_hostNextMs = 0;

bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// The run input held down for the put (the player keeps running while the host answers); released when the drill ends.
int32_t g_runOff = -1;
uint8_t g_runMask = 0;
uint8_t g_runWas = 0;
void ReleaseRun() {
    void* const player = coop::players::Registry::Get().Local();
    if (player && g_runOff >= 0 && R::IsLive(player)) {
        uint8_t* const b = static_cast<uint8_t*>(player) + g_runOff;
        *b = static_cast<uint8_t>((*b & ~g_runMask) | g_runWas);
    }
    g_runOff = -1;
}

// The container's `locked` flag: a locked container's openContainer shows the hint "Cannot open" and opens nothing.
bool LockedFlag(void* container, bool* known) { return CP::IsLocked(container, known); }

void Done(const char* verdict) {
    ReleaseRun();
    g_phase = Phase::Done;
    UE_LOGI("[CTAKE-DRILL] client DONE %s", verdict);
}

int NonEmpty(void* inventory) {
    std::vector<II::Item> recs;
    if (!CI::ReadContents(inventory, recs, 4096)) return -1;
    int n = 0;
    for (const II::Item& r : recs) n += II::IsEmpty(r) ? 0 : 1;
    return n;
}

struct WalkArg {
    std::shared_ptr<DR::DirectorGoal> goal;
    uint32_t gen;
    int seconds;
};

DWORD WINAPI WalkerThread(LPVOID arg) {
    const WalkArg a = *std::unique_ptr<WalkArg>(static_cast<WalkArg*>(arg));
    DR::ControlManager mgr;
    DR::AddWalkToProcesses(mgr, *a.goal);
    const bool arrived = mgr.Run(*a.goal, a.seconds) && a.goal->reached;
    std::lock_guard<std::mutex> lk(g_walkMu);
    if (g_walk.gen == a.gen) g_walk.result = arrived ? 1 : 2;
    return 0;
}

void StartWalk(const CP::Pick& pick) {
    auto goal = std::make_shared<DR::DirectorGoal>();
    goal->reachCm = CP::kStandCm;
    goal->epoch = DR::WalkEpoch();
    goal->targetActor = pick.actor;
    goal->targetPos = pick.pos;
    uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        g_walk.result = 0;
        gen = g_walk.gen;
    }
    auto* arg = new WalkArg{goal, gen, std::clamp(CP::WalkSeconds(pick.routeCm), 90, 900)};
    HANDLE h = ::CreateThread(nullptr, 0, &WalkerThread, arg, 0, nullptr);
    if (!h) {
        delete arg;
        std::lock_guard<std::mutex> lk(g_walkMu);
        g_walk.result = 2;
        return;
    }
    ::CloseHandle(h);
}

// The screen's drag out of a container: the bag's copy first, then the container's removal.
void Act() {
    void* const inv = CI::InventoryOf(g_container);
    if (!inv) { Done("the container has no inventory component -- FAIL"); return; }
    const bool opened = CV::Open(g_container);
    if (!opened || CV::Viewed() != g_container) {
        bool lockedKnown = false;
        const bool locked = LockedFlag(g_container, &lockedKnown);
        UE_LOGI("[CTAKE-DRILL] open diagnosis: openContainer ran=%d, locked=%d (known=%d), screen=%p, screen shows component=%p "
                "(the container's own %p), Viewed()=%p", opened ? 1 : 0, locked ? 1 : 0, lockedKnown ? 1 : 0, CV::Screen(),
                CV::ViewedInventory(), inv, CV::Viewed());
        Done(locked ? "the container is locked: the game refuses to open it (hint 'Cannot open') -- INCONCLUSIVE"
                    : "the inventory screen does not show the container after opencontainer -- FAIL");
        return;
    }
    void* const bag = CV::PlayerComponent();
    std::vector<II::Item> recs;
    if (!bag || !CI::ReadContents(inv, recs, 4096)) {
        Done("the bag or the container's records are unreadable -- FAIL");
        return;
    }
    g_idx = -1;
    for (size_t i = 0; i < recs.size(); ++i)
        if (!II::IsEmpty(recs[i])) { g_idx = static_cast<int32_t>(i); break; }
    g_records0 = NonEmpty(inv);
    g_bag0 = ue_wrap::inventory::LivePersonalStoreCount();
    if (g_idx < 0 || g_records0 < 2 || g_bag0 < 0) {
        Done("nothing to take or the bag is unreadable -- INCONCLUSIVE");
        return;
    }
    const II::Item item = recs[static_cast<size_t>(g_idx)];
    UE_LOGI("[CTAKE-DRILL] client takes record %d (%ls) of %d from eid=%u; the bag holds %d", g_idx,
            II::ClassLeaf(item).c_str(), g_records0, g_eid, g_bag0);
    if (!CI::AddItemAdvanced(bag, item)) {
        Done("the bag refused the copy -- INCONCLUSIVE");
        return;
    }
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(inv), L"removeInventoryItem");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<int32_t>(L"indexToRemove", g_idx) || !f.Set<bool>(L"slotbar", false) ||
        !ue_wrap::Call(inv, f)) {
        Done("removeInventoryItem did not run -- FAIL");
        return;
    }
    g_atMs = ::GetTickCount64();
    g_phase = Phase::Settling;
}

bool Judge(const char* when) {
    void* const inv = g_container ? CI::InventoryOf(g_container) : nullptr;
    const int records = inv ? NonEmpty(inv) : -1;
    const int bag = ue_wrap::inventory::LivePersonalStoreCount();
    UE_LOGI("[CTAKE-DRILL] client %s: container %d -> %d record(s), bag %d -> %d", when, g_records0, records, g_bag0,
            bag);
    return records == g_records0 - 1 && bag == g_bag0 + 1;
}

int g_put0 = -1;

// The hold-and-run put (the container's playerUsedOn with the hand's item and the run input down), after the take:
// a crowbar into the hand as a pickup puts it there, then the verb a run-and-use makes.
void PutBegin() {
    CV::Close();
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"crowbar", L"prop_crowbar_C")) {
        Done("the take PASSed; the put is INCONCLUSIVE, the crowbar could not be taken into the hand");
        return;
    }
    g_atMs = ::GetTickCount64();
    g_phase = Phase::PutHeld;
}

void PutAct() {
    void* const player = coop::players::Registry::Get().Local();
    void* const inv = g_container ? CI::InventoryOf(g_container) : nullptr;
    E::MainPlayerGrabState hand{};
    static R::InstanceOffset sName{L"holdingObjectName_main"};
    const int32_t nameOff = player ? sName.Of(player) : -1;
    if (!player || !inv || nameOff < 0 || !E::ReadMainPlayerGrabState(player, hand) || !hand.mainHand) {
        if (::GetTickCount64() - g_atMs > 5000) Done("the take PASSed; the put is INCONCLUSIVE, the hand holds nothing");
        return;
    }
    R::FName holdName{};
    std::memcpy(&holdName, static_cast<const uint8_t*>(player) + nameOff, sizeof(holdName));
    void* const rootFn = R::FindDispatchFunctionCached(R::ClassOf(g_container), L"K2_GetRootComponent");
    ue_wrap::ParamFrame rf(rootFn);
    ue_wrap::FVector at{};
    if (!rootFn || !rf.valid() || !ue_wrap::Call(g_container, rf) || !E::TryGetActorLocation(g_container, at)) {
        Done("the take PASSed; the put is INCONCLUSIVE, the container's root could not be read");
        return;
    }
    void* const root = rf.Get<void*>(L"ReturnValue");
    int32_t runOff = -1;
    uint8_t runMask = 0;
    if (!R::FindBoolProperty(R::ClassOf(player), L"input_run", runOff, runMask)) {
        Done("the take PASSed; the put is INCONCLUSIVE, input_run does not resolve");
        return;
    }
    uint8_t* const runByte = static_cast<uint8_t*>(player) + runOff;
    g_put0 = NonEmpty(inv);
    {   // The put fit's numbers on this peer: the volumes the host reads for a name and for the held actor, and the
        // container's bound. A volume of 0 would leave the capacity test off.
        CI::PutTerms terms{};
        const bool termsOk = CI::ReadPutTerms(g_container, terms);
        float heldVol = 0.f, bound = 0.f;
        const bool heldOk = ue_wrap::prop_gather::ReadVolumeAndBound(hand.mainHand, heldVol, bound);
        UE_LOGI("[CTAKE-DRILL] put fit: container total=%.1f max=%.1f infinite=%d (read=%d); held actor mesh volume=%.1f "
                "(read=%d)", terms.totalVolume, terms.maxVolume, terms.infinite ? 1 : 0, termsOk ? 1 : 0, heldVol,
                heldOk ? 1 : 0);
        {   // The game instance's volume tables Get Volume reads, and the volumes it answers for the container itself.
            void* const inst = ue_wrap::world_singleton::GameInstance();
            auto count = [&](const wchar_t* field) {
                const int32_t off = inst ? R::FindPropertyOffset(R::ClassOf(inst), field) : -1;
                return off < 0 ? -1 : ue_wrap::save_record::ReadArr(inst, off).num;
            };
            float contVol = 0.f, contBound = 0.f;
            const bool contOk = ue_wrap::prop_gather::ReadVolumeAndBound(g_container, contVol, contBound);
            UE_LOGI("[CTAKE-DRILL] put fit: game instance prop_names=%d prop_volumes=%d prop_meshes=%d; the container's own mesh "
                    "volume=%.1f (read=%d)", count(L"prop_names"), count(L"prop_volumes"), count(L"prop_meshes"), contVol,
                    contOk ? 1 : 0);
        }
        for (const wchar_t* nm : {L"crowbar", L"soap", L"drive", L"cube"}) {
            float v = 0.f;
            bool known = false;
            const bool ok = ue_wrap::prop_gather::VolumeOfName(nm, v, known);
            UE_LOGI("[CTAKE-DRILL] put fit: name '%ls' read=%d known=%d volume=%.1f -> %.0f of them fit the room left", nm,
                    ok ? 1 : 0, known ? 1 : 0, v, v > 0.f ? (terms.maxVolume - terms.totalVolume) / v : -1.0);
        }
    }
    UE_LOGI("[CTAKE-DRILL] client puts the held %ls into eid=%u (%d record(s)) with the run input down",
            R::ClassNameOf(hand.mainHand).c_str(), g_eid, g_put0);
    g_runOff = runOff;
    g_runMask = runMask;
    g_runWas = static_cast<uint8_t>(*runByte & runMask);
    *runByte = static_cast<uint8_t>(*runByte | runMask);
    const bool ran = ue_wrap::prop_use::CallPlayerUsedOn(g_container, player, root, at, root, hand.mainHand, holdName);
    if (!ran) {
        Done("the put's playerUsedOn did not run -- FAIL");
        return;
    }
    g_atMs = ::GetTickCount64();
    g_phase = Phase::PutSettling;
}

bool JudgePut(const char* when) {
    void* const inv = g_container ? CI::InventoryOf(g_container) : nullptr;
    const int records = inv ? NonEmpty(inv) : -1;
    UE_LOGI("[CTAKE-DRILL] client put %s: container %d -> %d record(s)", when, g_put0, records);
    return records == g_put0 + 1;
}

void ClientTick() {
    if (g_phase == Phase::Unpicked) {
        // The walk starts from the spot the verified runs used (drill_place.cpp): the walk is not what this drill judges.
        constexpr drill_place::Spot kWalkStart{-535.f, 310.f, 6665.f, 0.f};
        const drill_place::Result placed = drill_place::Run(g_place, coop::players::Registry::Get().Local(), kWalkStart,
                                                            ::GetTickCount64(), "the container walk");
        if (placed == drill_place::Result::Pending) return;
        if (placed == drill_place::Result::Failed) {
            Done("the placement before the container walk did not land -- INCONCLUSIVE");
            return;
        }
        CP::Pick pick;
        CP::Want want;
        want.minRecords = 2;
        want.skips = &CP::SkipLocked;
        if (!CP::PickContainer(want, pick)) {
            Done("no world container with two records is reachable -- INCONCLUSIVE");
            return;
        }
        g_eid = pick.eid;
        g_container = pick.actor;
        bool lockedKnown = false;
        const bool locked = LockedFlag(pick.actor, &lockedKnown);
        UE_LOGI("[CTAKE-DRILL] client picked eid=%u (%ls), %d record(s), route %.0f cm, locked=%d (known=%d)", pick.eid,
                R::ClassNameOf(pick.actor).c_str(), pick.records, pick.routeCm, locked ? 1 : 0, lockedKnown ? 1 : 0);
        StartWalk(pick);
        g_phase = Phase::Walking;
        return;
    }
    if (g_phase == Phase::Walking) {
        int result = 0;
        {
            std::lock_guard<std::mutex> lk(g_walkMu);
            result = g_walk.result;
        }
        if (result == 0) return;
        if (result == 2) {
            Done("the walk to the container did not arrive -- INCONCLUSIVE");
            return;
        }
        Act();
        return;
    }
    const uint64_t since = ::GetTickCount64() - g_atMs;
    if (g_phase == Phase::Settling && since >= kSettleMs) {
        if (!Judge("settled")) {
            Done("the take did not stand on this client -- FAIL");
            return;
        }
        g_phase = Phase::Rechecking;
        return;
    }
    if (g_phase == Phase::Rechecking && since >= kRecheckMs) {
        if (!Judge("rechecked")) {
            CV::Close();
            Done("the take was undone after the settle -- FAIL");
            return;
        }
        UE_LOGI("[CTAKE-DRILL] client take PASS -- the container is one record fewer and the bag one more");
        PutBegin();
        return;
    }
    if (g_phase == Phase::PutHeld) {
        if (since >= 500) PutAct();
        return;
    }
    if (g_phase == Phase::PutSettling && since >= kSettleMs) {
        if (!JudgePut("settled")) {
            Done("the take PASSed; the put did not stand on this client -- FAIL");
            return;
        }
        g_phase = Phase::PutRechecking;
        return;
    }
    if (g_phase == Phase::PutRechecking && since >= kRecheckMs) {
        Done(JudgePut("rechecked") ? "the take and the hold-and-run put both stood -- PASS"
                                   : "the take PASSed; the put was undone after the settle -- FAIL");
    }
}

void CollectOne(uint32_t eid, void* ctx) { static_cast<std::vector<uint32_t>*>(ctx)->push_back(eid); }

// The host's truth: every change of a world container's record count.
void HostTick() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_hostNextMs) return;
    g_hostNextMs = now + 500;
    std::vector<uint32_t> eids;
    CC::ForEachWorldContainer(&CollectOne, &eids);
    for (const uint32_t eid : eids) {
        int32_t records = -1;
        float vol = 0.f;
        if (!CC::ContentsDigest(eid, records, vol)) continue;
        const auto it = g_hostSeen.find(eid);
        if (it == g_hostSeen.end()) {
            g_hostSeen[eid] = records;
            continue;
        }
        if (it->second != records) {
            UE_LOGI("[CTAKE-DRILL] host eid=%u records %d -> %d", eid, it->second, records);
            it->second = records;
        }
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::container_take_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled()) return;
    if (!session || !session->connected() || !RoleIsReady()) return;
    if (coop::roster::LocalIsHost()) {
        HostTick();
        return;
    }
    if (g_phase != Phase::Done) ClientTick();
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        ++g_walk.gen;
        g_walk.result = 0;
    }
    g_phase = Phase::Unpicked;
    g_place = drill_place::Placement{};
    g_eid = 0;
    g_container = nullptr;
    g_idx = -1;
    g_hostSeen.clear();
}

}  // namespace coop::dev::container_take_drill

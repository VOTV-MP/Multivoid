// coop/dev/drone_cargo_take_drill.cpp -- see coop/dev/drone_cargo_take_drill.h.

#include "coop/dev/drone_cargo_take_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_pick.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/drone_cargo_sync.h"
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
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::drone_cargo_take_drill {
namespace {

namespace CI  = ue_wrap::container_inventory;
namespace CV  = ue_wrap::container_view;
namespace CP  = coop::dev::container_pick;
namespace DC  = coop::dev::drill_chat;
namespace DRN = ue_wrap::drone;
namespace II  = ue_wrap::inventory_item;
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;

constexpr const char* kSeededPrefix = "dcargo host seeded ";   // + the number of records the host put in the cargo
constexpr const char* kTakenLine    = "dcargo client taken";   // the client's take stood on its own copy
constexpr uint64_t kSettleMs  = 4000;   // the host's answer, and the truth it would send after a refusal, are in by then
constexpr uint64_t kRecheckMs = 8000;   // a refusal's give-back would have landed by here
constexpr uint64_t kBoundMs   = 120000; // a step that waits on the other peer or the world gives up after this

int NonEmpty(const std::vector<II::Item>& recs) {
    int n = 0;
    for (const II::Item& r : recs) n += II::IsEmpty(r) ? 0 : 1;
    return n;
}

int CargoNonEmpty(void* drone) {
    std::vector<II::Item> recs;
    return drone && DRN::ReadCargo(drone, recs, 4096) ? NonEmpty(recs) : -1;
}

// ---- the host ----------------------------------------------------------------------------------------------------

enum class HPhase { Seed, Watch, Judged };
HPhase   g_hPhase = HPhase::Seed;
uint64_t g_hSince = 0;
int      g_seeded = 0;
int      g_hostSeen = -1;
uint64_t g_hostNextMs = 0;
uint64_t g_takenAtMs = 0;

void HostFail(const char* why) {
    g_hPhase = HPhase::Judged;
    UE_LOGW("[DRONE-CARGO-DRILL] host FAIL: %s", why);
}

void HostSeed(coop::net::Session& s) {
    const uint64_t now = ::GetTickCount64();
    if (!g_hSince) g_hSince = now;
    if (now - g_hSince > kBoundMs) return HostFail("no seed: the drone, a world container record or the cargo write never came");
    if (!s.IsSlotWorldReady(1)) return;
    void* const drone = DRN::Find();
    if (!drone) return;
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
    switch (DRN::WriteCargo(drone, items)) {
    case DRN::CargoWrite::NotReady: return;
    case DRN::CargoWrite::Refused:  return HostFail("the drone's cargo write was refused");
    case DRN::CargoWrite::Written:  break;
    }
    // The dormant drone is parked tens of thousands of centimetres off, where no route leads and where a teleport would drop
    // a player from the sky. Reach is judged on the HOST's copy of the drone against the client's puppet, and the rig's
    // client starts where the host stands (join_at_host), so the drone is set down beside the host's player.
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!me || !E::TryGetActorLocation(me, at)) return;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
    const ue_wrap::FVector put{at.X + fwd.X * 120.f, at.Y + fwd.Y * 120.f, at.Z};
    if (!E::SetActorLocation(drone, put)) return HostFail("the drone could not be set down beside the host");
    g_seeded = 2;
    UE_LOGI("[DRONE-CARGO-DRILL] host set the drone down at (%.0f,%.0f,%.0f) and seeded its cargo with %d records of %ls (copied off eid=%u)", put.X, put.Y, put.Z, g_seeded,
            II::ClassLeaf(first).c_str(), pick.eid);
    DC::Say(std::string(kSeededPrefix) + std::to_string(g_seeded));
    g_hPhase = HPhase::Watch;
}

void HostWatch() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_hostNextMs) return;
    g_hostNextMs = now + 500;
    const int n = CargoNonEmpty(DRN::Find());
    if (n != g_hostSeen) {
        UE_LOGI("[DRONE-CARGO-DRILL] host cargo records %d -> %d", g_hostSeen, n);
        g_hostSeen = n;
    }
    if (!g_takenAtMs) {
        if (DC::PollDue() && DC::SeenHas(kTakenLine)) g_takenAtMs = now;
        return;
    }
    if (now - g_takenAtMs < 2000) return;
    g_hPhase = HPhase::Judged;
    if (n == g_seeded - 1)
        UE_LOGI("[DRONE-CARGO-DRILL] host PASS: the cargo holds %d of %d records after the client's take", n, g_seeded);
    else
        UE_LOGW("[DRONE-CARGO-DRILL] host FAIL: the cargo holds %d records after the client's take, want %d", n,
                g_seeded - 1);
}

void HostTick(coop::net::Session& s) {
    if (g_hPhase == HPhase::Seed) HostSeed(s);
    else if (g_hPhase == HPhase::Watch) HostWatch();
}

// ---- the client --------------------------------------------------------------------------------------------------

enum class Phase { Wait, Mirror, Placed, Settling, Rechecking, Done };
Phase    g_phase = Phase::Wait;
int      g_want = 0;      // the records the host said it put in
int      g_cargo0 = -1;
int      g_bag0 = -1;
int32_t  g_idx = -1;
uint64_t g_atMs = 0;
uint64_t g_startMs = 0;

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[DRONE-CARGO-DRILL] client DONE %s", verdict);
}

// The screen's drag out of the cargo: the bag's copy first, then the cargo's removal.
void Act(void* drone) {
    void* const inv = DRN::InventoryOf(drone);
    void* const bag = CV::PlayerComponent();
    std::vector<II::Item> recs;
    if (!inv || !bag || !DRN::ReadCargo(drone, recs, 4096)) {
        Done("the drone's cargo or the player's bag component is unreadable -- INCONCLUSIVE");
        return;
    }
    g_idx = -1;
    for (size_t i = 0; i < recs.size(); ++i)
        if (!II::IsEmpty(recs[i])) { g_idx = static_cast<int32_t>(i); break; }
    g_cargo0 = NonEmpty(recs);
    g_bag0 = ue_wrap::inventory::LivePersonalStoreCount();
    if (g_idx < 0 || g_cargo0 < 2 || g_bag0 < 0) {
        Done("nothing to take or the bag is unreadable -- INCONCLUSIVE");
        return;
    }
    const II::Item item = recs[static_cast<size_t>(g_idx)];
    UE_LOGI("[DRONE-CARGO-DRILL] client takes record %d (%ls) of %d from the drone's cargo; the bag holds %d", g_idx,
            II::ClassLeaf(item).c_str(), g_cargo0, g_bag0);
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
    const int cargo = CargoNonEmpty(DRN::Find());
    const int bag = ue_wrap::inventory::LivePersonalStoreCount();
    UE_LOGI("[DRONE-CARGO-DRILL] client %s: cargo %d -> %d record(s), bag %d -> %d", when, g_cargo0, cargo, g_bag0, bag);
    return cargo == g_cargo0 - 1 && bag == g_bag0 + 1;
}

void ClientTick() {
    const uint64_t now = ::GetTickCount64();
    if (!g_startMs) g_startMs = now;
    if (g_phase == Phase::Wait) {
        std::string rest;
        if (DC::SeenRestOf(kSeededPrefix, &rest)) {
            g_want = std::atoi(rest.c_str());
            g_phase = Phase::Mirror;
            return;
        }
        if (now - g_startMs > 2 * kBoundMs) Done("the host never said it seeded the cargo -- INCONCLUSIVE");
        return;
    }
    if (g_phase == Phase::Mirror) {
        void* const drone = DRN::Find();
        if (drone && g_want > 0 && CargoNonEmpty(drone) >= g_want) {
            g_atMs = now;   // the host's pose and its ledger of this player settle before the take is asked
            g_phase = Phase::Placed;
            return;
        }
        if (now - g_startMs > 3 * kBoundMs) Done("the mirror of the cargo never held the seeded records -- FAIL");
        return;
    }
    if (g_phase == Phase::Placed) {
        if (now - g_atMs < 4000) return;
        void* const drone = DRN::Find();
        if (!drone) { Done("the drone is gone -- INCONCLUSIVE"); return; }
        Act(drone);
        return;
    }
    const uint64_t since = now - g_atMs;
    if (g_phase == Phase::Settling && since >= kSettleMs) {
        if (!Judge("settled")) { Done("the take did not stand on this client -- FAIL"); return; }
        g_phase = Phase::Rechecking;
        return;
    }
    if (g_phase == Phase::Rechecking && since >= kRecheckMs) {
        if (!Judge("rechecked")) { Done("the take was undone after the settle -- FAIL"); return; }
        DC::Say(kTakenLine);
        Done("the cargo is one record fewer and the bag one more, and both still hold -- PASS");
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::drone_cargo_take_drill);
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
    g_seeded = 0;
    g_hostSeen = -1;
    g_takenAtMs = 0;
    g_want = 0;
    g_cargo0 = g_bag0 = -1;
    g_idx = -1;
    g_startMs = 0;
}

}  // namespace coop::dev::drone_cargo_take_drill

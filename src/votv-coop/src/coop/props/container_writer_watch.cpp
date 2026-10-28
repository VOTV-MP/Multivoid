// coop/props/container_writer_watch.cpp -- the addObject / takeObj watch on a container's inventory:
// a change notice, never an action. See coop/props/container_writer_watch.h.

#include "coop/props/container_writer_watch.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::props::container_writer_watch {
namespace {

namespace sg = ue_wrap::script_gate;
namespace ci = ue_wrap::container_inventory;

// Verb ids: addObject marks dirty; takeObj also arms the container-extraction birth latch that
// prop_drop_intent consumes (a client-extracted item's actor is admitted as a host-authoritative
// drop intent).
constexpr int kVerbDirty   = 1;   // addObject
constexpr int kVerbTakeObj = 2;   // takeObj
constexpr int kTagSale     = 3;   // droneSellLocation_C::sell

UE_FACT_FN(kRowAddObject, "container_writer_watch", nullptr, L"addObject", AnyClass);
UE_FACT_FN(kRowTakeObj,   "container_writer_watch", nullptr, L"takeObj",   AnyClass);
UE_FACT_FN(kRowSell, "container_writer_watch", L"droneSellLocation_C", L"sell", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_verbsRegistered = false;
bool g_verbEntered = false;   // the watch has fired at least once on this peer

// The takeObj-in-flight latch: armed at the verb's entry, consumed by prop_drop_intent at the
// extracted item's FinishSpawn enqueue (the item spawns inside the takeObj call, so the latch is
// live exactly then). Atomic exchange, so the consume is one-shot. A personal-inventory take arms
// it too, harmlessly; the drain's own gates filter those.
std::atomic<bool> g_takeObjInFlight{false};

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// Fires at the entry of addObject and takeObj on the game thread, a change notice rather than an
// action: it remembers which component was touched. No role gate: the verb is a local virtual
// call, so by the time it is seen the item has already moved on this machine, and an intent the
// host could deny cannot exist here. Gated on the host it dropped every client extraction: the
// client took one of two burgers, the host's slot stayed at two, and the world gained a burger.
sg::Verdict OnVerbEntry(const sg::Call& br) {
    // The first statement, ahead of every filter: a resolved verb name does not prove this callback
    // runs, and a registration once returned true with the callback inert for a whole session. If
    // this line is absent from a log, the lane is dead.
    if (!g_verbEntered) {
        g_verbEntered = true;
        UE_LOGI("container_contents: the verb watch ENTERED for the first time on this peer "
                "(role=%s) -- the addObject/takeObj edge is LIVE",
                IsHost() ? "HOST" : "CLIENT");
    }
    if (!br.object) return sg::Verdict::Run;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    // The verb filter matches on the name alone, so the context is discriminated here: the first
    // non-propInventory context carrying an addObject would otherwise poison the offset cache for
    // the session.
    if (!ci::IsInventory(br.object)) return sg::Verdict::Run;
    // Dev: the route census counts the verb by the scope open on the stack (observe-only).
    coop::dev::container_route_census::OnVerb(br, br.tag == kVerbTakeObj);
    // A takeObj on any inventory component arms the extraction latch; addObject must not.
    if (br.tag == kVerbTakeObj) g_takeObjInFlight.store(true, std::memory_order_relaxed);
    void* owner = ci::OwnerOf(br.object);
    if (!owner) return sg::Verdict::Run;
    const uint32_t eid =
        static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(owner));
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) return sg::Verdict::Run;
    // Resolve identity AT THE EDGE; the lane derefs nothing later.
    coop::props::container_contents_sync::MarkHostWrite(eid);
    return sg::Verdict::Run;
}

// The drone's sale. droneSellLocation_C::sell reads the drone's container, then empties it with
// Clear() on the save slot's GObjStack row and nameData and a direct updateVolumesAndMass: no
// addObject or takeObj runs, so the verb watch above never sees the write. The host marks the
// container dirty at the end of the body when it held records at the start, and the lane's sweep
// publishes the emptied slice.
// Declared on the drone's class: prop_coingun_C has a sell of its own and is not matched.
// The element id of the container of the drone a droneSellLocation_C names, or kInvalidId.
uint32_t SaleContainerEid(const sg::Call& br) {
    void* const container = ue_wrap::drone::ContainerOf(ue_wrap::drone::OfSellLocation(br.object));
    return container ? static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(container))
                     : static_cast<uint32_t>(coop::element::kInvalidId);
}

// The records the container held when the body began, so the post marks it only when the sale changed
// it: the body can return early without clearing anything. -1 when it did not read.
struct SaleBefore {
    uint32_t eid = static_cast<uint32_t>(coop::element::kInvalidId);
    int32_t records = -1;
};
SaleBefore g_saleBefore;

sg::Verdict OnSalePre(const sg::Call& br) {
    g_saleBefore = SaleBefore{};
    if (!IsHost()) return sg::Verdict::Run;
    const uint32_t eid = SaleContainerEid(br);
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) return sg::Verdict::Run;
    float vol = 0.f;
    int32_t records = -1;
    coop::props::container_contents_sync::ContentsDigest(eid, records, vol);
    g_saleBefore = SaleBefore{eid, records};
    return sg::Verdict::Run;
}

void OnSalePost(const sg::Call& br) {
    if (!IsHost()) return;
    const uint32_t eid = SaleContainerEid(br);
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) {
        UE_LOGW("container_contents: the drone's sale ran but its container has no element id -- "
                "the emptied container is not published");
        return;
    }
    const SaleBefore before = g_saleBefore;
    g_saleBefore = SaleBefore{};
    if (before.eid == eid && before.records == 0) return;   // already empty: the sale changed nothing
    coop::props::container_contents_sync::MarkHostWrite(eid);
    UE_LOGI("container_contents: the drone's sale cleared container eid=%u (%d records) -- marked for the sweep",
            eid, before.eid == eid ? before.records : -1);
}

bool g_saleSettled = false;
bool g_saleRegistered = false;

// Host only: registers the sale watch, then says once whether the gate took it. The names resolve
// on the caller's tick (script_gate::ResolvePendingNames).
void TickSaleWatch() {
    if (g_saleSettled || !IsHost()) return;
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::drone_sale_watch_off);
    if (off) {
        g_saleSettled = true;
        UE_LOGI("container_contents: the drone sale watch is OFF (drone_sale_watch_off)");
        return;
    }
    if (!g_saleRegistered) {
        g_saleRegistered = sg::WatchName(kRowSell, kTagSale, &OnSalePre, &OnSalePost);
        if (!g_saleRegistered) {
            g_saleSettled = true;
            UE_LOGE("container_contents: the drone sale watch was REFUSED by the gate -- a sold "
                    "drone container reaches the clients only through a later write");
        }
        return;
    }
    if (sg::NameWatchLive(kRowSell, kTagSale)) {
        g_saleSettled = true;
        UE_LOGI("container_contents: the drone sale watch is LIVE (droneSellLocation_C.sell)");
    } else if (sg::NameWatchSettled(kRowSell, kTagSale)) {
        g_saleSettled = true;
        UE_LOGE("container_contents: the drone sale watch is DEAD -- a sold drone container reaches "
                "the clients only through a later write");
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    TickSaleWatch();
    if (g_verbsRegistered) return;
    g_verbsRegistered =
        sg::WatchName(kRowAddObject, kVerbDirty, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowTakeObj,   kVerbTakeObj, &OnVerbEntry, nullptr);
    static bool s_saidFailed = false;
    if (!g_verbsRegistered && !s_saidFailed) {
        // Once. This block retries every tick, and the failure it reports is the gate
        // refusing permanently (a full table, or no detour), so an unlatched line here is a
        // warning per tick for the rest of the session.
        s_saidFailed = true;
        UE_LOGW("container_contents: verb registration FAILED -- the lane is inert");
    }
}

void OnDisconnect() { g_verbEntered = false; }

bool TakeObjInFlight() { return g_takeObjInFlight.exchange(false, std::memory_order_relaxed); }

bool VerbWatchEntered() { return g_verbEntered; }

}  // namespace coop::props::container_writer_watch

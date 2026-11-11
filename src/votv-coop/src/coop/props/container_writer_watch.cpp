// coop/props/container_writer_watch.cpp -- the writer watch on a container's inventory component: a change notice,
// never an action. On the host a hit marks the container for the sweep; on a client a shared container's mirror is
// display only, so a hit marks nothing. See coop/props/container_writer_watch.h.

#include "coop/props/container_writer_watch.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/container_writer_probe.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_contents_sync.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::props::container_writer_watch {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace ci = ue_wrap::container_inventory;

constexpr int kVerbAdd      = 1;   // addItemAsObject, addItemAdvanced, trimInventory: the component itself changed
constexpr int kVerbRemove   = 2;   // removeInventoryItem
constexpr int kVerbTransfer = 3;   // transferFromInvToInf: the component in its `to` parameter changed
constexpr int kTagDrop      = 4;   // the inventory screen's dropItems

UE_FACT_FN(kRowAddAsObject, "container_writer_watch", L"inventoryComponent_C", L"addItemAsObject", Declared);
UE_FACT_FN(kRowAddAdvanced, "container_writer_watch", L"inventoryComponent_C", L"addItemAdvanced", Declared);
UE_FACT_FN(kRowRemove,      "container_writer_watch", L"inventoryComponent_C", L"removeInventoryItem", Declared);
UE_FACT_FN(kRowTrim,        "container_writer_watch", L"inventoryComponent_C", L"trimInventory", Declared);
UE_FACT_FN(kRowTransfer,    "container_writer_watch", L"inventoryComponent_C", L"transferFromInvToInf", Declared);
UE_FACT_FN(kRowDrop,        "container_writer_watch", L"ui_FreeLayoutInventory_C", L"dropItems", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_verbsRegistered = false;
bool g_verbEntered = false;   // the watch has fired at least once on this peer

// The extraction latch: set at dropItems' entry and cleared at its exit, so it is live exactly while the screen spawns
// the items it drops; prop_drop_intent consumes it at such an item's FinishSpawn enqueue. Atomic exchange, so the
// consume is one-shot.
std::atomic<bool> g_extractInFlight{false};

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// The component a hit names: the body's own object, or transferFromInvToInf's `to`.
void* WrittenComponent(const sg::Call& br) {
    if (br.tag != kVerbTransfer) return br.object;
    static R::ParamOffset s_to{L"to"};
    const int32_t off = s_to.Of(br.function);
    if (off < 0 || !br.locals) return nullptr;
    void* to = nullptr;
    std::memcpy(&to, br.locals + off, sizeof(to));
    return to;
}

// Fires at the entry of each writer on the game thread, a change notice rather than an action: it remembers which
// component was touched. The verb is a local virtual call, so by the time the sweep reads it the item has moved on
// this machine. The role decides what it means:
//  - on the host it marks the container dirty, and the lane's sweep publishes it;
//  - on a client a shared world container's mirror is display only (the host holds the entries). The verb runs on the
//    client's copy, the watch forgets the applied hash so the host's next publication or its Gone truth puts the
//    mirror back, and the probe names the writer. A container the client threw is its author's until its birth slice
//    goes: that one marks like the host's, and its sweep ships the birth slice (container_birth).
// A verb inside a granted act's replay is handled by the replay itself and is left alone, by the latch.
sg::Verdict OnVerbEntry(const sg::Call& br) {
    // The first statement, ahead of every filter: a resolved verb name does not prove this callback
    // runs, and a registration once returned true with the callback inert for a whole session. If
    // this line is absent from a log, the lane is dead.
    if (!g_verbEntered) {
        g_verbEntered = true;
        UE_LOGI("container_contents: the verb watch ENTERED for the first time on this peer "
                "(role=%s) -- the inventory writer edge is LIVE",
                IsHost() ? "HOST" : "CLIENT");
    }
    void* const inv = WrittenComponent(br);
    if (!inv || !ci::IsInventory(inv)) return sg::Verdict::Run;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    // Dev: the route census counts the verb by the scope open on the stack (observe-only).
    coop::dev::container_route_census::OnVerb(br, br.tag == kVerbRemove);
    if (coop::props::container_act_request::InReplay()) return sg::Verdict::Run;
    void* owner = ci::OwnerOf(inv);
    if (!owner || !ci::IsWorldInventory(inv)) return sg::Verdict::Run;
    const uint32_t eid =
        static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(owner));
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) return sg::Verdict::Run;
    // Resolve identity AT THE EDGE; the lane derefs nothing later.
    if (!IsHost() && !coop::props::container_birth::IsBirthSlice(eid)) {
        // The verb runs, not cancelled: a writer that spawns an actor and then absorbs it would leave the
        // spawned actor behind; the host's truth restores the copy.
        coop::props::container_contents_sync::ForgetApplied(eid);
        coop::dev::container_writer_probe::OnClientVerb(eid, owner, br, br.tag == kVerbRemove);
        return sg::Verdict::Run;
    }
    coop::props::container_contents_sync::MarkHostWrite(eid);
    return sg::Verdict::Run;
}

sg::Verdict OnDropPre(const sg::Call&) {
    g_extractInFlight.store(true, std::memory_order_relaxed);
    return sg::Verdict::Run;
}

void OnDropPost(const sg::Call&) { g_extractInFlight.store(false, std::memory_order_relaxed); }

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (g_verbsRegistered) return;
    g_verbsRegistered =
        sg::WatchName(kRowAddAsObject, kVerbAdd, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowAddAdvanced, kVerbAdd, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowTrim, kVerbAdd, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowRemove, kVerbRemove, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowTransfer, kVerbTransfer, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowDrop, kTagDrop, &OnDropPre, &OnDropPost);
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

bool ExtractInFlight() { return g_extractInFlight.exchange(false, std::memory_order_relaxed); }

bool VerbWatchEntered() { return g_verbEntered; }

}  // namespace coop::props::container_writer_watch

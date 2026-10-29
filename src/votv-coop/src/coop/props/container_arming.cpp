// coop/props/container_arming.cpp -- see coop/props/container_arming.h.

#include "coop/props/container_arming.h"

#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_box.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/actor_end_play.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <unordered_map>

namespace coop::props::container_arming {

UE_FACT_FN(kRowSetCollision, "container_arming", L"PrimitiveComponent", L"SetCollisionEnabled", Declared);

namespace {

namespace cb = ue_wrap::container_box;
namespace cw = coop::props::container_slice_wire;
namespace E  = ue_wrap::engine;
namespace UH = ue_wrap::ufunction_hook;

// ECollisionEnabled as dingus() writes it: NoCollision disarms, QueryOnly arms.
constexpr uint8_t kNoCollision = 0;
constexpr uint8_t kQueryOnly = 1;
// An install whose row has not resolved is tried again no sooner than this, and given up after this many tries.
constexpr uint64_t kInstallRetryMs = 1000;
constexpr uint32_t kInstallTries = 30;

// One watched `overlap` component: the container it belongs to (its eid and its actor) and the armed state the hook last
// saw.
struct Watched {
    uint32_t eid = 0;
    void* owner = nullptr;
    bool armed = false;
};
std::unordered_map<void*, Watched> g_byComponent;   // the hook's lookup: one probe per SetCollisionEnabled call
// Untrack's, and Track's "already watched": the box and the actor a container is watched by.
struct Tracked { void* box = nullptr; void* owner = nullptr; };
std::map<uint32_t, Tracked> g_componentOf;
std::unordered_map<void*, uint32_t> g_eidOfOwner;   // the end-of-play sink's: a container's actor to its eid

coop::net::Session* g_session = nullptr;
bool g_hookBound = false;     // installed for this process; a Func patch has no unpatch
bool g_hookFailed = false;    // the row never resolved, or the patch was refused: said once, not retried
uint64_t g_nextTryMs = 0;
uint32_t g_tries = 0;
uint32_t g_edges = 0;         // armed-state changes the hook marked for publication
uint32_t g_mirrorWrites = 0;  // boxes a client's mirror wrote from a host flag
std::atomic<uint32_t> g_offThread{0};   // hook calls that arrived off the game thread and were skipped

bool IsHost() { return g_session && g_session->role() == coop::net::Role::Host; }

// An entry leaves the hook's set, and the eid's own record of it when that still names this box.
void EraseEntry(std::unordered_map<void*, Watched>::iterator it) {
    const auto have = g_componentOf.find(it->second.eid);
    if (have != g_componentOf.end() && have->second.box == it->first) g_componentOf.erase(have);
    g_eidOfOwner.erase(it->second.owner);
    g_byComponent.erase(it);
}

// The post hook on UPrimitiveComponent::SetCollisionEnabled. It runs inside the engine call and on every component's
// call in the game, so its first statements are the cheap rejects and a miss returns at once. A hit reads the box as the
// engine now holds it: a change of the armed state marks the container dirty, so the next sweep publishes it. MTA
// broadcasts an element's data to its peers when the server changes it (CStaticFunctionDefinitions.cpp:1005,
// SetElementData); here the game's own Blueprint makes the change, and the native it ends in is where it is caught.
void OnSetCollision(void* component, void*, void*) {
    if (g_byComponent.empty()) return;
    if (!ue_wrap::game_thread::IsGameThread()) {   // native dispatch is game-thread only: counted, so a call that is not shows
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto it = g_byComponent.find(component);
    if (it == g_byComponent.end()) return;
    Watched& w = it->second;
    void* const actor = coop::element::LivePropActor(w.eid);
    if (actor != w.owner || cb::OverlapComponent(actor) != component) {   // the container is gone, or the address is another object's
        EraseEntry(it);
        return;
    }
    const int now = E::GetComponentCollisionEnabled(component);
    if (now < 0) return;   // the box does not read: the next call reads it again
    const bool armed = now != kNoCollision;
    if (armed == w.armed) return;
    w.armed = armed;
    ++g_edges;
    UE_LOGI("container_arming: eid=%u box %s on the host (collision %d)", w.eid, armed ? "ARMED" : "disarmed", now);
    coop::props::container_contents_sync::MarkHostWrite(w.eid);
}

// A container's actor ends play: its watch goes with it, found by the actor whatever else has come to hold the address.
void OnEndPlay(void* actor, ue_wrap::actor_end_play::Reason) {
    if (g_eidOfOwner.empty()) return;
    const auto it = g_eidOfOwner.find(actor);
    if (it != g_eidOfOwner.end()) Untrack(it->second);
}

void TryInstall() {
    if (g_hookBound || g_hookFailed || !IsHost()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kInstallRetryMs;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowSetCollision);
    if (!fn) {   // the class is not loaded yet; a row that never resolves is given up, so the lookup does not go on for ever
        if (++g_tries >= kInstallTries) {
            g_hookFailed = true;
            UE_LOGE("container_arming: the SetCollisionEnabled row did not resolve in %u tries -- a container's arming "
                    "reaches its mirrors only with its next publication", kInstallTries);
        }
        return;
    }
    if (!UH::InstallPostHook(fn, &OnSetCollision)) {
        g_hookFailed = true;
        UE_LOGE("container_arming: the SetCollisionEnabled hook was refused -- a container's arming reaches its mirrors "
                "only with its next publication");
        return;
    }
    g_hookBound = true;
    UE_LOGI("container_arming: the SetCollisionEnabled hook is live on the host");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (!IsHost()) return;
    if (!ue_wrap::actor_end_play::AddSink(&OnEndPlay)) UE_LOGW("container_arming: the end-of-play sink table is full");
    TryInstall();
}

void Tick() {
    if (!g_hookBound && !g_hookFailed) TryInstall();
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("container_arming::OnDisconnect");
    UE_LOGI("container_arming: session end -- %u arming edge(s) marked, %u mirror box(es) written, %u hook call(s) off the "
            "game thread skipped", g_edges,
            g_mirrorWrites, g_offThread.load(std::memory_order_relaxed));
    g_byComponent.clear();
    g_componentOf.clear();
    g_eidOfOwner.clear();
    g_edges = 0;
    g_mirrorWrites = 0;
    g_offThread = 0;
    g_session = nullptr;
}

uint8_t ReadFlags(void* container) {
    void* const box = cb::OverlapComponent(container);
    return box && E::GetComponentCollisionEnabled(box) > kNoCollision ? cw::kFlagArmed : uint8_t{0};
}

bool IsArmed(void* container) { return (ReadFlags(container) & cw::kFlagArmed) != 0; }

// The container is the registry's actor for the eid, the same one the hook compares a hit with.
void Track(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("container_arming::Track");
    void* const container = coop::element::LivePropActor(eid);
    void* const box = cb::OverlapComponent(container);
    if (!box) return;
    const auto have = g_componentOf.find(eid);
    if (have != g_componentOf.end() && have->second.box == box) {
        const auto w = g_byComponent.find(box);
        if (w != g_byComponent.end() && w->second.eid == eid) return;   // watched already
    }
    Untrack(eid);
    // A roomba writes its own box from upd() on every peer: its edge has no consumer here, and each of its writes would
    // cost a reflected read. Its flags byte stays a physical read at each publication.
    if (cb::IsRoomba(container)) return;
    g_componentOf[eid] = Tracked{box, container};
    g_eidOfOwner[container] = eid;
    g_byComponent[box] = Watched{eid, container, E::GetComponentCollisionEnabled(box) > kNoCollision};
}

void Untrack(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("container_arming::Untrack");
    const auto have = g_componentOf.find(eid);
    if (have == g_componentOf.end()) return;
    // By address AND eid: the address may have passed to another tracked container's box since this one was freed.
    const auto w = g_byComponent.find(have->second.box);
    if (w != g_byComponent.end() && w->second.eid == eid) {
        EraseEntry(w);
        return;
    }
    const auto o = g_eidOfOwner.find(have->second.owner);   // the box is another container's now; this one's actor entry goes
    if (o != g_eidOfOwner.end() && o->second == eid) g_eidOfOwner.erase(o);
    g_componentOf.erase(have);
}

bool ApplyFlags(uint32_t eid, void* container, uint8_t flags) {
    UE_ASSERT_GAME_THREAD("container_arming::ApplyFlags");
    if (cb::IsRoomba(container)) return true;   // its own upd() owns the box: nothing for this lane to apply
    void* const box = cb::OverlapComponent(container);
    if (!box) return false;
    const bool want = (flags & cw::kFlagArmed) != 0;
    const int now = E::GetComponentCollisionEnabled(box);
    if (now < 0) return false;
    if ((now != kNoCollision) == want) return true;
    if (!E::SetComponentCollisionEnabled(box, want ? kQueryOnly : kNoCollision)) return false;
    ++g_mirrorWrites;
    UE_LOGI("container_arming: eid=%u the mirror's box %s from the host's flag", eid, want ? "ARMED" : "disarmed");
    return true;
}

}  // namespace coop::props::container_arming

// coop/props/container_arming.cpp -- see coop/props/container_arming.h.

#include "coop/props/container_arming.h"

#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_box.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

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
// An install that has not bound is tried again no sooner than this.
constexpr uint64_t kInstallRetryMs = 1000;

// One watched `overlap` component: the container it belongs to and the armed state the hook last saw.
struct Watched {
    uint32_t eid = 0;
    bool armed = false;
};
std::unordered_map<void*, Watched> g_byComponent;   // the hook's lookup: one probe per SetCollisionEnabled call
std::map<uint32_t, void*> g_componentOf;            // Untrack's, and Track's "already watched"

coop::net::Session* g_session = nullptr;
bool g_hookBound = false;     // installed for this process; a Func patch has no unpatch
bool g_hookFailed = false;    // the patch was refused after the row resolved: said once, not retried
uint64_t g_nextTryMs = 0;
uint32_t g_edges = 0;         // armed-state changes the hook marked for publication
uint32_t g_mirrorWrites = 0;  // boxes a client's mirror wrote from a host flag

bool IsHost() { return g_session && g_session->role() == coop::net::Role::Host; }

// The post hook on UPrimitiveComponent::SetCollisionEnabled. It runs inside the engine call and on every component's
// call in the game, so its first statement is the lookup and a miss returns at once. A hit reads the box as the engine
// now holds it: a change of the armed state marks the container dirty, so the next sweep publishes it.
void OnSetCollision(void* component, void*, void*) {
    if (g_byComponent.empty()) return;
    const auto it = g_byComponent.find(component);
    if (it == g_byComponent.end()) return;
    Watched& w = it->second;
    void* const actor = coop::element::LivePropActor(w.eid);
    if (!actor || cb::OverlapComponent(actor) != component) {   // the container is gone, or the address is another object's
        g_componentOf.erase(w.eid);
        g_byComponent.erase(it);
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

void TryInstall() {
    if (g_hookBound || g_hookFailed || !IsHost()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kInstallRetryMs;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowSetCollision);
    if (!fn) return;   // the class is not loaded yet
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
    TryInstall();
}

void Tick() {
    if (!g_hookBound && !g_hookFailed) TryInstall();
}

void OnDisconnect() {
    UE_LOGI("container_arming: session end -- %u arming edge(s) marked, %u mirror box(es) written", g_edges,
            g_mirrorWrites);
    g_byComponent.clear();
    g_componentOf.clear();
    g_edges = 0;
    g_mirrorWrites = 0;
    g_session = nullptr;
}

uint8_t ReadFlags(void* container) {
    void* const box = cb::OverlapComponent(container);
    return box && E::GetComponentCollisionEnabled(box) > kNoCollision ? cw::kFlagArmed : uint8_t{0};
}

void Track(uint32_t eid, void* container) {
    UE_ASSERT_GAME_THREAD("container_arming::Track");
    void* const box = cb::OverlapComponent(container);
    if (!box) return;
    const auto have = g_componentOf.find(eid);
    if (have != g_componentOf.end() && have->second == box) return;
    if (have != g_componentOf.end()) g_byComponent.erase(have->second);
    g_componentOf[eid] = box;
    g_byComponent[box] = Watched{eid, E::GetComponentCollisionEnabled(box) > kNoCollision};
}

void Untrack(uint32_t eid) {
    const auto have = g_componentOf.find(eid);
    if (have == g_componentOf.end()) return;
    g_byComponent.erase(have->second);
    g_componentOf.erase(have);
}

bool ApplyFlags(uint32_t eid, void* container, uint8_t flags) {
    UE_ASSERT_GAME_THREAD("container_arming::ApplyFlags");
    void* const box = cb::OverlapComponent(container);
    if (!box || cb::IsRoomba(container)) return false;
    const bool want = (flags & cw::kFlagArmed) != 0;
    const int now = E::GetComponentCollisionEnabled(box);
    if (now < 0 || (now != kNoCollision) == want) return false;
    if (!E::SetComponentCollisionEnabled(box, want ? kQueryOnly : kNoCollision)) return false;
    ++g_mirrorWrites;
    UE_LOGI("container_arming: eid=%u the mirror's box %s from the host's flag", eid, want ? "ARMED" : "disarmed");
    return true;
}

}  // namespace coop::props::container_arming

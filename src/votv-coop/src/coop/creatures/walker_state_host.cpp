// coop/creatures/walker_state_host.cpp -- see coop/creatures/walker_state_host.h.

#include "coop/creatures/walker_state_host.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/walker_graph.h"
#include "coop/creatures/walker_target.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::walker_state_host {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace GT = ue_wrap::game_thread;
namespace R = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace WG = coop::walker_graph;
namespace WT = coop::walker_target;

// The native the walker's Blueprint writes its mesh's damping through, on both edges of `looking`.
UE_FACT_FN(kRowSetAngularDamping, "walker_state_host", L"PrimitiveComponent", L"SetAngularDamping", Declared);

coop::net::Session* g_session = nullptr;  // game thread: every send is from a hook on it or from the tick

// What the hook's first compare reads: the walker's graph while the hook is armed, else null.
std::atomic<void*> g_graph{nullptr};
void*              g_pinnedGraph = nullptr;  // the graph `g_sites` was read from
WG::Calls          g_sites;
void*              g_dampFn = nullptr;
bool               g_hookInstalled = false;
bool               g_armed = false;
bool               g_subscribed = false;

// One walker's state, to every peer (`slot` < 0) or to one. False when it was not sent: no key yet (a walker still being born),
// or no peer to take it.
bool SendState(void* walker, uint8_t target, int slot, const char* why) {
    if (!g_session) return false;
    const std::wstring key = ue_wrap::prop::GetKeyString(walker);
    if (key.empty()) return false;
    coop::net::WalkerStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.looking = AC::ReadWalkerLooking(walker) ? 1 : 0;
    p.target = target;
    const bool ok = slot < 0 ? g_session->SendReliable(coop::net::ReliableKind::WalkerState, &p, sizeof(p))
                             : g_session->SendReliableToSlot(slot, coop::net::ReliableKind::WalkerState, &p, sizeof(p));
    if (ok) {
        const int hunted = target == WT::kNoTarget ? -1 : static_cast<int>(target);
        if (slot < 0) UE_LOGI("walker_state: sent walker '%ls' looking=%d target=%d (%s)", key.c_str(), p.looking, hunted, why);
        else UE_LOGI("walker_state: sent walker '%ls' looking=%d target=%d (%s, slot %d)", key.c_str(), p.looking, hunted, why, slot);
    }
    return ok;
}

bool IsSite(int32_t end) {
    const int n = g_sites.found < WG::kMaxCalls ? g_sites.found : WG::kMaxCalls;
    for (int i = 0; i < n; ++i)
        if (g_sites.ends[i] == end) return true;
    return false;
}

// The post hook on the damping write, game thread inside the engine's own call. Its first act is one compare of the calling
// frame's function with the walker's graph, which every other caller of the native fails; per call of any other caller that is
// the frame read and the compare, and with no walker held the hook is disarmed and the forwarder pays one load and a branch.
void OnDampingPost(void* /*context*/, void* source, void* /*result*/) {
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    void* const           graph = g_graph.load(std::memory_order_relaxed);
    if (frame.function != graph || !graph) return;
    if (!GT::IsGameThread() || !IsSite(frame.codeOffset) || !source) return;
    // Every call of the walker's graph here follows a change of `looking` (the chain is reached only when its formula differs
    // from the flag, and from the begin play), so the call is the edge and no last-sent value is kept.
    SendState(source, WT::TargetOf(source), -1, "looking edge");
}

void Disarm() {
    g_graph.store(nullptr, std::memory_order_relaxed);
    if (g_armed && UH::SetArmed(g_dampFn, &OnDampingPost, false)) g_armed = false;
}

// The target module's word: a graph while a walker is held, null when none is. The damping write's calls are pinned in the graph
// once per graph, the hook is installed once for the process and armed with the graph.
void OnGraph(void* graph) {
    if (!graph) {
        g_pinnedGraph = nullptr;
        Disarm();
        return;
    }
    if (graph != g_pinnedGraph) {
        g_pinnedGraph = nullptr;
        const ue_wrap::facts::Resolved native = ue_wrap::facts::Resolve(kRowSetAngularDamping);
        if (!native || !WG::FindCalls(graph, native.Fn(), g_sites)) {
            UE_LOGE("walker_state_host: the walker's graph or its damping call did not resolve -- a copy's pose is not sent");
            return;
        }
        if (g_sites.found < 1 || g_sites.found > WG::kMaxCalls) {
            UE_LOGE("walker_state_host: %d call(s) of the damping write in the walker's graph (1 to %d are expected) -- a "
                    "copy's pose is not sent", g_sites.found, WG::kMaxCalls);
            return;
        }
        g_pinnedGraph = graph;
        g_dampFn = native.Fn();
        UE_LOGI("walker_state_host: the walker writes its damping at %d site(s) of its graph, the first ending at offset %d",
                g_sites.found, g_sites.ends[0]);
    }
    if (!g_hookInstalled) {
        const ue_wrap::facts::Resolved native = ue_wrap::facts::Resolve(kRowSetAngularDamping);
        if (!native || !UH::InstallPostHook(native, &OnDampingPost, /*armed=*/false)) {
            UE_LOGE("walker_state_host: the damping seam did not install -- a copy's pose is not sent");
            return;
        }
        g_hookInstalled = true;
    }
    g_graph.store(graph, std::memory_order_relaxed);
    if (!g_armed && UH::SetArmed(g_dampFn, &OnDampingPost, true)) g_armed = true;
}

// The hunt changed (or ended): the walker's whole state goes out. A walker that is ending is not read through.
void OnTargetChanged(void* walker, uint8_t slot) {
    if (!walker || !R::IsLive(walker)) return;
    SendState(walker, slot, -1, "target edge");
}

struct JoinCtx {
    int slot;
};

void SendHeldTo(void* ctx, void* walker, uint8_t target) {
    SendState(walker, target, static_cast<JoinCtx*>(ctx)->slot, "join");
}

}  // namespace

void Install(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("walker_state_host::Install");
    if (session.role() != coop::net::Role::Host) return;
    g_session = &session;
    if (g_subscribed) return;
    g_subscribed = true;
    const bool target = WT::SubscribeTargetChanged(&OnTargetChanged);
    const bool graph = WT::SubscribeGraph(&OnGraph);
    if (!target || !graph)
        UE_LOGE("walker_state_host: the target module's subscriber table is full (target %d, graph %d) -- a copy is not told its "
                "walker's state", target ? 1 : 0, graph ? 1 : 0);
}

void SendJoinStateToSlot(int slot) {
    if (!g_session || slot <= 0) return;
    // The mannequin drill's red: a joiner is sent no walker state, so the drill's join step must FAIL on its missing row.
    static const bool s_redNoJoinState = coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill) == "no_join_state";
    if (s_redNoJoinState) return;
    JoinCtx ctx{slot};
    WT::ForEachHeld(&SendHeldTo, &ctx);
}

void OnDisconnect() {
    g_session = nullptr;
    g_pinnedGraph = nullptr;
    Disarm();
}

}  // namespace coop::walker_state_host

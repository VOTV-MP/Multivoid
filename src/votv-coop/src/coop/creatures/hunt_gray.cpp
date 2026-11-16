// coop/creatures/hunt_gray.cpp -- see coop/creatures/hunt_gray.h.

#include "coop/creatures/hunt_gray.h"

#include "coop/creatures/hunt_target.h"
#include "coop/creatures/hunt_target_rule.h"
#include "coop/creatures/served_player.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace coop::hunt_gray {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace SP = coop::served_player;

UE_FACT_FN(kRowDespawn, "hunt_gray", L"grayEventController_C", L"despawn", Declared);
constexpr int kTagDespawn = 0x48475259;  // 'HGRY'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;

struct Pending {
    void*   controller;
    int32_t index;
};
std::vector<Pending> g_pending;
unsigned g_served = 0;

bool OnHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host;
}

bool IsGray(void* actor) {
    return actor && R::NameEquals(R::NameOf(R::ClassOf(actor)), L"graytest_C");
}

// The controller's `despawn`, before its body: who the punch should land on. Its caller is the gray that was touched or that arrived;
// a call with no gray (a drill's) is judged from the controller.
sg::Verdict OnDespawnPre(const sg::Call& c) {
    if (!c.object || !OnHost() || !ue_wrap::game_thread::IsGameThread()) return sg::Verdict::Run;
    void* const origin = IsGray(c.callerObject) ? c.callerObject : c.object;
    const uint8_t slot = coop::hunt_target::NearestTo(origin, 0.f);
    if (slot == coop::players::kPeerIdHost) return sg::Verdict::Run;  // the Blueprint's own read: the host is the nearest
    if (!SP::Serve(c.object, slot, SP::kMainPlayerAny)) {
        UE_LOGW("hunt_gray: the controller cannot be served slot %u (the getMainPlayer watch is not ready); the punch lands on the host",
                static_cast<unsigned>(slot));
        return sg::Verdict::Run;
    }
    g_pending.push_back(Pending{c.object, R::InternalIndexOf(c.object)});
    ++g_served;
    UE_LOGI("hunt_gray: a gray %p calls despawn on %p: its punch is served to slot %u, the nearest player (#%u)", c.callerObject,
            c.object, static_cast<unsigned>(slot), g_served);
    return sg::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session& session) {
    g_session.store(&session, std::memory_order_release);
    if (session.role() != coop::net::Role::Host || g_watched) return;
    if (sg::WatchName(kRowDespawn, kTagDespawn, &OnDespawnPre, nullptr)) {
        g_watched = true;
        UE_LOGI("hunt_gray: watching %ls at the script-body gate (the gray's punch lands on the nearest player)", kRowDespawn.Name());
    }
}

void Tick(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host || g_pending.empty()) return;
    for (const Pending& p : g_pending)
        if (R::IsLiveByIndex(p.controller, p.index)) SP::Unserve(p.controller);
    g_pending.clear();
}

bool IsController(void* actor) {
    return actor && R::NameEquals(R::NameOf(R::ClassOf(actor)), L"grayEventController_C");
}

void OnDisconnect() {
    g_pending.clear();
    g_served = 0;
    g_session.store(nullptr, std::memory_order_release);
    // The name watch stays registered for the process; it does nothing off the host (OnHost).
}

}  // namespace coop::hunt_gray

// coop/interactables/toggle_verbs.cpp -- see coop/interactables/toggle_verbs.h.

#include "coop/interactables/toggle_verbs.h"

#include "coop/interactables/interactable_sync.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load stays local

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/lightswitch.h"

#include <atomic>
#include <cstdint>

namespace coop::toggle_verbs {
namespace {

namespace sg = ue_wrap::script_gate;

// A verb's name matches every class's function of that name, so each row's callback first tests
// the row's class.
struct Row {
    const wchar_t* verb;
    int tag;
    const char* what;
    bool (*IsInstance)(void* obj);
    void (*Edge)(void* actor);
};
constexpr Row kRows[] = {
    { L"use", 0x4C535755 /*'LSWU'*/, "a light switch's use", &ue_wrap::lightswitch::IsLightSwitch,
      &coop::interactable_sync::OnLightSwitchVerb },
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles
// once every watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kRowCount] = {};
bool g_settled = false;
uint64_t g_edges[kRowCount] = {};

const Row* RowOf(int tag, int* index) {
    for (int i = 0; i < kRowCount; ++i)
        if (kRows[i].tag == tag) { *index = i; return &kRows[i]; }
    return nullptr;
}

// Every peer, after the body.
void OnVerbPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.object) return;
    int i = -1;
    const Row* row = RowOf(call.tag, &i);
    if (!row || !row->IsInstance(call.object)) return;
    // A client's load of a world is the host's save; its devices' own calls there are not its player's.
    if (s->role() != coop::net::Role::Host && !coop::net_pump::IsInAnnouncedWorld(call.object)) return;
    ++g_edges[i];
    row->Edge(call.object);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(kRows[i].verb, kRows[i].tag, nullptr, &OnVerbPost)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[TOGGLE-VERB] the script-body gate refused the watch on %s -- for the rest of this process that "
                "lane cannot see it", kRows[i].what);
    }
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(kRows[i].verb, kRows[i].tag)) ++live;
    }
    if (live == kRowCount) {
        g_settled = true;
        UE_LOGI("[TOGGLE-VERB] the toggle verb gates are live: %d of %d", live, kRowCount);
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[TOGGLE-VERB] %d of %d toggle verb gates are dead (refused, or resolved into a full table)",
                kRowCount - live, kRowCount);
    }
}

void OnDisconnect() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_edges[i])
            UE_LOGI("[TOGGLE-VERB] session end -- %s reached its lane %llu time(s)", kRows[i].what,
                    static_cast<unsigned long long>(g_edges[i]));
        g_edges[i] = 0;
    }
}

}  // namespace coop::toggle_verbs
